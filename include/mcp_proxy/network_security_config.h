#ifndef MCP_PROXY_NETWORK_SECURITY_CONFIG_H
#define MCP_PROXY_NETWORK_SECURITY_CONFIG_H

#include <stdbool.h>

struct mcp_proxy_network_security_config;

int mcp_proxy_network_security_config_create(
    struct mcp_proxy_network_security_config **out,
    const char *command_line_path,
    bool required);
void mcp_proxy_network_security_config_destroy(
    struct mcp_proxy_network_security_config *config);

bool mcp_proxy_network_security_config_enabled(
    const struct mcp_proxy_network_security_config *config);
const char *mcp_proxy_network_security_config_ca_file(
    const struct mcp_proxy_network_security_config *config);
const char *mcp_proxy_network_security_config_certificate_file(
    const struct mcp_proxy_network_security_config *config);
const char *mcp_proxy_network_security_config_private_key_file(
    const struct mcp_proxy_network_security_config *config);
const char *mcp_proxy_network_security_config_server_name(
    const struct mcp_proxy_network_security_config *config);

#endif
