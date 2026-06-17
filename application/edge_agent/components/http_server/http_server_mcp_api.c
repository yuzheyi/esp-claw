/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * REST API for MCP Server configuration management.
 *
 * GET  /api/mcp_servers  → list all servers (JSON)
 * POST /api/mcp_servers  → add/remove/edit/toggle (JSON body)
 */
#include "http_server_priv.h"
#include "esp_check.h"
#include "esp_log.h"

#if !CONFIG_APP_CLAW_CAP_MCP_CLIENT
/* When MCP client is disabled, compile as stub */
esp_err_t http_server_register_mcp_routes(httpd_handle_t server)
{
    (void)server;
    return ESP_OK;
}
#else

#include <stdio.h>
#include <string.h>
#include "http_server_mcp_config.h"

static const char *TAG = "mcp_api";

/* ─── Helpers ───────────────────────────────────────────────────── */

/* Build a JSON response from the config */
static esp_err_t mcp_api_send_config(httpd_req_t *req, const mcp_server_config_t *config)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *servers = cJSON_CreateObject();
    for (size_t i = 0; i < config->count; i++) {
        const mcp_server_profile_t *p = &config->profiles[i];
        cJSON *srv = cJSON_CreateObject();
        cJSON_AddStringToObject(srv, "url", p->url);
        cJSON_AddStringToObject(srv, "token", p->token);
        cJSON_AddStringToObject(srv, "endpoint", p->endpoint);
        cJSON_AddStringToObject(srv, "description", p->description);
        cJSON_AddBoolToObject(srv, "enabled", p->enabled);
        cJSON_AddItemToObject(servers, p->name, srv);
    }
    cJSON_AddItemToObject(root, "servers", servers);

    const char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    free((void *)json);
    cJSON_Delete(root);
    return ESP_OK;
}

/* ─── GET handler: list all MCP servers ─────────────────────────── */

static esp_err_t mcp_api_get_handler(httpd_req_t *req)
{
    mcp_server_config_t config = {0};
    esp_err_t err = mcp_server_config_load(MCP_SERVER_CONFIG_PATH, &config);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to load config");
        return ESP_FAIL;
    }
    esp_err_t ret = mcp_api_send_config(req, &config);
    mcp_server_config_free(&config);
    return ret;
}

/* ─── POST handler: add/remove/edit/toggle ──────────────────────── */

static esp_err_t mcp_api_post_handler(httpd_req_t *req)
{
    /* Read body into heap buffer — do NOT use large stack arrays in httpd handlers
     * (default stack is only 4KB, 8KB array causes stack overflow → PC=0 crash) */
    int total = req->content_len;
    if (total <= 0 || total > 4096) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid content length");
        return ESP_FAIL;
    }

    char *buf = malloc((size_t)total + 1);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    int received = 0;
    while (received < total) {
        int ret = httpd_req_recv(req, buf + received, total - received);
        if (ret <= 0) {
            free(buf);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Recv failed");
            return ESP_FAIL;
        }
        received += ret;
    }
    buf[received] = '\0';

    cJSON *body = cJSON_Parse(buf);
    if (!body) {
        free(buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    /* Load existing config */
    mcp_server_config_t config = {0};
    mcp_server_config_load(MCP_SERVER_CONFIG_PATH, &config);

    const char *action = cJSON_GetStringValue(cJSON_GetObjectItem(body, "action"));
    cJSON *data = cJSON_GetObjectItem(body, "data");
    esp_err_t err = ESP_OK;

    if (action && strcmp(action, "add") == 0 && data) {
        mcp_server_profile_t profile = {0};
        snprintf(profile.name, sizeof(profile.name), "%s", cJSON_GetStringValue(cJSON_GetObjectItem(data, "name")) ?: "");
        snprintf(profile.url, sizeof(profile.url), "%s", cJSON_GetStringValue(cJSON_GetObjectItem(data, "url")) ?: "");
        snprintf(profile.token, sizeof(profile.token), "%s", cJSON_GetStringValue(cJSON_GetObjectItem(data, "token")) ?: "");
        snprintf(profile.endpoint, sizeof(profile.endpoint), "%s", cJSON_GetStringValue(cJSON_GetObjectItem(data, "endpoint")) ?: "");
        snprintf(profile.description, sizeof(profile.description), "%s", cJSON_GetStringValue(cJSON_GetObjectItem(data, "description")) ?: "");
        profile.enabled = cJSON_IsTrue(cJSON_GetObjectItem(data, "enabled"));
        if (profile.name[0] && profile.url[0]) {
            err = mcp_server_config_add(&config, &profile);
        } else {
            err = ESP_ERR_INVALID_ARG;
        }
    } else if (action && strcmp(action, "remove") == 0) {
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(body, "name"));
        if (name) {
            err = mcp_server_config_remove(&config, name);
        }
    } else if (action && strcmp(action, "toggle") == 0) {
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(body, "name"));
        if (name) {
            for (size_t i = 0; i < config.count; i++) {
                if (strcmp(config.profiles[i].name, name) == 0) {
                    config.profiles[i].enabled = !config.profiles[i].enabled;
                    break;
                }
            }
        }
    }

    if (err == ESP_OK) {
        err = mcp_server_config_save(MCP_SERVER_CONFIG_PATH, &config);
    }

    mcp_server_config_free(&config);
    cJSON_Delete(body);
    free(buf);

    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, esp_err_to_name(err));
        return ESP_FAIL;
    }

    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* ─── Route registration ────────────────────────────────────────── */

static const httpd_uri_t s_mcp_get = {
    .uri = "/api/mcp_servers",
    .method = HTTP_GET,
    .handler = mcp_api_get_handler,
};

static const httpd_uri_t s_mcp_post = {
    .uri = "/api/mcp_servers",
    .method = HTTP_POST,
    .handler = mcp_api_post_handler,
};

esp_err_t http_server_register_mcp_routes(httpd_handle_t server)
{
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &s_mcp_get),
                        TAG, "register GET /api/mcp_servers");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &s_mcp_post),
                        TAG, "register POST /api/mcp_servers");
    ESP_LOGI(TAG, "MCP server config API registered");
    return ESP_OK;
}

#endif /* CONFIG_APP_CLAW_CAP_MCP_CLIENT */
