package main

import (
	"bytes"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"os"

	"github.com/flynn/noise"
)

type result struct {
	Type               string `json:"type"`
	Msg1Len            int    `json:"msg1_len"`
	Msg2Len            int    `json:"msg2_len"`
	Msg2Hex            string `json:"msg2_hex"`
	LocalStaticPubHex  string `json:"local_static_pub_hex"`
	LocalEphemeralHex  string `json:"local_ephemeral_pub_hex"`
	RemoteEphemeralHex string `json:"remote_ephemeral_pub_hex"`
	PayloadLen         int    `json:"payload_len"`
}

func mustDecodeHexEnv(name, fallback string) []byte {
	value := os.Getenv(name)
	if value == "" {
		value = fallback
	}
	out, err := hex.DecodeString(value)
	if err != nil {
		panic(fmt.Sprintf("invalid %s: %v", name, err))
	}
	return out
}

func main() {
	msg1 := mustDecodeHexEnv("MSG1_HEX", "")
	if len(msg1) == 0 {
		panic("MSG1_HEX is required")
	}

	staticSecret := mustDecodeHexEnv("STATIC_SECRET_HEX",
		"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f")
	ephemeralSecret := mustDecodeHexEnv("EPHEMERAL_SECRET_HEX",
		"202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f")
	prologue := mustDecodeHexEnv("PROLOGUE_HEX", "")
	payload := mustDecodeHexEnv("PAYLOAD_HEX", "")

	if len(staticSecret) != 32 {
		panic("STATIC_SECRET_HEX must decode to 32 bytes")
	}
	if len(ephemeralSecret) != 32 {
		panic("EPHEMERAL_SECRET_HEX must decode to 32 bytes")
	}

	staticKp, err := noise.DH25519.GenerateKeypair(bytes.NewReader(staticSecret))
	if err != nil {
		panic(err)
	}

	hs, err := noise.NewHandshakeState(noise.Config{
		CipherSuite:   noise.NewCipherSuite(noise.DH25519, noise.CipherChaChaPoly, noise.HashSHA256),
		Pattern:       noise.HandshakeXX,
		Initiator:     false,
		StaticKeypair: staticKp,
		Prologue:      prologue,
		Random:        bytes.NewReader(ephemeralSecret),
	})
	if err != nil {
		panic(err)
	}

	if _, _, _, err := hs.ReadMessage(nil, msg1); err != nil {
		panic(err)
	}

	msg2, _, _, err := hs.WriteMessage(nil, payload)
	if err != nil {
		panic(err)
	}

	blob, _ := json.Marshal(result{
		Type:               "oracle_result",
		Msg1Len:            len(msg1),
		Msg2Len:            len(msg2),
		Msg2Hex:            hex.EncodeToString(msg2),
		LocalStaticPubHex:  hex.EncodeToString(staticKp.Public),
		LocalEphemeralHex:  hex.EncodeToString(hs.LocalEphemeral().Public),
		RemoteEphemeralHex: hex.EncodeToString(hs.PeerEphemeral()),
		PayloadLen:         len(payload),
	})
	fmt.Println(string(blob))
}
