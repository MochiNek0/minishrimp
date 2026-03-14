#include "tool_web_fetch.h"
#include "shrimp_config.h"
#include "proxy/http_proxy.h"

#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "cJSON.h"

static const char *TAG = "web_fetch";

#define OUTPUT_BUF_SIZE    (24 * 1024)  /* Final text output buffer */
#define HTML_CHUNK_SIZE    (1024)       /* Temporary HTML chunk buffer */

/* ── URL parsing ───────────────────────────────────────────────── */

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

/* ── Stream processor for HTML to text conversion ─────────────── */

typedef struct {
    char *output;           /* Output text buffer */
    size_t out_pos;         /* Current position in output */
    size_t out_size;        /* Size of output buffer */
    char tag_buf[128];      /* Buffer for incomplete tags */
    size_t tag_len;         /* Length of incomplete tag */
    bool skip_script;       /* Inside <script> tag */
    bool skip_style;        /* Inside <style> tag */
    bool skip_nav;          /* Inside <nav> tag */
    bool skip_footer;       /* Inside <footer> tag */
    bool last_was_space;    /* Last char was space (prevent multiple spaces) */
    bool is_jina;           /* Using Jina Reader (markdown output) */
    int status;             /* HTTP status code */
    bool got_headers;       /* Headers received */
    char *header_end;       /* Pointer to header end in output (for proxy mode) */
} stream_processor_t;

static bool is_whitespace(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* Check if tag_buf matches a tag name (case insensitive) */
static bool tag_matches(const char *tag_buf, const char *name)
{
    size_t name_len = strlen(name);
    if (tag_buf[0] != '<') return false;

    const char *tag_start = tag_buf + 1;
    /* Skip / for closing tags */
    if (*tag_start == '/') tag_start++;

    size_t tag_len = strlen(tag_start);
    if (tag_len < name_len) return false;

    /* Check if it matches and is followed by space, >, or end */
    if (strncasecmp(tag_start, name, name_len) != 0) return false;

    char next = tag_start[name_len];
    return next == '\0' || next == ' ' || next == '>' || next == '/';
}

/* Append char to output if space available */
static void append_char(stream_processor_t *sp, char c)
{
    if (sp->out_pos < sp->out_size - 1) {
        sp->output[sp->out_pos++] = c;
    }
}

/* Process a single character in HTML stream */
static void process_html_char(stream_processor_t *sp, char c)
{
    /* If using Jina Reader, just pass through */
    if (sp->is_jina) {
        append_char(sp, c);
        return;
    }

    /* Handle tag buffer */
    if (sp->tag_len > 0 || c == '<') {
        /* Add to tag buffer */
        if (sp->tag_len < sizeof(sp->tag_buf) - 1) {
            sp->tag_buf[sp->tag_len++] = c;
            sp->tag_buf[sp->tag_len] = '\0';
        }

        /* Check if we're still in a tag (no '>' yet) */
        if (c != '>') {
            /* Check for complete skip tags */
            if (tag_matches(sp->tag_buf, "script")) {
                sp->skip_script = true;
            } else if (tag_matches(sp->tag_buf, "style")) {
                sp->skip_style = true;
            } else if (tag_matches(sp->tag_buf, "nav")) {
                sp->skip_nav = true;
            } else if (tag_matches(sp->tag_buf, "footer")) {
                sp->skip_footer = true;
            }
            /* Check for closing skip tags */
            else if (tag_matches(sp->tag_buf, "/script")) {
                sp->skip_script = false;
            } else if (tag_matches(sp->tag_buf, "/style")) {
                sp->skip_style = false;
            } else if (tag_matches(sp->tag_buf, "/nav")) {
                sp->skip_nav = false;
            } else if (tag_matches(sp->tag_buf, "/footer")) {
                sp->skip_footer = false;
            }
            return;
        }

        /* Tag complete - check for block-level tags to add newline */
        if (tag_matches(sp->tag_buf, "br") ||
            tag_matches(sp->tag_buf, "/p") ||
            tag_matches(sp->tag_buf, "/div") ||
            tag_matches(sp->tag_buf, "/h1") || tag_matches(sp->tag_buf, "/h2") ||
            tag_matches(sp->tag_buf, "/h3") || tag_matches(sp->tag_buf, "/h4") ||
            tag_matches(sp->tag_buf, "/li") || tag_matches(sp->tag_buf, "li") ||
            tag_matches(sp->tag_buf, "/tr")) {
            if (sp->out_pos > 0 && sp->output[sp->out_pos - 1] != '\n') {
                append_char(sp, '\n');
                sp->last_was_space = true;
            }
        }

        /* Reset tag buffer */
        sp->tag_len = 0;
        sp->tag_buf[0] = '\0';
        return;
    }

    /* Skip content inside script/style/nav/footer */
    if (sp->skip_script || sp->skip_style || sp->skip_nav || sp->skip_footer) {
        return;
    }

    /* Handle HTML entities */
    if (c == '&') {
        /* We need to buffer entities - simplified: just skip to ; */
        return;
    }

    /* Handle whitespace */
    if (is_whitespace(c)) {
        if (!sp->last_was_space) {
            append_char(sp, ' ');
            sp->last_was_space = true;
        }
    } else {
        append_char(sp, c);
        sp->last_was_space = false;
    }
}

/* ── HTTP event handler for streaming ─────────────────────────── */

static esp_err_t stream_http_event_handler(esp_http_client_event_t *evt)
{
    stream_processor_t *sp = (stream_processor_t *)evt->user_data;

    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            /* For proxy mode, we need to parse headers first */
            if (!sp->got_headers) {
                /* Buffer headers temporarily at start of output */
                size_t copy = sp->out_pos + evt->data_len < sp->out_size - 1 ?
                              evt->data_len : sp->out_size - 1 - sp->out_pos;
                if (copy > 0) {
                    memcpy(sp->output + sp->out_pos, evt->data, copy);
                    sp->out_pos += copy;
                    sp->output[sp->out_pos] = '\0';
                }

                /* Look for end of headers */
                char *hdr_end = strstr(sp->output, "\r\n\r\n");
                if (hdr_end) {
                    /* Get status code */
                    if (strncmp(sp->output, "HTTP/", 5) == 0) {
                        const char *status_str = strchr(sp->output, ' ');
                        if (status_str) sp->status = atoi(status_str + 1);
                    }

                    /* Move body to start of buffer */
                    size_t body_offset = (hdr_end + 4) - sp->output;
                    size_t body_len = sp->out_pos - body_offset;

                    /* Process the body portion */
                    sp->got_headers = true;
                    sp->out_pos = 0;

                    const char *body = sp->output + body_offset;
                    for (size_t i = 0; i < body_len; i++) {
                        process_html_char(sp, body[i]);
                    }
                }
            } else {
                /* Process chunk directly */
                const char *data = (const char *)evt->data;
                for (int i = 0; i < evt->data_len; i++) {
                    process_html_char(sp, data[i]);
                }
            }
            break;

        case HTTP_EVENT_ON_FINISH:
            /* Ensure null termination */
            sp->output[sp->out_pos] = '\0';
            break;

        default:
            break;
    }
    return ESP_OK;
}

