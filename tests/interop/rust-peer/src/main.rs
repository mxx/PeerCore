use futures::StreamExt;
use libp2p::{identity, noise, swarm::SwarmEvent, tcp, yamux, Multiaddr, SwarmBuilder};
use serde_json::json;
use std::{env, time::Duration};

fn emit(value: serde_json::Value) {
    println!("{}", value);
}

fn env_or(name: &str, fallback: &str) -> String {
    env::var(name).unwrap_or_else(|_| fallback.to_string())
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
        .with_behaviour(|_| libp2p::swarm::dummy::Behaviour)
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

    let mut timer = tokio::time::sleep(Duration::from_millis(runtime_ms));
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
                    SwarmEvent::ConnectionEstablished { peer_id, endpoint, .. } => emit(json!({
                        "type": "connected",
                        "peer_id": peer_id.to_string(),
                        "endpoint": format!("{:?}", endpoint),
                    })),
                    SwarmEvent::ConnectionClosed { peer_id, .. } => emit(json!({
                        "type": "disconnected",
                        "peer_id": peer_id.to_string(),
                    })),
                    _ => {}
                }
            }
        }
    }
}
