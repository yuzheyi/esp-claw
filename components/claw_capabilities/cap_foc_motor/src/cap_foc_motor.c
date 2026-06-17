/*
 * FOC Motor Control - Capability API Implementation
 * Registers FOC motor control tools into the claw_cap system.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_foc_motor.h"
#include "foc_hw.h"
#include "foc_core.h"

#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "claw_cap.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "cap_foc_motor";

#define FOC_CONTROL_HZ         10000   /* 10kHz control loop */
#define FOC_CONTROL_PERIOD_US  (1000000 / FOC_CONTROL_HZ)  /* 100 us */

/* Global FOC state (single motor instance) */
static foc_hw_handle_t  s_hw = NULL;
static foc_core_t       s_core;
static SemaphoreHandle_t s_mutex;
static TaskHandle_t      s_control_task;
static bool              s_running;
static float             s_openloop_speed; /* rad/s, for open-loop mode */
static bool              s_openloop_mode = true; /* Start in open-loop */

/* ─── Execute callbacks ─────────────────────────────────────────── */

static esp_err_t foc_motor_init_execute(const char *input_json,
                                        const claw_cap_call_context_t *ctx,
                                        char *output, size_t output_size)
{
    (void)ctx;

    if (s_hw) {
        snprintf(output, output_size, "FOC motor already initialized");
        return ESP_OK;
    }

    /* Parse optional config overrides */
    foc_hw_config_t cfg = FOC_HW_DEFAULT_CONFIG();
    uint8_t pole_pairs = 7;

    if (input_json) {
        cJSON *root = cJSON_Parse(input_json);
        if (root) {
            cJSON *pp = cJSON_GetObjectItem(root, "pole_pairs");
            if (cJSON_IsNumber(pp)) pole_pairs = (uint8_t)pp->valueint;

            cJSON *pwm_hz = cJSON_GetObjectItem(root, "pwm_freq_hz");
            if (cJSON_IsNumber(pwm_hz)) cfg.pwm_freq_hz = (uint32_t)pwm_hz->valuedouble;

            cJSON *i_q = cJSON_GetObjectItem(root, "shunt_resistance");
            if (cJSON_IsNumber(i_q)) cfg.shunt_resistance = (float)i_q->valuedouble;

            cJSON_Delete(root);
        }
    }

    esp_err_t err = foc_hw_create(&s_hw, &cfg);
    if (err != ESP_OK) {
        snprintf(output, output_size, "FOC hw init failed: %s", esp_err_to_name(err));
        return err;
    }

    foc_core_init(&s_core, pole_pairs);
    s_mutex = xSemaphoreCreateMutex();

    bool has_enc = foc_hw_has_encoder(s_hw);
    snprintf(output, output_size,
             "FOC motor initialized: PWM=%luHz, GPIO %d/%d/%d, ADC1 CH%d/CH%d, "
             "pole_pairs=%u, encoder=%s",
             (unsigned long)cfg.pwm_freq_hz,
             cfg.pwm_u_gpio, cfg.pwm_v_gpio, cfg.pwm_w_gpio,
             cfg.current_u_adc_ch, cfg.current_v_adc_ch,
             pole_pairs,
             has_enc ? "AS5600" : "none");
    return ESP_OK;
}

static esp_err_t foc_motor_start_execute(const char *input_json,
                                         const claw_cap_call_context_t *ctx,
                                         char *output, size_t output_size)
{
    (void)ctx;

    if (!s_hw) {
        snprintf(output, output_size, "Error: FOC not initialized. Call foc_motor_init first.");
        return ESP_ERR_INVALID_STATE;
    }

    /* Parse optional target Iq */
    if (input_json) {
        cJSON *root = cJSON_Parse(input_json);
        if (root) {
            cJSON *target_iq = cJSON_GetObjectItem(root, "target_iq_ma");
            if (cJSON_IsNumber(target_iq)) {
                s_core.target_iq = (float)target_iq->valuedouble;
            }
            cJSON *target_id = cJSON_GetObjectItem(root, "target_id_ma");
            if (cJSON_IsNumber(target_id)) {
                s_core.target_id = (float)target_id->valuedouble;
            }
            cJSON *speed = cJSON_GetObjectItem(root, "openloop_speed_rad_s");
            if (cJSON_IsNumber(speed)) {
                s_openloop_speed = (float)speed->valuedouble;
                s_openloop_mode = true;
            }
            cJSON *closed_loop = cJSON_GetObjectItem(root, "closed_loop");
            if (cJSON_IsBool(closed_loop)) {
                s_openloop_mode = !cJSON_IsTrue(closed_loop);
            }
            cJSON_Delete(root);
        }
    }

    if (s_running) {
        snprintf(output, output_size, "Motor already running");
        return ESP_OK;
    }

    foc_hw_enable(s_hw);
    foc_core_reset(&s_core);
    s_running = true;

    snprintf(output, output_size, "Motor started (%s, Iq_target=%.0f mA, speed=%.1f rad/s)",
             s_openloop_mode ? "open-loop" : "closed-loop",
             s_core.target_iq, s_openloop_speed);
    return ESP_OK;
}

