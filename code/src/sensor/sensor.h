#pragma once

#include <stdint.h>

struct accel_data {
    int32_t x_mg;  /* milli-g */
    int32_t y_mg;
    int32_t z_mg;
};

typedef void (*sensor_motion_cb_t)(void);

int      sensor_module_init(sensor_motion_cb_t motion_cb);
int      sensor_get_accel(struct accel_data *out);
uint32_t sensor_get_step_count(void);
void     sensor_reset_step_count(void);
