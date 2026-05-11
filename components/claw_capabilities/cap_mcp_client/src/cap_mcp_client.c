/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_mcp_client.h"

#include <string.h>
#include <stdio.h>

#include "cJSON.h"
#include "claw_cap.h"
#include "esp_log.h"
#include "mdns.h"

#include "cap_mcp_client_config.h"
#include "cap_mcp_client_internal.h"

static const char *TAG = "mcp_client";

/* ─── Global MCP server configuration ─────────────────────────── */
static mcp_server_config_t s_mcp_config = {0};

/* ─── Dynamic schema buffers ───────────────────────────────────── */
static char s_mcp_call_schema[1024];
static char s_mcp_list_schema[1024];
static char s_mcp_call_desc[1024];
static char s_mcp_list_desc[1024];

/**
 * @brief Truncate string at a UTF-8 character boundary.
 * If buf[max_len-1] falls in the middle of a multi-byte sequence,
 * back up to the start of that character to avoid invalid code points.
 */
static void cap_mcp_truncate_utf8_safe(char *buf, size_t max_len)
{
    if (!buf || max_len == 0) return;
    size_t len = strlen(buf);
    if (len < max_len) return;

    /* Start from the truncation point and back up to a valid leading byte */
    size_t pos = max_len - 1;
    while (pos > 0 && (buf[pos] & 0xC0) == 0x80) {
        pos--;  /* Skip continuation bytes (10xxxxxx) */
    }
    /* pos now points at the leading byte of the last complete character */
    /* But if we backed up too far, just truncate cleanly */
    if (pos < max_len - 4 && pos > 0) {
        buf[pos] = '\0';
    } else {
        buf[max_len - 1] = '\0';
    }
}

/**
 * @brief Sanitize a string in-place: replace invalid UTF-8 bytes with spaces.
 * Scans for bytes that don't form valid multi-byte sequences and replaces them.
 */
void cap_mcp_sanitize_utf8(char *buf)
{
    if (!buf) return;
    size_t src = 0, dst = 0;
    while (buf[src]) {
        unsigned char c = (unsigned char)buf[src];
        if (c < 0x80) {
            /* Single byte (0x00-0x7F): valid ASCII */
            buf[dst++] = buf[src++];
        } else if (c >= 0xC2 && c <= 0xDF) {
            /* 2-byte sequence: check continuation byte */
            if (buf[src+1] && ((unsigned char)buf[src+1] & 0xC0) == 0x80) {
                buf[dst++] = buf[src++];
                buf[dst++] = buf[src++];
            } else {
                buf[dst++] = ' '; src++; /* Replace invalid lead */
            }
        } else if (c >= 0xE0 && c <= 0xEF) {
            /* 3-byte sequence: check 2 continuation bytes */
            if (buf[src+1] && buf[src+2] &&
                ((unsigned char)buf[src+1] & 0xC0) == 0x80 &&
                ((unsigned char)buf[src+2] & 0xC0) == 0x80) {
                buf[dst++] = buf[src++];
                buf[dst++] = buf[src++];
                buf[dst++] = buf[src++];
            } else {
                buf[dst++] = ' '; src++; /* Replace invalid lead */
            }
        } else if (c >= 0xF0 && c <= 0xF4) {
            /* 4-byte sequence: check 3 continuation bytes */
            if (buf[src+1] && buf[src+2] && buf[src+3] &&
                ((unsigned char)buf[src+1] & 0xC0) == 0x80 &&
                ((unsigned char)buf[src+2] & 0xC0) == 0x80 &&
                ((unsigned char)buf[src+3] & 0xC0) == 0x80) {
                buf[dst++] = buf[src++];
                buf[dst++] = buf[src++];
                buf[dst++] = buf[src++];
                buf[dst++] = buf[src++];
            } else {
                buf[dst++] = ' '; src++; /* Replace invalid lead */
            }
        } else {
            /* Invalid byte (0x80-0xBF continuation without lead, or 0xC0-0xC1 overlong) */
            buf[dst++] = ' '; src++;
        }
    }
    buf[dst] = '\0';
}

static esp_err_t cap_mcp_client_group_init(void)
{
    esp_err_t err = mdns_init();

    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    mdns_hostname_set("esp-claw");
    mdns_instance_name_set("esp-claw");
    return ESP_OK;
}

