use futures::{
    io::{AsyncReadExt, AsyncWriteExt},
    stream::StreamExt,
};
use libp2p::{
    core::{
        upgrade::{InboundUpgrade, OutboundUpgrade, UpgradeInfo},
        Endpoint,
    },
    identity, noise,
    swarm::{
        behaviour::{FromSwarm, NetworkBehaviour, NotifyHandler, ToSwarm},
        handler::OneShotHandler,
        ConnectionDenied, ConnectionId, Stream, StreamProtocol, SwarmEvent, THandler,
        THandlerInEvent, THandlerOutEvent,
    },
    tcp, yamux, Multiaddr, PeerId, SwarmBuilder,
};
use serde_json::json;
use std::{
    collections::{HashSet, VecDeque},
    convert::Infallible,
    env,
    iter,
    task::{Context, Poll},
    time::Duration,
};

static ECHO_PROTOCOL: StreamProtocol = StreamProtocol::new("/test/echo/1.0.0");
const ECHO_PAYLOAD: &[u8] = b"interop-ping";

fn emit(value: serde_json::Value) {
    println!("{}", value);
}

fn env_or(name: &str, fallback: &str) -> String {
    env::var(name).unwrap_or_else(|_| fallback.to_string())
}

#[derive(Debug)]
enum EchoStreamEvent {
    Inbound(Stream),
    Outbound(Stream),
}

#[derive(Debug, Clone, Copy)]
struct InboundEchoUpgrade;

#[derive(Debug, Clone, Copy)]
struct OutboundEchoUpgrade;

impl UpgradeInfo for InboundEchoUpgrade {
    type Info = StreamProtocol;
    type InfoIter = iter::Once<StreamProtocol>;

    fn protocol_info(&self) -> Self::InfoIter {
        iter::once(ECHO_PROTOCOL.clone())
    }
}

impl UpgradeInfo for OutboundEchoUpgrade {
    type Info = StreamProtocol;
    type InfoIter = iter::Once<StreamProtocol>;

    fn protocol_info(&self) -> Self::InfoIter {
        iter::once(ECHO_PROTOCOL.clone())
    }
}

impl InboundUpgrade<Stream> for InboundEchoUpgrade {
    type Output = EchoStreamEvent;
    type Error = Infallible;
    type Future = futures::future::Ready<Result<Self::Output, Self::Error>>;

    fn upgrade_inbound(self, stream: Stream, _: Self::Info) -> Self::Future {
        futures::future::ready(Ok(EchoStreamEvent::Inbound(stream)))
    }
}

impl OutboundUpgrade<Stream> for OutboundEchoUpgrade {
    type Output = EchoStreamEvent;
    type Error = Infallible;
    type Future = futures::future::Ready<Result<Self::Output, Self::Error>>;

    fn upgrade_outbound(self, stream: Stream, _: Self::Info) -> Self::Future {
        futures::future::ready(Ok(EchoStreamEvent::Outbound(stream)))
    }
}

struct EchoBehaviour {
    open_outbound_stream: bool,
    requested_connections: HashSet<ConnectionId>,
    queued_actions: VecDeque<ToSwarm<(), OutboundEchoUpgrade>>,
}

impl EchoBehaviour {
    fn new(open_outbound_stream: bool) -> Self {
        Self {
            open_outbound_stream,
            requested_connections: HashSet::new(),
            queued_actions: VecDeque::new(),
        }
    }
}

impl NetworkBehaviour for EchoBehaviour {
    type ConnectionHandler = OneShotHandler<InboundEchoUpgrade, OutboundEchoUpgrade, EchoStreamEvent>;
    type ToSwarm = ();

    fn handle_established_inbound_connection(
        &mut self,
        _: ConnectionId,
        _: PeerId,
        _: &Multiaddr,
        _: &Multiaddr,
    ) -> Result<THandler<Self>, ConnectionDenied> {
        Ok(OneShotHandler::new(
            libp2p::swarm::SubstreamProtocol::new(InboundEchoUpgrade, ()),
            Default::default(),
        ))
    }

    fn handle_established_outbound_connection(
        &mut self,
        _: ConnectionId,
        _: PeerId,
        _: &Multiaddr,
        _: Endpoint,
        _: libp2p::core::transport::PortUse,
    ) -> Result<THandler<Self>, ConnectionDenied> {
        Ok(OneShotHandler::new(
            libp2p::swarm::SubstreamProtocol::new(InboundEchoUpgrade, ()),
            Default::default(),
        ))
    }

    fn on_swarm_event(&mut self, event: FromSwarm) {
        if let FromSwarm::ConnectionEstablished(established) = event {
            if self.open_outbound_stream &&
                established.endpoint.is_dialer() &&
                self.requested_connections.insert(established.connection_id)
            {
                self.queued_actions.push_back(ToSwarm::NotifyHandler {
                    peer_id: established.peer_id,
                    handler: NotifyHandler::One(established.connection_id),
                    event: OutboundEchoUpgrade,
                });
            }
        }
    }

