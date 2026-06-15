/*
 * FOC Motor Control - Algorithm Layer Implementation
 * Clark/Park transforms, PI controllers, SVPWM modulation
 * SPDX-License-Identifier: Apache-2.0
 */
#include "foc_core.h"

#include <math.h>
#include <string.h>

#ifndef M_SQRT3
#define M_SQRT3 1.7320508075688772f
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

/* 1/sqrt(3) */
#define FOC_INV_SQRT3  (1.0f / M_SQRT3)
/* sqrt(3)/2 */
#define FOC_SQRT3_HALF (M_SQRT3 * 0.5f)
/* 2/3 */
#define FOC_TWO_THIRDS (2.0f / 3.0f)

void foc_core_init(foc_core_t *foc, uint8_t pole_pairs)
{
    memset(foc, 0, sizeof(*foc));
    foc->pi_id = (foc_pi_t)FOC_PI_DEFAULT_ID();
    foc->pi_iq = (foc_pi_t)FOC_PI_DEFAULT_IQ();
    foc->theta = 0.0f;
    foc->target_id = 0.0f;
    foc->target_iq = 0.0f;
    foc->pole_pairs = pole_pairs;
}

void foc_core_reset(foc_core_t *foc)
{
    foc->pi_id.integral = 0.0f;
    foc->pi_iq.integral = 0.0f;
    foc->theta = 0.0f;
}

float foc_pi_update(foc_pi_t *pi, float error, float dt)
{
    /* Integrate with anti-windup */
    float new_integral = pi->integral + error * pi->ki * dt;
    float output = pi->kp * error + new_integral;

    /* Clamp output and freeze integral on saturation */
    if (output > pi->out_max) {
        output = pi->out_max;
    } else if (output < pi->out_min) {
        output = pi->out_min;
    } else {
        /* Only update integral if not saturated */
        pi->integral = new_integral;
    }
    return output;
}

void foc_svpwm(float valpha, float vbeta, float vbus,
               float *duty_u, float *duty_v, float *duty_w)
{
    /*
     * Standard 7-segment SVPWM.
     * Reference: Voltage limit = Vbus / sqrt(3) for linear modulation.
     * Normalize to duty 0.0 ~ 1.0.
     */
    float inv_vbus_sqrt3 = (vbus > 0.01f) ? (1.0f / (vbus * M_SQRT3)) : 0.0f;

    /* Clamp reference vector magnitude */
    float vmag = sqrtf(valpha * valpha + vbeta * vbeta);
    float vmax = vbus * FOC_SQRT3_HALF; /* Linear region limit */
    if (vmag > vmax && vmag > 0.01f) {
        float scale = vmax / vmag;
        valpha *= scale;
        vbeta  *= scale;
    }

    /* Transform to phase voltages */
    float va = valpha;
    float vb = -0.5f * valpha + FOC_SQRT3_HALF * vbeta;
    float vc = -0.5f * valpha - FOC_SQRT3_HALF * vbeta;

    /* Shift to duty cycle (center-aligned, add 0.5 offset) */
    float offset = 0.5f;
    *duty_u = va * inv_vbus_sqrt3 + offset;
    *duty_v = vb * inv_vbus_sqrt3 + offset;
    *duty_w = vc * inv_vbus_sqrt3 + offset;

    /* Final clamp */
    if (*duty_u < 0.0f) *duty_u = 0.0f;
    if (*duty_u > 1.0f) *duty_u = 1.0f;
    if (*duty_v < 0.0f) *duty_v = 0.0f;
    if (*duty_v > 1.0f) *duty_v = 1.0f;
    if (*duty_w < 0.0f) *duty_w = 0.0f;
    if (*duty_w > 1.0f) *duty_w = 1.0f;
}

void foc_core_step(foc_core_t *foc, float i_u_ma, float i_v_ma, float dt,
                   float *duty_u, float *duty_v, float *duty_w)
{
    /* Clark: Ia=Iu, Ib=Iv → Ialpha, Ibeta */
    float ialpha, ibeta;
    foc_clark(i_u_ma, i_v_ma, &ialpha, &ibeta);

    /* Park: Ialpha, Ibeta → Id, Iq */
    float id, iq;
    foc_park(ialpha, ibeta, foc->theta, &id, &iq);

    /* PI controllers for Id and Iq */
    float vd = foc_pi_update(&foc->pi_id, foc->target_id - id, dt);
    float vq = foc_pi_update(&foc->pi_iq, foc->target_iq - iq, dt);

    /* Inverse Park: Vd, Vq → Valpha, Vbeta */
    float valpha, vbeta;
    foc_inv_park(vd, vq, foc->theta, &valpha, &vbeta);

    /* SVPWM: Valpha, Vbeta → duty */
    /* Assume Vbus = 1.0 (normalized) for duty output */
    foc_svpwm(valpha, vbeta, 1.0f, duty_u, duty_v, duty_w);
}

void foc_core_openloop_step(foc_core_t *foc, float speed_rad_s, float dt)
{
    foc->theta += speed_rad_s * dt;
    if (foc->theta > 2.0f * M_PI) {
        foc->theta -= 2.0f * M_PI;
    } else if (foc->theta < 0.0f) {
        foc->theta += 2.0f * M_PI;
    }
}
