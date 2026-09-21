#ifndef VERIFY_HARDWARE_H
#define VERIFY_HARDWARE_H

#include "esp_err.h"

/**
 * @brief Run the first phase of boot-time hardware verification.
 *
 * Covers the TCA9535 expander, the nets hanging off it and unit identity.
 * These probes toggle the LCD_RST and SX_NRST lines, so they must run BEFORE
 * lcd_init_driver() and spi_sx126x_init(): resetting an already-initialized
 * panel would leave the screen blank for the rest of the boot.
 *
 * Call from app_main immediately after gpio_utils_init() succeeds. Results are
 * buffered and printed by verify_hardware_run(), so a halt between the two
 * phases loses the report.
 */
void verify_hardware_run_early(void);

/**
 * @brief Run the second phase and print the combined report for both phases.
 *
 * Probes every remaining external IC with an observable interface (PSRAM,
 * LIS2DH12, MMC5603, SX1262, ST7789, mic, TSOP IR receiver, battery sense)
 * and prints a PASS/WARN/FAIL line per device plus a summary banner, so
 * assembly or connection faults show up in the serial log on the first boot
 * after flashing. Also emits a machine-parseable VHR| block for the
 * production fixture.
 *
 * Must be called from app_main after all buses/HALs are initialized but
 * BEFORE the application tasks are created: the probes assume they are the
 * only bus users and that no task has claimed the peripherals yet.
 */
void verify_hardware_run(void);

/**
 * @brief Bus-only self-test for the boot-halt path.
 *
 * Call from app_main when gpio_utils_init() fails, immediately before giving up,
 * passing that function's error code. Records the boot failure and then runs the
 * I2C census, so the one defect that stops the normal run from happening at all
 * still produces a parseable record naming the cause instead of silence.
 *
 * The overall verdict is always FAIL on this path. The census can legitimately
 * pass here - an address probe is the shortest transaction on the bus and
 * survives conditions that a register write does not - so reporting it alone
 * would tell a production fixture that a unit which never started a task is good.
 *
 * @param init_err The error gpio_utils_init() returned.
 */
void verify_hardware_run_bus_only(esp_err_t init_err);

/**
 * @brief Read back the tallies from the last completed run.
 *
 * Valid once verify_hardware_run() has returned; lets lcd_task render the
 * end-of-line operator card without re-running anything. Any pointer may be
 * NULL.
 */
void verify_hardware_get_summary(int *pass, int *warn, int *fail);

#endif // VERIFY_HARDWARE_H
