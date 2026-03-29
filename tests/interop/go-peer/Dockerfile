FROM golang:1.22-bookworm AS build

WORKDIR /app
COPY tests/interop/go-peer/go.mod /app/go.mod
COPY tests/interop/go-peer/main.go /app/main.go
RUN go mod tidy
RUN go build -o /usr/local/bin/go-libp2p-peer ./main.go

FROM debian:bookworm-slim
COPY --from=build /usr/local/bin/go-libp2p-peer /usr/local/bin/go-libp2p-peer
ENTRYPOINT ["/usr/local/bin/go-libp2p-peer"]