void cap_mcp_extract_content_text(const cJSON *content,
                                  char *output,
                                  size_t output_size)
{
    const cJSON *item = NULL;
    size_t offset = 0;

    if (!cJSON_IsArray(content) || output_size == 0) {
        if (output_size > 0) {
            output[0] = '\0';
        }
        return;
    }

    cJSON_ArrayForEach(item, content) {
        cJSON *type = cJSON_GetObjectItem(item, "type");

        if (!cJSON_IsString(type)) {
            continue;
        }

        if (strcmp(type->valuestring, "text") == 0) {
            cJSON *text = cJSON_GetObjectItem(item, "text");
            if (cJSON_IsString(text) && text->valuestring) {
                size_t len = strlen(text->valuestring);
                size_t room = output_size - 1 - offset;

                if (room > 0) {
                    if (len > room) {
                        len = room;
                    }
                    memcpy(output + offset, text->valuestring, len);
                    offset += len;
                }
            }
        }

        if (offset >= output_size - 1) {
            break;
        }
    }

    output[offset] = '\0';
    cap_mcp_sanitize_utf8(output);
}

static esp_err_t cap_mcp_call_execute(const char *input_json,
                                      const claw_cap_call_context_t *ctx,
                                      char *output,
                                      size_t output_size)
{
    cJSON *result = NULL;
    const char *error_message = NULL;
    cJSON *is_error = NULL;
    esp_err_t err;

    (void)ctx;

    err = cap_mcp_call_remote_tool(input_json, &result);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: MCP request failed (%s)", esp_err_to_name(err));
        return err;
    }

    error_message = cJSON_GetStringValue(cJSON_GetObjectItem(result, "error_message"));
    if (error_message && error_message[0]) {
        snprintf(output, output_size, "Error: %s", error_message);
        cJSON_Delete(result);
        return ESP_OK;
    }

    cap_mcp_extract_content_text(cJSON_GetObjectItem(result, "content"), output, output_size);
    if (output[0] == '\0') {
        is_error = cJSON_GetObjectItem(result, "isError");
        if (cJSON_IsBool(is_error) && cJSON_IsTrue(is_error)) {
            snprintf(output, output_size, "Error: Tool returned application error");
        } else {
            snprintf(output, output_size, "(empty)");
        }
    }

    cap_mcp_sanitize_utf8(output);
    cJSON_Delete(result);
    return ESP_OK;
}

static esp_err_t cap_mcp_list_execute(const char *input_json,
                                      const claw_cap_call_context_t *ctx,
                                      char *output,
                                      size_t output_size)
{
    cJSON *result = NULL;
    const char *error_message = NULL;
    cJSON *tools_array = NULL;
    cJSON *tool = NULL;
    size_t offset = 0;
    esp_err_t err;

    (void)ctx;

    err = cap_mcp_list_remote_tools(input_json, &result);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: MCP request failed (%s)", esp_err_to_name(err));
        return err;
    }

    error_message = cJSON_GetStringValue(cJSON_GetObjectItem(result, "error_message"));
    if (error_message && error_message[0]) {
        snprintf(output, output_size, "Error: %s", error_message);
        cJSON_Delete(result);
        return ESP_OK;
    }

    tools_array = cJSON_GetObjectItem(result, "tools");
    if (cJSON_IsArray(tools_array)) {
        size_t count = 0;
        size_t max_display = 0;
        cJSON_ArrayForEach(tool, tools_array) {
            count++;
        }
        
        /* Write summary line */
        int written = snprintf(output + offset, output_size - offset,
                               "Found %u tool(s) on the remote MCP server.\n\n", (unsigned)count);
        if (written > 0 && (size_t)written < output_size - offset) offset += (size_t)written;
        
        /* List tool names (compact, no descriptions) */
        cJSON_ArrayForEach(tool, tools_array) {
            const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(tool, "name"));
            size_t name_len = name ? strlen(name) : 9; /* "(no name)" */
            size_t line_len = 2 + name_len + 1; /* "  " + name + "\n" */
            
            /* Check if full line fits without truncation */
            if (line_len >= output_size - offset) {
                if (count > max_display) {
                    offset += snprintf(output + offset, output_size - offset,
                                       "...and %u more\n", (unsigned)(count - max_display));
                }
                break;
            }
            /* Reserve min 10 bytes at end for safety */
            if (output_size - offset - line_len < 10) {
                if (count > max_display) {
                    offset += snprintf(output + offset, output_size - offset,
                                       "...and %u more\n", (unsigned)(count - max_display));
                }
                break;
            }
            
            memcpy(output + offset, "  ", 2);
            offset += 2;
            memcpy(output + offset, name, name_len);
            offset += name_len;
            output[offset] = '\n';
            offset += 1;
            max_display++;
        }
    }

    cJSON *next_cursor = cJSON_GetObjectItem(result, "nextCursor");
    if (cJSON_IsString(next_cursor) && next_cursor->valuestring[0] && offset < output_size - 1) {
        offset += snprintf(output + offset,
                           output_size - offset,
                           "\n(nextCursor: %s)",
                           next_cursor->valuestring);
    }
    if (offset == 0) {
        snprintf(output, output_size, "(no tools)");
    } else if (offset >= output_size) {
        cap_mcp_truncate_utf8_safe(output, output_size);
    }

    cap_mcp_sanitize_utf8(output);
    cJSON_Delete(result);
    return ESP_OK;
}

