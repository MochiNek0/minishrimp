#include "tool_weather.h"
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

static const char *TAG = "weather";

#define WEATHER_BUF_SIZE    4096
#define GEO_API_URL         "https://geocoding-api.open-meteo.com/v1/search"
#define WEATHER_API_URL     "https://api.open-meteo.com/v1/forecast"

/* ── HTTP event handler for simple JSON response ─────────────────── */

typedef struct {
    char *buffer;
    size_t buffer_size;
    size_t buffer_pos;
    int status;
} http_buffer_t;

static esp_err_t http_buffer_event_handler(esp_http_client_event_t *evt)
{
    http_buffer_t *hb = (http_buffer_t *)evt->user_data;

    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (hb->buffer && hb->buffer_pos < hb->buffer_size - 1) {
                size_t copy = (hb->buffer_size - 1 - hb->buffer_pos) > (size_t)evt->data_len ?
                              (size_t)evt->data_len : hb->buffer_size - 1 - hb->buffer_pos;
                memcpy(hb->buffer + hb->buffer_pos, evt->data, copy);
                hb->buffer_pos += copy;
                hb->buffer[hb->buffer_pos] = '\0';
            }
            break;
        case HTTP_EVENT_ON_FINISH:
            break;
        default:
            break;
    }
    return ESP_OK;
}

/* ── Simple HTTP GET request ─────────────────────────────────────── */

static esp_err_t http_get(const char *url, char *response, size_t response_size, int *status_code)
{
    bool use_proxy = http_proxy_is_enabled();

    if (use_proxy) {
        /* Parse URL for proxy request */
        char host[256] = {0};
        char path[512] = {0};
        int port = 443;

        /* Simple URL parsing - extract host and path */
        const char *p = url;
        if (strncasecmp(p, "https://", 8) == 0) {
            p += 8;
            port = 443;
        }

        const char *slash = strchr(p, '/');
        const char *colon = strchr(p, ':');

        if (colon && (!slash || colon < slash)) {
            size_t host_len = colon - p;
            if (host_len < sizeof(host) - 1) {
                strncpy(host, p, host_len);
                host[host_len] = '\0';
                port = atoi(colon + 1);
                p = slash ? slash : p + strlen(p);
            }
        } else if (slash) {
            size_t host_len = slash - p;
            if (host_len < sizeof(host) - 1) {
                strncpy(host, p, host_len);
                host[host_len] = '\0';
                p = slash;
            }
        } else {
            strncpy(host, p, sizeof(host) - 1);
            p = "";
        }

        if (*p) {
            strncpy(path, p, sizeof(path) - 1);
        } else {
            strcpy(path, "/");
        }

        proxy_conn_t *conn = proxy_conn_open(host, port, 15000);
        if (!conn) {
            ESP_LOGE(TAG, "Failed to connect via proxy");
            return ESP_ERR_HTTP_CONNECT;
        }

        char header[1024];
        int hlen = snprintf(header, sizeof(header),
            "GET %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Accept: application/json\r\n"
            "Connection: close\r\n\r\n",
            path, host);

        if (proxy_conn_write(conn, header, hlen) < 0) {
            proxy_conn_close(conn);
            return ESP_ERR_HTTP_WRITE_DATA;
        }

        /* Read response */
        char tmp[1024];
        http_buffer_t hb = { .buffer = response, .buffer_size = response_size, .buffer_pos = 0 };
        bool got_headers = false;

        while (1) {
            int n = proxy_conn_read(conn, tmp, sizeof(tmp), 15000);
            if (n <= 0) break;

            if (!got_headers) {
                /* Buffer headers */
                size_t copy = hb.buffer_pos + n < hb.buffer_size - 1 ? n : hb.buffer_size - 1 - hb.buffer_pos;
                if (copy > 0) {
                    memcpy(hb.buffer + hb.buffer_pos, tmp, copy);
                    hb.buffer_pos += copy;
                    hb.buffer[hb.buffer_pos] = '\0';
                }

                char *hdr_end = strstr(hb.buffer, "\r\n\r\n");
                if (hdr_end) {
                    /* Get status code */
                    if (strncmp(hb.buffer, "HTTP/", 5) == 0) {
                        const char *status_str = strchr(hb.buffer, ' ');
                        if (status_str) *status_code = atoi(status_str + 1);
                    }

                    /* Move body to start */
                    size_t body_offset = (hdr_end + 4) - hb.buffer;
                    size_t body_len = hb.buffer_pos - body_offset;

                    got_headers = true;
                    hb.buffer_pos = 0;

                    if (body_len > 0) {
                        const char *body = hb.buffer + body_offset;
                        size_t copy = body_len < hb.buffer_size - 1 ? body_len : hb.buffer_size - 1;
                        memcpy(hb.buffer, body, copy);
                        hb.buffer_pos = copy;
                        hb.buffer[hb.buffer_pos] = '\0';
                    }
                }
            } else {
                /* Append body */
                size_t copy = hb.buffer_pos + n < hb.buffer_size - 1 ? n : hb.buffer_size - 1 - hb.buffer_pos;
                if (copy > 0) {
                    memcpy(hb.buffer + hb.buffer_pos, tmp, copy);
                    hb.buffer_pos += copy;
                    hb.buffer[hb.buffer_pos] = '\0';
                }
            }
        }

        proxy_conn_close(conn);
        response[hb.buffer_pos] = '\0';

    } else {
        /* Direct HTTP request */
        esp_http_client_config_t config = {
            .url = url,
            .event_handler = http_buffer_event_handler,
            .user_data = response,
            .timeout_ms = 15000,
            .buffer_size = WEATHER_BUF_SIZE,
            .crt_bundle_attach = esp_crt_bundle_attach,
        };

        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (!client) return ESP_FAIL;

        esp_http_client_set_header(client, "Accept", "application/json");

        esp_err_t err = esp_http_client_perform(client);
        *status_code = esp_http_client_get_status_code(client);
        esp_http_client_cleanup(client);

        if (err != ESP_OK) return err;
    }

    return ESP_OK;
}

