#pragma once

#include "esp_err.h"
#include <stddef.h>

/**
 * @brief Initialize the calendar tool.
 */
esp_err_t tool_calendar_init(void);

/**
 * @brief Execute the calendar tool.
 * 
 * @param input_json JSON with "year" and optionally "month".
 * @param output Buffer to store the calendar grid.
 * @param output_size Size of the output buffer.
 * @return esp_err_t 
 */
esp_err_t tool_calendar_execute(const char *input_json, char *output, size_t output_size);
