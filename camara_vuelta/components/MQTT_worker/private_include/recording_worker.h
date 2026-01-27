/**
 * @file recording_worker.h
 * @date December 2025
 * @author Simón Jaramillo <sjaramillo@yx.cl>
 *
 * @brief This header exposes helper functions for interacting with the
 * AWS IoT MQTT recording start/stop API.
 * @ingroup mqtt_worker
 */
#pragma once

/* Espressif includes */
#include <esp_check.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_system.h>
#include <mqtt_client.h>
/* Standard includes*/
#include <cJSON.h>
#include <sys/time.h>
/* FreeRTOS includes*/
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
/* Custom includes */
#include "recording_events.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rec_handler_conf_t {
  char                     thing_name[128];
  esp_mqtt_client_handle_t mqtt_client;
} rec_handler_conf_t;

/**
 * @brief Parse a recording start command and write the corresponding `recording_conf_t` object
 * fields.
 *
 * @param payload A cJSON object built from the MQTT payload of the command.
 * @param out_rec_conf A pointer to a `recording_conf_t` structure where the parsed configuration
 * will be saved. Can't be `NULL`.
 */
esp_err_t parse_recording_start(cJSON *payload, recording_conf_t *out_rec_conf);

/**
 * @brief The event handler for the `REC_STARTED` event.
 */
void rec_started_handler(void *handler_arg, esp_event_base_t event_base, int32_t event_id,
                         void *event_data);

/**
 * @brief The event handler for the `REC_DONE` event.
 */
void rec_done_handler(void *handler_arg, esp_event_base_t event_base, int32_t event_id,
                      void *event_data);

/**
 * @brief The event handler for the `REC_ERROR` event.
 */
void rec_error_handler(void *handler_arg, esp_event_base_t event_base, int32_t event_id,
                       void *event_data);

#ifdef __cplusplus
}
#endif