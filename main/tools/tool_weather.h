#pragma once

#include "esp_err.h"
#include <stddef.h>

/**
 * Initialize weather tool.
 */
esp_err_t tool_weather_init(void);

/**
 * Execute a weather query.
 *
 * @param input_json   JSON string with:
 *                     - "city": city name (required)
 *                     - "start_date": start date in YYYY-MM-DD format (optional, for forecast)
 *                     - "end_date": end date in YYYY-MM-DD format (optional, for forecast)
 *                     If start_date and end_date are provided, returns daily forecast.
 *                     Otherwise, returns current weather.
 * @param output       Output buffer for formatted weather info
 * @param output_size  Size of output buffer
 * @return ESP_OK on success
 */
esp_err_t tool_weather_execute(const char *input_json, char *output, size_t output_size);