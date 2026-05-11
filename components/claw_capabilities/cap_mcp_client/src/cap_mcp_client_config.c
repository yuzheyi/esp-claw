/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_mcp_client_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "cap_mcp_client_internal.h"
#include "esp_log.h"
#include "esp_vfs.h"

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

    size_t read = fread(buf, 1, (size_t)size, f);
    fclose(f);

    if ((long)read != size) {
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
    cJSON *server_item = NULL;
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
        ESP_LOGW(TAG, "Config file %s not found or empty, using empty config", config_path);
        return ESP_OK;  /* Not an error — empty config is valid */
    }

    root = cJSON_Parse(json_str);
    free(json_str);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse config JSON from %s", config_path);
        err = ESP_FAIL;
        goto cleanup;
    }

    servers_obj = cJSON_GetObjectItem(root, "servers");
    if (!cJSON_IsObject(servers_obj)) {
        ESP_LOGW(TAG, "No 'servers' object in config, using empty config");
        goto cleanup;
    }

    cJSON_ArrayForEach(server_item, servers_obj) {
        if (config->count >= config->capacity) {
            ESP_LOGW(TAG, "Too many servers (max %u), skipping rest",
                     (unsigned)config->capacity);
            break;
        }

        mcp_server_profile_t *profile = &config->profiles[config->count];
        memset(profile, 0, sizeof(*profile));

        /* name = cJSON object key */
        strlcpy(profile->name, server_item->string, sizeof(profile->name));

        /* url (required) */
        cJSON *url_item = cJSON_GetObjectItem(server_item, "url");
        if (cJSON_IsString(url_item) && url_item->valuestring[0]) {
            strlcpy(profile->url, url_item->valuestring, sizeof(profile->url));
        } else {
            ESP_LOGW(TAG, "Server '%s' has no URL, skipping", profile->name);
            continue;
        }

        /* token (optional) */
        cJSON *token_item = cJSON_GetObjectItem(server_item, "token");
        if (cJSON_IsString(token_item)) {
            strlcpy(profile->token, token_item->valuestring, sizeof(profile->token));
        }

        /* endpoint (optional, default "mcp"; explicit empty "" means URL-as-is) */
        cJSON *endpoint_item = cJSON_GetObjectItem(server_item, "endpoint");
        if (cJSON_IsString(endpoint_item)) {
            if (endpoint_item->valuestring[0]) {
                strlcpy(profile->endpoint, endpoint_item->valuestring, sizeof(profile->endpoint));
            }
            /* explicit "" → keep endpoint empty (use URL as-is) */
        } else {
            strlcpy(profile->endpoint, CAP_MCP_DEFAULT_ENDPOINT, sizeof(profile->endpoint));
        }

        /* description (optional) */
        cJSON *desc_item = cJSON_GetObjectItem(server_item, "description");
        if (cJSON_IsString(desc_item) && desc_item->valuestring[0]) {
            strlcpy(profile->description, desc_item->valuestring, sizeof(profile->description));
        }

        /* enabled (optional, default true) */
        cJSON *enabled_item = cJSON_GetObjectItem(server_item, "enabled");
        profile->enabled = (!cJSON_IsBool(enabled_item)) || cJSON_IsTrue(enabled_item);

        ESP_LOGI(TAG, "Loaded server: %s (%s)", profile->name, profile->url);
        config->count++;
    }

cleanup:
    cJSON_Delete(root);
    return err;
}