/* ── Weather code to description ─────────────────────────────────── */

static const char *weather_code_desc(int code)
{
    switch (code) {
        case 0: return "Clear sky";
        case 1: return "Mainly clear";
        case 2: return "Partly cloudy";
        case 3: return "Overcast";
        case 45: return "Fog";
        case 48: return "Depositing rime fog";
        case 51: return "Light drizzle";
        case 53: return "Moderate drizzle";
        case 55: return "Dense drizzle";
        case 56: return "Light freezing drizzle";
        case 57: return "Dense freezing drizzle";
        case 61: return "Slight rain";
        case 63: return "Moderate rain";
        case 65: return "Heavy rain";
        case 66: return "Light freezing rain";
        case 67: return "Heavy freezing rain";
        case 71: return "Slight snow";
        case 73: return "Moderate snow";
        case 75: return "Heavy snow";
        case 77: return "Snow grains";
        case 80: return "Slight rain showers";
        case 81: return "Moderate rain showers";
        case 82: return "Violent rain showers";
        case 85: return "Slight snow showers";
        case 86: return "Heavy snow showers";
        case 95: return "Thunderstorm";
        case 96: return "Thunderstorm with slight hail";
        case 99: return "Thunderstorm with heavy hail";
        default: return "Unknown";
    }
}

/* ── Get location coordinates ────────────────────────────────────── */

static bool get_coordinates(const char *city, double *lat, double *lon, char *country, size_t country_size)
{
    char url[512];
    snprintf(url, sizeof(url), "%s?name=%s&count=1&language=en&format=json",
             GEO_API_URL, city);

    char response[WEATHER_BUF_SIZE] = {0};
    int status = 0;

    esp_err_t err = http_get(url, response, sizeof(response), &status);
    if (err != ESP_OK || status >= 400) {
        ESP_LOGE(TAG, "Geocoding API failed: err=%d, status=%d", err, status);
        return false;
    }

    cJSON *root = cJSON_Parse(response);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse geocoding response");
        return false;
    }

    cJSON *results = cJSON_GetObjectItem(root, "results");
    if (!results || !cJSON_GetArraySize(results)) {
        ESP_LOGE(TAG, "No results found for city: %s", city);
        cJSON_Delete(root);
        return false;
    }

    cJSON *first = cJSON_GetArrayItem(results, 0);
    cJSON *lat_item = cJSON_GetObjectItem(first, "latitude");
    cJSON *lon_item = cJSON_GetObjectItem(first, "longitude");
    cJSON *country_item = cJSON_GetObjectItem(first, "country");

    if (!lat_item || !lon_item) {
        ESP_LOGE(TAG, "Missing coordinates in response");
        cJSON_Delete(root);
        return false;
    }

    *lat = lat_item->valuedouble;
    *lon = lon_item->valuedouble;

    if (country_item && country_item->valuestring) {
        strncpy(country, country_item->valuestring, country_size - 1);
    }

    cJSON_Delete(root);
    return true;
}

