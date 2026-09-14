#ifndef IR_EXP_TASK_H
#define IR_EXP_TASK_H

#include <stdint.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "ir_exp.h"

// ir_exp_status_t.flags
#define IR_EXP_FLAG_IMAGER      (1 << 0) // Thermal array answered and is streaming
#define IR_EXP_FLAG_TOF         (1 << 1) // Rangefinder answered
#define IR_EXP_FLAG_MEDICAL     (1 << 2) // Medical thermometer answered
#define IR_EXP_FLAG_WARMING     (1 << 3) // Awake but still emitting the -273.16 C sentinel
#define IR_EXP_FLAG_STABILIZING (1 << 4) // Inside the array's thermal settling window
#define IR_EXP_FLAG_FAULT       (1 << 5) // A sensor that was answering has gone quiet

// medical_c100 when the first measurement pair has not completed yet
#define IR_EXP_MEDICAL_NONE INT16_MIN

// Plain values only - lcd_task owns every LVGL object, so nothing here may be a pointer.
typedef struct {
    uint32_t seq;          // Bumped on every publish so the UI redraws only on change
    uint8_t  buf_idx;      // Which frame buffer ir_exp_get_frame() should be asked for
    int16_t  center_c50;   // Crosshair temperature, 0.02 C units (centre 2x2 average)
    int16_t  min_c50;      // Frame minimum, 0.02 C units (sentinel pixels excluded)
    int16_t  max_c50;      // Frame maximum, 0.02 C units
    int16_t  t_die_c100;   // Array die temperature, 0.01 C. NOT ambient.
    int32_t  dist_mm;      // Averaged range as displayed, or VL53L4CX_NO_TARGET
    int16_t  medical_c100; // Medical spot temperature, 0.01 C, or IR_EXP_MEDICAL_NONE
    uint8_t  flags;        // IR_EXP_FLAG_*
} ir_exp_status_t;

// Commands accepted on xIrExpCtrlQueue
typedef enum {
    IR_EXP_CMD_START = 0, // Probe, wake and begin streaming
    IR_EXP_CMD_STOP,      // Park every sensor, then acknowledge
    IR_EXP_CMD_PARK,      // Park every part now and free the task (pre-sleep)
} ir_exp_cmd_type_t;

typedef struct {
    uint8_t type; // ir_exp_cmd_type_t
    int16_t arg;  // Unused by every command currently defined
} ir_exp_cmd_t;

// Posted on xIrExpDoneQueue when a command finishes. STOP is acknowledged only after every sensor
// has been parked, which is what makes page teardown safe.
typedef struct {
    uint8_t  cmd;     // Which ir_exp_cmd_type_t completed
    bool     imager;  // Thermal array present
    bool     tof;     // Rangefinder present
    bool     medical; // Medical thermometer present
    bool     ok;      // Command-specific success (e.g. every sensor parked)
    bool     no_mem;  // START only: the frame buffers would not allocate
    uint32_t scl_hz;  // Bus clock the devices actually negotiated
} ir_exp_done_t;

extern QueueHandle_t xIrExpStatusQueue; // Length 1, xQueueOverwrite / xQueuePeek
extern QueueHandle_t xIrExpCtrlQueue;   // Commands in
extern QueueHandle_t xIrExpDoneQueue;   // Completion acknowledgements out

// Everything the rangefinder reports beyond the headline distance - every target, the error bars,
// the rates and the raw histogram. Length 1, xQueueOverwrite / xQueuePeek, kept apart from the
// status queue because it is 196 bytes against that struct's few dozen. Only written while the
// rangefinder is present.
extern QueueHandle_t xIrExpTofDetailQueue;

/**
 * @brief Create the expansion producer task, on entry to its page rather than at boot
 *
 *        Most units have no expansion module attached, so the 8 kB stack is allocated only while
 *        the page is open: the task frees itself once the session has been acknowledged as
 *        stopped. The command and status queues are created on the first call and then kept for
 *        the life of the firmware, so the page and the sleep path can always send.
 *
 *        Always spawns a fresh task. A task still alive here belongs to a teardown whose
 *        acknowledgement the previous visit gave up on, and it will read exactly one more command
 *        before deleting itself - so this waits for it to finish, then empties both queues before
 *        spawning. That wait is bounded but can block the caller for a couple of seconds, which is
 *        why the page paints its "Detecting module..." card first.
 *
 * @return True if a fresh task is up and the queues are clean. False means no command should
 *         be sent: either the queues would not allocate, the stack would not allocate, or a
 *         previous producer is wedged and never finished tearing down.
 */
bool ir_exp_task_create(void);

/**
 * @brief Whether the producer task currently exists
 *
 *        Keeps page teardown idempotent: the task frees itself once a STOP has been acknowledged,
 *        so a second STOP would sit unread on the command queue and be dequeued by the NEXT
 *        visit's task, tearing that session down before it began.
 *
 * @return True while the task is up
 */
bool ir_exp_task_is_running(void);

/**
 * @brief Put the whole expansion module into its low-power state before sleeping
 *
 *        MUST be called before entering light sleep, and before taking xI2CBusMutex to do so. If a
 *        session is live this hands the work to the producer task and waits for its
 *        acknowledgement; if no task exists it addresses the parts directly, which is the only
 *        thing that can reach a module whose page has never been opened.
 *
 *        Both Melexis parts power on measuring - the array at ~28 mA and the spot sensor at ~1 mA -
 *        and their driver handles being NULL says nothing about the hardware, so without this a
 *        module attached at the factory and never used would draw ~29 mA through every light sleep.
 *
 *        Bounded: it gives up rather than blocking the sleep path if the producer does not answer,
 *        and is cheap on units with no module (the writes simply NACK).
 */
void ir_exp_task_park(void);

/**
 * @brief Park the module without a producer task, waking nothing that is already asleep
 *
 *        Call once at boot so a never-opened module does not measure indefinitely, and let
 *        ir_exp_task_park() reach it on every later sleep. Does not use the drivers' init paths:
 *        the array's sends the wake byte and the spot sensor's forces continuous mode, so probing
 *        through them costs more power than it saves.
 *
 *        Takes xI2CBusMutex itself, so it must NOT be called with the bus already held.
 */
void ir_exp_cold_park_all(void);

/**
 * @brief Borrow a published frame buffer for reading.
 *
 *        The producer only ever writes the buffer it did not last publish, so the index carried in
 *        ir_exp_status_t stays stable until the next publish.
 *
 * @param [in] idx  buf_idx from the matching ir_exp_status_t
 *
 * @return Pointer to MLX90642_PIXELS entries in 0.02 C units, or NULL if not allocated
 */
const int16_t *ir_exp_get_frame(uint8_t idx);

#endif // IR_EXP_TASK_H
