/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * MCP Server configuration persistence (load/save from JSON file).
 */
#include "http_server_mcp_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

static const char *TAG = "mcp_config";

#define MCP_CONFIG_DEFAULT_CAPACITY  16
#define MCP_CONFIG_READ_BUF_SIZE     (16 * 1024)

/* ─── Internal helpers ──────────────────────────────────────────── */

static char *file_read_all(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0 || size > MCP_CONFIG_READ_BUF_SIZE) {
        fclose(f);
        return NULL;
    }

    char *buf = calloc(1, (size_t)size + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }

    size_t rd = fread(buf, 1, (size_t)size, f);
    fclose(f);

    if ((long)rd != size) {
        free(buf);
        return NULL;
    }
    return buf;
}

static esp_err_t file_write_all(const char *path, const char *content)
{
    FILE *f = fopen(path, "w");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s for writing", path);
        return ESP_FAIL;
    }
    size_t len = strlen(content);
    size_t written = fwrite(content, 1, len, f);
    fclose(f);
    return (written == len) ? ESP_OK : ESP_FAIL;
}

/* ─── Public API ────────────────────────────────────────────────── */

esp_err_t mcp_server_config_load(const char *config_path, mcp_server_config_t *config)
{
    char *json_str = NULL;
    cJSON *root = NULL;
    cJSON *servers_obj = NULL;
    esp_err_t err = ESP_OK;

    if (!config_path || !config) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(config, 0, sizeof(*config));
    config->capacity = MCP_CONFIG_DEFAULT_CAPACITY;
    config->profiles = calloc(config->capacity, sizeof(mcp_server_profile_t));
    if (!config->profiles) {
        return ESP_ERR_NO_MEM;
    }

    json_str = file_read_all(config_path);
    if (!json_str) {
        ESP_LOGW(TAG, "Config file %s not found, starting empty", config_path);
        return ESP_OK;  /* Empty config is valid */
    }

    root = cJSON_Parse(json_str);
    free(json_str);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse %s", config_path);
        return ESP_ERR_INVALID_STATE;
    }

    servers_obj = cJSON_GetObjectItem(root, "servers");
    if (!cJSON_IsObject(servers_obj)) {
        cJSON_Delete(root);
        return ESP_OK;  /* No servers key = empty */
    }

    cJSON *server_entry;
    cJSON_ArrayForEach(server_entry, servers_obj) {
        if (config->count >= config->capacity) {
            break;
        }
        mcp_server_profile_t *p = &config->profiles[config->count];
        memset(p, 0, sizeof(*p));

        snprintf(p->name, sizeof(p->name), "%s", server_entry->string);
        snprintf(p->url, sizeof(p->url), "%s", cJSON_GetStringValue(cJSON_GetObjectItem(server_entry, "url")) ?: "");
        snprintf(p->token, sizeof(p->token), "%s", cJSON_GetStringValue(cJSON_GetObjectItem(server_entry, "token")) ?: "");
        snprintf(p->endpoint, sizeof(p->endpoint), "%s", cJSON_GetStringValue(cJSON_GetObjectItem(server_entry, "endpoint")) ?: "");
        snprintf(p->description, sizeof(p->description), "%s", cJSON_GetStringValue(cJSON_GetObjectItem(server_entry, "description")) ?: "");
        p->enabled = cJSON_IsTrue(cJSON_GetObjectItem(server_entry, "enabled"));
        if (!p->enabled) {
            p->enabled = true;  /* Default enabled if not specified */
        }
        config->count++;
    }

    cJSON_Delete(root);
    ESP_LOGI(TAG, "Loaded %u MCP server profile(s) from %s",
             (unsigned)config->count, config_path);
    return err;
}

esp_err_t mcp_server_config_save(const char *config_path, const mcp_server_config_t *config)
{
    if (!config_path || !config) {
        return ESP_ERR_INVALID_ARG;
    }

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

    char *json_str = cJSON_Print(root);
    cJSON_Delete(root);
    if (!json_str) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = file_write_all(config_path, json_str);
    free(json_str);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Saved %u MCP server profile(s) to %s",
                 (unsigned)config->count, config_path);
    }
    return err;
}

void mcp_server_config_free(mcp_server_config_t *config)
{
    if (!config) return;
    free(config->profiles);
    config->profiles = NULL;
    config->count = 0;
    config->capacity = 0;
}

const mcp_server_profile_t *mcp_server_config_find(const mcp_server_config_t *config,
                                                    const char *name)
{
    if (!config || !name) return NULL;
    for (size_t i = 0; i < config->count; i++) {
        if (strcmp(config->profiles[i].name, name) == 0) {
            return &config->profiles[i];
        }
    }
    return NULL;
}

esp_err_t mcp_server_config_add(mcp_server_config_t *config,
                                 const mcp_server_profile_t *profile)
{
    if (!config || !profile) return ESP_ERR_INVALID_ARG;

    /* Check if name already exists — replace if so */
    for (size_t i = 0; i < config->count; i++) {
        if (strcmp(config->profiles[i].name, profile->name) == 0) {
            config->profiles[i] = *profile;
            return ESP_OK;
        }
    }

    if (config->count >= config->capacity) {
        size_t new_cap = config->capacity * 2;
        mcp_server_profile_t *new_buf = realloc(config->profiles,
                                                 new_cap * sizeof(mcp_server_profile_t));
        if (!new_buf) return ESP_ERR_NO_MEM;
        config->profiles = new_buf;
        config->capacity = new_cap;
    }

    config->profiles[config->count] = *profile;
    config->count++;
    return ESP_OK;
}

esp_err_t mcp_server_config_remove(mcp_server_config_t *config, const char *name)
{
    if (!config || !name) return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < config->count; i++) {
        if (strcmp(config->profiles[i].name, name) == 0) {
            /* Shift remaining entries */
            for (size_t j = i; j < config->count - 1; j++) {
                config->profiles[j] = config->profiles[j + 1];
            }
            config->count--;
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}
