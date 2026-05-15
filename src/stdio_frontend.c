#include "mcp_proxy/platform.h"
#include "mcp_proxy/stdio_frontend.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(MCP_PLATFORM_WINDOWS)
#include <fcntl.h>
#include <io.h>
#endif

static void write_json_error(const char *message)
{
    fprintf(stdout,
            "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32000,\"message\":\"%s\"}}\n",
            message ? message : "backend error");
    fflush(stdout);
}

static const char *skip_ws(const char *cursor)
{
    while (*cursor && isspace((unsigned char)*cursor))
        cursor++;
    return cursor;
}

static const char *scan_json_string(const char *cursor)
{
    if (*cursor != '"')
        return NULL;

    cursor++;
    while (*cursor) {
        if (*cursor == '\\') {
            cursor++;
            if (*cursor == '\0')
                return NULL;
            cursor++;
            continue;
        }
        if (*cursor == '"')
            return cursor + 1;
        cursor++;
    }

    return NULL;
}

static const char *skip_json_value(const char *cursor);

static const char *skip_json_object(const char *cursor)
{
    cursor++;
    for (;;) {
        cursor = skip_ws(cursor);
        if (*cursor == '}')
            return cursor + 1;
        if (*cursor != '"')
            return NULL;

        cursor = scan_json_string(cursor);
        if (!cursor)
            return NULL;

        cursor = skip_ws(cursor);
        if (*cursor != ':')
            return NULL;

        cursor = skip_json_value(cursor + 1);
        if (!cursor)
            return NULL;

        cursor = skip_ws(cursor);
        if (*cursor == ',') {
            cursor++;
            continue;
        }
        if (*cursor == '}')
            return cursor + 1;
        return NULL;
    }
}

static const char *skip_json_array(const char *cursor)
{
    cursor++;
    for (;;) {
        cursor = skip_ws(cursor);
        if (*cursor == ']')
            return cursor + 1;

        cursor = skip_json_value(cursor);
        if (!cursor)
            return NULL;

        cursor = skip_ws(cursor);
        if (*cursor == ',') {
            cursor++;
            continue;
        }
        if (*cursor == ']')
            return cursor + 1;
        return NULL;
    }
}

static const char *skip_json_literal(const char *cursor)
{
    if (*cursor == '\0')
        return NULL;

    while (*cursor && !isspace((unsigned char)*cursor) &&
           *cursor != ',' && *cursor != '}' && *cursor != ']')
        cursor++;

    return cursor;
}

static const char *skip_json_value(const char *cursor)
{
    cursor = skip_ws(cursor);
    if (*cursor == '{')
        return skip_json_object(cursor);
    if (*cursor == '[')
        return skip_json_array(cursor);
    if (*cursor == '"')
        return scan_json_string(cursor);
    if (*cursor == '-' || (*cursor >= '0' && *cursor <= '9') ||
        *cursor == 't' || *cursor == 'f' || *cursor == 'n')
        return skip_json_literal(cursor);
    return NULL;
}

/*
 * The backend only replies when the incoming JSON-RPC object carries a
 * top-level "id". Nested "id" fields inside params must not turn a
 * notification into a request, otherwise the adapter deadlocks waiting for a
 * response that will never arrive.
 */
static bool line_expects_response(const char *line)
{
    const char *cursor = skip_ws(line);

    if (*cursor != '{')
        return true;

    cursor++;
    for (;;) {
        const char *key_start;
        const char *after_key;
        size_t key_len;

        cursor = skip_ws(cursor);
        if (*cursor == '}')
            return false;
        if (*cursor != '"')
            return true;

        key_start = cursor + 1;
        after_key = scan_json_string(cursor);
        if (!after_key)
            return true;

        key_len = (size_t)(after_key - key_start - 1);
        cursor = skip_ws(after_key);
        if (*cursor != ':')
            return true;
        cursor++;

        if (key_len == 2 && key_start[0] == 'i' && key_start[1] == 'd')
            return true;

        cursor = skip_json_value(cursor);
        if (!cursor)
            return true;

        cursor = skip_ws(cursor);
        if (*cursor == ',') {
            cursor++;
            continue;
        }
        if (*cursor == '}')
            return false;
        return true;
    }
}

static void emit_backend_response(const char *data, size_t len)
{
    fwrite(data, 1, len, stdout);
    if (len == 0 || data[len - 1] != '\n')
        fputc('\n', stdout);
    fflush(stdout);
}

int mcp_proxy_stdio_run(const struct mcp_backend_config *config)
{
    struct mcp_backend_client *client = NULL;
    char *line = NULL;
    size_t cap = 0;
    int exit_code = 0;

#if defined(MCP_PLATFORM_WINDOWS)
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    if (mcp_backend_client_open(&client, config) != 0) {
        write_json_error(mcp_backend_client_last_error(client));
        mcp_backend_client_close(client);
        return 1;
    }

    while (true) {
        int ch;
        size_t len = 0;

        while ((ch = fgetc(stdin)) != EOF) {
            char *next;

            if (len + 2 > cap) {
                size_t next_cap = cap == 0 ? 4096 : cap * 2;

                next = realloc(line, next_cap);
                if (!next) {
                    exit_code = 1;
                    goto done;
                }
                line = next;
                cap = next_cap;
            }

            if (ch == '\n')
                break;
            line[len++] = (char)ch;
        }

        if (ch == EOF && len == 0)
            break;

        if (len > 0 && line[len - 1] == '\r')
            len--;
        if (len == 0)
            continue;
        line[len] = '\0';

        if (mcp_backend_client_send(client, line, len) != 0) {
            write_json_error(mcp_backend_client_last_error(client));
            exit_code = 1;
            break;
        }

        if (line_expects_response(line)) {
            char *response = NULL;
            size_t response_len = 0;

            if (mcp_backend_client_recv(client, &response, &response_len) != 0) {
                write_json_error(mcp_backend_client_last_error(client));
                exit_code = 1;
                break;
            }
            emit_backend_response(response, response_len);
            free(response);
        }

        if (ch == EOF)
            break;
    }

done:
    free(line);
    mcp_backend_client_close(client);
    return exit_code;
}
