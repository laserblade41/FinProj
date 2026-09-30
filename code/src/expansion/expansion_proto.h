/*
 * FinWatch Module Protocol (FWMP) v1 — wire format.
 *
 * Transport: I2C. The watch is the controller (host); an expansion module
 * attached to the pogo pins is the target, at a fixed address.
 *
 * The module exposes a flat register space read with the standard
 * "write offset, repeated START, read N" EEPROM idiom (i2c_write_read()).
 * Low registers hold a self-describing descriptor; high registers carry
 * runtime input/output reports.
 *
 * This header is wire format ONLY — no Zephyr dependencies, so the same
 * file can be shared verbatim with module-side firmware later.
 */

#pragma once

#include <stdint.h>

/* ---- Transport ---- */

/* 7-bit target address. Must stay outside the I2C reserved ranges
 * (0x00-0x07 and 0x78-0x7F). Dedicated bus, one module at a time. */
#define FWMP_I2C_ADDR        0x42

#define FWMP_MAGIC0          0x46  /* 'F' */
#define FWMP_MAGIC1          0x57  /* 'W' */
#define FWMP_PROTO_VER       0x01

/* ---- Descriptor register map ---- */

#define FWMP_REG_MAGIC0      0x00
#define FWMP_REG_MAGIC1      0x01
#define FWMP_REG_PROTO_VER   0x02
#define FWMP_REG_DESC_LEN    0x03  /* total descriptor bytes, incl. header + TLVs + CRC */
#define FWMP_REG_VID_L       0x04
#define FWMP_REG_VID_H       0x05
#define FWMP_REG_PID_L       0x06
#define FWMP_REG_PID_H       0x07
#define FWMP_REG_CAPS        0x08
#define FWMP_REG_N_TLV       0x09
#define FWMP_REG_TLV_START   0x0A

/* Fixed-size part of the descriptor, before the TLV entries. */
#define FWMP_DESC_HDR_LEN    FWMP_REG_TLV_START  /* 10 bytes */

/* CRC16 occupies the final two bytes of the descriptor. */
#define FWMP_DESC_CRC_LEN    2

/* Hard upper bound on a descriptor. The host rejects anything larger rather
 * than trusting a length byte from an untrusted module. */
#define FWMP_DESC_MAX_LEN    128

/* ---- Capability flags (FWMP_REG_CAPS) ---- */

#define FWMP_CAP_INPUT       (1U << 0)  /* module produces input reports */
#define FWMP_CAP_OUTPUT      (1U << 1)  /* module accepts output reports */

/* ---- TLV entry types ---- */

#define FWMP_TLV_NAME          0x01  /* ASCII, not NUL-terminated on the wire */
#define FWMP_TLV_INPUT_REPORT  0x02  /* [report_len:1][poll_hint_ms:2 LE] */
#define FWMP_TLV_OUTPUT_REPORT 0x03  /* [report_len:1] */
#define FWMP_TLV_ICON_ID       0x04  /* [icon_id:1] */

#define FWMP_NAME_MAX_LEN      24

/* ---- Runtime register map ---- */

#define FWMP_REG_STATUS        0x80
#define FWMP_REG_SEQ           0x81  /* bumped on each new input report */
#define FWMP_REG_INPUT         0x82
#define FWMP_REG_OUTPUT        0xC0

#define FWMP_REPORT_MAX_LEN    32

/* ---- STATUS bits (FWMP_REG_STATUS) ---- */

#define FWMP_STATUS_DATA_READY (1U << 0)
#define FWMP_STATUS_ERROR      (1U << 1)
#define FWMP_STATUS_BUSY       (1U << 2)
