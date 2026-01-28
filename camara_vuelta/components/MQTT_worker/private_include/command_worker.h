/**
 * @file command_worker.h
 * @date January 2026
 * @author Simón Jaramillo <sjaramillo@yx.cl>
 *
 * @brief This header exposes helper functions to manage camera command events.
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
/* FreeRTOS includes*/
/* Custom includes */
#include "command_events.h"
#include "handler_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief The event handler for the `CMD_DONE` event.
 */
void cmd_done_handler(void *handler_arg, esp_event_base_t event_base, int32_t event_id,
                      void *event_data);

/**
 * @brief The event handler for the `CMD_ERROR` event.
 */
void cmd_error_handler(void *handler_arg, esp_event_base_t event_base, int32_t event_id,
                       void *event_data);

/**
 * @brief Parse a cJSON payload for a command and publish the corresponding event.
 *
 * @param payload The cJSON payload to be parsed.
 * @param event_loop_handle An `esp_event_loop_handle_t` where the command events will be posted to.
 */
esp_err_t cmd_parse_cjson_payload(cJSON *payload, esp_event_loop_handle_t event_loop_handle);

#ifdef __cplusplus
}
#endif