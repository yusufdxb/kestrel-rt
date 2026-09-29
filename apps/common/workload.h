/* Synthetic robotics-like workload kernels (application code, FPU allowed). Deterministic: the
 * amount of work depends only on `units`, never on data, so execution time is a function of the
 * knob plus interference. */
#ifndef KESTREL_APP_WORKLOAD_H
#define KESTREL_APP_WORKLOAD_H
#include <stdint.h>

typedef struct {
    float x[4];      /* state estimate */
    float u[2];      /* actuator command */
    float integ[2];
    uint32_t seq;
} ctrl_state_t;

typedef struct {
    float y[4];      /* filtered measurement */
    uint32_t seq;
    uint32_t raw;    /* last raw ADC code (or synthetic) */
} sensor_msg_t;

/* One unit = one 4x4 matrix-vector product + PID update (about 0.1-0.2k cycles). */
void work_control(ctrl_state_t *s, const sensor_msg_t *m, uint32_t units);
/* One unit = one step of a 4-channel IIR filter on the input. */
void work_sensor(sensor_msg_t *m, uint32_t sample, uint32_t units);
/* One unit = bounds/consistency checks over the latest control and sensor data. */
uint32_t work_safety(const ctrl_state_t *c, const sensor_msg_t *m, uint32_t units);
/* Background: integer checksum over a buffer, `units` passes. */
uint32_t work_background(uint32_t units);
#endif
