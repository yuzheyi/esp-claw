/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * REST API for MCP Server configuration.
 *
 * GET  /api/mcp_servers           → list all servers (JSON)
 * POST /api/mcp_servers           → add/remove/edit/verify/toggle (JSON body)
 */
#include "http_server_priv.h"
#include "esp_check.h"
#include "esp_log.h"

#if !CONFIG_APP_CLAW_CAP_MCP_CLIENT
/* When MCP client is disabled, compile as stub */
esp_err_t http_server_register_mcp_routes(httpd_handle_t server)
{
    (void)server;
    ESP_LOGI("mcp_api", "MCP client disabled");
    return ESP_OK;
}
#else

#include <stdio.h>
#include <string.h>
#include "cap_mcp_client_config.h"
#include "cap_mcp_client_internal.h"

static const char *TAG = "mcp_api";

/* ── Helper: test connection via real MCP protocol ──────────── */
static esp_err_t test_server_mcp(const mcp_server_profile_t *profile,
                                  bool *reachable, char *detail, size_t detail_size)
{
    *reachable = false;
    if (detail && detail_size > 0) detail[0] = '\0';

    /* Step 1: Build input JSON for MCP list-tools.
     * Do NOT pass initialize:true — let cap_mcp_list_remote_tools()
     * decide whether to re-use a cached session or create a new one.
     * Forcing initialize with a stale session_id causes ESP_ERR_INVALID_RESPONSE.
     */
    cJSON *input = cJSON_CreateObject();
    if (!input) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(input, "server_url", profile->url);
    cJSON_AddStringToObject(input, "endpoint", profile->endpoint);
    if (profile->token[0]) {
        cJSON_AddStringToObject(input, "auth_token", profile->token);
    }

    char *json_str = cJSON_PrintUnformatted(input);
    cJSON_Delete(input);
    if (!json_str) return ESP_ERR_NO_MEM;

    /* Step 2: Call the existing MCP list-remote-tools logic */
    cJSON *result = NULL;
    esp_err_t err = cap_mcp_list_remote_tools(json_str, &result);
    free(json_str);

    if (err != ESP_OK) {
        if (detail) snprintf(detail, detail_size, "HTTP/transport error: %s", esp_err_to_name(err));
        *reachable = false;
        return err;
    }

    /* Check for MCP-level error */
    const char *err_msg = cJSON_GetStringValue(cJSON_GetObjectItem(result, "error_message"));
    if (err_msg && err_msg[0]) {
        if (detail) snprintf(detail, detail_size, "MCP error: %s", err_msg);
        *reachable = false;
        cJSON_Delete(result);
        return ESP_OK; /* Server responded, but MCP protocol error */
    }

    /* Success — count tools */
    cJSON *tools = cJSON_GetObjectItem(result, "tools");
    size_t tool_count = cJSON_IsArray(tools) ? cJSON_GetArraySize(tools) : 0;
    *reachable = true;
    if (detail) snprintf(detail, detail_size, "%u tool(s) available", (unsigned)tool_count);
    cJSON_Delete(result);
    return ESP_OK;
}