/* ── Direct fetch with streaming ───────────────────────────────── */

static esp_err_t fetch_stream_direct(const char *url, stream_processor_t *sp)
{
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = stream_http_event_handler,
        .user_data = sp,
        .timeout_ms = 30000,
        .buffer_size = 2048,  /* Small buffer for streaming */
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_FAIL;

    esp_http_client_set_header(client, "Accept",
        sp->is_jina ? "text/plain" : "text/html,application/xhtml+xml,text/plain;q=0.9");
    esp_http_client_set_header(client, "Accept-Language", "en-US,en;q=0.5");

    esp_err_t err = esp_http_client_perform(client);
    sp->status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) return err;
    if (sp->status >= 400) {
        ESP_LOGW(TAG, "HTTP error %d", sp->status);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ── Proxy fetch with streaming ───────────────────────────────── */

static esp_err_t fetch_stream_via_proxy(const parsed_url_t *pu, const char *full_url,
                                        bool use_jina, stream_processor_t *sp)
{
    const char *host = use_jina ? "r.jina.ai" : pu->host;
    int port = use_jina ? 443 : pu->port;

    proxy_conn_t *conn = proxy_conn_open(host, port, 30000);
    if (!conn) return ESP_ERR_HTTP_CONNECT;

    char header[1024];
    int hlen;

    if (use_jina) {
        /* URL-encode the original URL */
        char encoded_url[512];
        const char *src = full_url;
        char *dst = encoded_url;
        while (*src && dst < encoded_url + sizeof(encoded_url) - 4) {
            unsigned char c = (unsigned char)*src;
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~' ||
                c == '/' || c == ':' || c == '?' || c == '=' || c == '&') {
                *dst++ = *src++;
            } else {
                static const char hex[] = "0123456789ABCDEF";
                *dst++ = '%';
                *dst++ = hex[c >> 4];
                *dst++ = hex[c & 0x0F];
                src++;
            }
        }
        *dst = '\0';

        hlen = snprintf(header, sizeof(header),
            "GET /%s HTTP/1.1\r\n"
            "Host: r.jina.ai\r\n"
            "Accept: text/plain\r\n"
            "Connection: close\r\n\r\n",
            encoded_url);
    } else {
        hlen = snprintf(header, sizeof(header),
            "GET %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Accept: text/html,application/xhtml+xml,text/plain;q=0.9\r\n"
            "Accept-Language: en-US,en;q=0.5\r\n"
            "Connection: close\r\n\r\n",
            pu->path, pu->host);
    }

    if (proxy_conn_write(conn, header, hlen) < 0) {
        proxy_conn_close(conn);
        return ESP_ERR_HTTP_WRITE_DATA;
    }

    /* Read and process in chunks */
    char tmp[1024];
    bool got_headers = false;

    while (1) {
        int n = proxy_conn_read(conn, tmp, sizeof(tmp), 30000);
        if (n <= 0) break;

        if (!got_headers) {
            /* Buffer headers temporarily */
            size_t copy = sp->out_pos + n < sp->out_size - 1 ? n : sp->out_size - 1 - sp->out_pos;
            if (copy > 0) {
                memcpy(sp->output + sp->out_pos, tmp, copy);
                sp->out_pos += copy;
                sp->output[sp->out_pos] = '\0';
            }

            char *hdr_end = strstr(sp->output, "\r\n\r\n");
            if (hdr_end) {
                if (strncmp(sp->output, "HTTP/", 5) == 0) {
                    const char *status_str = strchr(sp->output, ' ');
                    if (status_str) sp->status = atoi(status_str + 1);
                }

                size_t body_offset = (hdr_end + 4) - sp->output;
                size_t body_len = sp->out_pos - body_offset;

                got_headers = true;
                sp->out_pos = 0;

                const char *body = sp->output + body_offset;
                for (size_t i = 0; i < body_len; i++) {
                    process_html_char(sp, body[i]);
                }
            }
        } else {
            for (int i = 0; i < n; i++) {
                process_html_char(sp, tmp[i]);
            }
        }
    }

    proxy_conn_close(conn);
    sp->output[sp->out_pos] = '\0';

    if (sp->status >= 400) {
        ESP_LOGW(TAG, "HTTP error %d via proxy", sp->status);
        return ESP_FAIL;
    }

    return ESP_OK;
}

