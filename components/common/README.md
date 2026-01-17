# Build notes

Some build settings were changed in `idf.py menuconfig` to allow proper device operation. This page notes the most impactful changes so if you ever get a weird error, you can check if it is actually a feature. *Hooray...*

## Changes

* [CONFIG_HEAP_PLACE_FUNCTION_INTO_FLASH=y](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c5/api-guides/performance/size.html#heap)
  * Description: Forces the entire heap component to be placed in flash memory.
  * Why: Reduce the IRAM usage and binary size.
  * Impact: It is only safe to enable this configuration if no functions from esp_heap_caps.h or esp_heap_trace.h are called from IRAM ISR which runs when cache is disabled.

* [CONFIG_ESP_WIFI_ENABLE_WPA3_SAE is not set](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c5/api-guides/performance/size.html#wi-fi)
  * Description: WPA3 support is disabled for Wi-Fi
  * Why: Not needed, can save some Wi-Fi binary size.
  * Impact: WPA3 is needed for some new Wi-Fi device certifications.
