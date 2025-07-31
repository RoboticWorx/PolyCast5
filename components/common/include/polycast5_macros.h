#ifndef POLYCAST5_MACROS_H
#define POLYCAST5_MACROS_H


#define POLYCAST5_DEBUG 1 // If debugging

#ifdef POLYCAST5_DEBUG
	//#define POLYCAST5_DEBUG_GPIO 1 // If debugging user buttons
	//#define POLYCAST5_DEBUG_ADC 1 // If debugging battery ADC
	//#define POLYCAST5_DEBUG_RAM 1 // Print RAM heap state on boot
	
	//#define POLYCAST5_ESPNOW_DUMP_NVS 1 // Show ESP-NOW NVS state on boot
	//#define POLYCAST5_WIFI_DUMP_NVS 1 // Show Wi-Fi NVS state on boot
	
	//#define POLYCAST5_IR_NVS_CLEAR 1 // Clear all IR namespaces 
	//#define POLYCAST5_WIFI_NVS_CLEAR 1 // Clear all Wi-Fi namespaces
	
	#define POLYCAST5_DIS_SLEEP_TIMER 1 // Disable sleep timer
#endif

//#define POLYCAST5_CYCLE_RGB_ON_BOOT 1 // Cycle through the RGB LED to make sure it is working


#endif // POLYCAST5_MACROS_H