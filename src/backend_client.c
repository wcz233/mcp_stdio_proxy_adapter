#include "mcp_proxy/backend_client.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#endif

#define MCP_PROXY_MAX_FRAME (16u * 1024u * 1024u)

struct mcp_backend_client {
    enum mcp_proxy_transport transport;
    unsigned int timeout_ms;
    char last_error[192];
#ifdef _WIN32
    HANDLE pipe;
    SOCKET socket_fd;
#else
    int fd;
#endif
};

static void set_error(struct mcp_backend_client *client, const char *message)
{
    snprintf(client->last_error, sizeof(client->last_error), "%s", message ? message : "backend error");
}

static uint32_t read_u32_be(const unsigned char *data)
{
    return ((uint32_t)data[0] << 24) |
           ((uint32_t)data[1] << 16) |
           ((uint32_t)data[2] << 8) |
           (uint32_t)data[3];
}

static void write_u32_be(unsigned char *data, uint32_t value)
{
    data[0] = (unsigned char)((value >> 24) & 0xffu);
    data[1] = (unsigned char)((value >> 16) & 0xffu);
    data[2] = (unsigned char)((value >> 8) & 0xffu);
    data[3] = (unsigned char)(value & 0xffu);
}

static unsigned long long now_ms(void)
{
#ifdef _WIN32
    return (unsigned long long)GetTickCount64();
#else
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (unsigned long long)ts.tv_sec * 1000ull + (unsigned long long)ts.tv_nsec / 1000000ull;
#endif
}

static void sleep_ms(unsigned int ms)
{
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec ts;

    ts.tv_sec = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)(ms % 1000u) * 1000000l;
    nanosleep(&ts, NULL);
#endif
}

#ifdef _WIN32
static int read_exact(struct mcp_backend_client *client, void *buffer, size_t len)
{
    unsigned char *cursor = buffer;

    while (len > 0) {
        if (client->transport == MCP_PROXY_TRANSPORT_PIPE) {
            DWORD chunk = len > ULONG_MAX ? ULONG_MAX : (DWORD)len;
            DWORD read_count = 0;

            if (!ReadFile(client->pipe, cursor, chunk, &read_count, NULL) || read_count == 0) {
                set_error(client, "backend read failed");
                return -1;
            }
            cursor += read_count;
            len -= read_count;
        } else {
            int chunk = len > INT_MAX ? INT_MAX : (int)len;
            int rc = recv(client->socket_fd, (char *)cursor, chunk, 0);

            if (rc <= 0) {
                set_error(client, "backend read failed");
                return -1;
            }
            cursor += rc;
            len -= (size_t)rc;
        }
    }

    return 0;
}

static int write_exact(struct mcp_backend_client *client, const void *buffer, size_t len)
{
    const unsigned char *cursor = buffer;

    while (len > 0) {
        if (client->transport == MCP_PROXY_TRANSPORT_PIPE) {
            DWORD chunk = len > ULONG_MAX ? ULONG_MAX : (DWORD)len;
            DWORD written = 0;

            if (!WriteFile(client->pipe, cursor, chunk, &written, NULL) || written == 0) {
                set_error(client, "backend write failed");
                return -1;
            }
            cursor += written;
            len -= written;
        } else {
            int chunk = len > INT_MAX ? INT_MAX : (int)len;
            int rc = send(client->socket_fd, (const char *)cursor, chunk, 0);

            if (rc <= 0) {
                set_error(client, "backend write failed");
                return -1;
            }
            cursor += rc;
            len -= (size_t)rc;
        }
    }

    return 0;
}

static int open_pipe(struct mcp_backend_client *client, const char *path)
{
    unsigned long long deadline = now_ms() + client->timeout_ms;

    do {
        client->pipe = CreateFileA(path,
                                   GENERIC_READ | GENERIC_WRITE,
                                   0,
                                   NULL,
                                   OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL,
                                   NULL);
        if (client->pipe != INVALID_HANDLE_VALUE)
            return 0;
        WaitNamedPipeA(path, 50);
    } while (now_ms() < deadline);

    if (client->pipe == INVALID_HANDLE_VALUE) {
        set_error(client, "failed to connect named pipe backend");
        return -1;
    }

    return 0;
}

