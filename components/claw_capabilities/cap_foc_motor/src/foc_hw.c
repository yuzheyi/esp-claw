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
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

static const char *TAG = "foc_hw";

#define FOC_MCPWM_GROUP_ID    0
#define FOC_ADC_UNIT          ADC_UNIT_1

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

    ESP_LOGI(TAG, "ADC init: U=ADC1_CH%d, V=ADC1_CH%d, scale=%.1f mA/mV",
             hw->adc_ch_u, hw->adc_ch_v, hw->adc_to_ma_scale);
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

    /* Convert mV to current mA:
     * I(mA) = (Vout_mV - offset_mV) * scale
     */
    *i_u_ma = ((float)mv_u - hw->config.current_offset_mv) * hw->adc_to_ma_scale;
    *i_v_ma = ((float)mv_v - hw->config.current_offset_mv) * hw->adc_to_ma_scale;

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
