/*
 * FOC Motor Control - Hardware Layer Implementation
 * MCPWM 3-phase PWM + ADC1 current sampling
 * SPDX-License-Identifier: Apache-2.0
 */
#include "foc_hw.h"

#include <string.h>
#include <math.h>

#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/mcpwm_timer.h"
#include "driver/mcpwm_oper.h"
#include "driver/mcpwm_cmpr.h"
#include "driver/mcpwm_gen.h"
#include "driver/i2c_master.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

static const char *TAG = "foc_hw";

#define FOC_MCPWM_GROUP_ID    0
#define FOC_ADC_UNIT          ADC_UNIT_1

/* AS5600 register addresses */
#define AS5600_REG_RAW_ANGLE_H  0x0C
#define AS5600_REG_RAW_ANGLE_L  0x0D
#define AS5600_REG_STATUS       0x0B
#define AS5600_REG_AGC          0x1A
#define AS5600_REG_MAGNITUDE_H  0x1B
#define AS5600_REG_MAGNITUDE_L  0x1C

/* Radians per raw count: 2π / 4096 */
#define AS5600_RAW_TO_RAD  (6.2831853f / 4096.0f)

struct foc_hw_t {
    /* MCPWM resources: each phase uses its own operator */
    mcpwm_timer_handle_t timer;
    mcpwm_oper_handle_t  oper[3];   /* one operator per phase (S3 max 2 cmpr/oper) */
    mcpwm_cmpr_handle_t  cmpr[3];
    mcpwm_gen_handle_t   gen[3];
    uint32_t             period_ticks;

    /* ADC resources */
    adc_oneshot_unit_handle_t adc_unit;
    adc_cali_handle_t         adc_cali;
    int adc_ch_u;
    int adc_ch_v;

    /* Conversion constants */
    float adc_to_ma_scale;   /* ADC raw → current (mA) scale factor */

    /* Auto-calibrated zero-current offsets (mA, per channel) */
    float offset_u_ma;
    float offset_v_ma;

    /* IIR low-pass filter state for current readings */
    float filt_iu_ma;
    float filt_iv_ma;
    float filt_alpha;        /* IIR coefficient (0~1, higher = smoother) */

    /* Encoder (AS5600 I2C) */
    i2c_master_bus_handle_t i2c_bus;
    i2c_master_dev_handle_t encoder_dev;
    float encoder_offset_rad;  /* Zero-position offset for calibration */

    /* Config snapshot */
    foc_hw_config_t config;
    bool enabled;
};

