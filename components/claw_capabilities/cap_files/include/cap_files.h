/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t cap_files_register_group(void);
esp_err_t cap_files_set_base_dir(const char *base_dir);

/**
 * @brief Add an allowed root path for file operations sandbox
 * @param root_path Absolute path to add as allowed root (e.g., "/sdcard")
 * @return ESP_OK on success
 */
esp_err_t cap_files_add_allowed_root(const char *root_path);

#ifdef __cplusplus
}
#endif
