# Build notes

Some build settings were changed in `idf.py menuconfig` to allow proper device operation. This page notes the most impactful changes so if you ever get a weird error, you can check if it is actually a feature. *Hooray...*

## Possible weird error changes

* [CONFIG_LIBC_PICOLIBC=y](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c5/api-guides/performance/size.html#picolibc-instead-of-newlib)
  * **Description**: Switch from Newlib to Picolibc C library.
  * **Why**: Picolibc C library provides smaller printf family functions and can reduce the binary size by up to 30 KB, depending on your application.
  * **Impact**: Option is experimental in ESP-IDF v5.5.2.

* [CONFIG_HEAP_PLACE_FUNCTION_INTO_FLASH=y](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c5/api-guides/performance/size.html#heap)
  * **Description**: Forces the entire heap component to be placed in flash memory.
  * **Why**: Reduce the IRAM usage and binary size.
  * **Impact**: It is only safe to enable this configuration if no functions from esp_heap_caps.h or esp_heap_trace.h are called from IRAM ISR which runs when cache is disabled.

* [CONFIG_FREERTOS_PLACE_FUNCTIONS_INTO_FLASH=y](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c5/api-guides/performance/ram-usage.html#optimizing-iram-usage)
  * **Description**: Non-ISR FreeRTOS functions will be placed into Flash memory instead of IRAM.
  * **Why**: Saves up to 8KB of IRAM depending on which functions are used.
  * **Impact**: Provided these functions are not incorrectly used from ISRs, this option is safe to enable in all configurations.
  