/**
 * @defgroup OTA_events OTA Events Component
 * @file OTA_events.h
 * @date January 2026
 * @author Simón Jaramillo <sjaramillo@yx.cl>
 *
 * @brief This header provides a simple event loop for communicating OTA download events
 *
 * @ingroup OTA_events
 */
#pragma once

/* Espressif includes */
#include <esp_check.h>
#include <esp_err.h>
#include <esp_event.h>
#include <esp_log.h>
#include <mqtt_client.h>
/* Standard includes*/
/* FreeRTOS includes*/
/* Custom includes */

#ifdef __cplusplus
extern "C" {
#endif

/* Declare the event base */
ESP_EVENT_DECLARE_BASE(OTA_EVENTS);

/**
 * @brief A data structure for OTA streams
 */
typedef struct ota_stream_t {
  esp_mqtt_client_handle_t client;
  char                     thing_name[128];
  char                     stream_id[128];
  char                     job_id[128];
  int                      stream_version;
  int                      file_index;
  uint32_t                 filesize;
  char                     file_signature[128];
} ota_stream_t;

/**
 * @brief A data structure for OTA chunks
 */
typedef struct ota_chunk_t {
  int      index;
  size_t   len;
  uint8_t *data;  // Pointer to a chunk buffer
  bool     last;
} ota_chunk_t;

/**
 * @brief All the possible OTA events
 */
typedef enum {
  OTA_JOB_RECEIVED = 1,  // MQTT Worker received a new OTA Job.
  OTA_CTRL_START,        // OTA Manager has accepted the job, begin download.
  OTA_JOB_REJECTED,      // OTA Manager has rejected the OTA Job.
  OTA_CTRL_PAUSE,
  OTA_CTRL_RESUME,
  OTA_CTRL_CANCEL,
  OTA_CTRL_DONE,  // OTA Manager has finished applying the OTA update.
  OTA_JOB_DONE,   // OTA partition has been validated, job finished.
  OTA_JOB_ERROR,
} OTA_event_t;

/**
 * @brief Create the OTA event loop
 */
esp_err_t OTA_eventloop_create();

/**
 * @brief Get the OTA event loop handle
 */
esp_err_t OTA_eventloop_get_handle(esp_event_loop_handle_t *out_handle);

#ifdef __cplusplus
}
#endif