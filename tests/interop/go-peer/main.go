package main

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"strconv"
	"time"

	libp2p "github.com/libp2p/go-libp2p"
	"github.com/libp2p/go-libp2p/core/network"
	"github.com/libp2p/go-libp2p/core/peer"
	"github.com/libp2p/go-libp2p/p2p/muxer/yamux"
	"github.com/libp2p/go-libp2p/p2p/security/noise"
	ma "github.com/multiformats/go-multiaddr"
)

func emit(kind string, fields map[string]any) {
	fields["type"] = kind
	blob, _ := json.Marshal(fields)
	fmt.Println(string(blob))
}

func envOr(name, fallback string) string {
	if value := os.Getenv(name); value != "" {
		return value
	}
	return fallback
}

func main() {
	ctx := context.Background()
	listenAddr := envOr("LISTEN_ADDR", "/ip4/0.0.0.0/tcp/4001")
	targetAddr := os.Getenv("TARGET_ADDR")
	runtimeMs, _ := strconv.Atoi(envOr("RUNTIME_MS", "15000"))

	listenMultiaddr, err := ma.NewMultiaddr(listenAddr)
	if err != nil {
		panic(err)
	}

	host, err := libp2p.New(
		libp2p.ListenAddrs(listenMultiaddr),
		libp2p.Security(noise.ID, noise.New),
		libp2p.Muxer("/yamux/1.0.0", yamux.DefaultTransport),
	)
	if err != nil {
		panic(err)
	}
	defer host.Close()

	host.Network().Notify(&network.NotifyBundle{
		ConnectedF: func(_ network.Network, conn network.Conn) {
			emit("connected", map[string]any{
				"peer_id": conn.RemotePeer().String(),
				"addr":    conn.RemoteMultiaddr().String(),
			})
		},
		DisconnectedF: func(_ network.Network, conn network.Conn) {
			emit("disconnected", map[string]any{
				"peer_id": conn.RemotePeer().String(),
			})
		},
	})

	host.SetStreamHandler("/test/echo/1.0.0", func(stream network.Stream) {
		defer stream.Close()
		emit("stream_opened", map[string]any{
			"peer_id": stream.Conn().RemotePeer().String(),
			"proto":   stream.Protocol(),
		})
		buf := make([]byte, 4096)
		for {
			n, readErr := stream.Read(buf)
			if n > 0 {
				if _, writeErr := stream.Write(buf[:n]); writeErr != nil {
					break
				}
			}
			if readErr != nil {
				break
			}
		}
	})

	addrs := make([]string, 0, len(host.Addrs()))
	for _, addr := range host.Addrs() {
		addrs = append(addrs, fmt.Sprintf("%s/p2p/%s", addr, host.ID()))
	}
	emit("ready", map[string]any{
		"peer_id": host.ID().String(),
		"listen":  addrs,
	})

	if targetAddr != "" {
		target, err := ma.NewMultiaddr(targetAddr)
		if err != nil {
			panic(err)
		}
		info, err := peer.AddrInfoFromP2pAddr(target)
		if err != nil {
			emit("dial_failed", map[string]any{"detail": "target address must include /p2p peer id"})
			<-time.After(time.Duration(runtimeMs) * time.Millisecond)
			emit("finished", map[string]any{"detail": "runtime elapsed"})
			return
		}
		if err := host.Connect(ctx, *info); err != nil {
			emit("dial_failed", map[string]any{"detail": err.Error()})
		} else {
			emit("dial_succeeded", map[string]any{"target": targetAddr})
			stream, err := host.NewStream(ctx, info.ID, "/test/echo/1.0.0")
			if err != nil {
				emit("stream_open_failed", map[string]any{
					"peer_id": info.ID.String(),
					"proto":   "/test/echo/1.0.0",
					"detail":  err.Error(),
				})
			} else {
				emit("stream_opened", map[string]any{
					"peer_id": info.ID.String(),
					"proto":   stream.Protocol(),
				})
				payload := []byte("interop-ping")
				if _, err := stream.Write(payload); err != nil {
					emit("stream_write_failed", map[string]any{"detail": err.Error()})
				} else {
					reply := make([]byte, len(payload))
					if _, err := io.ReadFull(stream, reply); err != nil {
						emit("stream_read_failed", map[string]any{"detail": err.Error()})
					} else {
						emit("stream_echo_received", map[string]any{
							"payload": string(reply),
						})
					}
				}
				_ = stream.Close()
			}
		}
	}

	<-time.After(time.Duration(runtimeMs) * time.Millisecond)
	emit("finished", map[string]any{"detail": "runtime elapsed"})
}
