/*
 * Real I2C backend for the expansion port.
 *
 * Compiled out when CONFIG_FINWATCH_EXPANSION_MOCK=y, in which case
 * expansion_mock.c provides these symbols instead.
 */

#include "expansion/expansion_transport.h"
#include "expansion/expansion_proto.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/logging/log.h>
#include <string.h>

#if !defined(CONFIG_FINWATCH_EXPANSION_MOCK)

LOG_MODULE_REGISTER(exp_i2c, LOG_LEVEL_INF);

/*
 * The module is hot-plugged, so it deliberately has no devicetree child node
 * — we take the bus controller itself and address FWMP_I2C_ADDR directly.
 */
static const struct device *bus = DEVICE_DT_GET(DT_NODELABEL(i2c3));

int expansion_transport_init(void)
{
    if (!device_is_ready(bus)) {
        LOG_ERR("expansion I2C bus not ready");
        return -ENODEV;
    }
    LOG_INF("expansion bus ready (addr 0x%02x)", FWMP_I2C_ADDR);
    return 0;
}

int expansion_transport_probe(void)
{
    /*
     * Zero-length write: emits START + address + STOP and nothing else.
     * A 0 return means the target ACKed its address. Same technique the
     * Zephyr i2c shell uses for bus scanning.
     */
    struct i2c_msg msg = {
        .buf   = NULL,
        .len   = 0U,
        .flags = I2C_MSG_WRITE | I2C_MSG_STOP,
    };

    return i2c_transfer(bus, &msg, 1, FWMP_I2C_ADDR);
}

int expansion_transport_read(uint8_t reg, uint8_t *buf, size_t len)
{
    /* Write the register offset, repeated START, then read — the standard
     * I2C EEPROM read transaction. */
    return i2c_write_read(bus, FWMP_I2C_ADDR, &reg, sizeof(reg), buf, len);
}

int expansion_transport_write(uint8_t reg, const uint8_t *buf, size_t len)
{
    uint8_t tmp[1 + FWMP_REPORT_MAX_LEN];

    if (len > FWMP_REPORT_MAX_LEN) {
        return -EINVAL;
    }

    /* Offset byte and payload must land in one transaction, so coalesce
     * them into a single buffer rather than issuing two writes. */
    tmp[0] = reg;
    memcpy(&tmp[1], buf, len);

    return i2c_write(bus, tmp, len + 1, FWMP_I2C_ADDR);
}

int expansion_transport_recover(void)
{
    return i2c_recover_bus(bus);
}

#endif /* !CONFIG_FINWATCH_EXPANSION_MOCK */
