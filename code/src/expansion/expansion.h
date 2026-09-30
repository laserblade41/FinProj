/*
 * Modular expansion port — host side.
 *
 * Detects a module on the pogo-pin I2C bus, reads and validates its
 * descriptor, and exposes its capabilities to the rest of the firmware.
 *
 * The 4-pin connector (SDA/SCL/3V3/GND) has no interrupt line, so presence
 * and input data are both discovered by polling. The poll rate adapts to
 * what the watch is doing — see expansion_set_foreground().
 */

#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "expansion/expansion_proto.h"

enum expansion_state {
    EXP_ABSENT,       /* nothing answering on the bus */
    EXP_ENUMERATING,  /* ACKed; reading/validating descriptor */
    EXP_ACTIVE,       /* descriptor accepted, reports flowing */
    EXP_ERROR,        /* module present but descriptor rejected */
};

struct expansion_info {
    char     name[FWMP_NAME_MAX_LEN + 1];  /* NUL-terminated host-side */
    uint16_t vid;
    uint16_t pid;
    uint8_t  caps;          /* FWMP_CAP_* bitmask */
    uint8_t  in_len;        /* input report length, 0 if none */
    uint8_t  out_len;       /* output report length, 0 if none */
    uint16_t poll_hint_ms;  /* module's requested poll interval, 0 if unset */
    uint8_t  icon_id;
};

/* Called on every state transition. Runs on the expansion poll thread, so
 * keep it short and do not block. info is NULL when state is EXP_ABSENT. */
typedef void (*expansion_cb_t)(enum expansion_state state,
                               const struct expansion_info *info);

int expansion_module_init(expansion_cb_t cb);

enum expansion_state expansion_get_state(void);

/* Valid only while state is EXP_ACTIVE; NULL otherwise. */
const struct expansion_info *expansion_get_info(void);

/* Copy the most recent input report. Returns the number of bytes written,
 * or negative errno if no module / no input capability. */
int expansion_get_report(uint8_t *buf, size_t len);

/* Send an output report to the module. Returns 0 or negative errno. */
int expansion_write_report(const uint8_t *buf, size_t len);

/* Tell the subsystem whether the module screen is currently visible, so it
 * can poll fast when the user is looking and back off when they are not. */
void expansion_set_foreground(bool foreground);

#if defined(CONFIG_FINWATCH_EXPANSION_MOCK)
/* Test hook: simulate a module being attached to / removed from the pogo
 * pins, so hot-plug paths are reachable without physical hardware. */
void expansion_mock_set_attached(bool attached);
bool expansion_mock_is_attached(void);

/* Test hook: corrupt the canned descriptor to exercise host-side rejection.
 * See enum expansion_mock_fault in expansion_mock.c. */
void expansion_mock_set_fault(int fault);
#endif
