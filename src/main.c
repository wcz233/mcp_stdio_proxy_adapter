#include "mcp_proxy/platform.h"
#include "mcp_proxy/stdio_frontend.h"
#if MCP_TCP_SECURITY_MTLS
#include "mcp_proxy/network_security_config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char *argv0)
{
    fprintf(stderr,
            "Usage: %s --transport named-pipe|unix-socket|tcp "
            "[--endpoint PATH] [--host HOST] [--port PORT] [--timeout-ms MS] "
            "[--network-security-config FILE]\n",
            argv0);
}

static int configure_tcp_security(struct mcp_backend_config *config,
                                  const char *security_path,
#if MCP_TCP_SECURITY_MTLS
                                  struct mcp_proxy_network_security_config **security)
#else
                                  void **security)
#endif
{
    const char *requested = getenv("MCP_TCP_SECURITY");
    bool requested_set = requested && requested[0] != '\0';

    *security = NULL;
    if (requested_set && strcmp(requested, "plaintext") != 0 && strcmp(requested, "mtls") != 0) {
        fputs("MCP_TCP_SECURITY must be plaintext or mtls\n", stderr);
        return -1;
    }
#if MCP_TCP_SECURITY_MTLS
    {
        bool requested_mtls = requested_set && strcmp(requested, "mtls") == 0;
        bool config_mtls;
        int rc = mcp_proxy_network_security_config_create(
            security,
            security_path,
            !requested_set || requested_mtls);

        if (rc != 0)
            return -1;
        config_mtls = mcp_proxy_network_security_config_enabled(*security);
        if (requested_set && *security && requested_mtls != config_mtls) {
            fputs("MCP_TCP_SECURITY conflicts with network_security.json enabled\n", stderr);
            return -1;
        }
        if (!requested_set && !*security) {
            fputs("network_security.json is required when MCP_TCP_SECURITY is unset\n", stderr);
            return -1;
        }
        config->tcp_mtls_enabled = *security && config_mtls;
        config->tls_ca_file = mcp_proxy_network_security_config_ca_file(*security);
        config->tls_cert_file = mcp_proxy_network_security_config_certificate_file(*security);
        config->tls_key_file = mcp_proxy_network_security_config_private_key_file(*security);
        config->tls_server_name = mcp_proxy_network_security_config_server_name(*security);
        fprintf(stderr,
                "TCP security mode: %s\n",
                config->tcp_mtls_enabled ? "mtls" : "plaintext");
        return 0;
    }
#else
    (void)config;
    (void)security_path;
    if (requested_set && strcmp(requested, "mtls") == 0) {
        fputs("MCP_TCP_SECURITY=mtls is unavailable in a plaintext build\n", stderr);
        return -1;
    }
    fputs("TCP security mode: plaintext\n", stderr);
    return 0;
#endif
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
    const char *security_path = NULL;
#if MCP_TCP_SECURITY_MTLS
    struct mcp_proxy_network_security_config *security = NULL;
#else
    void *security = NULL;
#endif
    int rc;
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
        } else if (strcmp(argv[i], "--network-security-config") == 0 && i + 1 < argc) {
            security_path = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    if (config.transport == MCP_PROXY_TRANSPORT_TCP &&
        configure_tcp_security(&config, security_path, &security) != 0) {
#if MCP_TCP_SECURITY_MTLS
        mcp_proxy_network_security_config_destroy(security);
#endif
        return 2;
    }

    rc = mcp_proxy_stdio_run(&config);
#if MCP_TCP_SECURITY_MTLS
    mcp_proxy_network_security_config_destroy(security);
#endif
    return rc;
}
