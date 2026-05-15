# MCP stdio proxy adapter

Thin Codex-facing stdio adapter for an independent `mcp_server`.

The adapter reads JSON-RPC line messages from stdin, forwards them to a backend
using `uint32_be length + JSON bytes`, and writes backend responses to stdout as
single-line JSON-RPC.

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
.\mcp_stdio_proxy_adapter.exe --transport tcp --host 127.0.0.1 --port 8765 --timeout-ms 3000
```