static esp_err_t cap_mcp_discover_execute(const char *input_json,
                                          const claw_cap_call_context_t *ctx,
                                          char *output,
                                          size_t output_size)
{
    cJSON *root = NULL;
    cJSON *devices = NULL;
    cJSON *device = NULL;
    size_t offset = 0;
    size_t found = 0;
    esp_err_t err;

    (void)ctx;

    err = cap_mcp_discover_services(input_json, &root);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: mDNS MCP discovery failed (%s)", esp_err_to_name(err));
        return err;
    }

    devices = cJSON_GetObjectItem(root, "devices");
    if (cJSON_IsArray(devices)) {
        cJSON_ArrayForEach(device, devices) {
            const char *instance = cJSON_GetStringValue(cJSON_GetObjectItem(device, "instance"));
            const char *hostname = cJSON_GetStringValue(cJSON_GetObjectItem(device, "hostname"));
            const char *ip = cJSON_GetStringValue(cJSON_GetObjectItem(device, "ip"));
            const char *endpoint = cJSON_GetStringValue(cJSON_GetObjectItem(device, "endpoint"));
            const char *url = cJSON_GetStringValue(cJSON_GetObjectItem(device, "url"));
            cJSON *port = cJSON_GetObjectItem(device, "port");
            int written = snprintf(output + offset,
                                   output_size - offset,
                                   "instance=%s\nhostname=%s\nip=%s\nport=%u\nendpoint=%s\nurl=%s\n\n",
                                   instance ? instance : "(unknown)",
                                   hostname ? hostname : "(unknown)",
                                   ip ? ip : "(unresolved)",
                                   cJSON_IsNumber(port) ? (unsigned)port->valueint : 0,
                                   endpoint ? endpoint : CAP_MCP_DEFAULT_ENDPOINT,
                                   url ? url : "(unknown)");

            if (written < 0 || (size_t)written >= output_size - offset) {
                offset = output_size - 1;
                break;
            }
            offset += (size_t)written;
            found++;
        }
    }

    cJSON_Delete(root);
    if (found == 0) {
        snprintf(output, output_size, "(no mcp servers discovered)");
    } else if (offset >= 2) {
        output[offset - 1] = '\0';
    }

    return ESP_OK;
}

static claw_cap_descriptor_t s_mcp_client_descriptors[] = {
    {
        .id = "mcp_call",
        .name = "mcp_call",
        .family = "mcp",
        .description = "Call a tool on a pre-configured MCP server.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =   /* Updated at runtime by cap_mcp_rebuild_schemas() */
        "{"
          "\"type\":\"object\","
          "\"properties\":{"
            "\"server\":{\"type\":\"string\",\"enum\":[],\"description\":\"Server name\"},"
            "\"tool_name\":{\"type\":\"string\",\"description\":\"Remote tool name to call\"},"
            "\"arguments\":{\"type\":\"object\",\"description\":\"Tool arguments\"}"
          "},"
          "\"required\":[\"server\",\"tool_name\"]"
        "}",
        .execute = cap_mcp_call_execute_v2,
    },
    {
        .id = "mcp_list_tools",
        .name = "mcp_list_tools",
        .family = "mcp",
        .description = "List tools from a pre-configured MCP server.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =   /* Updated at runtime by cap_mcp_rebuild_schemas() */
        "{"
          "\"type\":\"object\","
          "\"properties\":{"
            "\"server\":{\"type\":\"string\",\"enum\":[],\"description\":\"Server name\"},"
            "\"cursor\":{\"type\":\"string\",\"description\":\"Pagination cursor\"}"
          "},"
          "\"required\":[\"server\"]"
        "}",
        .execute = cap_mcp_list_execute_v2,
    },
    {
        .id = "mcp_discover",
        .name = "mcp_discover",
        .family = "mcp",
        .description = "Discover MCP servers on the local network via mDNS.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"timeout_ms\":{\"type\":\"integer\"},\"include_self\":{\"type\":\"boolean\"}}}",
        .execute = cap_mcp_discover_execute,
    },
};

