/**
 * @defgroup recording_events Recording Events Component
 * @file recording_events.h
 * @date November 2025
 * @author Simón Jaramillo <sjaramillo@yx.cl>
 *
 * @brief This header provides a simple event loop for communicating recording events
 *
 * @ingroup recording_events
 */

/* Espressif includes */
#include <esp_check.h>
#include <esp_err.h>
#include <esp_event.h>
#include <esp_log.h>
/* Standard includes*/
/* FreeRTOS includes*/
/* Custom includes */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Declare the event base */
ESP_EVENT_DECLARE_BASE(RECORDING_EVENTS);

/**
 * @brief All the possible recording events
 */
typedef enum {
  REC_BEGIN,
  REC_STARTED,
  REC_STOP,
  REC_DONE,
  REC_ERROR,
} recording_event_t;

/**
 * @brief Recording configuration event data structure
 */
typedef struct {
  uint16_t hres;                 // Horizontal resolution
  uint16_t vres;                 // Vertical resolution
  uint16_t fps;                  // Frames per second
  uint8_t  qp_max;               // Maximum quantization parameter
  uint8_t  qp_min;               // Minimum quantization parameter
  uint32_t timeout_seconds;      // Maximum video duration in seconds
  uint32_t target_bitrate;       // Target bitrate in bits per second
  char     transaction_id[128];  // Transaction ID for this recording
  bool     has_aws_job;          // Wether an AWS job is associated to this recorrding
  char     aws_job_id[65]        // Associated AWS job ID for this recording (if any)
} recording_conf_t;

/**
 * @brief Recording resulting file data structure
 */
typedef struct {
  char     filename[128];     // Name of the file where the recording was saved to
  uint64_t size;              // Size in bytes of the resulting file
  uint64_t recorded_seconds;  // How many seconds long the recording was
} recording_file_t;

/**
 * @brief Recording error data structure
 */
typedef struct {
  esp_err_t error_code;           // ESP IDF error code
  char      error_message[256];   // Message related to this error
  char      errored_module[128];  // Where this error was originated
} recording_error_t;

/**
 * @brief Create the recording event loop
 */
esp_err_t rec_eventloop_create();

/**
 * @brief Get the recording event loop handle
 */
esp_err_t rec_eventloop_get_handle(esp_event_loop_handle_t *out_handle);

#ifdef __cplusplus
}
#endif