/*
 * Modular expansion port: enumeration state machine and poll loop.
 *
 * ABSENT -> ENUMERATING -> ACTIVE, falling back to ABSENT on repeated NACK.
 *
 * Everything the module sends is untrusted: the descriptor is validated for
 * magic, protocol version, length bounds and CRC before any field is used,
 * and every TLV is bounds-checked against the declared descriptor length.
 */

#include "expansion/expansion.h"
#include "expansion/expansion_proto.h"
#include "expansion/expansion_transport.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(expansion, LOG_LEVEL_INF);

#define EXP_STACK_SIZE   1536
#define EXP_PRIORITY     10   /* below display (7) and sensor (8) */

/* Poll intervals. No interrupt line on the 4-pin connector, so these are the
 * whole latency/power tradeoff. */
#define POLL_ABSENT_MS       1000
#define POLL_BACKGROUND_MS    250
#define POLL_FOREGROUND_MS     50

/* Contact bounce on pogo pins: require agreement over several polls before
 * changing attach state, so a momentary glitch does not tear down a session. */
#define ATTACH_DEBOUNCE       2
#define DETACH_DEBOUNCE       3

/* Consecutive bus errors before attempting recovery. */
#define ERR_BEFORE_RECOVER    3

static enum expansion_state  state = EXP_ABSENT;
static struct expansion_info info;
static expansion_cb_t        user_cb;

static uint8_t report_buf[FWMP_REPORT_MAX_LEN];
static uint8_t report_len;
static uint8_t last_seq;

static bool foreground;
static bool transport_ok;

/* Guards info/report_buf against concurrent access from UI threads. */
static K_MUTEX_DEFINE(exp_lock);

static void set_state(enum expansion_state new_state)
{
    if (state == new_state) {
        return;
    }
    state = new_state;

    if (user_cb) {
        user_cb(state, (state == EXP_ACTIVE) ? &info : NULL);
    }
}

/*
 * Parse the TLV section. Returns 0 on success, -EINVAL if any entry claims a
 * length that would run past the end of the descriptor.
 */
static int parse_tlvs(const uint8_t *desc, uint8_t desc_len, uint8_t n_tlv)
{
    /* TLVs live between the fixed header and the trailing CRC.
     * Cursors are uint16_t, not uint8_t: desc_len is bounded to 128 by the
     * caller so 8-bit arithmetic would not actually wrap today, but keeping
     * the offsets wider means a future bump to FWMP_DESC_MAX_LEN cannot
     * silently turn these bounds checks into wraparound bugs. */
    const uint16_t tlv_end = (uint16_t)desc_len - FWMP_DESC_CRC_LEN;
    uint16_t pos = FWMP_DESC_HDR_LEN;

    for (uint8_t i = 0; i < n_tlv; i++) {
        /* Need at least a type and a length byte. */
        if (pos + 2 > tlv_end) {
            LOG_ERR("TLV %u: truncated header", i);
            return -EINVAL;
        }

        const uint8_t  type = desc[pos];
        const uint8_t  len  = desc[pos + 1];
        const uint16_t val  = pos + 2;

        if (val + len > tlv_end) {
            LOG_ERR("TLV %u: payload len %u overruns descriptor", i, len);
            return -EINVAL;
        }

        switch (type) {
        case FWMP_TLV_NAME: {
            const uint8_t n = MIN(len, (uint8_t)FWMP_NAME_MAX_LEN);
            memcpy(info.name, &desc[val], n);
            info.name[n] = '\0';
            break;
        }
        case FWMP_TLV_INPUT_REPORT:
            if (len < 3) {
                LOG_ERR("TLV %u: INPUT_REPORT needs 3 bytes, got %u", i, len);
                return -EINVAL;
            }
            info.in_len = desc[val];
            if (info.in_len > FWMP_REPORT_MAX_LEN) {
                LOG_ERR("input report len %u exceeds max %u",
                        info.in_len, FWMP_REPORT_MAX_LEN);
                return -EINVAL;
            }
            info.poll_hint_ms = sys_get_le16(&desc[val + 1]);
            break;

        case FWMP_TLV_OUTPUT_REPORT:
            if (len < 1) {
                LOG_ERR("TLV %u: OUTPUT_REPORT needs 1 byte", i);
                return -EINVAL;
            }
            info.out_len = desc[val];
            if (info.out_len > FWMP_REPORT_MAX_LEN) {
                LOG_ERR("output report len %u exceeds max %u",
                        info.out_len, FWMP_REPORT_MAX_LEN);
                return -EINVAL;
            }
            break;

        case FWMP_TLV_ICON_ID:
            if (len >= 1) {
                info.icon_id = desc[val];
            }
            break;

        default:
            /* Unknown TLV types are skipped, not rejected, that is what
             * makes the descriptor forward-extensible. */
            LOG_DBG("TLV %u: skipping unknown type 0x%02x", i, type);
            break;
        }

        pos = val + len;
    }

    return 0;
}

