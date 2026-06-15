/*
 * FOC Motor Control - Algorithm Layer Header
 * Clark/Park transforms, PI controllers, SVPWM modulation
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <math.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * PI controller state.
 */
typedef struct {
    float kp;           /* Proportional gain */
    float ki;           /* Integral gain */
    float integral;     /* Accumulated integral */
    float out_min;      /* Output lower limit */
    float out_max;      /* Output upper limit */
} foc_pi_t;

/**
 * FOC algorithm state.
 */
typedef struct {
    /* PI controllers for Id (flux) and Iq (torque) */
    foc_pi_t pi_id;
    foc_pi_t pi_iq;

    /* Electrical angle (radians), updated by sensor or open-loop ramp */
    float theta;

    /* Target values */
    float target_id;    /* Target d-axis current (mA), usually 0 */
    float target_iq;    /* Target q-axis current (mA), controls torque */

    /* Pole pairs (motor specific) */
    uint8_t pole_pairs;
} foc_core_t;

/**
 * Default PI gains for small BLDC motors.
 * Tune these for your specific motor.
 */
#define FOC_PI_DEFAULT_ID() { \
    .kp = 0.001f, .ki = 0.5f, \
    .integral = 0.0f, .out_min = -1.0f, .out_max = 1.0f \
}
#define FOC_PI_DEFAULT_IQ() { \
    .kp = 0.001f, .ki = 0.5f, \
    .integral = 0.0f, .out_min = -1.0f, .out_max = 1.0f \
}

/**
 * Initialize FOC core with default state.
 */
void foc_core_init(foc_core_t *foc, uint8_t pole_pairs);

/**
 * Reset PI controller integrators.
 */
void foc_core_reset(foc_core_t *foc);

/**
 * PI controller update (anti-windup clamping).
 */
float foc_pi_update(foc_pi_t *pi, float error, float dt);

/**
 * Clark transform: 3-phase (Ia, Ib) → 2-phase (Ialpha, Ibeta).
 * Ic = -(Ia + Ib) is assumed.
 */
static inline void foc_clark(float ia, float ib, float *ialpha, float *ibeta)
{
    *ialpha = ia;
    *ibeta  = 0.57735026919f * ia + 1.15470053838f * ib; /* (1/sqrt3)*Ia + (2/sqrt3)*Ib */
}

/**
 * Park transform: (Ialpha, Ibeta) → (Id, Iq) using electrical angle.
 */
static inline void foc_park(float ialpha, float ibeta, float theta,
                            float *id, float *iq)
{
    float cos_t = cosf(theta);
    float sin_t = sinf(theta);
    *id = ialpha * cos_t + ibeta * sin_t;
    *iq = -ialpha * sin_t + ibeta * cos_t;
}

/**
 * Inverse Park transform: (Vd, Vq) → (Valpha, Vbeta).
 */
static inline void foc_inv_park(float vd, float vq, float theta,
                                 float *valpha, float *vbeta)
{
    float cos_t = cosf(theta);
    float sin_t = sinf(theta);
    *valpha = vd * cos_t - vq * sin_t;
    *vbeta  = vd * sin_t + vq * cos_t;
}

/**
 * SVPWM modulation: (Valpha, Vbeta) → 3-phase duty (0.0~1.0).
 * Sectors are handled using the standard 7-segment SVPWM algorithm.
 */
void foc_svpwm(float valpha, float vbeta, float vbus,
               float *duty_u, float *duty_v, float *duty_w);

/**
 * Run one FOC control iteration.
 *
 * @param foc      FOC state
 * @param i_u_ma   U-phase current in mA
 * @param i_v_ma   V-phase current in mA
 * @param dt       Time step in seconds
 * @param duty_u   Output U-phase duty
 * @param duty_v   Output V-phase duty
 * @param duty_w   Output W-phase duty
 */
void foc_core_step(foc_core_t *foc, float i_u_ma, float i_v_ma, float dt,
                   float *duty_u, float *duty_v, float *duty_w);

/**
 * Open-loop angle ramp (for sensorless startup / testing).
 * Advances theta by electrical speed * dt.
 */
void foc_core_openloop_step(foc_core_t *foc, float speed_rad_s, float dt);

#ifdef __cplusplus
}
#endif
