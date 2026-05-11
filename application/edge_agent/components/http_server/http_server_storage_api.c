/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "http_server_priv.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"

/* Board manager headers for SD card mount/unmount */
#include "esp_board_manager.h"

static const char *TAG = "storage_api";

/* -----------------------------------------------------------------------
 * Storage device info
 * ----------------------------------------------------------------------- */
typedef struct {
    const char *id;           /* "fatfs" or "sdcard" */
    const char *name;         /* Display name */
    const char *mount_point;  /* /fatfs or /sdcard */
    const char *type;         /* "spiflash" or "sdmmc" */
    const char *bmgr_name;    /* board_manager device name, NULL for fatfs */
} storage_device_desc_t;

static const storage_device_desc_t s_devices[] = {
    {
        .id = "fatfs",
        .name = "SPI Flash",
        .mount_point = "/fatfs",
        .type = "spiflash",
        .bmgr_name = NULL,
    },
    {
        .id = "sdcard",
        .name = "SD Card",
        .mount_point = "/sdcard",
        .type = "sdmmc",
        .bmgr_name = "fs_sdcard",
    },
};
#define STORAGE_DEVICE_COUNT (sizeof(s_devices) / sizeof(s_devices[0]))

/* -----------------------------------------------------------------------
 * Helpers
 * ----------------------------------------------------------------------- */
static bool check_mounted(const char *mount_point)
{
    DIR *dir = opendir(mount_point);
    if (dir) {
        closedir(dir);
        return true;
    }
    return false;
}

static void get_storage_usage(const char *mount_point, uint64_t *total, uint64_t *free_)
{
    uint64_t total_bytes = 0;
    uint64_t free_bytes = 0;
    esp_err_t err = esp_vfs_fat_info(mount_point, &total_bytes, &free_bytes);
    if (err == ESP_OK) {
        *total = total_bytes;
        *free_ = free_bytes;
    } else {
        *total = 0;
        *free_ = 0;
    }
}

static const storage_device_desc_t *find_device_by_id(const char *id)
{
    for (size_t i = 0; i < STORAGE_DEVICE_COUNT; i++) {
        if (strcmp(s_devices[i].id, id) == 0) {
            return &s_devices[i];
        }
    }
    return NULL;
}

/* -----------------------------------------------------------------------
 * GET /api/storage — list storage devices
 * ----------------------------------------------------------------------- */
static esp_err_t storage_list_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    if (!root || !arr) {
        cJSON_Delete(root);
        cJSON_Delete(arr);
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddItemToObject(root, "devices", arr);

    for (size_t i = 0; i < STORAGE_DEVICE_COUNT; i++) {
        const storage_device_desc_t *dev = &s_devices[i];
        bool mounted = check_mounted(dev->mount_point);
        uint64_t total = 0, free_ = 0;
        if (mounted) {
            get_storage_usage(dev->mount_point, &total, &free_);
        }

        cJSON *item = cJSON_CreateObject();
        if (!item) {
            continue;
        }
        http_server_json_add_string(item, "id", dev->id);
        http_server_json_add_string(item, "name", dev->name);
        http_server_json_add_string(item, "mount_point", dev->mount_point);
        http_server_json_add_string(item, "type", dev->type);
        cJSON_AddBoolToObject(item, "mounted", mounted);
        cJSON_AddNumberToObject(item, "total_bytes", (double)total);
        cJSON_AddNumberToObject(item, "free_bytes", (double)free_);
        cJSON_AddItemToArray(arr, item);
    }

    return http_server_send_json_response(req, root);
}

/* -----------------------------------------------------------------------
 * POST /api/storage/mount — mount a storage device
 * Body: { "device_id": "sdcard" }
 * ----------------------------------------------------------------------- */
static esp_err_t storage_mount_handler(httpd_req_t *req)
{
    cJSON *root = NULL;
    if (http_server_parse_json_body(req, &root) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON body");
    }

    cJSON *id_item = cJSON_GetObjectItemCaseSensitive(root, "device_id");
    if (!cJSON_IsString(id_item) || !id_item->valuestring[0]) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing device_id");
    }

    const storage_device_desc_t *dev = find_device_by_id(id_item->valuestring);
    if (!dev) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unknown device");
    }
    cJSON_Delete(root);

    /* fatfs is always mounted — cannot be remounted via API */
    if (!dev->bmgr_name) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Device cannot be managed via API");
    }

    /* Already mounted? */
    if (check_mounted(dev->mount_point)) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");
        return httpd_resp_sendstr(req, "{\"ok\":true,\"message\":\"already mounted\"}");
    }

    /* Mount via board manager */
    esp_err_t err = esp_board_manager_init_device_by_name(dev->bmgr_name);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount %s: %s", dev->id, esp_err_to_name(err));
        char resp[128];
        snprintf(resp, sizeof(resp), "{\"ok\":false,\"error\":\"%s\"}", esp_err_to_name(err));
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, resp);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/* -----------------------------------------------------------------------
 * POST /api/storage/unmount — unmount a storage device
 * Body: { "device_id": "sdcard" }
 * ----------------------------------------------------------------------- */
