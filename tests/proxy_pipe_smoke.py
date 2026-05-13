import json
import os
import subprocess
import sys
import time


def send(proc, payload):
    proc.stdin.write(json.dumps(payload, separators=(",", ":")) + "\n")
    proc.stdin.flush()


def recv(proc):
    line = proc.stdout.readline()
    if not line:
        raise RuntimeError(f"adapter closed stdout; stderr={proc.stderr.read()}")
    return json.loads(line)


def main():
    adapter = sys.argv[1]
    server = sys.argv[2]
    endpoint = sys.argv[3]
    transport = "named-pipe" if os.name == "nt" else "unix-socket"

    env = os.environ.copy()
    env["MCP_ENABLE_STDIO"] = "0"
    env["MCP_ENABLE_PIPE"] = "1"
    env["MCP_PIPE_PATH"] = endpoint
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
        time.sleep(0.1)
        adapter_proc = subprocess.Popen(
            [adapter, "--transport", transport, "--endpoint", endpoint],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
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
                    "clientInfo": {"name": "proxy-pipe-smoke", "version": "0.1"},
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

        send(
            adapter_proc,
            {
                "jsonrpc": "2.0",
                "id": 2,
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
