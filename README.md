# MCP stdio proxy adapter

Thin Codex-facing stdio adapter for an independent `mcp_server`.

The adapter reads JSON-RPC line messages from stdin, forwards them to a backend
using `uint32_be length + JSON bytes` inside a mutually authenticated TLS 1.3
connection, and writes backend responses to stdout as single-line JSON-RPC.

## Build

### Linux

```bash
cmake -S . -B build -C config/linux_defconfig.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

### Windows

```powershell
cmake -S . -B build -C config/windows_defconfig.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

## Run examples

```powershell
.\mcp_stdio_proxy_adapter.exe --transport named-pipe --endpoint \\.\pipe\mcp-server
.\mcp_stdio_proxy_adapter.exe --transport tcp --host 127.0.0.1 --port 8765 `
  --tls-ca C:\mcp-secrets\ca.cert.pem `
  --tls-cert C:\mcp-secrets\client.cert.pem `
  --tls-key C:\mcp-secrets\client.key.pem `
  --tls-server-name localhost --timeout-ms 3000
```

TCP mode requires Mbed TLS 4.1 or later at build time and the three TLS file
arguments at runtime. `--tls-server-name` defaults to `--host` and must match a
DNS name or IP address in the server certificate SAN. Pipe transports are local
and do not use TLS.
