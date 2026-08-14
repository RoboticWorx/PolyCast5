#ifndef POLYCAST5_MACROS_H
#define POLYCAST5_MACROS_H


// Task priorities
#define POLYCAST5_PRIORITY_LOW (tskIDLE_PRIORITY + 0)
#define POLYCAST5_PRIORITY_MEDIUM (tskIDLE_PRIORITY + 1)
#define POLYCAST5_PRIORITY_HIGH (tskIDLE_PRIORITY + 2)
#define POLYCAST5_PRIORITY_INTERRUPT (tskIDLE_PRIORITY + 3)

// Memory control - SRAM currently: 171969/53.58 - Flash currently: 2945759 (app)
#define POLYCAST5_USE_PSRAM_BSS EXT_RAM_BSS_ATTR // For zero-initialized memory
#define POLYCAST5_USE_PSRAM_DATA EXT_RAM_NOINIT_ATTR // For non zero-initialized memory (initialized at runtime)

// Debugging
#define POLYCAST5_DEBUG 1 // If debugging: enables all ESP_LOGI/W

#ifdef POLYCAST5_DEBUG
    //#define POLYCAST5_DEBUG_GPIO 1 // If debugging user buttons
    //#define POLYCAST5_DEBUG_ADC 1 // If debugging battery ADC
    //#define POLYCAST5_DEBUG_RAM 1 // Print RAM heap state on boot
    //#define POLYCAST5_DEBUG_FS 1 // Print LittleFS assets size on boot
    //#define POLYCAST5_DEBUG_MAGNETO 1 // If debugging magnetometer
    #define POLYCAST5_DEBUG_CSI 1 // Log Wi-Fi CSI capture health every 256 frames

    //#define POLYCAST5_ESPNOW_DUMP_NVS 1 // Show ESP-NOW NVS state on boot
    //#define POLYCAST5_WIFI_DUMP_NVS 1 // Show Wi-Fi NVS state on boot
    //#define POLYCAST5_WIFI_DUMP_NETWORKS_NVS 1 // Show Wi-Fi NVS state on boot
    
    //#define POLYCAST5_IR_NVS_CLEAR 1 // Clear all IR namespaces 
    //#define POLYCAST5_WIFI_NVS_CLEAR 1 // Clear all Wi-Fi namespaces
    
    //#define POLYCAST5_DIS_SLEEP_TIMER 1 // Disable sleep timer
#endif

// Animation settings
// EXACTLY 3 ASSET-BACKED FLIPBOOKS MUST BE ENABLED: Default build enables CITY, BLACK_HOLE, and MATRIX. To use PYRAMID, disable CITY and vice versa.
// After switching EN macro, deleted the disabled folder from assets/anim/... and copy the new there from anims/...
// WATER is procedural (no LittleFS assets) so it does NOT count against the 3-flipbook limit and can stay enabled alongside them.
#define POLYCAST5_EN_CITY_ANIM 1 // Enables city night homescreen animation (60 frames)
#define POLYCAST5_EN_BLACK_HOLE_ANIM 1 // Enables black hole homescreen animation (18 frames)
#define POLYCAST5_EN_MATRIX_RAIN_ANIM 1 // Enables matrix rain homescreen animation (42 frames)
// #define POLYCAST5_EN_PYRAMID_ANIM 1 // Enables pyramid-alien homescreen animation (56 frames)
#define POLYCAST5_EN_WATER_ANIM 1 // Enables accelerometer-driven water-slosh homescreen animation (procedural, no assets)

// Settings/testing
//#define POLYCAST5_PERSIST_SELECTION_INDEX 1 // Persist selected menu option across NVS and home
//#define POLYCAST5_PASS_DEBUG 1 // Show password logs for debugging
//#define POLYCAST5_CYCLE_RGB_ON_BOOT 1 // Cycle through the RGB LED to make sure it is working
//#define POLYCAST5_CHECK_OTA_ON_CONN 1 // Check for OTA update on regular Wi-Fi connect

//#define POLYCAST5_SRS_CALIBRATING 1 // Calibrate SRS entry table based on const array in srs_memory.c

// Run at 240 MHz for UI perf, dropping to 160 around the only encrypted flash writes (C5 errata)
#define POLYCAST5_CPU_MAX_FREQ_MHZ 240          // chip rev v1.2+ only
#define POLYCAST5_CPU_FLASH_WRITE_FREQ_MHZ 160  // Encrypted-write cap (all revs)

#endif // POLYCAST5_MACROS_H