static const claw_cap_group_t s_mcp_client_group = {
    .group_id = "cap_mcp_client",
    .descriptors = s_mcp_client_descriptors,
    .descriptor_count = sizeof(s_mcp_client_descriptors) / sizeof(s_mcp_client_descriptors[0]),
    .group_init = cap_mcp_client_group_init,
};

/* ─── Dynamic schema rebuild ───────────────────────────────────── */

void cap_mcp_rebuild_schemas(void)
{
    char names_enum[512];
    char names_csv[512];

    if (s_mcp_config.count == 0) {
        /* No servers configured — use empty enum */
        names_enum[0] = '\0';
        names_csv[0] = '\0';
    } else {
        mcp_server_config_list_names_json(&s_mcp_config, names_enum, sizeof(names_enum));
        mcp_server_config_list_names(&s_mcp_config, names_csv, sizeof(names_csv));
    }

    snprintf(s_mcp_call_schema, sizeof(s_mcp_call_schema),
        "{"
          "\"type\":\"object\","
          "\"properties\":{"
            "\"server\":{\"type\":\"string\",\"enum\":[%s],"
              "\"description\":\"MCP server name\"},"
            "\"tool_name\":{\"type\":\"string\",\"description\":\"Remote tool name to call\"},"
            "\"arguments\":{\"type\":\"object\",\"description\":\"Tool arguments\"}"
          "},"
          "\"required\":[\"server\",\"tool_name\"]"
        "}", names_enum);

    snprintf(s_mcp_list_schema, sizeof(s_mcp_list_schema),
        "{"
          "\"type\":\"object\","
          "\"properties\":{"
            "\"server\":{\"type\":\"string\",\"enum\":[%s],"
              "\"description\":\"MCP server name\"},"
            "\"cursor\":{\"type\":\"string\",\"description\":\"Pagination cursor\"}"
          "},"
          "\"required\":[\"server\"]"
        "}", names_enum);

    if (s_mcp_config.count > 0) {
        snprintf(s_mcp_call_desc, sizeof(s_mcp_call_desc),
            "Call a tool on a pre-configured MCP server. "
            "Available servers: %s.", names_csv);
        snprintf(s_mcp_list_desc, sizeof(s_mcp_list_desc),
            "List tools from a pre-configured MCP server. "
            "Available servers: %s.", names_csv);
    } else {
        strlcpy(s_mcp_call_desc,
            "Call a tool on a pre-configured MCP server. "
            "No servers configured yet. Use 'mcp server add' to add one.",
            sizeof(s_mcp_call_desc));
        strlcpy(s_mcp_list_desc,
            "List tools from a pre-configured MCP server. "
            "No servers configured yet. Use 'mcp server add' to add one.",
            sizeof(s_mcp_list_desc));
    }

    /* Update descriptor pointers */
    s_mcp_client_descriptors[0].input_schema_json = s_mcp_call_schema;
    s_mcp_client_descriptors[0].description = s_mcp_call_desc;
    s_mcp_client_descriptors[1].input_schema_json = s_mcp_list_schema;
    s_mcp_client_descriptors[1].description = s_mcp_list_desc;
}

/* ─── v2 Execute: mcp_call (server-profile based) ──────────────── */

