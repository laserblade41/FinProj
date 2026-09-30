#include "ble/ble.h"
#include "rtc/rtc.h"
#include "display/watchface.h"

#include <zephyr/kernel.h>
#include <string.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/services/bas.h>
#include <zephyr/settings/settings.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(ble_mod, LOG_LEVEL_INF);

/* ---- Advertising parameters & data ---- */

/*
 * BT_LE_ADV_CONN / BT_LE_ADV_CONN_ONE_TIME were removed in Zephyr 3.7.
 * Build the param directly with BT_LE_ADV_PARAM + BT_LE_ADV_OPT_CONNECTABLE.
 */
/* NCS v3.2.1 / Zephyr 3.7: BT_LE_ADV_OPT_CONNECTABLE was renamed to BT_LE_ADV_OPT_CONN */
static const struct bt_le_adv_param adv_param =
    BT_LE_ADV_PARAM_INIT(BT_LE_ADV_OPT_CONN,
                         BT_GAP_ADV_FAST_INT_MIN_2,
                         BT_GAP_ADV_FAST_INT_MAX_2,
                         NULL);

static const struct bt_data ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
    BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME,
            sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

/* ---- Current Time Service (CTS)- manual GATT implementation ---- */

/* BT_UUID_CTS_CURRENT_TIME_VAL (0x2a2b) is in <zephyr/bluetooth/uuid.h> */
#define BT_UUID_CTS_CT  BT_UUID_DECLARE_16(BT_UUID_CTS_CURRENT_TIME_VAL)

/* Current Time characteristic value (10 bytes per BT spec) */
static uint8_t ct_buf[10];

static ssize_t ct_read(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                       void *buf, uint16_t len, uint16_t offset)
{
    struct rtc_time t;
    rtc_get_time(&t);

    ct_buf[0] = (uint8_t)(t.year & 0xFF);
    ct_buf[1] = (uint8_t)(t.year >> 8);
    ct_buf[2] = t.month;
    ct_buf[3] = t.day;
    ct_buf[4] = t.hour;
    ct_buf[5] = t.minute;
    ct_buf[6] = t.second;
    ct_buf[7] = 0;   /* day_of_week (0 = unknown) */
    ct_buf[8] = 0;   /* fractions256 */
    ct_buf[9] = 0;   /* adjust_reason */

    return bt_gatt_attr_read(conn, attr, buf, len, offset, ct_buf, sizeof(ct_buf));
}

static ssize_t ct_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                        const void *buf, uint16_t len, uint16_t offset,
                        uint8_t flags)
{
    ARG_UNUSED(attr);
    ARG_UNUSED(flags);

    if (offset != 0 || len < 7) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }

    const uint8_t *d = buf;
    uint16_t year  = d[0] | ((uint16_t)d[1] << 8);
    uint8_t  month = d[2], day = d[3];
    uint8_t  hour  = d[4], min = d[5], sec = d[6];

    /* Convert to epoch seconds from 2000-01-01 (rough, no DST) */
    uint32_t days = 0;
    for (uint16_t y = 2000; y < year; y++) {
        bool leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
        days += leap ? 366 : 365;
    }
    static const uint8_t dim[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    bool leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
    for (uint8_t m = 1; m < month; m++) {
        days += dim[m - 1] + (leap && m == 2 ? 1 : 0);
    }
    days += day - 1;

    uint32_t epoch = days * 86400U + hour * 3600U + min * 60U + sec;
    rtc_set_epoch(epoch);

    LOG_INF("CTS: time set to %04u-%02u-%02u %02u:%02u:%02u",
            year, month, day, hour, min, sec);
    return len;
}

/* ---- Custom notification characteristic ---- */

#define BT_UUID_NOTIF_SVC  BT_UUID_DECLARE_128(                          \
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcdef0))
#define BT_UUID_NOTIF_CHAR BT_UUID_DECLARE_128(                          \
    BT_UUID_128_ENCODE(0x12345678, 0x1234, 0x5678, 0x1234, 0x56789abcdef1))

static uint8_t notif_buf[64];
static uint16_t notif_len;

static ssize_t notif_write(struct bt_conn *conn,
                           const struct bt_gatt_attr *attr,
                           const void *buf, uint16_t len,
                           uint16_t offset, uint8_t flags)
{
    ARG_UNUSED(conn);
    ARG_UNUSED(attr);
    ARG_UNUSED(flags);

    if (offset != 0) return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);

    notif_len = MIN(len, sizeof(notif_buf) - 1);
    memcpy(notif_buf, buf, notif_len);
    notif_buf[notif_len] = '\0';

    watchface_show_notification((const char *)notif_buf);

    return len;
}

static void ct_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    ARG_UNUSED(attr);
    ARG_UNUSED(value);
}

/*
 * Pairing requirement disabled for now, _ENCRYPT perms + bt_conn_set_security()
 * in connected() kept hitting BT_SECURITY_ERR_AUTH_REQUIREMENT on first-time
 * Android pairing even with an auth_cb registered. Reverted to plain
 * READ/WRITE so the app is usable while that's investigated further.
 */
