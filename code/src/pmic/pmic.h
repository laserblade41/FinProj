#pragma once

#include <stdbool.h>
#include <stdint.h>

int     pmic_module_init(void);
uint8_t pmic_get_batt_percent(void);
bool    pmic_is_charging(void);