    fn on_connection_handler_event(
        &mut self,
        peer_id: PeerId,
        _connection_id: ConnectionId,
        event: THandlerOutEvent<Self>,
    ) {
        match event {
            Ok(EchoStreamEvent::Inbound(mut stream)) => {
                let peer = peer_id.to_string();
                emit(json!({
                    "type": "stream_opened",
                    "peer_id": peer,
                    "proto": ECHO_PROTOCOL.as_ref(),
                }));
                tokio::spawn(async move {
                    let mut buf = vec![0_u8; 4096];
                    loop {
                        match stream.read(&mut buf).await {
                            Ok(0) => break,
                            Ok(n) => {
                                if let Err(err) = stream.write_all(&buf[..n]).await {
                                    emit(json!({
                                        "type": "stream_write_failed",
                                        "detail": err.to_string(),
                                    }));
                                    break;
                                }
                            }
                            Err(err) => {
                                emit(json!({
                                    "type": "stream_read_failed",
                                    "detail": err.to_string(),
                                }));
                                break;
                            }
                        }
                    }
                });
            }
            Ok(EchoStreamEvent::Outbound(mut stream)) => {
                let peer = peer_id.to_string();
                emit(json!({
                    "type": "stream_opened",
                    "peer_id": peer,
                    "proto": ECHO_PROTOCOL.as_ref(),
                }));
                tokio::spawn(async move {
                    if let Err(err) = stream.write_all(ECHO_PAYLOAD).await {
                        emit(json!({
                            "type": "stream_write_failed",
                            "detail": err.to_string(),
                        }));
                        return;
                    }

                    let mut reply = vec![0_u8; ECHO_PAYLOAD.len()];
                    if let Err(err) = stream.read_exact(&mut reply).await {
                        emit(json!({
                            "type": "stream_read_failed",
                            "detail": err.to_string(),
                        }));
                        return;
                    }

                    emit(json!({
                        "type": "stream_echo_received",
                        "payload": String::from_utf8_lossy(&reply),
                    }));
                    let _ = stream.close().await;
                });
            }
            Err(err) => emit(json!({
                "type": "stream_open_failed",
                "detail": err.to_string(),
                "proto": ECHO_PROTOCOL.as_ref(),
            })),
        }
    }

    fn poll(
        &mut self,
        _: &mut Context<'_>,
    ) -> Poll<ToSwarm<Self::ToSwarm, THandlerInEvent<Self>>> {
        if let Some(action) = self.queued_actions.pop_front() {
            return Poll::Ready(action);
        }
        Poll::Pending
    }
}

#[tokio::main]
async fn main() {
    let listen_addr: Multiaddr = env_or("LISTEN_ADDR", "/ip4/0.0.0.0/tcp/4002")
        .parse()
        .expect("invalid LISTEN_ADDR");
    let target_addr = env::var("TARGET_ADDR").ok();
    let runtime_ms: u64 = env_or("RUNTIME_MS", "15000")
        .parse()
        .expect("invalid RUNTIME_MS");

    let keypair = identity::Keypair::generate_ed25519();
    let mut swarm = SwarmBuilder::with_existing_identity(keypair)
        .with_tokio()
        .with_tcp(tcp::Config::default().nodelay(true), noise::Config::new, yamux::Config::default)
        .expect("tcp transport")
        .with_behaviour(|_| EchoBehaviour::new(target_addr.is_some()))
        .expect("behaviour")
        .build();

    let local_peer_id = swarm.local_peer_id().to_string();
    swarm.listen_on(listen_addr).expect("listen");
    emit(json!({
        "type": "ready",
        "peer_id": local_peer_id,
    }));

    if let Some(target) = target_addr {
        let multiaddr: Multiaddr = target.parse().expect("invalid TARGET_ADDR");
        if let Err(err) = swarm.dial(multiaddr.clone()) {
            emit(json!({
                "type": "dial_failed",
                "detail": err.to_string(),
                "target": multiaddr.to_string(),
            }));
        } else {
            emit(json!({
                "type": "dial_requested",
                "target": multiaddr.to_string(),
            }));
        }
    }

    let timer = tokio::time::sleep(Duration::from_millis(runtime_ms));
    tokio::pin!(timer);

    loop {
        tokio::select! {
            _ = &mut timer => {
                emit(json!({"type":"finished","detail":"runtime elapsed"}));
                break;
            }
            Some(event) = swarm.next() => {
                match event {
                    SwarmEvent::NewListenAddr { address, .. } => emit(json!({
                        "type": "listen_addr",
                        "address": format!("{}/p2p/{}", address, swarm.local_peer_id()),
                    })),
                    SwarmEvent::ConnectionEstablished { peer_id, endpoint, .. } => {
                        emit(json!({
                            "type": "connected",
                            "peer_id": peer_id.to_string(),
                            "endpoint": format!("{:?}", endpoint),
                        }));
                        if endpoint.is_dialer() {
                            emit(json!({
                                "type": "dial_succeeded",
                                "target": peer_id.to_string(),
                            }));
                        }
                    }
                    SwarmEvent::ConnectionClosed { peer_id, .. } => emit(json!({
                        "type": "disconnected",
                        "peer_id": peer_id.to_string(),
                    })),
                    SwarmEvent::OutgoingConnectionError { peer_id, error, .. } => emit(json!({
                        "type": "dial_failed",
                        "detail": error.to_string(),
                        "peer_id": peer_id.map(|p| p.to_string()),
                    })),
                    _ => {}
                }
            }
        }
    }
}