static esp_err_t init_mcpwm(foc_hw_handle_t hw, const foc_hw_config_t *cfg)
{
    esp_err_t ret;

    /* Timer */
    mcpwm_timer_config_t timer_cfg = {
        .group_id = FOC_MCPWM_GROUP_ID,
        .clk_src = MCPWM_TIMER_CLK_SRC_DEFAULT,
        .resolution_hz = cfg->pwm_resolution_hz,
        .count_mode = MCPWM_TIMER_COUNT_MODE_UP,
        .period_ticks = cfg->pwm_resolution_hz / cfg->pwm_freq_hz,
    };
    hw->period_ticks = timer_cfg.period_ticks;
    ESP_RETURN_ON_ERROR(mcpwm_new_timer(&timer_cfg, &hw->timer), TAG, "new timer");

    const int pwm_gpios[3] = {cfg->pwm_u_gpio, cfg->pwm_v_gpio, cfg->pwm_w_gpio};

    /* Each phase gets its own operator (ESP32-S3: max 2 comparators per operator) */
    for (int i = 0; i < 3; i++) {
        mcpwm_operator_config_t oper_cfg = {
            .group_id = FOC_MCPWM_GROUP_ID,
        };
        ESP_GOTO_ON_ERROR(mcpwm_new_operator(&oper_cfg, &hw->oper[i]),
                          fail_partial, TAG, "new operator %d", i);
        ESP_GOTO_ON_ERROR(mcpwm_operator_connect_timer(hw->oper[i], hw->timer),
                          fail_partial, TAG, "connect timer %d", i);

        /* Comparator */
        mcpwm_comparator_config_t cmpr_cfg = {
            .flags.update_cmp_on_tez = true,
        };
        ESP_GOTO_ON_ERROR(mcpwm_new_comparator(hw->oper[i], &cmpr_cfg, &hw->cmpr[i]),
                          fail_partial, TAG, "new comparator %d", i);

        /* Generator */
        mcpwm_generator_config_t gen_cfg = {
            .gen_gpio_num = pwm_gpios[i],
        };
        ESP_GOTO_ON_ERROR(mcpwm_new_generator(hw->oper[i], &gen_cfg, &hw->gen[i]),
                          fail_partial, TAG, "new generator %d", i);

        /* Standard PWM: UP count, timer empty → HIGH, compare → LOW */
        ESP_GOTO_ON_ERROR(
            mcpwm_generator_set_actions_on_timer_event(hw->gen[i],
                MCPWM_GEN_TIMER_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, MCPWM_TIMER_EVENT_EMPTY, MCPWM_GEN_ACTION_HIGH),
                MCPWM_GEN_TIMER_EVENT_ACTION_END()),
            fail_partial, TAG, "set timer actions %d", i);

        ESP_GOTO_ON_ERROR(
            mcpwm_generator_set_actions_on_compare_event(hw->gen[i],
                MCPWM_GEN_COMPARE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, hw->cmpr[i], MCPWM_GEN_ACTION_LOW),
                MCPWM_GEN_COMPARE_EVENT_ACTION_END()),
            fail_partial, TAG, "set compare actions %d", i);

        /* Initial 50% duty */
        ESP_GOTO_ON_ERROR(mcpwm_comparator_set_compare_value(hw->cmpr[i], hw->period_ticks / 2),
                          fail_partial, TAG, "set initial compare %d", i);
    }

    ESP_RETURN_ON_ERROR(mcpwm_timer_enable(hw->timer), TAG, "timer enable");
    hw->enabled = false;

    ESP_LOGI(TAG, "MCPWM init: PWM=%dHz on GPIO %d/%d/%d, period=%lu ticks",
             cfg->pwm_freq_hz, cfg->pwm_u_gpio, cfg->pwm_v_gpio, cfg->pwm_w_gpio,
             (unsigned long)hw->period_ticks);
    return ESP_OK;

fail_partial:
    for (int j = 2; j >= 0; j--) {
        if (hw->gen[j])  { mcpwm_del_generator(hw->gen[j]);   hw->gen[j] = NULL; }
        if (hw->cmpr[j]) { mcpwm_del_comparator(hw->cmpr[j]); hw->cmpr[j] = NULL; }
        if (hw->oper[j]) { mcpwm_del_operator(hw->oper[j]);   hw->oper[j] = NULL; }
    }
    mcpwm_del_timer(hw->timer);
    hw->timer = NULL;
    return ret;
}

static esp_err_t init_adc(foc_hw_handle_t hw, const foc_hw_config_t *cfg)
{
    esp_err_t ret;

    /* ADC oneshot unit */
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = FOC_ADC_UNIT,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&unit_cfg, &hw->adc_unit), TAG, "new adc unit");

    /* Configure both channels */
    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };

    hw->adc_ch_u = cfg->current_u_adc_ch;
    hw->adc_ch_v = cfg->current_v_adc_ch;

    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(hw->adc_unit, hw->adc_ch_u, &chan_cfg),
                        TAG, "config adc ch U");
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(hw->adc_unit, hw->adc_ch_v, &chan_cfg),
                        TAG, "config adc ch V");

    /* Calibration (line fitting for better accuracy) */
#if SOC_ADC_CALIBRATION_V1_SUPPORTED
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = FOC_ADC_UNIT,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    ret = adc_cali_create_scheme_curve_fitting(&cali_cfg, &hw->adc_cali);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ADC calibration not available: %s, using raw values", esp_err_to_name(ret));
        hw->adc_cali = NULL;
    }
