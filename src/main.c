#include "mcp_proxy/platform.h"
#include "mcp_proxy/stdio_frontend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char *argv0)
{
    fprintf(stderr,
            "Usage: %s --transport named-pipe|unix-socket|tcp "
            "[--endpoint PATH] [--host HOST] [--port PORT] [--timeout-ms MS]\n",
            argv0);
}

static unsigned int parse_uint(const char *text, unsigned int default_value)
{
    char *end = NULL;
    unsigned long value;

    if (!text || text[0] == '\0')
        return default_value;

    value = strtoul(text, &end, 10);
    if (!end || *end != '\0' || value > 65535ul)
        return default_value;

    return (unsigned int)value;
}

int main(int argc, char **argv)
{
    struct mcp_backend_config config;
    int i;

    memset(&config, 0, sizeof(config));
#if defined(MCP_PLATFORM_WINDOWS)
    config.transport = MCP_PROXY_TRANSPORT_PIPE;
    config.endpoint = "\\\\.\\pipe\\mcp-server";
#else
    config.transport = MCP_PROXY_TRANSPORT_PIPE;
    config.endpoint = "/tmp/mcp-server.sock";
#endif
    config.host = "127.0.0.1";
    config.port = 8765;
    config.timeout_ms = 3000;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--transport") == 0 && i + 1 < argc) {
            const char *transport = argv[++i];

            if (strcmp(transport, "named-pipe") == 0 || strcmp(transport, "unix-socket") == 0) {
                config.transport = MCP_PROXY_TRANSPORT_PIPE;
            } else if (strcmp(transport, "tcp") == 0) {
                config.transport = MCP_PROXY_TRANSPORT_TCP;
            } else {
                usage(argv[0]);
                return 2;
            }
        } else if (strcmp(argv[i], "--endpoint") == 0 && i + 1 < argc) {
            config.endpoint = argv[++i];
        } else if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
            config.host = argv[++i];
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            config.port = parse_uint(argv[++i], config.port);
        } else if (strcmp(argv[i], "--timeout-ms") == 0 && i + 1 < argc) {
            config.timeout_ms = parse_uint(argv[++i], config.timeout_ms);
        } else if (strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    return mcp_proxy_stdio_run(&config);
}
