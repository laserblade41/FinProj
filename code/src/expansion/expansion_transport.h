/*
 * Transport abstraction for the expansion port.
 *
 * Two backends implement this: the real I2C one (expansion_transport.c) and
 * a mock (expansion_mock.c, CONFIG_FINWATCH_EXPANSION_MOCK) that serves a
 * canned descriptor so the protocol, state machine and UI can be exercised
 * before any physical module hardware exists.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

/* Bring up the underlying bus. Returns 0, or negative errno if the bus
 * device is not ready. */
int expansion_transport_init(void);

/* Is a module currently answering on the bus?
 * Returns 0 if the module ACKed, negative errno otherwise. */
int expansion_transport_probe(void);

/* Read len bytes starting at register reg. Returns 0 or negative errno. */
int expansion_transport_read(uint8_t reg, uint8_t *buf, size_t len);

/* Write len bytes starting at register reg. Returns 0 or negative errno. */
int expansion_transport_write(uint8_t reg, const uint8_t *buf, size_t len);

/* Attempt to un-stick a jammed bus (e.g. a module unplugged mid-transaction
 * leaving SDA held low). Returns 0 or negative errno. */
int expansion_transport_recover(void);
