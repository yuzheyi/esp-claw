/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief MCP Server profile configuration.
 */
typedef struct {
    char name[32];           /*!< Service alias */
    char url[512];           /*!< Server base URL */
    char token[512];         /*!< Bearer token (may be empty) */
    char endpoint[64];       /*!< Endpoint path (default "mcp") */
    char description[128];   /*!< Human-readable description */
    bool enabled;            /*!< Whether this server is enabled */
} mcp_server_profile_t;

/**
 * @brief Global MCP server configuration.
 */
typedef struct {
    mcp_server_profile_t *profiles;
    size_t count;
    size_t capacity;
} mcp_server_config_t;

#define MCP_SERVER_CONFIG_PATH "/fatfs/mcp_servers.json"

esp_err_t mcp_server_config_load(const char *config_path, mcp_server_config_t *config);
esp_err_t mcp_server_config_save(const char *config_path, const mcp_server_config_t *config);
void mcp_server_config_free(mcp_server_config_t *config);
const mcp_server_profile_t *mcp_server_config_find(const mcp_server_config_t *config, const char *name);
esp_err_t mcp_server_config_add(mcp_server_config_t *config, const mcp_server_profile_t *profile);
esp_err_t mcp_server_config_remove(mcp_server_config_t *config, const char *name);

#ifdef __cplusplus
}
#endif
