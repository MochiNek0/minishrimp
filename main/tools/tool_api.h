#pragma once

#include "esp_err.h"
#include <stddef.h>

/**
 * Execute a generic API call.
 *
 * @param input_json   JSON string with fields: endpoint (required), method (optional),
 *                     token (optional, Bearer token), body (optional, for POST/PUT)
 * @param output       Output buffer for response body
 * @param output_size  Size of output buffer
 * @return ESP_OK on success
 */
esp_err_t tool_api_call_execute(const char *input_json, char *output, size_t output_size);
