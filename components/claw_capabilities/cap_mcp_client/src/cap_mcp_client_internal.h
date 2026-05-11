/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "cJSON.h"
#include "claw_cap.h"
#include "cap_mcp_client_config.h"
#include "esp_err.h"

#define CAP_MCP_DEFAULT_ENDPOINT      "mcp"
#define CAP_MCP_MDNS_SERVICE_TYPE     "_mcp"
#define CAP_MCP_MDNS_SERVICE_PROTO    "_tcp"
#define CAP_MCP_DISCOVER_TIMEOUT_MS   3000

/* Existing core API (used by both old and new execute paths) */
esp_err_t cap_mcp_list_remote_tools(const char *input_json, cJSON **result_out);
esp_err_t cap_mcp_call_remote_tool(const char *input_json, cJSON **result_out);
esp_err_t cap_mcp_discover_services(const char *input_json, cJSON **result_out);

/* Existing text extraction helper */
void cap_mcp_extract_content_text(const cJSON *content, char *output, size_t output_size);
void cap_mcp_sanitize_utf8(char *buf);

/* New v2 execute functions (server-profile based) */
esp_err_t cap_mcp_call_execute_v2(const char *input_json,
                                   const claw_cap_call_context_t *ctx,
                                   char *output,
                                   size_t output_size);

esp_err_t cap_mcp_list_execute_v2(const char *input_json,
                                   const claw_cap_call_context_t *ctx,
                                   char *output,
                                   size_t output_size);

/* Schema rebuild (call after config changes) */
void cap_mcp_rebuild_schemas(void);

/* Config accessors */
const mcp_server_config_t *cap_mcp_get_config(void);
mcp_server_config_t *cap_mcp_get_config_mutable(void);