static int open_tcp(struct mcp_backend_client *client, const char *host, unsigned int port)
{
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    struct addrinfo *item;
    char port_text[16];
    int rc;
    unsigned long long deadline = now_ms() + client->timeout_ms;

    snprintf(port_text, sizeof(port_text), "%u", port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;

    rc = getaddrinfo(host, port_text, &hints, &result);
    if (rc != 0) {
        set_error(client, "failed to resolve tcp backend");
        return -1;
    }

    do {
        client->socket_fd = INVALID_SOCKET;
        for (item = result; item; item = item->ai_next) {
            client->socket_fd = socket(item->ai_family, item->ai_socktype, item->ai_protocol);
            if (client->socket_fd == INVALID_SOCKET)
                continue;
            if (connect(client->socket_fd, item->ai_addr, (int)item->ai_addrlen) == 0)
                break;
            closesocket(client->socket_fd);
            client->socket_fd = INVALID_SOCKET;
        }
        if (client->socket_fd != INVALID_SOCKET)
            break;
        sleep_ms(50);
    } while (now_ms() < deadline);

    freeaddrinfo(result);
    if (client->socket_fd == INVALID_SOCKET) {
        set_error(client, "failed to connect tcp backend");
        return -1;
    }

    return 0;
}
#else
static int read_exact(struct mcp_backend_client *client, void *buffer, size_t len)
{
    unsigned char *cursor = buffer;

    while (len > 0) {
        ssize_t rc = read(client->fd, cursor, len);

        if (rc <= 0) {
            set_error(client, "backend read failed");
            return -1;
        }
        cursor += rc;
        len -= (size_t)rc;
    }

    return 0;
}

static int write_exact(struct mcp_backend_client *client, const void *buffer, size_t len)
{
    const unsigned char *cursor = buffer;

    while (len > 0) {
        ssize_t rc = write(client->fd, cursor, len);

        if (rc <= 0) {
            set_error(client, "backend write failed");
            return -1;
        }
        cursor += rc;
        len -= (size_t)rc;
    }

    return 0;
}

static int open_pipe(struct mcp_backend_client *client, const char *path)
{
    struct sockaddr_un addr;
    unsigned long long deadline = now_ms() + client->timeout_ms;

    if (strlen(path) >= sizeof(addr.sun_path)) {
        set_error(client, "unix socket path is too long");
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, path);

    do {
        client->fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (client->fd < 0) {
            set_error(client, "failed to create unix socket");
            return -1;
        }

        if (connect(client->fd, (struct sockaddr *)&addr, sizeof(addr)) == 0)
            return 0;

        close(client->fd);
        client->fd = -1;
        sleep_ms(50);
    } while (now_ms() < deadline);

    set_error(client, "failed to connect unix socket backend");
    return -1;
}

static int open_tcp(struct mcp_backend_client *client, const char *host, unsigned int port)
{
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    struct addrinfo *item;
    char port_text[16];
    int rc;
    unsigned long long deadline = now_ms() + client->timeout_ms;

    snprintf(port_text, sizeof(port_text), "%u", port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;

    rc = getaddrinfo(host, port_text, &hints, &result);
    if (rc != 0) {
        set_error(client, "failed to resolve tcp backend");
        return -1;
    }

    do {
        client->fd = -1;
        for (item = result; item; item = item->ai_next) {
            client->fd = socket(item->ai_family, item->ai_socktype, item->ai_protocol);
            if (client->fd < 0)
                continue;
            if (connect(client->fd, item->ai_addr, item->ai_addrlen) == 0)
                break;
            close(client->fd);
            client->fd = -1;
        }
        if (client->fd >= 0)
            break;
        sleep_ms(50);
    } while (now_ms() < deadline);

    freeaddrinfo(result);
    if (client->fd < 0) {
        set_error(client, "failed to connect tcp backend");
        return -1;
    }

    return 0;
}
#endif

int mcp_backend_client_open(struct mcp_backend_client **out,
                            const struct mcp_backend_config *config)
{
    struct mcp_backend_client *client;
    int rc;
#ifdef _WIN32
    WSADATA wsa;
#endif

    *out = NULL;
    if (!config)
        return -1;

    client = calloc(1, sizeof(*client));
    if (!client)
        return -1;

    client->transport = config->transport;
    client->timeout_ms = config->timeout_ms ? config->timeout_ms : 3000;
#ifdef _WIN32
    client->pipe = INVALID_HANDLE_VALUE;
    client->socket_fd = INVALID_SOCKET;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        set_error(client, "WSAStartup failed");
        rc = -1;
        goto done;
    }
#else
    client->fd = -1;
#endif

    if (config->transport == MCP_PROXY_TRANSPORT_PIPE) {
        rc = open_pipe(client, config->endpoint);
    } else if (config->transport == MCP_PROXY_TRANSPORT_TCP) {
        rc = open_tcp(client, config->host, config->port);
    } else {
        set_error(client, "unsupported backend transport");
        rc = -1;
    }

#ifdef _WIN32
done:
#endif
    if (rc != 0) {
        *out = client;
        return -1;
    }

    *out = client;
    return 0;
}

void mcp_backend_client_close(struct mcp_backend_client *client)
{
    if (!client)
        return;

#ifdef _WIN32
    if (client->pipe != INVALID_HANDLE_VALUE)
        CloseHandle(client->pipe);
    if (client->socket_fd != INVALID_SOCKET)
        closesocket(client->socket_fd);
    WSACleanup();
#else
    if (client->fd >= 0)
        close(client->fd);
#endif
    free(client);
}

int mcp_backend_client_send(struct mcp_backend_client *client, const char *data, size_t len)
{
    unsigned char header[4];

    if (!client || !data || len == 0 || len > UINT32_MAX) {
        if (client)
            set_error(client, "invalid backend frame");
        return -1;
    }

    write_u32_be(header, (uint32_t)len);
    if (write_exact(client, header, sizeof(header)) != 0)
        return -1;
    return write_exact(client, data, len);
}

int mcp_backend_client_recv(struct mcp_backend_client *client, char **out_data, size_t *out_len)
{
    unsigned char header[4];
    uint32_t len;
    char *data;

    *out_data = NULL;
    *out_len = 0;
    if (!client)
        return -1;

    if (read_exact(client, header, sizeof(header)) != 0)
        return -1;

    len = read_u32_be(header);
    if (len == 0 || len > MCP_PROXY_MAX_FRAME) {
        set_error(client, "invalid backend frame length");
        return -1;
    }

    data = malloc((size_t)len + 1);
    if (!data) {
        set_error(client, "out of memory");
        return -1;
    }

    if (read_exact(client, data, len) != 0) {
        free(data);
        return -1;
    }

    data[len] = '\0';
    *out_data = data;
    *out_len = (size_t)len;
    return 0;
}

const char *mcp_backend_client_last_error(const struct mcp_backend_client *client)
{
    if (!client || client->last_error[0] == '\0')
        return "backend error";
    return client->last_error;
}
