#include "mcp_proxy/backend_client.h"
#include "mcp_proxy/platform.h"

#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(MCP_PLATFORM_WINDOWS)
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
    mbedtls_x509_crt ca;
    mbedtls_x509_crt certificate;
    mbedtls_pk_context private_key;
    mbedtls_ssl_config tls_config;
    mbedtls_ssl_context tls;
    int tls_initialized;
#if defined(MCP_PLATFORM_WINDOWS)
    HANDLE pipe;
    SOCKET socket_fd;
#else
    int fd;
#endif
};

static void set_error(struct mcp_backend_client *client, const char *message);

static void set_tls_error(struct mcp_backend_client *client, const char *operation, int rc)
{
    char detail[112];

    mbedtls_strerror(rc, detail, sizeof(detail));
    snprintf(client->last_error,
             sizeof(client->last_error),
             "%s: %s (-0x%04x)",
             operation,
             detail,
             (unsigned int)-rc);
}

static int tls_socket_send(void *ctx, const unsigned char *buffer, size_t len)
{
    struct mcp_backend_client *client = ctx;
#if defined(MCP_PLATFORM_WINDOWS)
    int chunk = len > INT_MAX ? INT_MAX : (int)len;
    int rc = send(client->socket_fd, (const char *)buffer, chunk, 0);
#else
    ssize_t rc = send(client->fd, buffer, len, 0);
#endif

    return rc > 0 ? (int)rc : MBEDTLS_ERR_NET_SEND_FAILED;
}

static int tls_socket_recv(void *ctx, unsigned char *buffer, size_t len)
{
    struct mcp_backend_client *client = ctx;
#if defined(MCP_PLATFORM_WINDOWS)
    int chunk = len > INT_MAX ? INT_MAX : (int)len;
    int rc = recv(client->socket_fd, (char *)buffer, chunk, 0);
#else
    ssize_t rc = recv(client->fd, buffer, len, 0);
#endif

    return rc > 0 ? (int)rc : MBEDTLS_ERR_NET_RECV_FAILED;
}

static int start_tls(struct mcp_backend_client *client, const struct mcp_backend_config *config)
{
    const char *server_name = config->tls_server_name ? config->tls_server_name : config->host;
    int rc;

    if (!config->tls_ca_file || !config->tls_cert_file || !config->tls_key_file) {
        set_error(client, "TCP backend requires CA, certificate, and private key files");
        return -1;
    }
    if (psa_crypto_init() != PSA_SUCCESS) {
        set_error(client, "psa_crypto_init failed");
        return -1;
    }

    mbedtls_x509_crt_init(&client->ca);
    mbedtls_x509_crt_init(&client->certificate);
    mbedtls_pk_init(&client->private_key);
    mbedtls_ssl_config_init(&client->tls_config);
    mbedtls_ssl_init(&client->tls);
    client->tls_initialized = 1;

    rc = mbedtls_x509_crt_parse_file(&client->ca, config->tls_ca_file);
    if (rc < 0) {
        set_tls_error(client, "load CA certificate", rc);
        return -1;
    }
    rc = mbedtls_x509_crt_parse_file(&client->certificate, config->tls_cert_file);
    if (rc < 0) {
        set_tls_error(client, "load client certificate", rc);
        return -1;
    }
    rc = mbedtls_pk_parse_keyfile(&client->private_key, config->tls_key_file, NULL);
    if (rc != 0) {
        set_tls_error(client, "load client private key", rc);
        return -1;
    }
    rc = mbedtls_ssl_config_defaults(&client->tls_config,
                                     MBEDTLS_SSL_IS_CLIENT,
                                     MBEDTLS_SSL_TRANSPORT_STREAM,
                                     MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc != 0) {
        set_tls_error(client, "configure TLS client", rc);
        return -1;
    }
    mbedtls_ssl_conf_authmode(&client->tls_config, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&client->tls_config, &client->ca, NULL);
    mbedtls_ssl_conf_min_tls_version(&client->tls_config, MBEDTLS_SSL_VERSION_TLS1_3);
    mbedtls_ssl_conf_max_tls_version(&client->tls_config, MBEDTLS_SSL_VERSION_TLS1_3);
    mbedtls_ssl_conf_tls13_key_exchange_modes(
        &client->tls_config,
        MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_EPHEMERAL);
    mbedtls_ssl_conf_session_tickets(&client->tls_config,
                                     MBEDTLS_SSL_SESSION_TICKETS_DISABLED);
    rc = mbedtls_ssl_conf_own_cert(&client->tls_config,
                                   &client->certificate,
                                   &client->private_key);
    if (rc != 0 || (rc = mbedtls_ssl_setup(&client->tls, &client->tls_config)) != 0 ||
        (rc = mbedtls_ssl_set_hostname(&client->tls, server_name)) != 0) {
        set_tls_error(client, "initialize TLS client", rc);
        return -1;
    }
    mbedtls_ssl_set_bio(&client->tls, client, tls_socket_send, tls_socket_recv, NULL);
    do {
        rc = mbedtls_ssl_handshake(&client->tls);
    } while (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE);
    if (rc != 0 || mbedtls_ssl_get_verify_result(&client->tls) != 0) {
        set_tls_error(client,
                      "TLS handshake failed",
                      rc != 0 ? rc : MBEDTLS_ERR_SSL_BAD_CERTIFICATE);
        return -1;
    }
    return 0;
}

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
#if defined(MCP_PLATFORM_WINDOWS)
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
#if defined(MCP_PLATFORM_WINDOWS)
    Sleep(ms);
#else
    struct timespec ts;

    ts.tv_sec = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)(ms % 1000u) * 1000000l;
    nanosleep(&ts, NULL);
#endif
}

#if defined(MCP_PLATFORM_WINDOWS)
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
            int rc = mbedtls_ssl_read(&client->tls, cursor, len);

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
            int rc = mbedtls_ssl_write(&client->tls, cursor, len);

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
        int rc = client->transport == MCP_PROXY_TRANSPORT_TCP
                     ? mbedtls_ssl_read(&client->tls, cursor, len)
                     : (int)read(client->fd, cursor, len);

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
        int rc = client->transport == MCP_PROXY_TRANSPORT_TCP
                     ? mbedtls_ssl_write(&client->tls, cursor, len)
                     : (int)write(client->fd, cursor, len);

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
#if defined(MCP_PLATFORM_WINDOWS)
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
#if defined(MCP_PLATFORM_WINDOWS)
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
        if (rc == 0)
            rc = start_tls(client, config);
    } else {
        set_error(client, "unsupported backend transport");
        rc = -1;
    }

#if defined(MCP_PLATFORM_WINDOWS)
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

#if defined(MCP_PLATFORM_WINDOWS)
    if (client->pipe != INVALID_HANDLE_VALUE)
        CloseHandle(client->pipe);
    if (client->socket_fd != INVALID_SOCKET)
        closesocket(client->socket_fd);
    WSACleanup();
#else
    if (client->fd >= 0)
        close(client->fd);
#endif
    if (client->tls_initialized) {
        mbedtls_ssl_free(&client->tls);
        mbedtls_ssl_config_free(&client->tls_config);
        mbedtls_pk_free(&client->private_key);
        mbedtls_x509_crt_free(&client->certificate);
        mbedtls_x509_crt_free(&client->ca);
    }
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
