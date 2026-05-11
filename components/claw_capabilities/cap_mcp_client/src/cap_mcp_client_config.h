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
 *
 * Each profile stores connection details for one remote MCP server.
 * The transport mode (HTTP vs SSE) is auto-detected from endpoint/URL
 * and does not need to be stored.
 */
typedef struct {
    char name[32];           /*!< Service alias, e.g. "weather" */
    char url[512];           /*!< Server base URL */
    char token[512];         /*!< Bearer token for authentication (may be empty) */
    char endpoint[64];       /*!< Endpoint path, default "mcp"; "/sse" prefix triggers SSE */
    char description[128];   /*!< Human-readable description (shown to LLM) */
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

/**
 * @brief Default path for MCP server configuration file.
 */
#define MCP_SERVER_CONFIG_PATH "/fatfs/mcp_servers.json"

/**
 * @brief Load MCP server configuration from a JSON file.
 *
 * Expected format:
 * @code
 * {
 *   "servers": {
 *     "weather": {
 *       "url": "http://cloud.yujj.top:3000",
 *       "token": "sk-xxx",
 *       "endpoint": "mcp",
 *       "description": "Weather query service"
 *     }
 *   }
 * }
 * @endcode
 *
 * @param[in]  config_path  Path to the JSON config file
 * @param[out] config       Configuration to populate
 * @return ESP_OK on success
 */
esp_err_t mcp_server_config_load(const char *config_path, mcp_server_config_t *config);

/**
 * @brief Save MCP server configuration to a JSON file.
 *
 * @param[in] config_path  Path to the JSON config file
 * @param[in] config       Configuration to save
 * @return ESP_OK on success
 */
esp_err_t mcp_server_config_save(const char *config_path, const mcp_server_config_t *config);

/**
 * @brief Free resources allocated by mcp_server_config_load().
 *
 * @param[in] config  Configuration to free
 */
void mcp_server_config_free(mcp_server_config_t *config);

/**
 * @brief Find a server profile by name.
 *
 * @param[in] config     Loaded configuration
 * @param[in] name       Server name to find
 * @return Pointer to the profile, or NULL if not found
 */
const mcp_server_profile_t *mcp_server_config_find(const mcp_server_config_t *config,
                                                    const char *name);

/**
 * @brief Add a new server profile to the configuration.
 *
 * @param[in,out] config   Configuration to modify
 * @param[in]     profile  Profile to add (will be copied)
 * @return ESP_OK on success, ESP_ERR_NO_MEM if at capacity
 */
esp_err_t mcp_server_config_add(mcp_server_config_t *config,
                                 const mcp_server_profile_t *profile);

/**
 * @brief Remove a server profile by name.
 *
 * @param[in,out] config  Configuration to modify
 * @param[in]     name    Server name to remove
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if not found
 */
esp_err_t mcp_server_config_remove(mcp_server_config_t *config, const char *name);

/**
 * @brief Validate a configuration (check required fields, URL format, etc.).
 *
 * @param[in] config  Configuration to validate
 * @return ESP_OK if valid, ESP_ERR_INVALID_ARG if invalid
 */
esp_err_t mcp_server_config_validate(const mcp_server_config_t *config);

/**
 * @brief List all server names as a comma-separated string.
 *
 * Example output: "weather, ha, github"
 *
 * @param[in]  config     Configuration to query
 * @param[out] names_csv  Buffer to receive the CSV string
 * @param[in]  csv_size   Size of the buffer
 * @return ESP_OK on success
 */
esp_err_t mcp_server_config_list_names(const mcp_server_config_t *config,
                                        char *names_csv, size_t csv_size);

/**
 * @brief List all server names as a JSON enum array string.
 *
 * Example output: "\"weather\",\"ha\",\"github\""
 *
 * @param[in]  config      Configuration to query
 * @param[out] names_json  Buffer to receive the JSON enum string
 * @param[in]  json_size   Size of the buffer
 * @return ESP_OK on success
 */
esp_err_t mcp_server_config_list_names_json(const mcp_server_config_t *config,
                                             char *names_json, size_t json_size);

#ifdef __cplusplus
}
#endif
