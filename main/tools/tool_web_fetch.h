#pragma once

#include "esp_err.h"
#include <stddef.h>

/**
 * Execute a web fetch.
 *
 * @param input_json   JSON string with "url" field
 * @param output       Output buffer for extracted web content
 * @param output_size  Size of output buffer
 * @return ESP_OK on success
 */
esp_err_t tool_web_fetch_execute(const char *input_json, char *output, size_t output_size);