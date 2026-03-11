#include "wifi_manager.h"
#include "shrimp_config.h"

#include <string.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"

static const char *TAG = "wifi";

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_count = 0;
static char s_ip_str[16] = "0.0.0.0";
static bool s_connected = false;
static char s_current_ssid[33] = {0};
static char s_current_pass[65] = {0};

/* Hex utilities for Chinese SSIDs */
static void bytes_to_hex(const uint8_t *in, size_t in_len, char *out) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < in_len; i++) {
        out[i * 2] = hex[(in[i] >> 4) & 0x0F];
        out[i * 2 + 1] = hex[in[i] & 0x0F];
    }
    out[in_len * 2] = '\0';
}

static const char *wifi_reason_to_str(wifi_err_reason_t reason)
{
    switch (reason) {
    case WIFI_REASON_AUTH_EXPIRE: return "AUTH_EXPIRE";
    case WIFI_REASON_AUTH_FAIL: return "AUTH_FAIL";
    case WIFI_REASON_ASSOC_EXPIRE: return "ASSOC_EXPIRE";
    case WIFI_REASON_ASSOC_FAIL: return "ASSOC_FAIL";
    case WIFI_REASON_HANDSHAKE_TIMEOUT: return "HANDSHAKE_TIMEOUT";
    case WIFI_REASON_NO_AP_FOUND: return "NO_AP_FOUND";
    case WIFI_REASON_BEACON_TIMEOUT: return "BEACON_TIMEOUT";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: return "4WAY_HANDSHAKE_TIMEOUT";
    case WIFI_REASON_MIC_FAILURE: return "MIC_FAILURE";
    case WIFI_REASON_CONNECTION_FAIL: return "CONNECTION_FAIL";
    default: return "UNKNOWN";
    }
}

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
        if (disc) {
            ESP_LOGW(TAG, "Disconnected (reason=%d:%s)", disc->reason, wifi_reason_to_str(disc->reason));
        }
        if (s_retry_count < SHRIMP_WIFI_MAX_RETRY) {
            /* Exponential backoff: 1s, 2s, 4s, 8s, ... capped at 30s */
            uint32_t delay_ms = SHRIMP_WIFI_RETRY_BASE_MS << s_retry_count;
            if (delay_ms > SHRIMP_WIFI_RETRY_MAX_MS) {
                delay_ms = SHRIMP_WIFI_RETRY_MAX_MS;
            }
            ESP_LOGW(TAG, "Disconnected, retry %d/%d in %" PRIu32 "ms",
                     s_retry_count + 1, SHRIMP_WIFI_MAX_RETRY, delay_ms);
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
            esp_wifi_connect();
            s_retry_count++;
        } else {
            ESP_LOGE(TAG, "Failed to connect after %d retries", SHRIMP_WIFI_MAX_RETRY);
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            /* Consider triggering fallback to AP or next WiFi here if needed */
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Connected! IP: %s", s_ip_str);
        s_retry_count = 0;
        s_connected = true;

        /* Dynamically save the successfully connected WiFi to the NVS list */
        if (s_current_ssid[0] != '\0') {
            nvs_handle_t nvs;
            if (nvs_open(SHRIMP_NVS_WIFI, NVS_READWRITE, &nvs) == ESP_OK) {
                size_t len = 0;
                char *json_str = NULL;
                cJSON *list = NULL;
                
                if (nvs_get_str(nvs, SHRIMP_NVS_KEY_WIFI_LIST, NULL, &len) == ESP_OK && len > 0) {
                    json_str = malloc(len);
                    if (json_str && nvs_get_str(nvs, SHRIMP_NVS_KEY_WIFI_LIST, json_str, &len) == ESP_OK) {
                        list = cJSON_Parse(json_str);
                    }
                    if (json_str) free(json_str);
                }
                
                if (!list || !cJSON_IsArray(list)) {
                    if (list) cJSON_Delete(list);
                    list = cJSON_CreateArray();
                }

                /* Check if it already exists, if so, move to front */
                int found_idx = -1;
                int array_sz = cJSON_GetArraySize(list);
                for (int i = 0; i < array_sz; i++) {
                    cJSON *item = cJSON_GetArrayItem(list, i);
                    cJSON *issid = cJSON_GetObjectItem(item, "ssid");
                    if (issid && cJSON_IsString(issid) && strcmp(issid->valuestring, s_current_ssid) == 0) {
                        found_idx = i;
                        break;
                    }
                }

                if (found_idx >= 0) {
                    cJSON *item = cJSON_DetachItemFromArray(list, found_idx);
                    cJSON_InsertItemInArray(list, 0, item); /* Move to front */
                } else {
                    cJSON *new_item = cJSON_CreateObject();
                    cJSON_AddStringToObject(new_item, "ssid", s_current_ssid);
                    cJSON_AddStringToObject(new_item, "password", s_current_pass);
                    cJSON_InsertItemInArray(list, 0, new_item);
                }

                /* Limit to 5 entries */
                while(cJSON_GetArraySize(list) > 5) {
                    cJSON_DeleteItemFromArray(list, cJSON_GetArraySize(list) - 1);
                }

                char *new_json_str = cJSON_PrintUnformatted(list);
                if (new_json_str) {
                    nvs_set_str(nvs, SHRIMP_NVS_KEY_WIFI_LIST, new_json_str);
                    nvs_commit(nvs);
                    free(new_json_str);
                    ESP_LOGI(TAG, "Saved %s to WiFi list", s_current_ssid);
                }
                cJSON_Delete(list);
                nvs_close(nvs);
            }
        }

        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

esp_err_t wifi_manager_init(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    ESP_LOGI(TAG, "WiFi manager initialized");
    return ESP_OK;
}

esp_err_t wifi_manager_start(void)
{
    wifi_config_t wifi_cfg = {0};
    bool found_and_connected = false;

    /* Scan surrounding APs to match with NVS list */
    ESP_LOGI(TAG, "Starting boot-time WiFi scan...");
    wifi_scan_config_t scan_cfg = {0};
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    
    uint16_t ap_count = 0;
    wifi_ap_record_t *ap_list = NULL;
    
    if (err == ESP_OK) {
        esp_wifi_scan_get_ap_num(&ap_count);
        if (ap_count > 0) {
            ap_list = calloc(ap_count, sizeof(wifi_ap_record_t));
            if (ap_list) {
                esp_wifi_scan_get_ap_records(&ap_count, ap_list);
            }
        }
    } else {
        ESP_LOGE(TAG, "Boot-time WiFi scan failed: %s", esp_err_to_name(err));
    }

    /* 1. Try to connect using the NVS WiFi list */
    nvs_handle_t nvs;
    if (nvs_open(SHRIMP_NVS_WIFI, NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = 0;
        char *json_str = NULL;
        if (nvs_get_str(nvs, SHRIMP_NVS_KEY_WIFI_LIST, NULL, &len) == ESP_OK && len > 0) {
            json_str = malloc(len);
            if (json_str && nvs_get_str(nvs, SHRIMP_NVS_KEY_WIFI_LIST, json_str, &len) == ESP_OK) {
                cJSON *list = cJSON_Parse(json_str);
                if (list && cJSON_IsArray(list)) {
                    int array_sz = cJSON_GetArraySize(list);
                    for (int i = 0; i < array_sz && !found_and_connected; i++) {
                        cJSON *item = cJSON_GetArrayItem(list, i);
                        cJSON *ssid = cJSON_GetObjectItem(item, "ssid");
                        cJSON *pass = cJSON_GetObjectItem(item, "password");
                        
                        if (ssid && cJSON_IsString(ssid) && pass && cJSON_IsString(pass)) {
                            /* Verify if this SSID is nearby */
                            bool is_nearby = false;
                            /* If scan failed, we blindly try the first one anyway */
                            if (!ap_list || ap_count == 0) {
                                is_nearby = true;
                            } else {
                                for (uint16_t j = 0; j < ap_count; j++) {
                                    if (strcmp((const char *)ap_list[j].ssid, ssid->valuestring) == 0) {
                                        is_nearby = true;
                                        break;
                                    }
                                }
                            }
                            
                            if (is_nearby) {
                                strncpy((char *)wifi_cfg.sta.ssid, ssid->valuestring, sizeof(wifi_cfg.sta.ssid)-1);
                                strncpy((char *)wifi_cfg.sta.password, pass->valuestring, sizeof(wifi_cfg.sta.password)-1);
                                found_and_connected = true;
                                ESP_LOGI(TAG, "Selected nearby network from list: %s", ssid->valuestring);
                            }
                        }
                    }
                }
                if (list) cJSON_Delete(list);
            }
            if (json_str) free(json_str);
        }
        
        /* Backward compatibility with single legacy connection */
        if (!found_and_connected) {
            len = sizeof(wifi_cfg.sta.ssid);
            if (nvs_get_str(nvs, SHRIMP_NVS_KEY_SSID, (char *)wifi_cfg.sta.ssid, &len) == ESP_OK && wifi_cfg.sta.ssid[0] != '\0') {
                len = sizeof(wifi_cfg.sta.password);
                nvs_get_str(nvs, SHRIMP_NVS_KEY_PASS, (char *)wifi_cfg.sta.password, &len);
                found_and_connected = true;
                ESP_LOGI(TAG, "Selected legacy NVS network: %s", wifi_cfg.sta.ssid);
            }
        }
        
        nvs_close(nvs);
    }

    if (ap_list) free(ap_list);

    /* 2. Fall back to build-time secrets */
    if (!found_and_connected) {
        if (SHRIMP_SECRET_WIFI_SSID[0] != '\0') {
            strncpy((char *)wifi_cfg.sta.ssid, SHRIMP_SECRET_WIFI_SSID, sizeof(wifi_cfg.sta.ssid) - 1);
            strncpy((char *)wifi_cfg.sta.password, SHRIMP_SECRET_WIFI_PASS, sizeof(wifi_cfg.sta.password) - 1);
            found_and_connected = true;
            ESP_LOGI(TAG, "Selected build-time network: %s", wifi_cfg.sta.ssid);
        }
    }

    if (!found_and_connected) {
        ESP_LOGW(TAG, "No WiFi credentials available. Use AP to configure.");
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Connecting to SSID: %s", wifi_cfg.sta.ssid);
    
    strncpy(s_current_ssid, (const char *)wifi_cfg.sta.ssid, sizeof(s_current_ssid)-1);
    strncpy(s_current_pass, (const char *)wifi_cfg.sta.password, sizeof(s_current_pass)-1);

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    return ESP_OK;
}

esp_err_t wifi_manager_wait_connected(uint32_t timeout_ms)
{
    TickType_t ticks = (timeout_ms == UINT32_MAX) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE, ticks);

    if (bits & WIFI_CONNECTED_BIT) {
        return ESP_OK;
    }
    return ESP_ERR_TIMEOUT;
}

bool wifi_manager_is_connected(void)
{
    return s_connected;
}

const char *wifi_manager_get_ip(void)
{
    return s_ip_str;
}

esp_err_t wifi_manager_set_credentials(const char *ssid, const char *password)
{
    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open(SHRIMP_NVS_WIFI, NVS_READWRITE, &nvs));
    ESP_ERROR_CHECK(nvs_set_str(nvs, SHRIMP_NVS_KEY_SSID, ssid));
    ESP_ERROR_CHECK(nvs_set_str(nvs, SHRIMP_NVS_KEY_PASS, password));
    ESP_ERROR_CHECK(nvs_commit(nvs));
    nvs_close(nvs);
    ESP_LOGI(TAG, "WiFi single credential overridden for SSID: %s", ssid);
    
    strncpy(s_current_ssid, ssid, sizeof(s_current_ssid)-1);
    s_current_ssid[sizeof(s_current_ssid)-1] = '\0';
    strncpy(s_current_pass, password, sizeof(s_current_pass)-1);
    s_current_pass[sizeof(s_current_pass)-1] = '\0';
    
    return ESP_OK;
}

esp_err_t wifi_manager_set_wifi_list(const char *json_str) {
    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open(SHRIMP_NVS_WIFI, NVS_READWRITE, &nvs));
    ESP_ERROR_CHECK(nvs_set_str(nvs, SHRIMP_NVS_KEY_WIFI_LIST, json_str));
    ESP_ERROR_CHECK(nvs_commit(nvs));
    nvs_close(nvs);
    ESP_LOGI(TAG, "WiFi list overridden");
    return ESP_OK;
}



