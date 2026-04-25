#include "tool_api.h"
#include "shrimp_config.h"
#include "proxy/http_proxy.h"

#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdbool.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"

static const char *TAG = "api_call";

/* ── URL parsing (copied from tool_web_fetch.c) ────────────────── */

typedef struct {
    char scheme[8];
    char host[256];
    int port;
    char path[512];
} parsed_url_t;

static bool parse_url(const char *url, parsed_url_t *out)
{
    memset(out, 0, sizeof(*out));
    const char *p = url;

    if (strncasecmp(p, "https://", 8) == 0) {
        strcpy(out->scheme, "https");
        out->port = 443;
        p += 8;
    } else if (strncasecmp(p, "http://", 7) == 0) {
        strcpy(out->scheme, "http");
        out->port = 80;
        p += 7;
    } else {
        return false;
    }

    const char *slash = strchr(p, '/');
    const char *colon = strchr(p, ':');
    const char *host_end = slash ? slash : p + strlen(p);

    if (colon && colon < host_end) {
        size_t host_len = colon - p;
        if (host_len >= sizeof(out->host)) host_len = sizeof(out->host) - 1;
        strncpy(out->host, p, host_len);
        out->host[host_len] = '\0';
        out->port = atoi(colon + 1);
        p = slash ? slash : p + strlen(p);
    } else {
        size_t host_len = host_end - p;
        if (host_len >= sizeof(out->host)) host_len = sizeof(out->host) - 1;
        strncpy(out->host, p, host_len);
        out->host[host_len] = '\0';
        p = host_end;
    }

    if (*p) {
        strncpy(out->path, p, sizeof(out->path) - 1);
    } else {
        strcpy(out->path, "/");
    }

    return out->host[0] != '\0';
}

/* ── HTTP event handler for response accumulation ───────────────── */

typedef struct {
    char *output;
    size_t out_pos;
    size_t out_size;
    int status;
    bool truncated;
} accum_t;

static esp_err_t accum_http_event_handler(esp_http_client_event_t *evt)
{
    accum_t *acc = (accum_t *)evt->user_data;

    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (evt->data_len > 0) {
                size_t copy = (acc->out_pos + evt->data_len < acc->out_size - 1)
                              ? evt->data_len
                              : acc->out_size - 1 - acc->out_pos;
                if (copy > 0) {
                    memcpy(acc->output + acc->out_pos, evt->data, copy);
                    acc->out_pos += copy;
                }
                if (copy < (size_t)evt->data_len) {
                    acc->truncated = true;
                }
            }
            break;
        case HTTP_EVENT_ON_FINISH:
            acc->output[acc->out_pos] = '\0';
            break;
        default:
            break;
    }
    return ESP_OK;
}

/* ── Direct request via esp_http_client ─────────────────────────── */

static bool method_is_write(const char *method)
{
    return strcasecmp(method, "POST") == 0 || strcasecmp(method, "PUT") == 0;
}

static esp_err_t request_direct(const char *url, const char *method, const char *token,
                                const char *body, accum_t *acc)
{
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = accum_http_event_handler,
        .user_data = acc,
        .timeout_ms = 30000,
        .buffer_size = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_FAIL;

    /* Set method */
    if (strcmp(method, "POST") == 0) {
        esp_http_client_set_method(client, HTTP_METHOD_POST);
    } else if (strcmp(method, "PUT") == 0) {
        esp_http_client_set_method(client, HTTP_METHOD_PUT);
    } else if (strcmp(method, "DELETE") == 0) {
        esp_http_client_set_method(client, HTTP_METHOD_DELETE);
    } else {
        esp_http_client_set_method(client, HTTP_METHOD_GET);
    }

    /* Set auth header */
    if (token && token[0]) {
        char auth_header[512];
        snprintf(auth_header, sizeof(auth_header), "Bearer %s", token);
        esp_http_client_set_header(client, "Authorization", auth_header);
    }

    /* Set body for POST/PUT */
    esp_http_client_set_header(client, "Connection", "close");
    if (body && body[0] && method_is_write(method)) {
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, body, strlen(body));
    }

    esp_err_t err = esp_http_client_perform(client);
    acc->status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) return err;
    return ESP_OK;
}

/* ── Proxy request via HTTP CONNECT tunnel ─────────────────────── */

