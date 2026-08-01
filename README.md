# MCP stdio proxy adapter

Thin Codex-facing stdio adapter for an independent `mcp_server`. The adapter reads
JSON-RPC lines from stdin and forwards length-prefixed frames over a named pipe,
Unix-domain socket, plaintext TCP, or mutually authenticated TLS 1.3 TCP.

## Build

`MCP_TCP_SECURITY` controls compiled TCP capabilities and defaults to `mtls`:

```bash
cmake -S . -B build -DMCP_TCP_SECURITY=plaintext -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

A `plaintext` build does not compile or link Jansson or Mbed TLS. An `mtls` standalone
build requires CMake packages for both libraries. Building this adapter through the
parent `mcp_server` project reuses its bundled copies.

## Run

Local transports do not use the TCP security configuration:

```powershell
.\mcp_stdio_proxy_adapter.exe --transport named-pipe --endpoint \\.\pipe\mcp-server
```

For TCP, select the runtime mode with `MCP_TCP_SECURITY` and use the same
`network_security.json` schema as `mcp_server`. The adapter accepts the config path
through `--network-security-config` or `MCP_NETWORK_SECURITY_CONFIG`; the command-line
path has priority.

```powershell
$env:MCP_TCP_SECURITY = "mtls"
.\mcp_stdio_proxy_adapter.exe --transport tcp --host 127.0.0.1 --port 8765 `
  --network-security-config C:\mcp-config\network_security.json `
  --timeout-ms 3000
```

When `mtls.server_name` is empty, `--host` is used for certificate SAN validation.
An mTLS-capable build may run in plaintext only when `MCP_TCP_SECURITY=plaintext` and
the selected JSON also has `enabled=false`. A plaintext-only build rejects
`MCP_TCP_SECURITY=mtls`.
