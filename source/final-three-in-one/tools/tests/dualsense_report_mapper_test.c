#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dualsense_report_mapper.h"

static int s_failures;
static int64_t s_now_us;
static void expect_i16(const char *name, int16_t expected, int16_t actual);

int64_t esp_timer_get_time(void)
{
    return s_now_us;
}

void dualsense_report_make_neutral(uint8_t report[DUALSENSE_INPUT_PAYLOAD_SIZE])
{
    memset(report, 0, DUALSENSE_INPUT_PAYLOAD_SIZE);
    report[0] = 0x80;
    report[1] = 0x80;
    report[2] = 0x80;
    report[3] = 0x80;
    report[7] = 0x08;
    report[25] = 0x00;
    report[26] = 0xe0;
}

void switch2_state_to_internal(const switch2_state_t *src,
                               internal_gamepad_state_t *dst)
{
    (void)src;
    internal_gamepad_state_reset(dst);
}

static int16_t read_i16_le(const uint8_t *src)
{
    return (int16_t)((uint16_t)src[0] | ((uint16_t)src[1] << 8));
}

static uint32_t read_u32_le(const uint8_t *src)
{
    return (uint32_t)src[0] | ((uint32_t)src[1] << 8) |
           ((uint32_t)src[2] << 16) | ((uint32_t)src[3] << 24);
}

static void expect_u32(const char *name, uint32_t expected, uint32_t actual)
{
    if (expected != actual) {
        fprintf(stderr, "FAIL %s: expected %lu, got %lu\n", name,
                (unsigned long)expected, (unsigned long)actual);
        s_failures++;
    }
}

static void test_elapsed_sensor_clock(void)
{
    uint8_t report[DUALSENSE_INPUT_PAYLOAD_SIZE];
    s_now_us = 9000000000LL;
    dualsense_report_mapper_init();
    dualsense_report_mapper_neutral(report);
    expect_u32("Initial sensor clock", 10200000u, read_u32_le(report + 27));
    uint32_t previous = read_u32_le(report + 27);
    // Regular USB polls, a missed poll, a failed submission/retry, and the
    // former 50 ms neutral cadence must all describe real elapsed time.
    const uint32_t gaps_us[] = {4000, 4000, 8000, 1000, 50000, 7500};
    for (size_t i = 0; i < sizeof(gaps_us) / sizeof(gaps_us[0]); i++) {
        s_now_us += gaps_us[i];
        dualsense_report_mapper_refresh_timing(report);
        uint32_t current = read_u32_le(report + 27);
        expect_u32("Elapsed sensor clock delta", gaps_us[i] * 3u,
                   current - previous);
        expect_u32("Cached report sequence", (uint32_t)i + 1u, report[6]);
        previous = current;
    }

    // Cross the 32-bit sensor tick wrap, then the 32-bit microsecond wrap.
    s_now_us = 9000000000LL + (int64_t)((UINT32_MAX - 10200000u) / 3u);
    dualsense_report_mapper_refresh_timing(report);
    previous = read_u32_le(report + 27);
    s_now_us += 4000;
    dualsense_report_mapper_refresh_timing(report);
    expect_u32("Sensor clock wrap", 12000u, read_u32_le(report + 27) - previous);
    s_now_us = 9000000000LL + UINT32_MAX - 1000;
    dualsense_report_mapper_refresh_timing(report);
    previous = read_u32_le(report + 27);
    s_now_us += 4000;
    dualsense_report_mapper_refresh_timing(report);
    expect_u32("Microsecond clock wrap", 12000u, read_u32_le(report + 27) - previous);
}

