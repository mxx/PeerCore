FROM golang:1.25-bookworm AS build

WORKDIR /app
COPY tests/interop/go-noise-oracle/go.mod /app/go.mod
COPY tests/interop/go-noise-oracle/main.go /app/main.go
RUN go mod tidy
RUN go build -o /usr/local/bin/go-noise-oracle ./main.go

FROM debian:bookworm-slim
COPY --from=build /usr/local/bin/go-noise-oracle /usr/local/bin/go-noise-oracle
ENTRYPOINT ["/usr/local/bin/go-noise-oracle"]