#endif

    /* Pre-calculate ADC → current conversion:
     * INA240 output: Vout = offset + I * R_shunt * gain  (all in Volts)
     * INA240 output in mV: Vout_mV = offset_mV + I_A * R * gain * 1000
     * delta_mV = I_A * R * gain * 1000
     * I_mA = I_A * 1000 = delta_mV / (R * gain)
     * Scale = 1 / (R * gain)  → I(mA) = (mV - offset) * scale
     *
     * Example: R=0.001Ω, gain=20 → scale = 1/0.02 = 50 mA/mV
     * 1A current → 20mV delta → 50 * 20 = 1000 mA ✓
     */
    hw->adc_to_ma_scale = 1.0f / (cfg->shunt_resistance * cfg->ina240_gain);

    /* IIR low-pass filter coefficient.
     * At 10kHz sampling: alpha = dt / (Tf + dt)
     * Tf = 0.02s (20ms) → alpha = 0.0001/(0.02+0.0001) ≈ 0.00497
     * Lower alpha = smoother but slower response */
    hw->filt_alpha = 0.005f;
    hw->filt_iu_ma = 0.0f;
    hw->filt_iv_ma = 0.0f;

    ESP_LOGI(TAG, "ADC init: U=ADC1_CH%d, V=ADC1_CH%d, scale=%.1f mA/mV",
             hw->adc_ch_u, hw->adc_ch_v, hw->adc_to_ma_scale);
    return ESP_OK;
}

/* Auto-calibrate zero-current offsets.
 * Called once during init with motor stopped (no PWM current).
 * Samples each ADC channel 2000 times and averages the result.
 * This compensates for ESP32-S3 ADC nonlinearity and per-channel offsets. */
static void foc_hw_calibrate_current_offset(foc_hw_handle_t hw)
{
    const int calibration_rounds = 2000;
    int32_t sum_u = 0, sum_v = 0;
    int raw_u = 0, raw_v = 0;
    int mv_u = 0, mv_v = 0;

    ESP_LOGI(TAG, "Calibrating current sensor offsets (%d samples)...", calibration_rounds);

    for (int i = 0; i < calibration_rounds; i++) {
        adc_oneshot_read(hw->adc_unit, hw->adc_ch_u, &raw_u);
        adc_oneshot_read(hw->adc_unit, hw->adc_ch_v, &raw_v);

        if (hw->adc_cali) {
            adc_cali_raw_to_voltage(hw->adc_cali, raw_u, &mv_u);
            adc_cali_raw_to_voltage(hw->adc_cali, raw_v, &mv_v);
        } else {
            mv_u = (int)((float)raw_u * hw->config.adc_vref_mv / 4095.0f);
            mv_v = (int)((float)raw_v * hw->config.adc_vref_mv / 4095.0f);
        }

        /* Convert mV to mA (before offset subtraction) */
        sum_u += mv_u;
        sum_v += mv_v;

        vTaskDelay(pdMS_TO_TICKS(1));
    }

    /* Average mV, then convert to offset mA */
    float avg_mv_u = (float)sum_u / calibration_rounds;
    float avg_mv_v = (float)sum_v / calibration_rounds;
    hw->offset_u_ma = (avg_mv_u - hw->config.current_offset_mv) * hw->adc_to_ma_scale;
    hw->offset_v_ma = (avg_mv_v - hw->config.current_offset_mv) * hw->adc_to_ma_scale;

    ESP_LOGI(TAG, "Current offsets calibrated: U=%.1f mA (%.0f mV), V=%.1f mA (%.0f mV)",
             hw->offset_u_ma, avg_mv_u, hw->offset_v_ma, avg_mv_v);
}

