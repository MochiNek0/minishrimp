#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

/**
 * Register config UI + API routes on an existing httpd server.
 */
esp_err_t web_config_register(httpd_handle_t server);