/* ── Get weather data (current only) ───────────────────────────────── */

static bool get_weather_current(double lat, double lon, char *output, size_t output_size)
{
    char url[1024];
    snprintf(url, sizeof(url),
        "%s?latitude=%.4f&longitude=%.4f"
        "&current=temperature_2m,relative_humidity_2m,apparent_temperature,"
        "is_day,precipitation,rain,showers,snowfall,cloud_cover,"
        "pressure_msl,surface_pressure,wind_speed_10m,wind_direction_10m,wind_gusts_10m,"
        "weather_code"
        "&timezone=auto",
        WEATHER_API_URL, lat, lon);

    char response[WEATHER_BUF_SIZE] = {0};
    int status = 0;

    esp_err_t err = http_get(url, response, sizeof(response), &status);
    if (err != ESP_OK || status >= 400) {
        ESP_LOGE(TAG, "Weather API failed: err=%d, status=%d", err, status);
        return false;
    }

    cJSON *root = cJSON_Parse(response);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse weather response");
        return false;
    }

    cJSON *current = cJSON_GetObjectItem(root, "current");
    if (!current) {
        ESP_LOGE(TAG, "No current weather data");
        cJSON_Delete(root);
        return false;
    }

    /* Extract values */
    double temp = 0, feels_like = 0, humidity = 0, pressure = 0, surf_pressure = 0;
    double wind_speed = 0, wind_dir = 0, wind_gusts = 0;
    int weather_code = 0, is_day = 1;
    double precip = 0, rain = 0, snowfall = 0, cloud_cover = 0;

    cJSON *item;

    item = cJSON_GetObjectItem(current, "temperature_2m");
    if (item) temp = item->valuedouble;

    item = cJSON_GetObjectItem(current, "apparent_temperature");
    if (item) feels_like = item->valuedouble;

    item = cJSON_GetObjectItem(current, "relative_humidity_2m");
    if (item) humidity = item->valuedouble;

    item = cJSON_GetObjectItem(current, "pressure_msl");
    if (item) pressure = item->valuedouble;

    item = cJSON_GetObjectItem(current, "surface_pressure");
    if (item) surf_pressure = item->valuedouble;

    item = cJSON_GetObjectItem(current, "wind_speed_10m");
    if (item) wind_speed = item->valuedouble;

    item = cJSON_GetObjectItem(current, "wind_direction_10m");
    if (item) wind_dir = item->valuedouble;

    item = cJSON_GetObjectItem(current, "wind_gusts_10m");
    if (item) wind_gusts = item->valuedouble;

    item = cJSON_GetObjectItem(current, "weather_code");
    if (item) weather_code = item->valueint;

    item = cJSON_GetObjectItem(current, "is_day");
    if (item) is_day = item->valueint;

    item = cJSON_GetObjectItem(current, "precipitation");
    if (item) precip = item->valuedouble;

    item = cJSON_GetObjectItem(current, "rain");
    if (item) rain = item->valuedouble;

    item = cJSON_GetObjectItem(current, "snowfall");
    if (item) snowfall = item->valuedouble;

    item = cJSON_GetObjectItem(current, "cloud_cover");
    if (item) cloud_cover = item->valuedouble;

    /* Format output */
    const char *weather_desc = weather_code_desc(weather_code);
    const char *day_night = is_day ? "Day" : "Night";

    /* Wind direction to compass */
    const char *compass[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    int dir_idx = (int)((wind_dir + 22.5) / 45.0) % 8;
    const char *wind_dir_str = compass[dir_idx];

    int len = snprintf(output, output_size,
        "Weather: %s (%s)\n"
        "Temperature: %.1f°C (Feels like: %.1f°C)\n"
        "Humidity: %.0f%%\n"
        "Cloud Cover: %.0f%%\n"
        "Wind: %.1f km/h from %s, gusts up to %.1f km/h\n"
        "Pressure: %.0f hPa (sea level), %.0f hPa (surface)\n"
        "Precipitation: %.1f mm (Rain: %.1f mm, Snow: %.1f cm)",
        weather_desc, day_night,
        temp, feels_like,
        humidity,
        cloud_cover,
        wind_speed, wind_dir_str, wind_gusts,
        pressure, surf_pressure,
        precip, rain, snowfall);

    output[len] = '\0';
    cJSON_Delete(root);
    return true;
}