static esp_err_t init_encoder(foc_hw_handle_t hw, const foc_hw_config_t *cfg)
{
    if (cfg->encoder_type == FOC_ENCODER_NONE) {
        ESP_LOGI(TAG, "Encoder: disabled (open-loop only)");
        return ESP_OK;
    }

    if (cfg->encoder_type != FOC_ENCODER_AS5600) {
        ESP_LOGW(TAG, "Unsupported encoder type %d", cfg->encoder_type);
        return ESP_OK;
    }

    /* Get the I2C master bus handle already initialized by board manager */
    i2c_master_bus_handle_t bus = NULL;
    esp_err_t ret = i2c_master_get_bus_handle(0, &bus);
    if (ret != ESP_OK || !bus) {
        ESP_LOGW(TAG, "I2C bus 0 not available (err=%s), encoder disabled",
                 esp_err_to_name(ret));
        hw->encoder_dev = NULL;
        return ESP_OK;  /* Non-fatal: fall back to open-loop */
    }
    hw->i2c_bus = bus;

    /* Add AS5600 device to the bus */
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = cfg->encoder_i2c_addr,
        .scl_speed_hz = cfg->encoder_i2c_freq_hz,
    };
    ret = i2c_master_bus_add_device(bus, &dev_cfg, &hw->encoder_dev);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to add AS5600 at 0x%02X (err=%s), encoder disabled",
                 cfg->encoder_i2c_addr, esp_err_to_name(ret));
        hw->encoder_dev = NULL;
        return ESP_OK;  /* Non-fatal */
    }

    /* Verify communication by reading status register */
    uint8_t status = 0;
    uint8_t reg = AS5600_REG_STATUS;
    ret = i2c_master_transmit_receive(hw->encoder_dev, &reg, 1, &status, 1, 100);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "AS5600 not responding at 0x%02X (err=%s), encoder disabled",
                 cfg->encoder_i2c_addr, esp_err_to_name(ret));
        i2c_master_bus_rm_device(hw->encoder_dev);
        hw->encoder_dev = NULL;
        return ESP_OK;  /* Non-fatal */
    }

    hw->encoder_offset_rad = 0.0f;
    ESP_LOGI(TAG, "Encoder: AS5600 @ I2C 0x%02X, %luHz, status=0x%02X",
             cfg->encoder_i2c_addr, (unsigned long)cfg->encoder_i2c_freq_hz, status);
    return ESP_OK;
}

esp_err_t foc_hw_create(foc_hw_handle_t *ret_hw, const foc_hw_config_t *config)
{
    ESP_RETURN_ON_FALSE(ret_hw && config, ESP_ERR_INVALID_ARG, TAG, "invalid args");
    ESP_RETURN_ON_FALSE(config->pwm_freq_hz > 0 && config->pwm_freq_hz < 100000,
                        ESP_ERR_INVALID_ARG, TAG, "invalid pwm freq");

    foc_hw_handle_t hw = calloc(1, sizeof(struct foc_hw_t));
    ESP_RETURN_ON_FALSE(hw, ESP_ERR_NO_MEM, TAG, "alloc failed");

    hw->config = *config;

    esp_err_t ret = init_mcpwm(hw, config);
    if (ret != ESP_OK) {
        free(hw);
        return ret;
    }

    ret = init_adc(hw, config);
    if (ret != ESP_OK) {
        foc_hw_destroy(hw);
        return ret;
    }

    init_encoder(hw, config);  /* Non-fatal: falls back to open-loop if unavailable */

    /* Auto-calibrate current sensor zero offsets (motor must be stopped) */
    foc_hw_calibrate_current_offset(hw);

    *ret_hw = hw;
    ESP_LOGI(TAG, "FOC hardware layer created");
    return ESP_OK;
}

esp_err_t foc_hw_destroy(foc_hw_handle_t hw)
{
    if (!hw) return ESP_OK;

    foc_hw_disable(hw);

    if (hw->timer) {
        mcpwm_timer_disable(hw->timer);
        mcpwm_del_timer(hw->timer);
    }
    for (int i = 0; i < 3; i++) {
        if (hw->gen[i])  mcpwm_del_generator(hw->gen[i]);
        if (hw->cmpr[i]) mcpwm_del_comparator(hw->cmpr[i]);
        if (hw->oper[i]) mcpwm_del_operator(hw->oper[i]);
    }

    if (hw->adc_cali)  adc_cali_delete_scheme_curve_fitting(hw->adc_cali);
    if (hw->adc_unit)  adc_oneshot_del_unit(hw->adc_unit);

    if (hw->encoder_dev) {
        i2c_master_bus_rm_device(hw->encoder_dev);
    }

    free(hw);
    return ESP_OK;
}

esp_err_t foc_hw_set_duty(foc_hw_handle_t hw, float duty_u, float duty_v, float duty_w)
{
    ESP_RETURN_ON_FALSE(hw, ESP_ERR_INVALID_ARG, TAG, "null hw");

    float duties[3] = {duty_u, duty_v, duty_w};
    for (int i = 0; i < 3; i++) {
        float d = duties[i];
        if (d < 0.0f) d = 0.0f;
        if (d > 1.0f) d = 1.0f;
        uint32_t cmp = (uint32_t)(d * hw->period_ticks);
        mcpwm_comparator_set_compare_value(hw->cmpr[i], cmp);
    }
    return ESP_OK;
}