static esp_err_t storage_unmount_handler(httpd_req_t *req)
{
    cJSON *root = NULL;
    if (http_server_parse_json_body(req, &root) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON body");
    }

    cJSON *id_item = cJSON_GetObjectItemCaseSensitive(root, "device_id");
    if (!cJSON_IsString(id_item) || !id_item->valuestring[0]) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing device_id");
    }

    const storage_device_desc_t *dev = find_device_by_id(id_item->valuestring);
    if (!dev) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unknown device");
    }
    cJSON_Delete(root);

    if (!dev->bmgr_name) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Device cannot be managed via API");
    }

    if (!check_mounted(dev->mount_point)) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");
        return httpd_resp_sendstr(req, "{\"ok\":true,\"message\":\"already unmounted\"}");
    }

    esp_err_t err = esp_board_manager_deinit_device_by_name(dev->bmgr_name);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to unmount %s: %s", dev->id, esp_err_to_name(err));
        char resp[128];
        snprintf(resp, sizeof(resp), "{\"ok\":false,\"error\":\"%s\"}", esp_err_to_name(err));
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, resp);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/* -----------------------------------------------------------------------
 * Recursively delete all contents under a directory (not the dir itself)
 * Returns 0 on success, -1 on error
 * ----------------------------------------------------------------------- */
static int recursive_delete_contents(const char *path, int depth)
{
    if (depth > 8) {
        return 0; /* Safety: limit recursion depth */
    }
    DIR *dir = opendir(path);
    if (!dir) {
        return -1;
    }
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        size_t plen = strlen(path);
        size_t nlen = strlen(entry->d_name);
        if (plen + 1 + nlen + 1 > HTTP_SERVER_PATH_MAX) {
            continue; /* Skip entries with path too long */
        }
        char child[HTTP_SERVER_PATH_MAX + 64];
        snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
        struct stat st;
        if (stat(child, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                recursive_delete_contents(child, depth + 1);
                rmdir(child);
            } else {
                unlink(child);
            }
        }
    }
    closedir(dir);
    return 0;
}

/* -----------------------------------------------------------------------
 * POST /api/storage/format — format SD card (clear all files)
 * Body: { "device_id": "sdcard" }
 * 
 * Flow: unmount → mount → clear all files recursively
 * ----------------------------------------------------------------------- */
static esp_err_t storage_format_handler(httpd_req_t *req)
{
    cJSON *root = NULL;
    if (http_server_parse_json_body(req, &root) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON body");
    }

    cJSON *id_item = cJSON_GetObjectItemCaseSensitive(root, "device_id");
    if (!cJSON_IsString(id_item) || !id_item->valuestring[0]) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing device_id");
    }

    const storage_device_desc_t *dev = find_device_by_id(id_item->valuestring);
    if (!dev) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unknown device");
    }
    cJSON_Delete(root);

    if (!dev->bmgr_name) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Device cannot be formatted via API");
    }

    /* Step 1: Unmount if currently mounted */
    if (check_mounted(dev->mount_point)) {
        ESP_LOGI(TAG, "Unmounting %s before format...", dev->id);
        esp_err_t err = esp_board_manager_deinit_device_by_name(dev->bmgr_name);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to unmount before format: %s", esp_err_to_name(err));
            char resp[128];
            snprintf(resp, sizeof(resp), "{\"ok\":false,\"error\":\"unmount failed: %s\"}", esp_err_to_name(err));
            httpd_resp_set_type(req, "application/json");
            httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");
            httpd_resp_set_status(req, "500 Internal Server Error");
            return httpd_resp_sendstr(req, resp);
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    /* Step 2: Re-mount the card */
    ESP_LOGI(TAG, "Formatting %s (%s)...", dev->id, dev->mount_point);
    esp_err_t mount_err = esp_board_manager_init_device_by_name(dev->bmgr_name);
    if (mount_err != ESP_OK || !check_mounted(dev->mount_point)) {
        ESP_LOGE(TAG, "Cannot mount card for formatting: %s", esp_err_to_name(mount_err));
        char resp[256];
        snprintf(resp, sizeof(resp),
                 "{\"ok\":false,\"error\":\"Cannot mount card. Please format as FAT32 on PC first.\"}");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, resp);
    }

    /* Step 3: Recursively delete all contents */
    ESP_LOGI(TAG, "Card mounted. Clearing all files recursively...");
    recursive_delete_contents(dev->mount_point, 0);

    ESP_LOGI(TAG, "SD card format (clear) complete.");
    /* Keep card mounted after format */
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/* -----------------------------------------------------------------------
 * Register routes
 * ----------------------------------------------------------------------- */
esp_err_t http_server_register_storage_routes(httpd_handle_t server)
{
    const httpd_uri_t handlers[] = {
        { .uri = "/api/storage",        .method = HTTP_GET,  .handler = storage_list_handler },
        { .uri = "/api/storage/mount",   .method = HTTP_POST, .handler = storage_mount_handler },
        { .uri = "/api/storage/unmount", .method = HTTP_POST, .handler = storage_unmount_handler },
        { .uri = "/api/storage/format",  .method = HTTP_POST, .handler = storage_format_handler },
    };

    for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); ++i) {
        esp_err_t err = httpd_register_uri_handler(server, &handlers[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}
