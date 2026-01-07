#ifndef INFRARED_FUNCS_H
#define INFRARED_FUNCS_H

#include "driver/rmt_rx.h"
#include "driver/rmt_tx.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stddef.h>

// Configuration macros
#define RMT_RX_GPIO 24
#define RMT_TX_GPIO 12
#define RMT_RESOLUTION_HZ 1000000 // 1us resolution

#define MAX_PULSES 128
#define PULSE_BLOCK 96
#define MIN_VALID_PULSES 10

#define FINAL_GAP_US 10000 // 10ms for final gap

#define RANDOM_TX_THRESHOLD 15
#define RANDOM_TX_DELAY_MS 3000

#define MAX_STORED_SIGNALS 1650 // 30 signals per remote if 50 (33 - 3 default)
#define MAX_REMOTES 50

// Stored signal structure
typedef struct {
    size_t length;
    rmt_symbol_word_t pulses[];
} ir_signal_t;

// Remote structure
typedef struct ir_remote {
    char *name;
    char **signal_names;
    ir_signal_t **signals;
    size_t num_signals;
} ir_remote_t;

extern ir_remote_t remotes[MAX_REMOTES];
extern size_t num_remotes;
extern size_t ir_current_remote;

/** 
 * @brief Initialise RMT RX
 */
void infrared_init_rx(void);

/** 
 * @brief Initialise RMT TX
 */
void infrared_init_tx(void);

/** 
 * @brief Restart and re-initialize RX config
 */
void infrared_restart_rx(void);

/** 
 * @brief Disable RX config
 */
void infrared_disable_rx(void);

/**
 * @brief Transmit infrared signal
 *
 * @param [in] rmt_symbol_word_t Signal to transmit
 * @param [in] length Length of signal to transmit
 */
void infrared_transmit_ir(rmt_symbol_word_t *signal, size_t length);

/** 
 * @brief Ensure there is enough space to add a new signals
 */
bool infrared_ensure_capacity(void);

/** 
 * @brief Loads remotes and signals from NVS flash
 */
void infrared_nvs_load_remotes(void);

/** 
 * @brief Saves a specific signal blob and name for a remote to NVS
 *
 * @param remote_idx Index of the remote
 * @param sig_idx Index of the signal within the remote
 * @param sig The signal data
 * @param name The signal name
 */
void infrared_nvs_save_signal_to_remote(size_t remote_idx, size_t sig_idx, ir_signal_t *sig, const char *name);

/** 
 * @brief Saves the number of signals for a remote to NVS
 *
 * @param remote_idx Index of the remote
 */
void infrared_nvs_save_remote_nsig(size_t remote_idx);

/** 
 * @brief Saves a remote's name to NVS
 *
 * @param remote_idx Index of the remote
 */
void infrared_nvs_save_remote_name(size_t remote_idx);

/** 
 * @brief Saves all remotes and signals to NVS
 */
void infrared_nvs_save_all_remotes(void);

/** 
 * @brief Deletes a given signal from a given remote over NVS
 *
 * @param remote_idx Index of the remote
 * @param sig_idx Index of the signal to delete
 */
void infrared_nvs_delete_signal_from_remote(size_t remote_idx, size_t sig_idx);

/** 
 * @brief Deletes a given remote from NVS
 *
 * @param remote_idx Index of the remote to delete
 */
void infrared_nvs_delete_remote(size_t remote_idx);

/** 
 * @brief Clear all IR data from NVS
 */
void infrared_clear_nvs(void);

#endif // INFRARED_FUNCS_H