esp_err_t foc_hw_read_currents(foc_hw_handle_t hw, float *i_u_ma, float *i_v_ma)
{
    ESP_RETURN_ON_FALSE(hw && i_u_ma && i_v_ma, ESP_ERR_INVALID_ARG, TAG, "null args");

    int raw_u = 0, raw_v = 0;
    int mv_u = 0, mv_v = 0;

    adc_oneshot_read(hw->adc_unit, hw->adc_ch_u, &raw_u);
    adc_oneshot_read(hw->adc_unit, hw->adc_ch_v, &raw_v);

    if (hw->adc_cali) {
        adc_cali_raw_to_voltage(hw->adc_cali, raw_u, &mv_u);
        adc_cali_raw_to_voltage(hw->adc_cali, raw_v, &mv_v);
    } else {
        /* Fallback: approximate mV from raw 12-bit value */
        mv_u = (int)((float)raw_u * hw->config.adc_vref_mv / 4095.0f);
        mv_v = (int)((float)raw_v * hw->config.adc_vref_mv / 4095.0f);
    }

    /* Convert mV to current mA, subtract auto-calibrated zero offset:
     * I(mA) = (Vout_mV - offset_mV) * scale - calibrated_offset_ma
     */
    float raw_iu = ((float)mv_u - hw->config.current_offset_mv) * hw->adc_to_ma_scale - hw->offset_u_ma;
    float raw_iv = ((float)mv_v - hw->config.current_offset_mv) * hw->adc_to_ma_scale - hw->offset_v_ma;

    /* IIR low-pass filter: y[n] = y[n-1] + alpha * (x[n] - y[n-1])
     * Reduces ESP32-S3 ADC noise at the cost of slight phase delay */
    hw->filt_iu_ma += hw->filt_alpha * (raw_iu - hw->filt_iu_ma);
    hw->filt_iv_ma += hw->filt_alpha * (raw_iv - hw->filt_iv_ma);

    *i_u_ma = hw->filt_iu_ma;
    *i_v_ma = hw->filt_iv_ma;

    return ESP_OK;
}

esp_err_t foc_hw_enable(foc_hw_handle_t hw)
{
    ESP_RETURN_ON_FALSE(hw, ESP_ERR_INVALID_ARG, TAG, "null hw");
    if (!hw->enabled) {
        ESP_RETURN_ON_ERROR(mcpwm_timer_start_stop(hw->timer, MCPWM_TIMER_START_NO_STOP),
                            TAG, "timer start");
        hw->enabled = true;
        ESP_LOGI(TAG, "PWM enabled");
    }
    return ESP_OK;
}

esp_err_t foc_hw_disable(foc_hw_handle_t hw)
{
    if (!hw) return ESP_OK;
    if (hw->enabled && hw->timer) {
        mcpwm_timer_start_stop(hw->timer, MCPWM_TIMER_STOP_EMPTY);
        hw->enabled = false;
        ESP_LOGI(TAG, "PWM disabled");
    }
    return ESP_OK;
}

bool foc_hw_has_encoder(foc_hw_handle_t hw)
{
    return hw && hw->encoder_dev != NULL;
}

esp_err_t foc_hw_read_encoder(foc_hw_handle_t hw, float *angle_rad)
{
    ESP_RETURN_ON_FALSE(hw && angle_rad, ESP_ERR_INVALID_ARG, TAG, "null args");
    ESP_RETURN_ON_FALSE(hw->encoder_dev, ESP_ERR_INVALID_STATE, TAG, "no encoder");

    /* Read raw angle registers: 0x0C (high) + 0x0D (low) → 12-bit value */
    uint8_t reg = AS5600_REG_RAW_ANGLE_H;
    uint8_t buf[2] = {0};
    esp_err_t ret = i2c_master_transmit_receive(hw->encoder_dev, &reg, 1, buf, 2, 100);
    if (ret != ESP_OK) {
        return ret;
    }

    uint16_t raw = ((uint16_t)(buf[0] & 0x0F) << 8) | buf[1];  /* 12-bit: 0..4095 */
    float mech_angle = (float)raw * AS5600_RAW_TO_RAD;           /* [0, 2π) */

    /* Apply calibration offset */
    mech_angle -= hw->encoder_offset_rad;
    if (mech_angle < 0.0f) {
        mech_angle += 6.2831853f;
    }

    *angle_rad = mech_angle;
    return ESP_OK;
}