/* ── Get weather data (daily forecast) ─────────────────────────────── */

static bool get_weather_forecast(double lat, double lon, const char *start_date, const char *end_date,
                                  char *output, size_t output_size)
{
    char url[1024];
    snprintf(url, sizeof(url),
        "%s?latitude=%.4f&longitude=%.4f"
        "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_sum,"
        "rain_sum,showers_sum,snowfall_sum,precipitation_probability_max,wind_speed_10m_max,"
        "wind_gusts_10m_max,wind_direction_10m_dominant"
        "&timezone=auto"
        "&start_date=%s&end_date=%s",
        WEATHER_API_URL, lat, lon, start_date, end_date);

    char response[WEATHER_BUF_SIZE] = {0};
    int status = 0;

    esp_err_t err = http_get(url, response, sizeof(response), &status);
    if (err != ESP_OK || status >= 400) {
        ESP_LOGE(TAG, "Weather API failed: err=%d, status=%d", err, status);
        return false;
    }

    cJSON *root = cJSON_Parse(response);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse weather response");
        return false;
    }

    cJSON *daily = cJSON_GetObjectItem(root, "daily");
    if (!daily) {
        ESP_LOGE(TAG, "No daily weather data");
        cJSON_Delete(root);
        return false;
    }

    cJSON *time_array = cJSON_GetObjectItem(daily, "time");
    cJSON *weather_code_array = cJSON_GetObjectItem(daily, "weather_code");
    cJSON *temp_max_array = cJSON_GetObjectItem(daily, "temperature_2m_max");
    cJSON *temp_min_array = cJSON_GetObjectItem(daily, "temperature_2m_min");
    cJSON *precip_sum_array = cJSON_GetObjectItem(daily, "precipitation_sum");
    cJSON *precip_prob_array = cJSON_GetObjectItem(daily, "precipitation_probability_max");
    cJSON *wind_max_array = cJSON_GetObjectItem(daily, "wind_speed_10m_max");
    cJSON *wind_gusts_array = cJSON_GetObjectItem(daily, "wind_gusts_10m_max");
    cJSON *wind_dir_array = cJSON_GetObjectItem(daily, "wind_direction_10m_dominant");

    if (!time_array || !cJSON_IsArray(time_array)) {
        ESP_LOGE(TAG, "Missing time array in daily data");
        cJSON_Delete(root);
        return false;
    }

    int days = cJSON_GetArraySize(time_array);
    if (days == 0) {
        ESP_LOGE(TAG, "No daily data available");
        cJSON_Delete(root);
        return false;
    }

    /* Wind direction to compass */
    const char *compass[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};

    char *buf = output;
    size_t remaining = output_size;
    int offset = 0;

    /* Format header */
    offset = snprintf(buf, remaining, "Forecast:\n");
    buf += offset;
    remaining -= offset;

    for (int i = 0; i < days; i++) {
        cJSON *date_item = cJSON_GetArrayItem(time_array, i);
        if (!date_item || !date_item->valuestring) continue;

        const char *date = date_item->valuestring;

        int weather_code = 0;
        double temp_max = 0, temp_min = 0, precip_sum = 0, precip_prob = 0;
        double wind_max = 0, wind_gusts = 0, wind_dir = 0;

        if (weather_code_array) {
            cJSON *item = cJSON_GetArrayItem(weather_code_array, i);
            if (item) weather_code = item->valueint;
        }
        if (temp_max_array) {
            cJSON *item = cJSON_GetArrayItem(temp_max_array, i);
            if (item) temp_max = item->valuedouble;
        }
        if (temp_min_array) {
            cJSON *item = cJSON_GetArrayItem(temp_min_array, i);
            if (item) temp_min = item->valuedouble;
        }
        if (precip_sum_array) {
            cJSON *item = cJSON_GetArrayItem(precip_sum_array, i);
            if (item) precip_sum = item->valuedouble;
        }
        if (precip_prob_array) {
            cJSON *item = cJSON_GetArrayItem(precip_prob_array, i);
            if (item) precip_prob = item->valuedouble;
        }
        if (wind_max_array) {
            cJSON *item = cJSON_GetArrayItem(wind_max_array, i);
            if (item) wind_max = item->valuedouble;
        }
        if (wind_gusts_array) {
            cJSON *item = cJSON_GetArrayItem(wind_gusts_array, i);
            if (item) wind_gusts = item->valuedouble;
        }
        if (wind_dir_array) {
            cJSON *item = cJSON_GetArrayItem(wind_dir_array, i);
            if (item) wind_dir = item->valuedouble;
        }

        const char *weather_desc = weather_code_desc(weather_code);
        const char *wind_dir_str = compass[(int)((wind_dir + 22.5) / 45.0) % 8];

        /* Format date nicely (YYYY-MM-DD -> MM-DD) */
        const char *date_short = strlen(date) >= 10 ? date + 5 : date;

        offset = snprintf(buf, remaining, "%s: %s, %02.0f~%02.0f°C, %s, Rain: %.0f%% (%.1fmm), Wind: %.0f km/h %s (gusts: %.0f)\n",
                         date_short, weather_desc, temp_min, temp_max,
                         precip_prob > 0 ? "Rain likely" : "No rain",
                         precip_prob, precip_sum,
                         wind_max, wind_dir_str, wind_gusts);

        if (offset > remaining) break;
        buf += offset;
        remaining -= offset;
    }

    cJSON_Delete(root);
    return true;
}