static void test_cached_motion_does_not_recalibrate(void)
{
    internal_gamepad_state_t state;
    internal_gamepad_state_reset(&state);
    state.accel_valid = true;
    state.gyro_valid = true;
    state.accel[2] = 4096;
    state.gyro[0] = 12;
    uint8_t report[DUALSENSE_INPUT_PAYLOAD_SIZE];
    uint8_t original[DUALSENSE_INPUT_PAYLOAD_SIZE];
    dualsense_report_mapper_init();
    dualsense_report_mapper_from_internal(&state, report, NULL);
    memcpy(original, report, sizeof(original));
    for (int i = 0; i < 1000; i++) {
        s_now_us += 4000;
        dualsense_report_mapper_refresh_timing(report);
        for (size_t byte = 0; byte < sizeof(report); byte++) {
            if (byte == 6 || (byte >= 27 && byte <= 30)) continue;
            expect_u32("Cached payload preserved", original[byte], report[byte]);
        }
    }
    dualsense_report_mapper_from_internal(&state, report, NULL);
    expect_i16("Repeated USB polls do not finish calibration", 14,
               read_i16_le(report + 15));
    for (int i = 0; i < 248; i++) {
        dualsense_report_mapper_from_internal(&state, report, NULL);
    }
    expect_i16("250 source updates finish calibration", 0,
               read_i16_le(report + 15));
}

static void expect_i16(const char *name, int16_t expected, int16_t actual)
{
    if (expected == actual) {
        return;
    }

    fprintf(stderr,
            "FAIL %s: expected %d, got %d\n",
            name,
            (int)expected,
            (int)actual);
    s_failures++;
}

static void test_ps5_motion_mapping(void)
{
    internal_gamepad_state_t state;
    internal_gamepad_state_reset(&state);
    state.gyro_valid = true;
    state.accel_valid = true;
    state.gyro[0] = 100;
    state.gyro[1] = 200;
    state.gyro[2] = -300;
    state.accel[0] = 10;
    state.accel[1] = 4096;
    state.accel[2] = 20;

    uint8_t report[DUALSENSE_INPUT_PAYLOAD_SIZE];
    dualsense_input_debug_t debug;
    dualsense_report_mapper_init();
    dualsense_report_mapper_from_internal(&state, report, &debug);

    expect_i16("DualSense gyro X = source X",
               115,
               read_i16_le(report + 15));
    expect_i16("DualSense gyro Y = source Z",
               -345,
               read_i16_le(report + 17));
    expect_i16("DualSense gyro Z = -source Y",
               -230,
               read_i16_le(report + 19));
    expect_i16("DualSense accel X = source X",
               20,
               read_i16_le(report + 21));
    expect_i16("DualSense accel Y = source Z",
               40,
               read_i16_le(report + 23));
    expect_i16("DualSense accel Z = -source Y",
               -8192,
               read_i16_le(report + 25));

    expect_i16("Debug gyro X reports mapped value", 115, debug.gyro[0]);
    expect_i16("Debug gyro Y reports mapped value", -345, debug.gyro[1]);
    expect_i16("Debug gyro Z reports mapped value", -230, debug.gyro[2]);
    expect_i16("Debug accel X reports mapped value", 20, debug.accel[0]);
    expect_i16("Debug accel Y reports mapped value", 40, debug.accel[1]);
    expect_i16("Debug accel Z reports mapped value", -8192, debug.accel[2]);
}

static void test_neutral_gravity(void)
{
    uint8_t report[DUALSENSE_INPUT_PAYLOAD_SIZE];
    dualsense_report_mapper_init();
    dualsense_report_mapper_neutral(report);
    expect_i16("DualSense neutral accel Z is -1g",
               -8192,
               read_i16_le(report + 25));
}