/* ── Check if response indicates blocking ─────────────────────── */

static bool is_blocked_response(const char *text)
{
    if (!text || strlen(text) < 50) return false;

    if (strcasestr(text, "cloudflare") && strcasestr(text, "challenge")) return true;
    if (strcasestr(text, "Access Denied")) return true;
    if (strcasestr(text, "captcha") && strcasestr(text, "recaptcha")) return true;

    return false;
}

/* ── Execute ──────────────────────────────────────────────────── */

esp_err_t tool_web_fetch_execute(const char *input_json, char *output, size_t output_size)
{
    /* Parse input to get URL */
    cJSON *input = cJSON_Parse(input_json);
    if (!input) {
        snprintf(output, output_size, "Error: Invalid input JSON");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *url_item = cJSON_GetObjectItem(input, "url");
    if (!url_item || !cJSON_IsString(url_item) || url_item->valuestring[0] == '\0') {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: Missing 'url' field");
        return ESP_ERR_INVALID_ARG;
    }

    const char *url = url_item->valuestring;
    ESP_LOGI(TAG, "Fetching: %s", url);

    /* Parse URL */
    parsed_url_t pu;
    if (!parse_url(url, &pu)) {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: Invalid URL format");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON_Delete(input);

    /* Initialize stream processor - output buffer is provided by caller */
    stream_processor_t sp = {0};
    sp.output = output;
    sp.out_size = output_size;
    sp.last_was_space = true;

    /* Try direct fetch first */
    esp_err_t err;
    bool use_proxy = http_proxy_is_enabled();

    if (use_proxy) {
        err = fetch_stream_via_proxy(&pu, url, false, &sp);
    } else {
        err = fetch_stream_direct(url, &sp);
    }

    /* Check if we need Jina Reader fallback */
    bool need_jina = (err != ESP_OK) || (sp.status >= 400) || is_blocked_response(output);

    if (need_jina) {
        ESP_LOGI(TAG, "Direct fetch failed or blocked, trying Jina Reader...");

        /* Reset processor for Jina */
        memset(&sp, 0, sizeof(sp));
        sp.output = output;
        sp.out_size = output_size;
        sp.is_jina = true;

        if (use_proxy) {
            err = fetch_stream_via_proxy(&pu, url, true, &sp);
        } else {
            char jina_url[600];
            snprintf(jina_url, sizeof(jina_url), "https://r.jina.ai/%s", url);
            err = fetch_stream_direct(jina_url, &sp);
        }

        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Fetched %d chars via Jina Reader", (int)sp.out_pos);
        }
    } else {
        ESP_LOGI(TAG, "Fetched %d chars directly", (int)sp.out_pos);
    }

    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: Failed to fetch URL. The page may require authentication or have anti-bot protection.");
        return err;
    }

    if (sp.out_pos == 0) {
        snprintf(output, output_size, "Error: Empty response from server");
        return ESP_FAIL;
    }

    /* Ensure null termination */
    output[sp.out_pos] = '\0';

    /* Trim trailing whitespace */
    while (sp.out_pos > 0 && is_whitespace(output[sp.out_pos - 1])) {
        output[--sp.out_pos] = '\0';
    }

    ESP_LOGI(TAG, "Extracted %d chars of text", (int)sp.out_pos);
    return ESP_OK;
}