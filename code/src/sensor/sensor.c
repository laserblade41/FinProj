/*
 * IMU subsystem: BMI270.
 *
 * Motion wake is interrupt-driven. The sensor's any-motion feature runs
 * continuously in the accelerometer's low-power path and asserts INT1 when the
 * slope of the acceleration exceeds a threshold; the application core sleeps
 * until then instead of polling.
 *
 * Steps can only happen while the wrist is moving, so step detection runs only
 * inside an "active window" opened by that interrupt and closed
 * ACTIVE_WINDOW_MS after the last motion event. While still, this thread is
 * blocked on a semaphore and costs nothing.
 *
 * Unlike the part this design originally carried, the Zephyr driver exposes
 * any-motion as a first-class SENSOR_TRIG_MOTION trigger, so there is no need
 * to program feature registers over I2C by hand:
 * CONFIG_BMI270_TRIGGER_GLOBAL_THREAD lets the driver own INT1, read
 * INT_STATUS_0 and dispatch the event. The threshold and duration are set
 * through the standard attribute API before the trigger is armed: the driver
 * caches them and writes the feature registers at trigger-set time, so the
 * order matters.
 *
 * The driver also uploads the sensor's ~8 KB configuration file during init;
 * none of the feature engine works before that completes.
 *
 * If the trigger cannot be armed (pin not wired on a bench setup, or the
 * register writes fail) the subsystem falls back to the previous 10 Hz polling
 * behaviour rather than losing step counting entirely.
 */

#include "sensor/sensor.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(sensor_mod, LOG_LEVEL_INF);

#define SENSOR_STACK_SIZE  2048
#define SENSOR_PRIORITY    8
#define SENSOR_PERIOD_MS   100   /* 10 Hz sampling *within* an active window */

/* How long after the last motion event we keep sampling before going back to
 * sleep. Long enough not to clip the tail of a stride, short enough that a
 * still wrist stops costing CPU almost immediately. */
#define ACTIVE_WINDOW_MS   3000

/* Step detection thresholds (in milli-g) */
#define STEP_HIGH_MG  1300   /* acceleration peak above 1.3 g → step candidate */
#define STEP_LOW_MG   900    /* must drop below 0.9 g to reset */

/*
 * Accelerometer configuration. These were devicetree properties on the previous
 * part; bosch,bmi270.yaml declares only irq-gpios, so they are set at runtime.
 *
 * ±4 g gives much better resolution for step detection than full scale, and
 * step peaks only reach ~1.3 g. 100 Hz is the nearest BMI270 rate to the 104 Hz
 * used before, and is also the point at which the driver selects the
 * performance filter rather than the power-optimised one.
 */
#define ACCEL_RANGE_G     4
#define ACCEL_ODR_HZ      100

/*
 * Any-motion threshold, as the fractional part of one g in micro-g. The driver
 * scales this against a 10-bit field spanning 1 g (~0.49 mg/LSB), so 125000
 * micro-g ≈ 125 mg of slope, a wrist raise clears that comfortably while an
 * arm resting on a desk does not. Note that val1 must stay zero: any positive
 * integer part saturates the threshold to the full 1 g.
 */
#define ANYMO_THRESHOLD_UG   125000

/* Any-motion duration, in ms. The feature register is 20 ms/LSB, so this is one
 * LSB: interrupt on the first qualifying sample rather than after a delay. */
#define ANYMO_DURATION_MS    20

static const struct device *imu_dev =
    DEVICE_DT_GET(DT_NODELABEL(bmi270));

/* The driver keeps this pointer, so it must outlive sensor_module_init(). */
static const struct sensor_trigger motion_trigger = {
    .type = SENSOR_TRIG_MOTION,
    .chan = SENSOR_CHAN_ACCEL_XYZ,
};

static struct accel_data latest_accel;
static volatile uint32_t step_count;
static sensor_motion_cb_t motion_cb;

static struct k_mutex accel_mutex;
static bool imu_ready;

/* Given by the driver's trigger handler, taken by the sensor thread. */
static K_SEM_DEFINE(motion_sem, 0, 1);

/* False when the trigger could not be armed; the thread then reverts to
 * continuous polling. */
static bool use_interrupt;

/* Convert sensor_value (m/s²) to milli-g */
static int32_t ms2_to_mg(const struct sensor_value *v)
{
    /* 1 g = 9.80665 m/s²; val1 in m/s², val2 in µm/s² */
    int64_t us2 = (int64_t)v->val1 * 1000000 + v->val2;
    return (int32_t)(us2 / 9807);
}

static void step_detect(int32_t x_mg, int32_t y_mg, int32_t z_mg)
{
    static bool above_threshold;

    int64_t mag_sq = (int64_t)x_mg * x_mg +
                     (int64_t)y_mg * y_mg +
                     (int64_t)z_mg * z_mg;
    if (!above_threshold && mag_sq > (int64_t)STEP_HIGH_MG * STEP_HIGH_MG) {
        above_threshold = true;
        step_count++;
    } else if (above_threshold && mag_sq < (int64_t)STEP_LOW_MG * STEP_LOW_MG) {
        above_threshold = false;
    }
}

