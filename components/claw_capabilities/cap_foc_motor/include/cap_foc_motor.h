/*
 * FOC Motor Control - Capability API Header
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Register the FOC motor capability group.
 * Must be called during app_capabilities_init.
 */
esp_err_t cap_foc_motor_register_group(void);

#ifdef __cplusplus
}
#endif
