import json
import os
import subprocess
import sys
from pathlib import Path


def send(proc, payload):
    proc.stdin.write(json.dumps(payload, separators=(",", ":")) + "\n")
    proc.stdin.flush()


def recv(proc):
    line = proc.stdout.readline()
    if not line:
        raise RuntimeError(f"adapter closed stdout; stderr={proc.stderr.read()}")
    return json.loads(line)


def write_security_config(path, tls_dir, identity=None, server_name=""):
    enabled = identity is not None
    path.write_text(
        json.dumps(
            {
                "version": 1,
                "enabled": enabled,
                "mtls": {
                    "ca_file": str(tls_dir / "ca.cert.pem") if enabled else "",
                    "certificate_file": str(tls_dir / f"{identity}.cert.pem") if enabled else "",
                    "private_key_file": str(tls_dir / f"{identity}.key.pem") if enabled else "",
                    "server_name": server_name,
                },
            }
        ),
        encoding="utf-8",
    )


def main():
    adapter = sys.argv[1]
    server = sys.argv[2]
    port = int(sys.argv[3])
    security_mode = sys.argv[4]
    tls_dir = Path(sys.argv[5])

    env = os.environ.copy()
    env["MCP_ENABLE_STDIO"] = "0"
    env["MCP_ENABLE_TCP"] = "1"
    env["MCP_TCP_HOST"] = "127.0.0.1"
    env["MCP_TCP_PORT"] = str(port)
    env["MCP_TCP_SECURITY"] = security_mode
    adapter_env = os.environ.copy()
    adapter_env["MCP_TCP_SECURITY"] = security_mode
    adapter_args = [
        adapter,
        "--transport",
        "tcp",
        "--host",
        "127.0.0.1",
        "--port",
        str(port),
    ]
    if security_mode == "mtls":
        server_security = tls_dir / "server_network_security.json"
        adapter_security = tls_dir / "adapter_network_security.json"
        write_security_config(server_security, tls_dir, "node-a")
        write_security_config(adapter_security, tls_dir, "adapter", "localhost")
        env["MCP_NETWORK_SECURITY_CONFIG"] = str(server_security)
        adapter_args.extend(["--network-security-config", str(adapter_security)])
    else:
        plaintext_security = tls_dir / "plaintext_network_security.json"
        write_security_config(plaintext_security, tls_dir)
        env["MCP_NETWORK_SECURITY_CONFIG"] = str(plaintext_security)
        adapter_args.extend(["--network-security-config", str(plaintext_security)])
    server_proc = subprocess.Popen(
        [server],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=env,
        text=True,
        encoding="utf-8",
    )

    adapter_proc = None
    try:
        adapter_proc = subprocess.Popen(
            adapter_args,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            env=adapter_env,
        )

        send(
            adapter_proc,
            {
                "jsonrpc": "2.0",
                "id": 1,
                "method": "initialize",
                "params": {
                    "protocolVersion": "2024-11-05",
                    "capabilities": {},
                    "clientInfo": {"name": "proxy-tcp-smoke", "version": "0.1"},
                },
            },
        )
        init = recv(adapter_proc)
        assert init["result"]["serverInfo"]["name"] == "mcp_server", init

        send(
            adapter_proc,
            {
                "jsonrpc": "2.0",
                "method": "notifications/initialized",
                "params": {"meta": {"id": 999}},
            },
        )

        send(adapter_proc, {"jsonrpc": "2.0", "id": 2, "method": "tools/list", "params": {}})
        tools = recv(adapter_proc)["result"]["tools"]
        assert "system.ping" in {tool["name"] for tool in tools}, tools

        send(
            adapter_proc,
            {
                "jsonrpc": "2.0",
                "id": 3,
                "method": "tools/call",
                "params": {"name": "system.ping", "arguments": {}},
            },
        )
        ping = recv(adapter_proc)
        assert ping["result"]["isError"] is False, ping
        assert ping["result"]["content"][0]["text"] == "pong", ping
    finally:
        if adapter_proc and adapter_proc.stdin:
            adapter_proc.stdin.close()
        if adapter_proc:
            try:
                adapter_proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                adapter_proc.kill()
                adapter_proc.wait(timeout=5)
        server_proc.terminate()
        try:
            server_proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            server_proc.kill()
            server_proc.wait(timeout=5)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
