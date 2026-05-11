/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "http_server_priv.h"
#include <dirent.h>
#include "esp_vfs_fat.h"

#define SDCARD_MOUNT_POINT  "/sdcard"

static void add_sdcard_status(cJSON *root)
{
    DIR *dir = opendir(SDCARD_MOUNT_POINT);
    bool mounted = (dir != NULL);
    if (dir) {
        closedir(dir);
    }

    cJSON_AddBoolToObject(root, "sdcard_mounted", mounted);

    if (mounted) {
        uint64_t total_bytes = 0;
        uint64_t free_bytes = 0;
        esp_err_t err = esp_vfs_fat_info(SDCARD_MOUNT_POINT, &total_bytes, &free_bytes);
        if (err == ESP_OK) {
            cJSON_AddNumberToObject(root, "sdcard_total_bytes", (double)total_bytes);
            cJSON_AddNumberToObject(root, "sdcard_free_bytes", (double)free_bytes);
        } else {
            cJSON_AddNumberToObject(root, "sdcard_total_bytes", 0);
            cJSON_AddNumberToObject(root, "sdcard_free_bytes", 0);
        }
        http_server_json_add_string(root, "sdcard_mount_point", SDCARD_MOUNT_POINT);
        http_server_json_add_string(root, "sdcard_error", "");
    } else {
        cJSON_AddNumberToObject(root, "sdcard_total_bytes", 0);
        cJSON_AddNumberToObject(root, "sdcard_free_bytes", 0);
        http_server_json_add_string(root, "sdcard_mount_point", "");
        http_server_json_add_string(root, "sdcard_error", "Not mounted");
    }
}

static esp_err_t status_handler(httpd_req_t *req)
{
    http_server_ctx_t *ctx = http_server_ctx();
    http_server_wifi_status_t status = {0};
    esp_err_t err = ctx->services.get_wifi_status(&status);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read Wi-Fi status");
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddBoolToObject(root, "wifi_connected", status.wifi_connected);
    http_server_json_add_string(root, "ip", status.ip);
    http_server_json_add_string(root, "storage_base_path", ctx->storage_base_path);
    cJSON_AddBoolToObject(root, "ap_active", status.ap_active);
    http_server_json_add_string(root, "ap_ssid", status.ap_ssid);
    http_server_json_add_string(root, "ap_ip", status.ap_ip);
    http_server_json_add_string(root, "wifi_mode", status.wifi_mode);

    add_sdcard_status(root);

    return http_server_send_json_response(req, root);
}

static esp_err_t restart_handler(httpd_req_t *req)
{
    http_server_ctx_t *ctx = http_server_ctx();
    esp_err_t err = ctx->services.restart_device ? ctx->services.restart_device() : ESP_ERR_NOT_SUPPORTED;
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to restart device");
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(root, "ok", true);
    http_server_json_add_string(root, "message", "device restart scheduled");
    return http_server_send_json_response(req, root);
}

esp_err_t http_server_register_status_routes(httpd_handle_t server)
{
    const httpd_uri_t handlers[] = {
        { .uri = "/api/status", .method = HTTP_GET, .handler = status_handler },
        { .uri = "/api/restart", .method = HTTP_POST, .handler = restart_handler },
    };

    for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); i++) {
        esp_err_t err = httpd_register_uri_handler(server, &handlers[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}