static void test_stationary_gyro_bias_is_removed_once(void)
{
    internal_gamepad_state_t state;
    internal_gamepad_state_reset(&state);
    state.accel_valid = true;
    state.gyro_valid = true;
    state.accel[2] = 4096;
    state.gyro[0] = 12;
    state.gyro[1] = -4;
    state.gyro[2] = -20;

    uint8_t report[DUALSENSE_INPUT_PAYLOAD_SIZE];
    dualsense_report_mapper_init();
    for (int i = 0; i < 250; i++) {
        dualsense_report_mapper_from_internal(&state, report, NULL);
    }
    dualsense_report_mapper_from_internal(&state, report, NULL);
    expect_i16("DualSense calibrated gyro X is zero", 0, read_i16_le(report + 15));
    expect_i16("DualSense calibrated gyro Y is zero", 0, read_i16_le(report + 17));
    expect_i16("DualSense calibrated gyro Z is zero", 0, read_i16_le(report + 19));
}

static void test_stationary_calibration_with_aim_held(void)
{
    internal_gamepad_state_t state;
    internal_gamepad_state_reset(&state);
    state.accel_valid = true;
    state.gyro_valid = true;
    state.accel[2] = 4096;
    state.gyro[0] = 12;
    state.gyro[1] = -4;
    state.gyro[2] = -20;
    state.l2 = INTERNAL_GAMEPAD_TRIGGER_MAX;
    internal_gamepad_state_set_button(&state, INTERNAL_GAMEPAD_BUTTON_L2, true);

    uint8_t report[DUALSENSE_INPUT_PAYLOAD_SIZE];
    dualsense_report_mapper_init();
    for (int i = 0; i < 250; i++) {
        dualsense_report_mapper_from_internal(&state, report, NULL);
    }
    expect_i16("Aim held: calibrated gyro X", 0, read_i16_le(report + 15));
    expect_i16("Aim held: calibrated gyro Y", 0, read_i16_le(report + 17));
    expect_i16("Aim held: calibrated gyro Z", 0, read_i16_le(report + 19));
    expect_u32("Aim trigger remains held", 255u, report[4]);
    expect_u32("Aim button remains held", 4u, report[8] & 4u);

    // Normal motion after calibration must still pass through immediately.
    state.gyro[2] += 300;
    dualsense_report_mapper_from_internal(&state, report, NULL);
    expect_i16("Aim held: deliberate yaw is preserved", 345,
               read_i16_le(report + 17));
}

static void test_moving_with_aim_held_does_not_calibrate(void)
{
    internal_gamepad_state_t state;
    internal_gamepad_state_reset(&state);
    state.accel_valid = true;
    state.gyro_valid = true;
    state.accel[2] = 4096;
    state.gyro[2] = 1000;
    internal_gamepad_state_set_button(&state, INTERNAL_GAMEPAD_BUTTON_L2, true);
    uint8_t report[DUALSENSE_INPUT_PAYLOAD_SIZE];
    dualsense_report_mapper_init();
    for (int i = 0; i < 300; i++) {
        dualsense_report_mapper_from_internal(&state, report, NULL);
    }
    expect_i16("Moving while aiming does not become zero", 1150,
               read_i16_le(report + 17));
}

static void test_i16_min_negation_saturates(void)
{
    internal_gamepad_state_t state;
    internal_gamepad_state_reset(&state);
    state.gyro_valid = true;
    state.gyro[0] = INT16_MIN;

    uint8_t report[DUALSENSE_INPUT_PAYLOAD_SIZE];
    dualsense_report_mapper_init();
    dualsense_report_mapper_from_internal(&state, report, NULL);

    expect_i16("DualSense gyro X preserves INT16_MIN when X is not inverted",
               INT16_MIN,
               read_i16_le(report + 15));
}

int main(void)
{
    test_ps5_motion_mapping();
    test_i16_min_negation_saturates();
    test_neutral_gravity();
    test_stationary_gyro_bias_is_removed_once();
    test_elapsed_sensor_clock();
    test_cached_motion_does_not_recalibrate();
    test_stationary_calibration_with_aim_held();
    test_moving_with_aim_held_does_not_calibrate();

    if (s_failures != 0) {
        return 1;
    }

    puts("dualsense report mapper tests passed");
    return 0;
}