esp_err_t cap_mcp_call_execute_v2(const char *input_json,
                                   const claw_cap_call_context_t *ctx,
                                   char *output,
                                   size_t output_size)
{
    cJSON *input = NULL;
    cJSON *result = NULL;
    esp_err_t err;

    (void)ctx;

    if (!input_json || !output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    output[0] = '\0';

    input = cJSON_Parse(input_json);
    if (!input || !cJSON_IsObject(input)) {
        snprintf(output, output_size, "Error: Invalid JSON input");
        cJSON_Delete(input);
        return ESP_OK;
    }

    /* 1. Parse server name */
    cJSON *server_item = cJSON_GetObjectItem(input, "server");
    if (!cJSON_IsString(server_item) || !server_item->valuestring[0]) {
        snprintf(output, output_size, "Error: 'server' field is required");
        cJSON_Delete(input);
        return ESP_OK;
    }

    /* 2. Look up profile */
    const mcp_server_profile_t *profile = mcp_server_config_find(&s_mcp_config,
                                                                  server_item->valuestring);
    if (!profile) {
        snprintf(output, output_size,
                 "Error: Unknown server '%s'. Use 'mcp server list' to see available servers.",
                 server_item->valuestring);
        cJSON_Delete(input);
        return ESP_OK;
    }

    /* 3. Build full input_json with resolved URL, token, endpoint */
    cJSON *full_input = cJSON_CreateObject();
    if (!full_input) {
        cJSON_Delete(input);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(full_input, "server_url", profile->url);
    cJSON_AddStringToObject(full_input, "endpoint", profile->endpoint);
    if (profile->token[0]) {
        cJSON_AddStringToObject(full_input, "auth_token", profile->token);
    }

    /* Copy tool_name and arguments from input */
    cJSON *tool_name = cJSON_GetObjectItem(input, "tool_name");
    cJSON *arguments = cJSON_GetObjectItem(input, "arguments");
    const char *tool_name_str = cJSON_IsString(tool_name) ? tool_name->valuestring : NULL;
    if (tool_name_str) {
        cJSON_AddStringToObject(full_input, "tool_name", tool_name_str);
    }
    if (cJSON_IsObject(arguments)) {
        cJSON_AddItemToObject(full_input, "arguments", cJSON_Duplicate(arguments, 1));
    }

    char *full_json = cJSON_PrintUnformatted(full_input);
    cJSON_Delete(full_input);
    cJSON_Delete(input);

    if (!full_json) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "[V2 CALL] server=%s tool=%s",
             profile->name, tool_name_str ? tool_name_str : "?");

    /* 4. Call existing remote tool logic */
    err = cap_mcp_call_remote_tool(full_json, &result);
    free(full_json);

    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: MCP request failed (%s)", esp_err_to_name(err));
        return err;
    }

    /* 5. Format output (same logic as old cap_mcp_call_execute) */
    const char *error_message = cJSON_GetStringValue(cJSON_GetObjectItem(result, "error_message"));
    if (error_message && error_message[0]) {
        snprintf(output, output_size, "Error: %s", error_message);
        cJSON_Delete(result);
        return ESP_OK;
    }

    cap_mcp_extract_content_text(cJSON_GetObjectItem(result, "content"), output, output_size);
    if (output[0] == '\0') {
        cJSON *is_error = cJSON_GetObjectItem(result, "isError");
        if (cJSON_IsBool(is_error) && cJSON_IsTrue(is_error)) {
            snprintf(output, output_size, "Error: Tool returned application error");
        } else {
            snprintf(output, output_size, "(empty)");
        }
    }

    cap_mcp_sanitize_utf8(output);
    cJSON_Delete(result);
    return ESP_OK;
}

/* ─── v2 Execute: mcp_list_tools (server-profile based) ────────── */