static esp_err_t mcp_servers_get_handler(httpd_req_t *req)
{
    const mcp_server_config_t *config = cap_mcp_get_config();
    cJSON *root = cJSON_CreateObject(), *servers = cJSON_CreateArray();
    if (!root || !servers) { cJSON_Delete(root); cJSON_Delete(servers); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM"); return ESP_FAIL; }
    cJSON_AddItemToObject(root, "servers", servers);
    for (size_t i = 0; i < config->count; i++) {
        const mcp_server_profile_t *p = &config->profiles[i];
        cJSON *s = cJSON_CreateObject();
        if (!s) continue;
        cJSON_AddStringToObject(s, "name", p->name);
        cJSON_AddStringToObject(s, "url", p->url);
        cJSON_AddStringToObject(s, "token", p->token[0] ? "***" : "");
        cJSON_AddStringToObject(s, "endpoint", p->endpoint);
        cJSON_AddBoolToObject(s, "enabled", p->enabled);
        if (p->description[0]) cJSON_AddStringToObject(s, "description", p->description);
        cJSON_AddItemToArray(servers, s);
    }
    http_server_send_json_response(req, root);
    return ESP_OK;
}

static esp_err_t mcp_servers_post_handler(httpd_req_t *req)
{
    cJSON *input = NULL, *response = cJSON_CreateObject();
    const char *action = NULL;
    if (http_server_parse_json_body(req, &input) != ESP_OK || !input) { cJSON_Delete(input); cJSON_Delete(response); httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON"); return ESP_FAIL; }
    if (!response) { cJSON_Delete(input); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM"); return ESP_FAIL; }
    action = cJSON_GetStringValue(cJSON_GetObjectItem(input, "action"));
    if (!action) { cJSON_AddStringToObject(response, "error", "Missing 'action'"); goto send; }

    if (strcmp(action, "add") == 0 || strcmp(action, "edit") == 0) {
        char n[32]={0}, u[512]={0}, t[512]={0}, e[64]={0}, d[128]={0};
        http_server_json_read_string(input, "name", n, sizeof(n));
        http_server_json_read_string(input, "url", u, sizeof(u));
        if (!n[0]||!u[0]) { cJSON_AddStringToObject(response,"error","name+url required"); goto send; }
        http_server_json_read_string(input, "token", t, sizeof(t));
        http_server_json_read_string(input, "description", d, sizeof(d));
        strlcpy(e,"mcp",sizeof(e));
        http_server_json_read_string(input, "endpoint", e, sizeof(e));
        mcp_server_profile_t p={0};
        strlcpy(p.name,n,sizeof(p.name)); strlcpy(p.url,u,sizeof(p.url));
        strlcpy(p.token,t,sizeof(p.token)); strlcpy(p.endpoint,e,sizeof(p.endpoint));
        strlcpy(p.description,d,sizeof(p.description));
        p.enabled = true;
        /* Check "enabled" from JSON */
        cJSON *enabled_item = cJSON_GetObjectItem(input, "enabled");
        if (cJSON_IsBool(enabled_item)) p.enabled = cJSON_IsTrue(enabled_item);

        mcp_server_config_t *cfg = cap_mcp_get_config_mutable();
        esp_err_t err = mcp_server_config_add(cfg, &p);
        if (err != ESP_OK) { cJSON_AddStringToObject(response,"error","Add/edit failed"); goto send; }
        mcp_server_config_save(MCP_SERVER_CONFIG_PATH, cfg);
        cap_mcp_rebuild_schemas();
        cJSON_AddStringToObject(response,"status","ok");
        cJSON_AddStringToObject(response,"message", strcmp(action,"add")==0 ? "Added" : "Updated");

    } else if (strcmp(action,"remove")==0) {
        char n[32]={0}; http_server_json_read_string(input,"name",n,sizeof(n));
        if (!n[0]) { cJSON_AddStringToObject(response,"error","name required"); goto send; }
        mcp_server_config_t *cfg=cap_mcp_get_config_mutable();
        if (mcp_server_config_remove(cfg,n)!=ESP_OK) { cJSON_AddStringToObject(response,"error","Not found"); goto send; }
        mcp_server_config_save(MCP_SERVER_CONFIG_PATH,cfg);
        cap_mcp_rebuild_schemas();
        cJSON_AddStringToObject(response,"status","ok"); cJSON_AddStringToObject(response,"message","Removed");

    } else if (strcmp(action,"toggle")==0) {
        char n[32]={0}; http_server_json_read_string(input,"name",n,sizeof(n));
        if (!n[0]) { cJSON_AddStringToObject(response,"error","name required"); goto send; }
        mcp_server_config_t *cfg = cap_mcp_get_config_mutable();
        const mcp_server_profile_t *found = mcp_server_config_find(cfg, n);
        if (!found) { cJSON_AddStringToObject(response,"error","Not found"); goto send; }
        /* Toggle via remove+add */
        mcp_server_profile_t updated = *found;
        updated.enabled = !found->enabled;
        mcp_server_config_remove(cfg, n);
        mcp_server_config_add(cfg, &updated);
        mcp_server_config_save(MCP_SERVER_CONFIG_PATH, cfg);
        cap_mcp_rebuild_schemas();
        cJSON_AddStringToObject(response,"status","ok");
        cJSON_AddBoolToObject(response,"enabled", updated.enabled);

    } else if (strcmp(action,"verify")==0) {
        char n[32]={0}; http_server_json_read_string(input,"name",n,sizeof(n));
        if (!n[0]) { cJSON_AddStringToObject(response,"error","name required"); goto send; }
        const mcp_server_config_t *cfg = cap_mcp_get_config();
        const mcp_server_profile_t *found = mcp_server_config_find(cfg, n);
        if (!found) { cJSON_AddStringToObject(response,"error","Not found"); goto send; }

        bool reachable = false;
        char detail[128] = {0};
        esp_err_t err = test_server_mcp(found, &reachable, detail, sizeof(detail));
        if (err != ESP_OK && !detail[0]) {
            snprintf(detail, sizeof(detail), "Error: %s", esp_err_to_name(err));
        }
        cJSON_AddBoolToObject(response, "reachable", reachable);
        cJSON_AddStringToObject(response, "status", reachable ? "online" : "offline");
        cJSON_AddStringToObject(response, "detail", detail);

    } else { cJSON_AddStringToObject(response,"error","Unknown action"); }
send:
    cJSON_Delete(input); http_server_send_json_response(req, response); return ESP_OK;
}

esp_err_t http_server_register_mcp_routes(httpd_handle_t server)
{
    httpd_uri_t g={.uri="/api/mcp_servers",.method=HTTP_GET,.handler=mcp_servers_get_handler};
    httpd_uri_t p={.uri="/api/mcp_servers",.method=HTTP_POST,.handler=mcp_servers_post_handler};
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server,&g),TAG,"GET fail");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server,&p),TAG,"POST fail");
    ESP_LOGI(TAG,"MCP API ready"); return ESP_OK;
}
#endif