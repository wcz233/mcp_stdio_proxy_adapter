#ifndef MCP_PROXY_PLATFORM_H
#define MCP_PROXY_PLATFORM_H

#if defined(MCP_PLATFORM_WINDOWS) && defined(MCP_PLATFORM_LINUX)
#error "Only one of MCP_PLATFORM_WINDOWS or MCP_PLATFORM_LINUX may be defined"
#endif

#if !defined(MCP_PLATFORM_WINDOWS) && !defined(MCP_PLATFORM_LINUX)
#error "Build configuration must define MCP_PLATFORM_WINDOWS or MCP_PLATFORM_LINUX"
#endif

#endif