/*
 * Fetch and validate the descriptor. Returns 0 and fills `info` on success.
 */
static int enumerate(void)
{
    uint8_t desc[FWMP_DESC_MAX_LEN];
    int ret;

    /* Read the fixed header first so we learn the real length before
     * deciding how much more to read. */
    ret = expansion_transport_read(FWMP_REG_MAGIC0, desc, FWMP_DESC_HDR_LEN);
    if (ret < 0) {
        LOG_ERR("descriptor header read failed: %d", ret);
        return ret;
    }

    if (desc[FWMP_REG_MAGIC0] != FWMP_MAGIC0 ||
        desc[FWMP_REG_MAGIC1] != FWMP_MAGIC1) {
        LOG_ERR("bad magic %02x %02x", desc[FWMP_REG_MAGIC0], desc[FWMP_REG_MAGIC1]);
        return -EINVAL;
    }

    if (desc[FWMP_REG_PROTO_VER] != FWMP_PROTO_VER) {
        LOG_ERR("unsupported protocol version %u (need %u)",
                desc[FWMP_REG_PROTO_VER], FWMP_PROTO_VER);
        return -ENOTSUP;
    }

    const uint8_t desc_len = desc[FWMP_REG_DESC_LEN];

    /* Bound the length before trusting it, it must be big enough to contain
     * the header and CRC, and must fit our buffer. */
    if (desc_len < FWMP_DESC_HDR_LEN + FWMP_DESC_CRC_LEN ||
        desc_len > FWMP_DESC_MAX_LEN) {
        LOG_ERR("descriptor length %u out of range [%u, %u]",
                desc_len, FWMP_DESC_HDR_LEN + FWMP_DESC_CRC_LEN, FWMP_DESC_MAX_LEN);
        return -EINVAL;
    }

    /* Now pull the whole thing, including TLVs and CRC. */
    ret = expansion_transport_read(FWMP_REG_MAGIC0, desc, desc_len);
    if (ret < 0) {
        LOG_ERR("descriptor body read failed: %d", ret);
        return ret;
    }

    const uint16_t crc_calc = crc16_ccitt(0xffff, desc, desc_len - FWMP_DESC_CRC_LEN);
    const uint16_t crc_recv = sys_get_le16(&desc[desc_len - FWMP_DESC_CRC_LEN]);

    if (crc_calc != crc_recv) {
        LOG_ERR("descriptor CRC mismatch: calc %04x, recv %04x", crc_calc, crc_recv);
        return -EINVAL;
    }

    /* Descriptor is trustworthy from here on. */
    memset(&info, 0, sizeof(info));
    info.vid  = sys_get_le16(&desc[FWMP_REG_VID_L]);
    info.pid  = sys_get_le16(&desc[FWMP_REG_PID_L]);
    info.caps = desc[FWMP_REG_CAPS];

    ret = parse_tlvs(desc, desc_len, desc[FWMP_REG_N_TLV]);
    if (ret < 0) {
        return ret;
    }

    if (info.name[0] == '\0') {
        strcpy(info.name, "Unnamed Module");
    }

    LOG_INF("module \"%s\" vid=%04x pid=%04x caps=%02x in=%u out=%u",
            info.name, info.vid, info.pid, info.caps, info.in_len, info.out_len);
    return 0;
}

/* Poll STATUS and pull a fresh input report if one is waiting. */
static int poll_report(void)
{
    uint8_t hdr[2];  /* STATUS, SEQ, adjacent, so one read gets both */
    int ret;

    if (!(info.caps & FWMP_CAP_INPUT) || info.in_len == 0) {
        return 0;
    }

    ret = expansion_transport_read(FWMP_REG_STATUS, hdr, sizeof(hdr));
    if (ret < 0) {
        return ret;
    }

    if (!(hdr[0] & FWMP_STATUS_DATA_READY)) {
        return 0;
    }

    /* SEQ unchanged means we already have this report. */
    if (hdr[1] == last_seq) {
        return 0;
    }

    uint8_t tmp[FWMP_REPORT_MAX_LEN];

    ret = expansion_transport_read(FWMP_REG_INPUT, tmp, info.in_len);
    if (ret < 0) {
        return ret;
    }

    k_mutex_lock(&exp_lock, K_FOREVER);
    memcpy(report_buf, tmp, info.in_len);
    report_len = info.in_len;
    k_mutex_unlock(&exp_lock);

    last_seq = hdr[1];
    return 0;
}

