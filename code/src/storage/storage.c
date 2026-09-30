#include "storage/storage.h"

#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/devicetree/fixed-partitions.h>
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(storage, LOG_LEVEL_INF);

#define SETTINGS_PATH "/lfs/settings.bin"

/*
 * NCS PM overrides FIXED_PARTITION_ID(label) → PM_##label##_ID, but PM does
 * not auto-generate IDs for external SPI flash DT partitions.  Use
 * DT_FIXED_PARTITION_ID() directly (= DT_DEP_ORD of the node) which bypasses
 * the PM macro.  CONFIG_FLASH_MAP_LABELS=y makes flash_area_open() accept
 * these DT-derived IDs at runtime.
 */
#define EXT_STORAGE_AREA_ID \
    DT_FIXED_PARTITION_ID(DT_NODELABEL(ext_storage_partition))

FS_LITTLEFS_DECLARE_DEFAULT_CONFIG(lfs_cfg);

static struct fs_mount_t lfs_mnt = {
    .type        = FS_LITTLEFS,
    .fs_data     = &lfs_cfg,
    .storage_dev = (void *)(uintptr_t)EXT_STORAGE_AREA_ID,
    .mnt_point   = "/lfs",
};

static bool mounted;

int storage_module_init(void)
{
    int ret = fs_mount(&lfs_mnt);
    if (ret < 0) {
        LOG_ERR("LittleFS mount failed: %d", ret);
        return ret;
    }
    mounted = true;
    LOG_INF("LittleFS mounted on MX25U51245G (/lfs)");
    return 0;
}

int storage_load_settings(struct watch_settings *s)
{
    if (!mounted) return -ENODEV;

    struct fs_file_t f;
    fs_file_t_init(&f);

    int ret = fs_open(&f, SETTINGS_PATH, FS_O_READ);
    if (ret < 0) {
        /* First boot: populate defaults */
        s->tz_offset_hours = 0;
        s->step_goal       = 10000;
        s->brightness      = 80;
        return 0;
    }

    ssize_t n = fs_read(&f, s, sizeof(*s));
    fs_close(&f);

    if (n != sizeof(*s)) {
        LOG_WRN("Settings file truncated (%zd); using defaults", n);
        s->tz_offset_hours = 0;
        s->step_goal       = 10000;
        s->brightness      = 80;
    }
    return 0;
}

int storage_save_settings(const struct watch_settings *s)
{
    if (!mounted) return -ENODEV;

    struct fs_file_t f;
    fs_file_t_init(&f);

    int ret = fs_open(&f, SETTINGS_PATH, FS_O_WRITE | FS_O_CREATE | FS_O_TRUNC);
    if (ret < 0) {
        LOG_ERR("Cannot open settings for writing: %d", ret);
        return ret;
    }

    ssize_t n = fs_write(&f, s, sizeof(*s));
    fs_close(&f);

    if (n != sizeof(*s)) {
        LOG_ERR("Settings write incomplete (%zd/%zu)", n, sizeof(*s));
        return -EIO;
    }
    return 0;
}