BT_GATT_SERVICE_DEFINE(cts_svc,
    BT_GATT_PRIMARY_SERVICE(BT_UUID_DECLARE_16(BT_UUID_CTS_VAL)),
    BT_GATT_CHARACTERISTIC(BT_UUID_CTS_CT,
        BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
        BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
        ct_read, ct_write, NULL),
    BT_GATT_CCC(ct_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
);

BT_GATT_SERVICE_DEFINE(notif_svc,
    BT_GATT_PRIMARY_SERVICE(BT_UUID_NOTIF_SVC),
    BT_GATT_CHARACTERISTIC(BT_UUID_NOTIF_CHAR,
        BT_GATT_CHRC_WRITE_WITHOUT_RESP,
        BT_GATT_PERM_WRITE,
        NULL, notif_write, NULL),
    BT_GATT_CUD("Notification Text", BT_GATT_PERM_READ),
);

/* ---- Connection callbacks ---- */

/*
 * Restarting advertising immediately in the disconnect callback can
 * transiently fail (e.g. -EAGAIN) while the connection context is still
 * being torn down over the IPC link to the network core. Retry on a
 * work queue instead of silently giving up, or the watch becomes
 * unconnectable until the next reboot.
 */
static void adv_start_work_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(adv_start_work, adv_start_work_fn);

static void adv_start_work_fn(struct k_work *work)
{
    ARG_UNUSED(work);

    int ret = bt_le_adv_start(&adv_param, ad, ARRAY_SIZE(ad), NULL, 0);
    if (ret == -EAGAIN || ret == -EALREADY) {
        LOG_WRN("bt_le_adv_start retry (%d)", ret);
        k_work_schedule(&adv_start_work, K_MSEC(100));
    } else if (ret < 0) {
        LOG_ERR("bt_le_adv_start failed: %d", ret);
    } else {
        LOG_INF("BLE advertising as \"%s\"", CONFIG_BT_DEVICE_NAME);
    }
}

static void connected(struct bt_conn *conn, uint8_t err)
{
    if (err) {
        LOG_ERR("BLE connect failed: %u", err);
        return;
    }
    LOG_INF("BLE connected");
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
    LOG_INF("BLE disconnected (reason 0x%02x); restarting adv", reason);
    k_work_schedule(&adv_start_work, K_NO_WAIT);
}

static void security_changed(struct bt_conn *conn, bt_security_t level,
                             enum bt_security_err err)
{
    if (err) {
        LOG_WRN("Pairing failed: level %u, err %u", level, err);
    } else {
        LOG_INF("Pairing succeeded: level %u", level);
    }
}

BT_CONN_CB_DEFINE(conn_cbs) = {
    .connected        = connected,
    .disconnected     = disconnected,
    .security_changed = security_changed,
};

/*
 * No bt_conn_auth_cb was registered, which left the SMP "Just Works" confirm
 * step ambiguous rather than cleanly declaring "no input/output" — leading
 * to the peer aborting with BT_SECURITY_ERR_AUTH_REQUIREMENT. Registering
 * pairing_confirm here (auto-accept, since we have no display/buttons to
 * prompt the user) explicitly opts into Just Works pairing.
 */
static void auth_pairing_confirm(struct bt_conn *conn)
{
    bt_conn_auth_pairing_confirm(conn);
}

static void auth_cancel(struct bt_conn *conn)
{
    ARG_UNUSED(conn);
    LOG_WRN("Pairing cancelled");
}

static struct bt_conn_auth_cb auth_cb = {
    .pairing_confirm = auth_pairing_confirm,
    .cancel          = auth_cancel,
};

/* ---- Public API ---- */

int ble_module_init(void)
{
    int ret = bt_enable(NULL);
    if (ret < 0) {
        LOG_ERR("bt_enable failed: %d", ret);
        return ret;
    }

    ret = bt_conn_auth_cb_register(&auth_cb);
    if (ret < 0) {
        LOG_ERR("bt_conn_auth_cb_register failed: %d", ret);
        return ret;
    }

    if (IS_ENABLED(CONFIG_BT_SETTINGS)) {
        settings_load();
    }

    ret = bt_le_adv_start(&adv_param, ad, ARRAY_SIZE(ad), NULL, 0);
    if (ret < 0) {
        LOG_ERR("bt_le_adv_start failed: %d", ret);
        return ret;
    }

    LOG_INF("BLE advertising as \"%s\"", CONFIG_BT_DEVICE_NAME);
    return 0;
}

void ble_notify_battery(uint8_t percent)
{
    bt_bas_set_battery_level(percent);
}

void ble_set_notification_text(const char *text, uint16_t len)
{
    uint16_t n = MIN(len, sizeof(notif_buf) - 1);
    memcpy(notif_buf, text, n);
    notif_buf[n] = '\0';
    notif_len = n;
    LOG_INF("BLE notification text set: \"%s\"", notif_buf);
    watchface_show_notification((const char *)notif_buf);
}
