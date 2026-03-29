package main

import (
	"bytes"
	"crypto/hmac"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"os"

	"github.com/flynn/noise"
	"golang.org/x/crypto/chacha20poly1305"
	"golang.org/x/crypto/curve25519"
)

type traceResult struct {
	Type                 string `json:"type"`
	Msg1Len              int    `json:"msg1_len"`
	Msg2Len              int    `json:"msg2_len"`
	Msg3Len              int    `json:"msg3_len"`
	FlynnMsg2Hex         string `json:"flynn_msg2_hex"`
	ManualMsg2Hex        string `json:"manual_msg2_hex"`
	ManualEqualsFlynnMsg2 bool  `json:"manual_equals_flynn_msg2"`
	Msg2ReadByInitiator  bool   `json:"msg2_read_by_initiator"`
	InitiatorPayloadLen  int    `json:"initiator_payload_len"`
	InitiatorReadMsg2Err string `json:"initiator_read_msg2_err,omitempty"`
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

func sha256h(parts ...[]byte) []byte {
	h := sha256.New()
	for _, p := range parts {
		h.Write(p)
	}
	return h.Sum(nil)
}

func hmacSHA256(key, data []byte) []byte {
	mac := hmac.New(sha256.New, key)
	mac.Write(data)
	return mac.Sum(nil)
}

func hkdf2(ck, ikm []byte) ([]byte, []byte) {
	temp := hmacSHA256(ck, ikm)
	out1 := hmacSHA256(temp, []byte{0x01})
	out2Input := make([]byte, 0, len(out1)+1)
	out2Input = append(out2Input, out1...)
	out2Input = append(out2Input, 0x02)
	out2 := hmacSHA256(temp, out2Input)
	return out1, out2
}

func x25519Base(secret []byte) []byte {
	pub, err := curve25519.X25519(secret, curve25519.Basepoint)
	if err != nil {
		panic(err)
	}
	return pub
}

func main() {
	initStaticSecret := mustDecodeHexEnv("INIT_STATIC_SECRET_HEX",
		"606162636465666768696a6b6c6d6e6f707172737475767778797a7b7c7d7e7f")
	initEphemeralSecret := mustDecodeHexEnv("INIT_EPHEMERAL_SECRET_HEX",
		"404142434445464748494a4b4c4d4e4f505152535455565758595a5b5c5d5e5f")
	respStaticSecret := mustDecodeHexEnv("RESP_STATIC_SECRET_HEX",
		"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f")
	respEphemeralSecret := mustDecodeHexEnv("RESP_EPHEMERAL_SECRET_HEX",
		"202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f")

	if len(initStaticSecret) != 32 || len(initEphemeralSecret) != 32 ||
		len(respStaticSecret) != 32 || len(respEphemeralSecret) != 32 {
		panic("all secrets must decode to 32 bytes")
	}

	initStaticKp, err := noise.DH25519.GenerateKeypair(bytes.NewReader(initStaticSecret))
	if err != nil {
		panic(err)
	}
	respStaticKp, err := noise.DH25519.GenerateKeypair(bytes.NewReader(respStaticSecret))
	if err != nil {
		panic(err)
	}

	initiator, err := noise.NewHandshakeState(noise.Config{
		CipherSuite:   noise.NewCipherSuite(noise.DH25519, noise.CipherChaChaPoly, noise.HashSHA256),
		Pattern:       noise.HandshakeXX,
		Initiator:     true,
		StaticKeypair: initStaticKp,
		Prologue:      []byte{},
		Random:        bytes.NewReader(initEphemeralSecret),
	})
	if err != nil {
		panic(err)
	}

	responder, err := noise.NewHandshakeState(noise.Config{
		CipherSuite:   noise.NewCipherSuite(noise.DH25519, noise.CipherChaChaPoly, noise.HashSHA256),
		Pattern:       noise.HandshakeXX,
		Initiator:     false,
		StaticKeypair: respStaticKp,
		Prologue:      []byte{},
		Random:        bytes.NewReader(respEphemeralSecret),
	})
	if err != nil {
		panic(err)
	}

	msg1, _, _, err := initiator.WriteMessage(nil, nil)
	if err != nil {
		panic(err)
	}
	if _, _, _, err := responder.ReadMessage(nil, msg1); err != nil {
		panic(err)
	}
	msg2, _, _, err := responder.WriteMessage(nil, nil)
	if err != nil {
		panic(err)
	}

	// Manual msg2 reconstruction for step-by-step comparison with flynn internals.
	proto := []byte("Noise_XX_25519_ChaChaPoly_SHA256")
	ck := make([]byte, 32)
	copy(ck, proto)
	h := make([]byte, 32)
	copy(h, proto)
	h = sha256h(h, []byte{}) // MixHash(empty prologue)
	h = sha256h(h, msg1)     // MixHash(e initiator)
	respEphemeralPub := x25519Base(respEphemeralSecret)
	h = sha256h(h, respEphemeralPub) // MixHash(e responder)

	ee, err := curve25519.X25519(respEphemeralSecret, msg1)
	if err != nil {
		panic(err)
	}
	ck, key1 := hkdf2(ck, ee)
	aead1, err := chacha20poly1305.New(key1)
	if err != nil {
		panic(err)
	}
	nonce0 := make([]byte, 12)
	ctStatic := aead1.Seal(nil, nonce0, respStaticKp.Public, h)
	h = sha256h(h, ctStatic)

	es, err := curve25519.X25519(respStaticSecret, msg1)
	if err != nil {
		panic(err)
	}
	ck, key2 := hkdf2(ck, es)
	aead2, err := chacha20poly1305.New(key2)
	if err != nil {
		panic(err)
	}
	ctPayload := aead2.Seal(nil, nonce0, nil, h)
	_ = ck
	manualMsg2 := make([]byte, 0, len(respEphemeralPub)+len(ctStatic)+len(ctPayload))
	manualMsg2 = append(manualMsg2, respEphemeralPub...)
	manualMsg2 = append(manualMsg2, ctStatic...)
	manualMsg2 = append(manualMsg2, ctPayload...)

	payload, _, _, readErr := initiator.ReadMessage(nil, msg2)
	result := traceResult{
		Type:                "flynn_initiator_read_msg2",
		Msg1Len:             len(msg1),
		Msg2Len:             len(msg2),
		FlynnMsg2Hex:        hex.EncodeToString(msg2),
		ManualMsg2Hex:       hex.EncodeToString(manualMsg2),
		ManualEqualsFlynnMsg2: bytes.Equal(msg2, manualMsg2),
		Msg2ReadByInitiator: readErr == nil,
		InitiatorPayloadLen: len(payload),
	}
	if readErr != nil {
		result.InitiatorReadMsg2Err = readErr.Error()
	} else {
		msg3, _, _, err := initiator.WriteMessage(nil, nil)
		if err != nil {
			panic(err)
		}
		result.Msg3Len = len(msg3)
	}

	blob, _ := json.Marshal(result)
	fmt.Println(string(blob))
}