/* ── Execute ─────────────────────────────────────────────────────── */

esp_err_t tool_weather_init(void)
{
    return ESP_OK;
}

esp_err_t tool_weather_execute(const char *input_json, char *output, size_t output_size)
{
    /* Parse input to get city name */
    cJSON *input = cJSON_Parse(input_json);
    if (!input) {
        snprintf(output, output_size, "Error: Invalid input JSON");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *city_item = cJSON_GetObjectItem(input, "city");
    if (!city_item || !cJSON_IsString(city_item) || city_item->valuestring[0] == '\0') {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: Missing 'city' field");
        return ESP_ERR_INVALID_ARG;
    }

    const char *city = city_item->valuestring;

    /* Parse optional date parameters */
    cJSON *start_date_item = cJSON_GetObjectItem(input, "start_date");
    cJSON *end_date_item = cJSON_GetObjectItem(input, "end_date");

    const char *start_date = NULL;
    const char *end_date = NULL;

    if (start_date_item && cJSON_IsString(start_date_item) && start_date_item->valuestring[0] != '\0') {
        start_date = start_date_item->valuestring;
    }
    if (end_date_item && cJSON_IsString(end_date_item) && end_date_item->valuestring[0] != '\0') {
        end_date = end_date_item->valuestring;
    }

    ESP_LOGI(TAG, "Getting weather for city: %s, start_date: %s, end_date: %s",
             city, start_date ? start_date : "none", end_date ? end_date : "none");

    /* Get coordinates */
    double lat, lon;
    char country[64] = {0};

    if (!get_coordinates(city, &lat, &lon, country, sizeof(country))) {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: Could not find city '%s'. Please check the city name.", city);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Found coordinates: %.4f, %.4f (%s)", lat, lon, country);

    char weather_data[WEATHER_BUF_SIZE];
    bool success;

    /* Check if we have date parameters for forecast */
    if (start_date && end_date) {
        success = get_weather_forecast(lat, lon, start_date, end_date, weather_data, sizeof(weather_data));
    } else {
        /* Default to current weather */
        success = get_weather_current(lat, lon, weather_data, sizeof(weather_data));
    }

    if (!success) {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: Failed to fetch weather data for '%s'", city);
        return ESP_FAIL;
    }

    /* Build final output with location info */
    if (country[0]) {
        snprintf(output, output_size, "%s, %s\n\n%s",
                 city, country, weather_data);
    } else {
        snprintf(output, output_size, "%s\n\n%s",
                 city, weather_data);
    }
    output[output_size - 1] = '\0';

    cJSON_Delete(input);
    return ESP_OK;
}