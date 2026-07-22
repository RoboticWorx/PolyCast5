#ifndef VERIFY_HARDWARE_H
#define VERIFY_HARDWARE_H

/**
 * @brief Run the boot-time hardware verification and log the results.
 *
 * Probes every external IC with an observable interface (TCA9535, LIS2DH12,
 * MMC5603, SX1262, ST7789, mic, TSOP IR receiver, PSRAM, battery sense)
 * and prints a PASS/WARN/FAIL line per device plus a summary banner, so
 * assembly or connection faults show up in the serial log on the first boot
 * after flashing.
 *
 * Must be called from app_main after all buses/HALs are initialized but
 * BEFORE the application tasks are created: the probes assume they are the
 * only bus users and that no task has claimed the peripherals yet.
 */
void verify_hardware_run(void);

#endif // VERIFY_HARDWARE_H
