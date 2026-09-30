/*
 * Mock backend for the expansion port (CONFIG_FINWATCH_EXPANSION_MOCK=y).
 *
 * Stands in for the real I2C transport so the protocol, state machine and UI
 * can be exercised before any physical module exists. Simulates a "Test Dial"
 * module that produces a rotating 4-byte input report and accepts a 1-byte
 * output report.
 *
 * Also injects malformed descriptors on demand, so the host's rejection paths
 * are actually reachable in testing rather than theoretical.
 */

#include "expansion/expansion.h"
#include "expansion/expansion_transport.h"
#include "expansion/expansion_proto.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/logging/log.h>
#include <string.h>

#if defined(CONFIG_FINWATCH_EXPANSION_MOCK)

LOG_MODULE_REGISTER(exp_mock, LOG_LEVEL_INF);

enum expansion_mock_fault {
    MOCK_FAULT_NONE = 0,
    MOCK_FAULT_BAD_MAGIC,
    MOCK_FAULT_BAD_VERSION,
    MOCK_FAULT_BAD_CRC,
    MOCK_FAULT_OVERSIZE_LEN,
    MOCK_FAULT_TLV_OVERRUN,
    MOCK_FAULT_BUS_ERROR,
};

#define MOCK_NAME      "Test Dial"
#define MOCK_IN_LEN    4
#define MOCK_OUT_LEN   1
#define MOCK_POLL_HINT 100

static uint8_t desc[FWMP_DESC_MAX_LEN];
static uint8_t desc_len;

static bool attached;
static int  fault = MOCK_FAULT_NONE;

static uint8_t mock_seq;
static uint8_t mock_report[MOCK_IN_LEN];
static uint8_t mock_output;
static int64_t last_report_ms;

/* Build the canned descriptor, applying whatever fault is currently set.
 * CRC is computed here rather than hardcoded so the "good" case is always
 * self-consistent no matter how the payload is edited. */
static void build_descriptor(void)
{
    uint8_t p = 0;

    memset(desc, 0, sizeof(desc));

    desc[FWMP_REG_MAGIC0]    = (fault == MOCK_FAULT_BAD_MAGIC) ? 0x00 : FWMP_MAGIC0;
    desc[FWMP_REG_MAGIC1]    = FWMP_MAGIC1;
    desc[FWMP_REG_PROTO_VER] = (fault == MOCK_FAULT_BAD_VERSION) ? 0x99 : FWMP_PROTO_VER;
    sys_put_le16(0xABCD, &desc[FWMP_REG_VID_L]);
    sys_put_le16(0x0001, &desc[FWMP_REG_PID_L]);
    desc[FWMP_REG_CAPS]  = FWMP_CAP_INPUT | FWMP_CAP_OUTPUT;
    desc[FWMP_REG_N_TLV] = 4;

    p = FWMP_DESC_HDR_LEN;

    /* NAME */
    desc[p++] = FWMP_TLV_NAME;
    desc[p++] = strlen(MOCK_NAME);
    memcpy(&desc[p], MOCK_NAME, strlen(MOCK_NAME));
    p += strlen(MOCK_NAME);

    /* INPUT_REPORT: len + poll hint */
    desc[p++] = FWMP_TLV_INPUT_REPORT;
    if (fault == MOCK_FAULT_TLV_OVERRUN) {
        /* Claim far more payload than actually remains in the descriptor —
         * the host must catch this instead of reading past the buffer. */
        desc[p++] = 200;
    } else {
        desc[p++] = 3;
    }
    desc[p++] = MOCK_IN_LEN;
    sys_put_le16(MOCK_POLL_HINT, &desc[p]);
    p += 2;

    /* OUTPUT_REPORT */
    desc[p++] = FWMP_TLV_OUTPUT_REPORT;
    desc[p++] = 1;
    desc[p++] = MOCK_OUT_LEN;

    /* ICON_ID */
    desc[p++] = FWMP_TLV_ICON_ID;
    desc[p++] = 1;
    desc[p++] = 0;

    /* Trailing CRC over everything before it. */
    desc_len = p + FWMP_DESC_CRC_LEN;

    uint16_t crc = crc16_ccitt(0xffff, desc, p);
    if (fault == MOCK_FAULT_BAD_CRC) {
        crc ^= 0xFFFF;
    }
    sys_put_le16(crc, &desc[p]);

    /* Declared length is written last so the oversize fault can lie about it
     * without disturbing the real layout. */
    desc[FWMP_REG_DESC_LEN] =
        (fault == MOCK_FAULT_OVERSIZE_LEN) ? 200 : desc_len;
}

/* Advance the simulated dial so the UI shows something moving. */
static void update_report(void)
{
    const int64_t now = k_uptime_get();

    if (now - last_report_ms < 500) {
        return;
    }
    last_report_ms = now;

    mock_seq++;
    mock_report[0] = mock_seq;              /* dial position */
    mock_report[1] = (mock_seq * 3) & 0xFF; /* arbitrary second axis */
    mock_report[2] = mock_output;           /* echo last output written */
    mock_report[3] = 0x5A;                  /* constant, easy to eyeball */
}

int expansion_transport_init(void)
{
    build_descriptor();
    LOG_INF("mock expansion transport ready (descriptor %u bytes)", desc_len);
    return 0;
}

int expansion_transport_probe(void)
{
    if (fault == MOCK_FAULT_BUS_ERROR) {
        return -EIO;
    }
    return attached ? 0 : -ENODEV;
}

int expansion_transport_read(uint8_t reg, uint8_t *buf, size_t len)
{
    if (!attached) {
        return -ENODEV;
    }
    if (fault == MOCK_FAULT_BUS_ERROR) {
        return -EIO;
    }

    update_report();

    /* Descriptor space */
    if (reg < FWMP_DESC_MAX_LEN) {
        if (reg + len > sizeof(desc)) {
            return -EINVAL;
        }
        memcpy(buf, &desc[reg], len);
        return 0;
    }

    /* STATUS / SEQ — adjacent so the host reads both in one go */
    if (reg == FWMP_REG_STATUS) {
        if (len < 1) {
            return -EINVAL;
        }
        buf[0] = FWMP_STATUS_DATA_READY;
        if (len > 1) {
            buf[1] = mock_seq;
        }
        return 0;
    }

    /* Input report */
    if (reg == FWMP_REG_INPUT) {
        const size_t n = MIN(len, (size_t)MOCK_IN_LEN);
        memcpy(buf, mock_report, n);
        return 0;
    }

    return -EINVAL;
}

int expansion_transport_write(uint8_t reg, const uint8_t *buf, size_t len)
{
    if (!attached) {
        return -ENODEV;
    }
    if (fault == MOCK_FAULT_BUS_ERROR) {
        return -EIO;
    }

    if (reg == FWMP_REG_OUTPUT && len >= 1) {
        mock_output = buf[0];
        LOG_INF("mock module received output report: 0x%02x", mock_output);
        return 0;
    }

    return -EINVAL;
}

int expansion_transport_recover(void)
{
    LOG_INF("mock bus recovery");
    return 0;
}

/* ---- Test hooks ---- */

void expansion_mock_set_attached(bool a)
{
    attached = a;
    LOG_INF("mock module %s", a ? "ATTACHED" : "DETACHED");
}

bool expansion_mock_is_attached(void)
{
    return attached;
}

void expansion_mock_set_fault(int f)
{
    fault = f;
    build_descriptor();
    LOG_INF("mock fault mode = %d", f);
}

#endif /* CONFIG_FINWATCH_EXPANSION_MOCK */