/* Read one accelerometer sample and feed the step detector. */
static void sample_once(void)
{
    struct sensor_value accel[3];

    if (sensor_sample_fetch(imu_dev) != 0 ||
        sensor_channel_get(imu_dev, SENSOR_CHAN_ACCEL_XYZ, accel) != 0) {
        return;
    }

    int32_t x = ms2_to_mg(&accel[0]);
    int32_t y = ms2_to_mg(&accel[1]);
    int32_t z = ms2_to_mg(&accel[2]);

    k_mutex_lock(&accel_mutex, K_FOREVER);
    latest_accel.x_mg = x;
    latest_accel.y_mg = y;
    latest_accel.z_mg = z;
    k_mutex_unlock(&accel_mutex);

    step_detect(x, y, z);
}

/*
 * Trigger handler. Runs on the system work queue rather than in interrupt
 * context, but there is still nothing to do here but hand off to the sensor
 * thread, since the driver has already read and cleared INT_STATUS_0 and confirmed
 * that this was an any-motion event.
 */
static void motion_trigger_handler(const struct device *dev,
                                   const struct sensor_trigger *trig)
{
    ARG_UNUSED(dev);
    ARG_UNUSED(trig);

    k_sem_give(&motion_sem);
}

/* Configure the accelerometer and arm the any-motion trigger on INT1. */
static int configure_motion(void)
{
    struct sensor_value val;
    int ret;

    val.val1 = ACCEL_RANGE_G;   /* in g; val2 unused */
    val.val2 = 0;
    ret = sensor_attr_set(imu_dev, SENSOR_CHAN_ACCEL_XYZ,
                          SENSOR_ATTR_FULL_SCALE, &val);
    if (ret != 0) {
        LOG_WRN("accel range: %d", ret);
        return ret;
    }

    /* Also powers the accelerometer on: a zero ODR clears PWR_CTRL.acc_en. */
    val.val1 = ACCEL_ODR_HZ;
    val.val2 = 0;
    ret = sensor_attr_set(imu_dev, SENSOR_CHAN_ACCEL_XYZ,
                          SENSOR_ATTR_SAMPLING_FREQUENCY, &val);
    if (ret != 0) {
        LOG_WRN("accel ODR: %d", ret);
        return ret;
    }

    /* Threshold and duration must both be set before the trigger is armed: the
     * driver only writes the feature registers from sensor_trigger_set(). */
    val.val1 = 0;
    val.val2 = ANYMO_THRESHOLD_UG;
    ret = sensor_attr_set(imu_dev, SENSOR_CHAN_ACCEL_XYZ,
                          SENSOR_ATTR_SLOPE_TH, &val);
    if (ret != 0) {
        LOG_WRN("any-motion threshold: %d", ret);
        return ret;
    }

    val.val1 = ANYMO_DURATION_MS;
    val.val2 = 0;
    ret = sensor_attr_set(imu_dev, SENSOR_CHAN_ACCEL_XYZ,
                          SENSOR_ATTR_SLOPE_DUR, &val);
    if (ret != 0) {
        LOG_WRN("any-motion duration: %d", ret);
        return ret;
    }

    ret = sensor_trigger_set(imu_dev, &motion_trigger, motion_trigger_handler);
    if (ret != 0) {
        LOG_WRN("any-motion trigger: %d", ret);
        return ret;
    }

    return 0;
}

/*
 * One active window: sample at 10 Hz until ACTIVE_WINDOW_MS passes with no
 * further motion.
 */
static void run_active_window(void)
{
    int64_t last_motion = k_uptime_get();

    while (k_uptime_get() - last_motion < ACTIVE_WINDOW_MS) {
        sample_once();

        if (k_sem_take(&motion_sem, K_MSEC(SENSOR_PERIOD_MS)) == 0) {
            /* Another any-motion event: the wrist is still moving. */
            last_motion = k_uptime_get();
        }
    }
}

static void sensor_thread_fn(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    while (1) {
        if (!imu_ready) {
            k_msleep(SENSOR_PERIOD_MS);
            continue;
        }

        if (!use_interrupt) {
            /* Fallback: the original always-on 10 Hz poll. */
            sample_once();
            k_msleep(SENSOR_PERIOD_MS);
            continue;
        }

        /* Idle: block indefinitely. No wakeups until the IMU says otherwise. */
        k_sem_take(&motion_sem, K_FOREVER);

        if (motion_cb) {
            motion_cb();
        }

        run_active_window();
    }
}

K_THREAD_DEFINE(sensor_tid, SENSOR_STACK_SIZE,
                sensor_thread_fn, NULL, NULL, NULL,
                SENSOR_PRIORITY, 0, 0);

int sensor_module_init(sensor_motion_cb_t cb)
{
    k_mutex_init(&accel_mutex);
    motion_cb = cb;

    if (!device_is_ready(imu_dev)) {
        LOG_ERR("BMI270 not ready");
        return -ENODEV;
    }
    imu_ready = true;

    if (configure_motion() == 0) {
        use_interrupt = true;
        LOG_INF("BMI270 IMU ready (any-motion wake on INT1)");
    } else {
        use_interrupt = false;
        LOG_WRN("BMI270 IMU ready (INT1 unavailable, falling back to polling)");
    }

    return 0;
}

int sensor_get_accel(struct accel_data *out)
{
    k_mutex_lock(&accel_mutex, K_FOREVER);
    *out = latest_accel;
    k_mutex_unlock(&accel_mutex);
    return 0;
}

uint32_t sensor_get_step_count(void)
{
    return step_count;
}

void sensor_reset_step_count(void)
{
    step_count = 0;
}