esp_err_t mcp_server_config_save(const char *config_path, const mcp_server_config_t *config)
{
    cJSON *root = NULL;
    cJSON *servers = NULL;
    char *json_str = NULL;
    esp_err_t err;

    if (!config_path || !config) {
        return ESP_ERR_INVALID_ARG;
    }

    root = cJSON_CreateObject();
    servers = cJSON_CreateObject();
    if (!root || !servers) {
        cJSON_Delete(root);
        cJSON_Delete(servers);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddItemToObject(root, "servers", servers);

    for (size_t i = 0; i < config->count; i++) {
        const mcp_server_profile_t *p = &config->profiles[i];
        cJSON *server = cJSON_CreateObject();
        if (!server) {
            err = ESP_ERR_NO_MEM;
            goto cleanup;
        }

        cJSON_AddStringToObject(server, "url", p->url);
        if (p->token[0]) {
            cJSON_AddStringToObject(server, "token", p->token);
        }
        if (p->endpoint[0] && strcmp(p->endpoint, CAP_MCP_DEFAULT_ENDPOINT) != 0) {
            cJSON_AddStringToObject(server, "endpoint", p->endpoint);
        }
        if (p->description[0]) {
            cJSON_AddStringToObject(server, "description", p->description);
        }
        if (!p->enabled) {
            cJSON_AddBoolToObject(server, "enabled", false);
        }

        cJSON_AddItemToObject(servers, p->name, server);
    }

    json_str = cJSON_Print(root);
    if (!json_str) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    err = file_write_all(config_path, json_str);

cleanup:
    free(json_str);
    cJSON_Delete(root);
    return err;
}

void mcp_server_config_free(mcp_server_config_t *config)
{
    if (!config) {
        return;
    }
    free(config->profiles);
    config->profiles = NULL;
    config->count = 0;
    config->capacity = 0;
}

const mcp_server_profile_t *mcp_server_config_find(const mcp_server_config_t *config,
                                                    const char *name)
{
    if (!config || !name || !name[0]) {
        return NULL;
    }

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
    if (!config || !profile || !profile->name[0] || !profile->url[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Check for duplicate name */
    for (size_t i = 0; i < config->count; i++) {
        if (strcmp(config->profiles[i].name, profile->name) == 0) {
            /* Replace existing */
            config->profiles[i] = *profile;
            return ESP_OK;
        }
    }

    /* Grow if needed */
    if (config->count >= config->capacity) {
        size_t new_cap = config->capacity * 2;
        mcp_server_profile_t *new_profiles = realloc(config->profiles,
                                                      new_cap * sizeof(mcp_server_profile_t));
        if (!new_profiles) {
            return ESP_ERR_NO_MEM;
        }
        config->profiles = new_profiles;
        config->capacity = new_cap;
    }

    config->profiles[config->count] = *profile;
    config->count++;
    return ESP_OK;
}

esp_err_t mcp_server_config_remove(mcp_server_config_t *config, const char *name)
{
    if (!config || !name || !name[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < config->count; i++) {
        if (strcmp(config->profiles[i].name, name) == 0) {
            /* Shift remaining entries */
            memmove(&config->profiles[i], &config->profiles[i + 1],
                    (config->count - i - 1) * sizeof(mcp_server_profile_t));
            config->count--;
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t mcp_server_config_validate(const mcp_server_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < config->count; i++) {
        const mcp_server_profile_t *p = &config->profiles[i];

        if (!p->name[0]) {
            ESP_LOGE(TAG, "Server %u: name is empty", (unsigned)i);
            return ESP_ERR_INVALID_ARG;
        }
        if (!p->url[0]) {
            ESP_LOGE(TAG, "Server '%s': url is empty", p->name);
            return ESP_ERR_INVALID_ARG;
        }
        if (strncmp(p->url, "http://", 7) != 0 && strncmp(p->url, "https://", 8) != 0) {
            ESP_LOGE(TAG, "Server '%s': url must start with http:// or https://", p->name);
            return ESP_ERR_INVALID_ARG;
        }
    }
    return ESP_OK;
}

esp_err_t mcp_server_config_list_names(const mcp_server_config_t *config,
                                        char *names_csv, size_t csv_size)
{
    if (!config || !names_csv || csv_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    names_csv[0] = '\0';
    size_t offset = 0;

    for (size_t i = 0; i < config->count; i++) {
        size_t name_len = strlen(config->profiles[i].name);
        size_t need = (i > 0 ? 2 : 0) + name_len + 1; /* ", " + name + '\0' */

        if (offset + need > csv_size) {
            break;
        }
        if (i > 0) {
            names_csv[offset++] = ',';
            names_csv[offset++] = ' ';
        }
        memcpy(names_csv + offset, config->profiles[i].name, name_len);
        offset += name_len;
    }
    names_csv[offset] = '\0';
    return ESP_OK;
}

esp_err_t mcp_server_config_list_names_json(const mcp_server_config_t *config,
                                             char *names_json, size_t json_size)
{
    if (!config || !names_json || json_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    names_json[0] = '\0';
    size_t offset = 0;

    for (size_t i = 0; i < config->count; i++) {
        size_t name_len = strlen(config->profiles[i].name);
        /* "\"name\"," = 1 + name_len + 2 + 1 = name_len + 4 */
        size_t need = (i > 0 ? 1 : 0) + 1 + name_len + 2 + 1;

        if (offset + need > json_size) {
            break;
        }
        if (i > 0) {
            names_json[offset++] = ',';
        }
        names_json[offset++] = '"';
        memcpy(names_json + offset, config->profiles[i].name, name_len);
        offset += name_len;
        names_json[offset++] = '"';
        names_json[offset++] = '\0';  /* temporary null for strlen compat */
        offset--;  /* back up so next write overwrites the temp null */
    }
    names_json[offset] = '\0';
    return ESP_OK;
}
