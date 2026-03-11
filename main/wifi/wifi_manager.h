#pragma once

#include "esp_err.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

/**
 * Initialize WiFi subsystem (STA/AP).
 */
esp_err_t wifi_manager_init(void);

/**
 * Start WiFi connection. Non-blocking, fires events.
 */
esp_err_t wifi_manager_start(void);


/**
 * Start configuration access point (fallback when no STA credentials).
 */
esp_err_t wifi_manager_start_config_ap(void);

/**
 * Block until WiFi is connected or failed.
 * @param timeout_ms  Max time to wait (portMAX_DELAY for forever)
 * @return ESP_OK if connected, ESP_ERR_TIMEOUT otherwise
 */
esp_err_t wifi_manager_wait_connected(uint32_t timeout_ms);

/**
 * Check if WiFi is currently connected.
 */
bool wifi_manager_is_connected(void);

/**
 * Get the current IP address string (or "0.0.0.0" if not connected).
 */
const char *wifi_manager_get_ip(void);

/**
 * Save WiFi credentials to NVS.
 */
esp_err_t wifi_manager_set_credentials(const char *ssid, const char *password);

/**
 * Get the event group for WiFi state (WIFI_CONNECTED_BIT / WIFI_FAIL_BIT).
 */
EventGroupHandle_t wifi_manager_get_event_group(void);

/**
 * Scan and print nearby APs.
 */
void wifi_manager_scan_and_print(void);

/**
 * Scan nearby APs and return a JSON string (requires free() by caller).
 * The JSON string will contain an array of objects: 
 * [{"ssid_hex": "...", "rssi": -70, "auth": 3}, ...]
 */
esp_err_t wifi_manager_get_scan_results(char **out_json_str);

/**
 * Save WiFi list to NVS.
 * @param json_str Array of objects [{"ssid": "...", "password": "..."}, ...]
 */
esp_err_t wifi_manager_set_wifi_list(const char *json_str);