static esp_err_t request_via_proxy(const char *url, const char *method, const char *token,
                                   const char *body, accum_t *acc)
{
    parsed_url_t pu;
    if (!parse_url(url, &pu)) return ESP_FAIL;

    /* The proxy tunnel implementation is HTTPS-only (TLS over CONNECT/SOCKS). */
    if (strcasecmp(pu.scheme, "https") != 0) {
        ESP_LOGE(TAG, "Proxy mode only supports https:// endpoints (got %s://)", pu.scheme);
        return ESP_ERR_INVALID_ARG;
    }

    const char *host = pu.host;
    int port = pu.port;

    proxy_conn_t *conn = proxy_conn_open(host, port, 30000);
    if (!conn) return ESP_ERR_HTTP_CONNECT;

    /* Build request line and headers */
    char header[1024];
    int hlen = snprintf(header, sizeof(header),
        "%s %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Connection: close\r\n",
        method, pu.path[0] ? pu.path : "/", pu.host, pu.port);

    if (token && token[0]) {
        hlen += snprintf(header + hlen, sizeof(header) - hlen,
            "Authorization: Bearer %s\r\n", token);
    }

    if (body && body[0] && method_is_write(method)) {
        hlen += snprintf(header + hlen, sizeof(header) - hlen,
            "Content-Type: application/json\r\n");
    }

    if (body && body[0] && method_is_write(method)) {
        hlen += snprintf(header + hlen, sizeof(header) - hlen,
            "Content-Length: %zu\r\n", strlen(body));
    }

    /* End headers */
    hlen += snprintf(header + hlen, sizeof(header) - hlen, "\r\n");

    if (proxy_conn_write(conn, header, hlen) < 0) {
        proxy_conn_close(conn);
        return ESP_ERR_HTTP_WRITE_DATA;
    }

    if (body && body[0] && method_is_write(method)) {
        if (proxy_conn_write(conn, body, (int)strlen(body)) < 0) {
            proxy_conn_close(conn);
            return ESP_ERR_HTTP_WRITE_DATA;
        }
    }

    /* Read response */
    char tmp[1024];
    bool got_headers = false;
    char header_buf[1024];
    size_t header_len = 0;
    memset(header_buf, 0, sizeof(header_buf));

    while (1) {
        int n = proxy_conn_read(conn, tmp, sizeof(tmp), 30000);
        if (n <= 0) break;

        if (!got_headers) {
            size_t hdr_copy = (header_len + (size_t)n < sizeof(header_buf) - 1)
                              ? (size_t)n
                              : (sizeof(header_buf) - 1 - header_len);
            if (hdr_copy > 0) {
                memcpy(header_buf + header_len, tmp, hdr_copy);
                header_len += hdr_copy;
                header_buf[header_len] = '\0';
            }

            char *hdr_end = strstr(header_buf, "\r\n\r\n");
            if (!hdr_end) {
                if (hdr_copy < (size_t)n) {
                    ESP_LOGE(TAG, "Response headers too large");
                    proxy_conn_close(conn);
                    return ESP_ERR_NO_MEM;
                }
                continue;
            }

            if (strncmp(header_buf, "HTTP/", 5) == 0) {
                const char *status_str = strchr(header_buf, ' ');
                if (status_str) acc->status = atoi(status_str + 1);
            }

            got_headers = true;

            size_t header_total = (size_t)((hdr_end + 4) - header_buf);
            size_t body_in_header_buf = header_len > header_total ? (header_len - header_total) : 0;
            if (body_in_header_buf > 0) {
                size_t copy = (acc->out_pos + body_in_header_buf < acc->out_size - 1)
                              ? body_in_header_buf
                              : (acc->out_size - 1 - acc->out_pos);
                if (copy > 0) {
                    memcpy(acc->output + acc->out_pos, header_buf + header_total, copy);
                    acc->out_pos += copy;
                }
                if (copy < body_in_header_buf) {
                    acc->truncated = true;
                    break;
                }
            }

            if ((size_t)n > hdr_copy) {
                size_t remaining = (size_t)n - hdr_copy;
                size_t copy = (acc->out_pos + remaining < acc->out_size - 1)
                              ? remaining
                              : (acc->out_size - 1 - acc->out_pos);
                if (copy > 0) {
                    memcpy(acc->output + acc->out_pos, tmp + hdr_copy, copy);
                    acc->out_pos += copy;
                }
                if (copy < remaining) {
                    acc->truncated = true;
                    break;
                }
            }
        } else {
            size_t copy = acc->out_pos + n < acc->out_size - 1
                          ? n : acc->out_size - 1 - acc->out_pos;
            if (copy > 0) {
                memcpy(acc->output + acc->out_pos, tmp, copy);
                acc->out_pos += copy;
            }
            if (copy < (size_t)n) {
                acc->truncated = true;
                break;
            }
        }
    }

    proxy_conn_close(conn);
    acc->output[acc->out_pos] = '\0';

    return ESP_OK;
}