static uint32_t current_poll_interval(void)
{
    if (state != EXP_ACTIVE) {
        return POLL_ABSENT_MS;
    }
    if (!foreground) {
        return POLL_BACKGROUND_MS;
    }
    /* Honour the module's hint when it asks for something slower than our
     * fastest rate; never poll faster than POLL_FOREGROUND_MS. */
    if (info.poll_hint_ms > POLL_FOREGROUND_MS) {
        return info.poll_hint_ms;
    }
    return POLL_FOREGROUND_MS;
}

static void expansion_thread_fn(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    uint8_t ack_run = 0;    /* consecutive successful probes */
    uint8_t nack_run = 0;   /* consecutive failed probes */
    uint8_t err_run = 0;    /* consecutive bus errors while active */

    while (1) {
        if (!transport_ok) {
            k_msleep(POLL_ABSENT_MS);
            continue;
        }

        const bool present = (expansion_transport_probe() == 0);

        if (present) {
            nack_run = 0;
            if (ack_run < ATTACH_DEBOUNCE) {
                ack_run++;
            }
        } else {
            ack_run = 0;
            if (nack_run < DETACH_DEBOUNCE) {
                nack_run++;
            }
        }

        switch (state) {
        case EXP_ABSENT:
            if (ack_run >= ATTACH_DEBOUNCE) {
                set_state(EXP_ENUMERATING);
                if (enumerate() == 0) {
                    last_seq = 0;
                    report_len = 0;
                    err_run = 0;
                    set_state(EXP_ACTIVE);
                } else {
                    /* Present but unusable. Stay in ERROR until it goes away,
                     * rather than retrying enumeration every poll. */
                    set_state(EXP_ERROR);
                }
            }
            break;

        case EXP_ACTIVE:
            if (nack_run >= DETACH_DEBOUNCE) {
                LOG_INF("module detached");
                set_state(EXP_ABSENT);
                break;
            }
            if (poll_report() < 0) {
                if (++err_run >= ERR_BEFORE_RECOVER) {
                    LOG_WRN("%u consecutive bus errors; recovering", err_run);
                    (void)expansion_transport_recover();
                    err_run = 0;
                    set_state(EXP_ABSENT);
                }
            } else {
                err_run = 0;
            }
            break;

        case EXP_ERROR:
            if (nack_run >= DETACH_DEBOUNCE) {
                set_state(EXP_ABSENT);
            }
            break;

        case EXP_ENUMERATING:
            /* Transient: enumerate() runs to completion above. */
            break;
        }

        k_msleep(current_poll_interval());
    }
}

K_THREAD_DEFINE(expansion_tid, EXP_STACK_SIZE,
                expansion_thread_fn, NULL, NULL, NULL,
                EXP_PRIORITY, 0, 0);

int expansion_module_init(expansion_cb_t cb)
{
    user_cb = cb;

    int ret = expansion_transport_init();
    if (ret < 0) {
        return ret;
    }

    transport_ok = true;
    return 0;
}

enum expansion_state expansion_get_state(void)
{
    return state;
}

const struct expansion_info *expansion_get_info(void)
{
    return (state == EXP_ACTIVE) ? &info : NULL;
}

int expansion_get_report(uint8_t *buf, size_t len)
{
    if (state != EXP_ACTIVE) {
        return -ENODEV;
    }

    k_mutex_lock(&exp_lock, K_FOREVER);
    const size_t n = MIN(len, (size_t)report_len);
    memcpy(buf, report_buf, n);
    k_mutex_unlock(&exp_lock);

    return (int)n;
}

int expansion_write_report(const uint8_t *buf, size_t len)
{
    if (state != EXP_ACTIVE) {
        return -ENODEV;
    }
    if (!(info.caps & FWMP_CAP_OUTPUT)) {
        return -ENOTSUP;
    }
    if (len > info.out_len) {
        return -EINVAL;
    }

    return expansion_transport_write(FWMP_REG_OUTPUT, buf, len);
}

void expansion_set_foreground(bool fg)
{
    foreground = fg;
}
