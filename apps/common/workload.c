#include "apps/common/workload.h"

static const float k_A[4][4] = {
    {0.98f, 0.01f, 0.00f, 0.00f},
    {0.00f, 0.97f, 0.02f, 0.00f},
    {0.01f, 0.00f, 0.96f, 0.01f},
    {0.00f, 0.02f, 0.00f, 0.95f},
};

void work_control(ctrl_state_t *s, const sensor_msg_t *m, uint32_t units)
{
    for (uint32_t k = 0; k < units; k++) {
        float nx[4];
        for (uint32_t i = 0; i < 4u; i++) {
            float acc = 0.0f;
            for (uint32_t j = 0; j < 4u; j++) {
                acc += k_A[i][j] * s->x[j];
            }
            nx[i] = acc + 0.05f * m->y[i];
        }
        for (uint32_t i = 0; i < 4u; i++) {
            s->x[i] = nx[i];
        }
        for (uint32_t i = 0; i < 2u; i++) {
            float e = 1.0f - s->x[i];
            s->integ[i] += 0.001f * e;
            if (s->integ[i] > 10.0f) {
                s->integ[i] = 10.0f;
            } else if (s->integ[i] < -10.0f) {
                s->integ[i] = -10.0f;
            }
            s->u[i] = 2.0f * e + 0.5f * s->integ[i] - 0.1f * s->x[i + 2u];
        }
    }
    s->seq++;
}

void work_sensor(sensor_msg_t *m, uint32_t sample, uint32_t units)
{
    float in = (float)(sample & 0x3FFFu) * (1.0f / 16384.0f);
    for (uint32_t k = 0; k < units; k++) {
        for (uint32_t i = 0; i < 4u; i++) {
            m->y[i] = 0.9f * m->y[i] + 0.1f * (in + 0.01f * (float)i);
        }
    }
    m->raw = sample;
    m->seq++;
}

uint32_t work_safety(const ctrl_state_t *c, const sensor_msg_t *m, uint32_t units)
{
    uint32_t violations = 0u;
    for (uint32_t k = 0; k < units; k++) {
        for (uint32_t i = 0; i < 2u; i++) {
            if (!(c->u[i] > -50.0f && c->u[i] < 50.0f)) {
                violations++;
            }
        }
        for (uint32_t i = 0; i < 4u; i++) {
            if (!(m->y[i] >= -0.1f && m->y[i] <= 1.2f)) {
                violations++;
            }
        }
    }
    return violations;
}

uint32_t work_background(uint32_t units)
{
    static const uint8_t data[256] = {1u, 2u, 3u};
    uint32_t acc = 0x811C9DC5u;
    for (uint32_t k = 0; k < units; k++) {
        for (uint32_t i = 0; i < sizeof(data); i++) {
            acc = (acc ^ data[i]) * 16777619u;
        }
    }
    return acc;
}
