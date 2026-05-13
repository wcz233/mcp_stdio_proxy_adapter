#ifndef MCP_PROXY_BACKEND_CLIENT_H
#define MCP_PROXY_BACKEND_CLIENT_H

#include <stddef.h>

enum mcp_proxy_transport {
    MCP_PROXY_TRANSPORT_PIPE = 1,
    MCP_PROXY_TRANSPORT_TCP = 2,
};

struct mcp_backend_config {
    enum mcp_proxy_transport transport;
    const char *endpoint;
    const char *host;
    unsigned int port;
    unsigned int timeout_ms;
};

struct mcp_backend_client;

int mcp_backend_client_open(struct mcp_backend_client **out,
                            const struct mcp_backend_config *config);
void mcp_backend_client_close(struct mcp_backend_client *client);
int mcp_backend_client_send(struct mcp_backend_client *client, const char *data, size_t len);
int mcp_backend_client_recv(struct mcp_backend_client *client, char **out_data, size_t *out_len);
const char *mcp_backend_client_last_error(const struct mcp_backend_client *client);

#endif