/* ── Execute ────────────────────────────────────────────────────── */

esp_err_t tool_api_call_execute(const char *input_json, char *output, size_t output_size)
{
    if (!output || output_size < 2) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *input = cJSON_Parse(input_json);
    if (!input) {
        snprintf(output, output_size, "Error: Invalid input JSON");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *endpoint_item = cJSON_GetObjectItem(input, "endpoint");
    if (!endpoint_item || !cJSON_IsString(endpoint_item) || endpoint_item->valuestring[0] == '\0') {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: Missing 'endpoint' field");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *method_item = cJSON_GetObjectItem(input, "method");
    const char *method = (method_item && cJSON_IsString(method_item)) ? method_item->valuestring : "GET";

    cJSON *token_item = cJSON_GetObjectItem(input, "token");
    const char *token = (token_item && cJSON_IsString(token_item)) ? token_item->valuestring : NULL;

    cJSON *body_item = cJSON_GetObjectItem(input, "body");
    const char *body = NULL;
    char *body_alloc = NULL;
    if (body_item) {
        if (cJSON_IsString(body_item)) {
            body = body_item->valuestring;
        } else if (cJSON_IsObject(body_item) || cJSON_IsArray(body_item)) {
            body_alloc = cJSON_PrintUnformatted(body_item);
            body = body_alloc;
        }
    }

    const char *endpoint = endpoint_item->valuestring;
    ESP_LOGI(TAG, "API call: %s %s", method, endpoint);

    /* Validate method */
    if (strcasecmp(method, "GET") != 0 && strcasecmp(method, "POST") != 0 &&
        strcasecmp(method, "PUT") != 0 && strcasecmp(method, "DELETE") != 0) {
        cJSON_Delete(input);
        free(body_alloc);
        snprintf(output, output_size, "Error: Invalid method '%s'. Use GET, POST, PUT, or DELETE", method);
        return ESP_ERR_INVALID_ARG;
    }

    /* Initialize accumulator */
    accum_t acc = { .output = output, .out_pos = 0, .out_size = output_size, .status = 0, .truncated = false };

    /* Choose direct or proxy */
    bool use_proxy = http_proxy_is_enabled();
    esp_err_t err;

    if (use_proxy) {
        err = request_via_proxy(endpoint, method, token, body, &acc);
    } else {
        err = request_direct(endpoint, method, token, body, &acc);
    }

    cJSON_Delete(input);
    free(body_alloc);

    if (err != ESP_OK) {
        if (acc.out_pos == 0) {
            snprintf(output, output_size, "Error: Request failed (0x%x)", err);
        } else {
            int n = snprintf(output + acc.out_pos, output_size - acc.out_pos, "\n[ERR 0x%x]", err);
            if (n > 0) acc.out_pos += (size_t)n;
        }
        return err;
    }

    if (acc.status >= 400) {
        /* Preserve any server-provided error body, but annotate with status. */
        if (acc.out_pos == 0) {
            snprintf(output, output_size, "Error: HTTP %d", acc.status);
        } else {
            int n = snprintf(output + acc.out_pos, output_size - acc.out_pos, "\n[HTTP %d]", acc.status);
            if (n > 0) acc.out_pos += (size_t)n;
        }
        return ESP_FAIL;
    }

    /* Trim trailing whitespace */
    while (acc.out_pos > 0 &&
           (output[acc.out_pos - 1] == ' ' || output[acc.out_pos - 1] == '\t' ||
            output[acc.out_pos - 1] == '\n' || output[acc.out_pos - 1] == '\r')) {
        output[--acc.out_pos] = '\0';
    }

    if (acc.truncated) {
        const char *suffix = "\n[truncated]";
        size_t suffix_len = strlen(suffix);
        if (acc.out_pos + suffix_len < output_size) {
            memcpy(output + acc.out_pos, suffix, suffix_len + 1);
            acc.out_pos += suffix_len;
        }
    }

    ESP_LOGI(TAG, "API call returned %d bytes, status %d", (int)acc.out_pos, acc.status);
    return ESP_OK;
}
