/*
 * FOC Motor Control - Hardware Layer Header
 * MCPWM 3-phase PWM generation + ADC current sampling
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
 * FOC hardware configuration.
 */
typedef struct {
    /* MCPWM 3-phase PWM output GPIOs */
    int pwm_u_gpio;         /* U phase PWM output */
    int pwm_v_gpio;         /* V phase PWM output */
    int pwm_w_gpio;         /* W phase PWM output */

    /* ADC current sampling channels (ADC1 only to avoid WiFi interference) */
    int current_u_adc_ch;   /* U phase current ADC channel (ADC1 channel number) */
    int current_v_adc_ch;   /* V phase current ADC channel (ADC1 channel number) */

    /* PWM parameters */
    uint32_t pwm_freq_hz;   /* PWM frequency (default 20000 = 20kHz) */
    uint32_t pwm_resolution_hz; /* MCPWM timer resolution (default 10MHz) */

    /* ADC parameters */
    float adc_vref_mv;      /* ADC reference voltage (default 3300.0) */
    float current_offset_mv;/* INA240 zero-current output (AVDD/2, default 1650.0) */
    float shunt_resistance; /* Current sense resistor (default 0.001 ohm) */
    float ina240_gain;      /* INA240 gain (default 20.0) */

    /* Control loop */
    uint32_t isr_stack_size;/* FOC ISR task stack size (default 4096) */
    int isr_core_id;        /* Core to pin FOC task (default 1) */
} foc_hw_config_t;

/**
 * FOC hardware handle (opaque).
 */
typedef struct foc_hw_t *foc_hw_handle_t;

/**
 * Default hardware configuration for YZY board.
 * PWM: GPIO21/47/48, ADC: CH2(GPIO3)/CH7(GPIO8)
 */
#define FOC_HW_DEFAULT_CONFIG() { \
    .pwm_u_gpio = 21, \
    .pwm_v_gpio = 47, \
    .pwm_w_gpio = 48, \
    .current_u_adc_ch = 2,  /* ADC1_CH2 = GPIO3 */ \
    .current_v_adc_ch = 7,  /* ADC1_CH7 = GPIO8 */ \
    .pwm_freq_hz = 20000, \
    .pwm_resolution_hz = 10000000, \
    .adc_vref_mv = 3300.0f, \
    .current_offset_mv = 1650.0f, \
    .shunt_resistance = 0.001f, \
    .ina240_gain = 20.0f, \
    .isr_stack_size = 4096, \
    .isr_core_id = 1, \
}

/* Lifecycle */
esp_err_t foc_hw_create(foc_hw_handle_t *ret_hw, const foc_hw_config_t *config);
esp_err_t foc_hw_destroy(foc_hw_handle_t hw);

/* PWM control: set 3-phase duty (0.0 ~ 1.0) */
esp_err_t foc_hw_set_duty(foc_hw_handle_t hw, float duty_u, float duty_v, float duty_w);

/* ADC sampling: read U/V phase current in mA (blocking, ~10us) */
esp_err_t foc_hw_read_currents(foc_hw_handle_t hw, float *i_u_ma, float *i_v_ma);

/* Enable/disable PWM output */
esp_err_t foc_hw_enable(foc_hw_handle_t hw);
esp_err_t foc_hw_disable(foc_hw_handle_t hw);

#ifdef __cplusplus
}
#endif