esp_err_t cap_mcp_list_execute_v2(const char *input_json,
                                   const claw_cap_call_context_t *ctx,
                                   char *output,
                                   size_t output_size)
{
    cJSON *input = NULL;
    cJSON *result = NULL;
    esp_err_t err;

    (void)ctx;

    if (!input_json || !output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    output[0] = '\0';

    input = cJSON_Parse(input_json);
    if (!input || !cJSON_IsObject(input)) {
        snprintf(output, output_size, "Error: Invalid JSON input");
        cJSON_Delete(input);
        return ESP_OK;
    }

    /* 1. Parse server name */
    cJSON *server_item = cJSON_GetObjectItem(input, "server");
    if (!cJSON_IsString(server_item) || !server_item->valuestring[0]) {
        snprintf(output, output_size, "Error: 'server' field is required");
        cJSON_Delete(input);
        return ESP_OK;
    }

    /* 2. Look up profile */
    const mcp_server_profile_t *profile = mcp_server_config_find(&s_mcp_config,
                                                                  server_item->valuestring);
    if (!profile) {
        snprintf(output, output_size,
                 "Error: Unknown server '%s'. Use 'mcp server list' to see available servers.",
                 server_item->valuestring);
        cJSON_Delete(input);
        return ESP_OK;
    }

    /* 3. Build full input_json with resolved URL, token, endpoint */
    cJSON *full_input = cJSON_CreateObject();
    if (!full_input) {
        cJSON_Delete(input);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(full_input, "server_url", profile->url);
    cJSON_AddStringToObject(full_input, "endpoint", profile->endpoint);
    if (profile->token[0]) {
        cJSON_AddStringToObject(full_input, "auth_token", profile->token);
    }

    /* Copy cursor from input */
    cJSON *cursor = cJSON_GetObjectItem(input, "cursor");
    if (cJSON_IsString(cursor) && cursor->valuestring[0]) {
        cJSON_AddStringToObject(full_input, "cursor", cursor->valuestring);
    }

    char *full_json = cJSON_PrintUnformatted(full_input);
    cJSON_Delete(full_input);
    cJSON_Delete(input);

    if (!full_json) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "[V2 LIST] server=%s", profile->name);

    /* 4. Call existing list remote tools logic */
    err = cap_mcp_list_remote_tools(full_json, &result);
    free(full_json);

    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: MCP request failed (%s)", esp_err_to_name(err));
        return err;
    }

    /* 5. Format output (same logic as old cap_mcp_list_execute) */
    const char *error_message = cJSON_GetStringValue(cJSON_GetObjectItem(result, "error_message"));
    if (error_message && error_message[0]) {
        snprintf(output, output_size, "Error: %s", error_message);
        cJSON_Delete(result);
        return ESP_OK;
    }

    cJSON *tools_array = cJSON_GetObjectItem(result, "tools");
    if (cJSON_IsArray(tools_array)) {
        size_t count = 0;
        size_t offset = 0;
        cJSON *tool = NULL;

        cJSON_ArrayForEach(tool, tools_array) {
            count++;
        }

        int written = snprintf(output + offset, output_size - offset,
                               "Found %u tool(s) on '%s':\n\n", (unsigned)count, profile->name);
        if (written > 0 && (size_t)written < output_size - offset) {
            offset += (size_t)written;
        }

        cJSON_ArrayForEach(tool, tools_array) {
            const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(tool, "name"));
            size_t name_len = name ? strlen(name) : 9;
            size_t line_len = 2 + name_len + 1;

            if (offset + line_len >= output_size - 10) {
                if (count > 0) {
                    snprintf(output + offset, output_size - offset,
                             "...and more\n");
                }
                break;
            }
            memcpy(output + offset, "  ", 2);
            offset += 2;
            memcpy(output + offset, name ? name : "(no name)", name_len);
            offset += name_len;
            output[offset++] = '\n';
        }
        output[offset] = '\0';
    }

    cJSON *next_cursor = cJSON_GetObjectItem(result, "nextCursor");
    if (cJSON_IsString(next_cursor) && next_cursor->valuestring[0]) {
        size_t len = strlen(output);
        snprintf(output + len, output_size - len, "\n(nextCursor: %s)", next_cursor->valuestring);
    }

    if (output[0] == '\0') {
        snprintf(output, output_size, "(no tools)");
    }

    cap_mcp_sanitize_utf8(output);
    cJSON_Delete(result);
    return ESP_OK;
}

/* ─── Config access ────────────────────────────────────────────── */

const mcp_server_config_t *cap_mcp_get_config(void)
{
    return &s_mcp_config;
}

mcp_server_config_t *cap_mcp_get_config_mutable(void)
{
    return &s_mcp_config;
}

/* ─── Group registration ───────────────────────────────────────── */

esp_err_t cap_mcp_client_register_group(void)
{
    esp_err_t err;

    if (claw_cap_group_exists(s_mcp_client_group.group_id)) {
        return ESP_OK;
    }

    /* Load MCP server configuration */
    err = mcp_server_config_load(MCP_SERVER_CONFIG_PATH, &s_mcp_config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to load MCP server config, starting with empty config");
        /* Not fatal — start with empty config */
    }

    /* Validate loaded config */
    err = mcp_server_config_validate(&s_mcp_config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "MCP server config validation failed, clearing config");
        mcp_server_config_free(&s_mcp_config);
    }

    /* Build dynamic schemas from loaded config */
    cap_mcp_rebuild_schemas();

    ESP_LOGI(TAG, "MCP client initialized with %u server(s)", (unsigned)s_mcp_config.count);

    return claw_cap_register_group(&s_mcp_client_group);
}