esp_err_t wifi_manager_start_config_ap(void)
{
    wifi_config_t ap_cfg = {0};
    const char *ssid = SHRIMP_CONFIG_AP_SSID;
    const char *pass = SHRIMP_CONFIG_AP_PASS;

    strncpy((char *)ap_cfg.ap.ssid, ssid, sizeof(ap_cfg.ap.ssid) - 1);
    strncpy((char *)ap_cfg.ap.password, pass, sizeof(ap_cfg.ap.password) - 1);
    ap_cfg.ap.ssid_len = strlen(ssid);
    ap_cfg.ap.channel = 1;
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.authmode = (pass[0] == '\0') ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Config AP started: SSID=%s (IP: 192.168.4.1)", ssid);
    return ESP_OK;
}
EventGroupHandle_t wifi_manager_get_event_group(void)
{
    return s_wifi_event_group;
}

void wifi_manager_scan_and_print(void)
{
    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
    };

    ESP_LOGI(TAG, "Scanning nearby APs...");

    /* Pause auto-connect to allow scan */
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(200));

    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true /* block */);
    if (err == ESP_ERR_WIFI_STATE) {
        /* Try a quick stop/start cycle and scan again */
        esp_wifi_stop();
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_wifi_start();
        vTaskDelay(pdMS_TO_TICKS(200));
        err = esp_wifi_scan_start(&scan_cfg, true /* block */);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Scan failed: %s", esp_err_to_name(err));
        esp_wifi_connect();
        return;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count == 0) {
        ESP_LOGW(TAG, "No APs found");
        esp_wifi_connect();
        return;
    }

    wifi_ap_record_t *ap_list = calloc(ap_count, sizeof(wifi_ap_record_t));
    if (!ap_list) {
        ESP_LOGE(TAG, "Out of memory for AP list");
        return;
    }

    uint16_t ap_max = ap_count;
    if (esp_wifi_scan_get_ap_records(&ap_max, ap_list) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get AP records");
        free(ap_list);
        esp_wifi_connect();
        return;
    }

    ESP_LOGI(TAG, "Found %u APs:", ap_max);
    for (uint16_t i = 0; i < ap_max; i++) {
        const wifi_ap_record_t *ap = &ap_list[i];
        ESP_LOGI(TAG, "  [%u] SSID=%s RSSI=%d CH=%d Auth=%d",
                 i + 1, (const char *)ap->ssid, ap->rssi, ap->primary, ap->authmode);
    }

    free(ap_list);
    esp_wifi_connect();
}