static esp_err_t foc_motor_stop_execute(const char *input_json,
                                        const claw_cap_call_context_t *ctx,
                                        char *output, size_t output_size)
{
    (void)input_json;
    (void)ctx;

    if (!s_hw) {
        snprintf(output, output_size, "FOC not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    s_running = false;
    foc_hw_disable(s_hw);

    snprintf(output, output_size, "Motor stopped");
    return ESP_OK;
}

static esp_err_t foc_motor_set_target_execute(const char *input_json,
                                              const claw_cap_call_context_t *ctx,
                                              char *output, size_t output_size)
{
    (void)ctx;

    if (!s_hw) {
        snprintf(output, output_size, "FOC not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (input_json) {
        cJSON *root = cJSON_Parse(input_json);
        if (root) {
            cJSON *target_iq = cJSON_GetObjectItem(root, "target_iq_ma");
            if (cJSON_IsNumber(target_iq)) {
                s_core.target_iq = (float)target_iq->valuedouble;
            }
            cJSON *target_id = cJSON_GetObjectItem(root, "target_id_ma");
            if (cJSON_IsNumber(target_id)) {
                s_core.target_id = (float)target_id->valuedouble;
            }
            cJSON *speed = cJSON_GetObjectItem(root, "openloop_speed_rad_s");
            if (cJSON_IsNumber(speed)) {
                s_openloop_speed = (float)speed->valuedouble;
            }
            cJSON_Delete(root);
        }
    }

    snprintf(output, output_size,
             "Targets set: Id=%.0f mA, Iq=%.0f mA, openloop_speed=%.1f rad/s",
             s_core.target_id, s_core.target_iq, s_openloop_speed);
    return ESP_OK;
}

static esp_err_t foc_motor_status_execute(const char *input_json,
                                          const claw_cap_call_context_t *ctx,
                                          char *output, size_t output_size)
{
    (void)input_json;
    (void)ctx;

    if (!s_hw) {
        snprintf(output, output_size, "FOC not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    float i_u = 0, i_v = 0;
    foc_hw_read_currents(s_hw, &i_u, &i_v);

    /* Read encoder angle if available */
    float enc_angle = 0.0f;
    bool has_enc = foc_hw_has_encoder(s_hw);
    if (has_enc) {
        foc_hw_read_encoder(s_hw, &enc_angle);
    }

    snprintf(output, output_size,
             "running=%d, mode=%s, theta=%.2f rad, Iu=%.1f mA, Iv=%.1f mA, "
             "target_Id=%.0f, target_Iq=%.0f, speed=%.1f, encoder=%s, enc_angle=%.3f",
             s_running,
             s_openloop_mode ? "open-loop" : "closed-loop",
             s_core.theta, i_u, i_v,
             s_core.target_id, s_core.target_iq,
             s_openloop_speed,
             has_enc ? "yes" : "no",
             enc_angle);
    return ESP_OK;
}

static esp_err_t foc_motor_align_execute(const char *input_json,
                                         const claw_cap_call_context_t *ctx,
                                         char *output, size_t output_size)
{
    (void)ctx;

    if (!s_hw) {
        snprintf(output, output_size, "FOC not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (!foc_hw_has_encoder(s_hw)) {
        snprintf(output, output_size, "No encoder available for alignment");
        return ESP_ERR_INVALID_STATE;
    }

    /* Parse optional alignment parameters */
    float align_current_ma = 500.0f;   /* Default alignment current */
    int align_time_ms = 500;           /* Default hold time */

    if (input_json) {
        cJSON *root = cJSON_Parse(input_json);
        if (root) {
            cJSON *cur = cJSON_GetObjectItem(root, "align_current_ma");
            if (cJSON_IsNumber(cur)) align_current_ma = (float)cur->valuedouble;
            cJSON *t = cJSON_GetObjectItem(root, "align_time_ms");
            if (cJSON_IsNumber(t)) align_time_ms = t->valueint;
            cJSON_Delete(root);
        }
    }

    /* Step 1: Apply alignment current at theta=0 (d-axis) to lock rotor */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_core.theta = 0.0f;
    s_core.target_id = align_current_ma;
    s_core.target_iq = 0.0f;
    s_openloop_mode = false;
    s_running = true;
    foc_hw_enable(s_hw);
    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "Alignment: holding Id=%.0f mA at theta=0 for %d ms",
             align_current_ma, align_time_ms);
    vTaskDelay(pdMS_TO_TICKS(align_time_ms));

    /* Step 2: Read encoder angle at aligned position */
    float enc_angle = 0.0f;
    esp_err_t ret = foc_hw_read_encoder(s_hw, &enc_angle);
    if (ret != ESP_OK) {
        s_running = false;
        foc_hw_disable(s_hw);
        snprintf(output, output_size, "Encoder read failed during alignment: %s",
                 esp_err_to_name(ret));
        return ret;
    }

    /* Step 3: Stop alignment current */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_running = false;
    s_core.target_id = 0.0f;
    s_openloop_mode = true;
    xSemaphoreGive(s_mutex);
    foc_hw_disable(s_hw);

    /* The encoder angle at alignment position becomes our zero reference */
    snprintf(output, output_size,
             "Alignment complete: enc_angle=%.3f rad (%.1f deg). "
             "Rotor locked at theta=0. Ready for closed-loop.",
             enc_angle, enc_angle * 180.0f / 3.14159265f);
    return ESP_OK;
}

/* ─── Control loop task ─────────────────────────────────────────── */

static void foc_control_task(void *arg)
{
    (void)arg;

    const float dt = 1.0f / FOC_CONTROL_HZ;
    int64_t next_wakeup = esp_timer_get_time();

    while (1) {
        next_wakeup += FOC_CONTROL_PERIOD_US;

        if (s_running && s_hw) {
            xSemaphoreTake(s_mutex, portMAX_DELAY);

            if (s_openloop_mode) {
                /* Open-loop: advance angle, apply fixed voltage */
                foc_core_openloop_step(&s_core, s_openloop_speed, dt);

                /* Generate SVPWM from open-loop angle with moderate voltage */
                float valpha = cosf(s_core.theta) * 0.4f;
                float vbeta  = sinf(s_core.theta) * 0.4f;
                float du, dv, dw;
                foc_svpwm(valpha, vbeta, 1.0f, &du, &dv, &dw);
                foc_hw_set_duty(s_hw, du, dv, dw);
            } else {
                /* Closed-loop FOC with encoder */
                float i_u, i_v;
                foc_hw_read_currents(s_hw, &i_u, &i_v);

                /* Read encoder angle (mechanical) and convert to electrical */
                float mech_angle = 0.0f;
                if (foc_hw_read_encoder(s_hw, &mech_angle) == ESP_OK) {
                    /* Electrical angle = mechanical angle × pole_pairs */
                    s_core.theta = mech_angle * (float)s_core.pole_pairs;
                    /* Wrap to [0, 2π) */
                    while (s_core.theta >= 6.2831853f) s_core.theta -= 6.2831853f;
                    while (s_core.theta < 0.0f)        s_core.theta += 6.2831853f;
                }
                /* If encoder read fails, keep last theta (graceful degradation) */

                float du, dv, dw;
                foc_core_step(&s_core, i_u, i_v, dt, &du, &dv, &dw);
                foc_hw_set_duty(s_hw, du, dv, dw);
            }

            xSemaphoreGive(s_mutex);
        }

        /* Precise timing: spin until next wakeup time */
        int64_t now = esp_timer_get_time();
        if (next_wakeup > now) {
            int64_t remaining = next_wakeup - now;
            if (remaining > 100) {
                /* Use FreeRTOS yield for >100us remaining to avoid busy spin */
                vTaskDelay(1);
                /* Fine-tune the last bit */
                while (esp_timer_get_time() < next_wakeup) {
                    /* tight spin */
                }
            }
        }
        /* If we're behind schedule, reset to current time to avoid runaway */
        if (esp_timer_get_time() - next_wakeup > FOC_CONTROL_PERIOD_US) {
            next_wakeup = esp_timer_get_time();
        }
    }
}

/* ─── Capability registration ──────────────────────────────────── */

static const claw_cap_descriptor_t s_foc_motor_descriptors[] = {
    {
        .id = "foc_motor_init",
        .name = "foc_motor_init",
        .family = "motor",
        .description = "Initialize FOC motor hardware (MCPWM + ADC).",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{"
        "\"pole_pairs\":{\"type\":\"integer\",\"default\":7},"
        "\"pwm_freq_hz\":{\"type\":\"integer\",\"default\":20000},"
        "\"shunt_resistance\":{\"type\":\"number\",\"default\":0.001}"
        "}}",
        .execute = foc_motor_init_execute,
    },
    {
        .id = "foc_motor_start",
        .name = "foc_motor_start",
        .family = "motor",
        .description = "Start the motor with given targets (open-loop or closed-loop).",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{"
        "\"target_iq_ma\":{\"type\":\"number\",\"description\":\"Target q-axis current in mA (torque)\"},"
        "\"target_id_ma\":{\"type\":\"number\",\"default\":0,\"description\":\"Target d-axis current in mA\"},"
        "\"openloop_speed_rad_s\":{\"type\":\"number\",\"default\":10,\"description\":\"Open-loop electrical speed\"},"
        "\"closed_loop\":{\"type\":\"boolean\",\"default\":false,\"description\":\"Enable closed-loop current control\"}"
        "}}",
        .execute = foc_motor_start_execute,
    },
    {
        .id = "foc_motor_stop",
        .name = "foc_motor_stop",
        .family = "motor",
        .description = "Stop the motor (disable PWM output).",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = foc_motor_stop_execute,
    },
    {
        .id = "foc_motor_set_target",
        .name = "foc_motor_set_target",
        .family = "motor",
        .description = "Update motor targets while running (Iq torque, Id flux, speed).",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{"
        "\"target_iq_ma\":{\"type\":\"number\"},"
        "\"target_id_ma\":{\"type\":\"number\"},"
        "\"openloop_speed_rad_s\":{\"type\":\"number\"}"
        "}}",
        .execute = foc_motor_set_target_execute,
    },
    {
        .id = "foc_motor_status",
        .name = "foc_motor_status",
        .family = "motor",
        .description = "Read current motor state (running, currents, angle, targets, encoder).",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = foc_motor_status_execute,
    },
    {
        .id = "foc_motor_align",
        .name = "foc_motor_align",
        .family = "motor",
        .description = "Align rotor to theta=0 using d-axis current, then read encoder for calibration.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{"
        "\"align_current_ma\":{\"type\":\"number\",\"default\":500,\"description\":\"Alignment current in mA\"},"
        "\"align_time_ms\":{\"type\":\"integer\",\"default\":500,\"description\":\"Hold time in ms\"}"
        "}}",
        .execute = foc_motor_align_execute,
    },
};

static const claw_cap_group_t s_foc_motor_group = {
    .group_id = "cap_foc_motor",
    .descriptors = s_foc_motor_descriptors,
    .descriptor_count = sizeof(s_foc_motor_descriptors) / sizeof(s_foc_motor_descriptors[0]),
};

esp_err_t cap_foc_motor_register_group(void)
{
    if (claw_cap_group_exists(s_foc_motor_group.group_id)) {
        return ESP_OK;
    }

    /* Create control loop task (starts idle, waits for motor start) */
    if (!s_control_task) {
        xTaskCreatePinnedToCore(foc_control_task, "foc_ctrl", 4096, NULL, 10, &s_control_task, 1);
    }

    return claw_cap_register_group(&s_foc_motor_group);
}