esp_err_t wifi_manager_get_scan_results(char **out_json_str) {
    if (!out_json_str) return ESP_ERR_INVALID_ARG;
    *out_json_str = NULL;
    
    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
    };

    ESP_LOGI(TAG, "Scanning nearby APs for API...");

    wifi_mode_t old_mode;
    esp_wifi_get_mode(&old_mode);
    if (old_mode == WIFI_MODE_AP) {
        esp_wifi_set_mode(WIFI_MODE_APSTA);
        /* Wait for STA interface to be ready after mode switch */
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    bool reconnect = s_connected;
    if (s_connected) {
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        /* Retry after a delay instead of stop/start which kills the AP */
        ESP_LOGW(TAG, "Scan attempt 1 failed (%s), retrying...", esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(500));
        err = esp_wifi_scan_start(&scan_cfg, true);
    }
    
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Scan failed: %s", esp_err_to_name(err));
        if (old_mode == WIFI_MODE_AP) {
            esp_wifi_set_mode(WIFI_MODE_AP);
        }
        if (reconnect) esp_wifi_connect();
        return err;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    
    cJSON *root = cJSON_CreateArray();
    if (!root) {
        if (reconnect) esp_wifi_connect();
        return ESP_ERR_NO_MEM;
    }

    if (ap_count > 0) {
        wifi_ap_record_t *ap_list = calloc(ap_count, sizeof(wifi_ap_record_t));
        if (ap_list) {
            if (esp_wifi_scan_get_ap_records(&ap_count, ap_list) == ESP_OK) {
                for (uint16_t i = 0; i < ap_count; i++) {
                    cJSON *ap_obj = cJSON_CreateObject();
                    char hex_buf[65] = {0};
                    bytes_to_hex(ap_list[i].ssid, strlen((const char *)ap_list[i].ssid), hex_buf);
                    
                    cJSON_AddStringToObject(ap_obj, "ssid_hex", hex_buf);
                    cJSON_AddNumberToObject(ap_obj, "rssi", ap_list[i].rssi);
                    cJSON_AddNumberToObject(ap_obj, "auth", ap_list[i].authmode);
                    cJSON_AddItemToArray(root, ap_obj);
                }
            }
            free(ap_list);
        }
    }
    
    char *json_txt = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    
    if (old_mode == WIFI_MODE_AP) {
        esp_wifi_set_mode(WIFI_MODE_AP);
    }
    
    if (reconnect) {
        esp_wifi_connect();
    }
    
    if (json_txt) {
        *out_json_str = json_txt;
        return ESP_OK;
    }
    
    return ESP_ERR_NO_MEM;
}
