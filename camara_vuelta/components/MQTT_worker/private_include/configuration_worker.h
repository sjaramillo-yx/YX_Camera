/**
 * @file configuration_worker.h
 * @date January 2026
 * @author Simón Jaramillo <sjaramillo@yx.cl>
 *
 * @brief This header exposes helper functions to manage camera configuration events.
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
#include "configuration_events.h"
#include "handler_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Parse a cJSON payload and extract configuration values if present.
 *
 * @param payload The cJSON payload to be parsed.
 * @param[out] out_conf The `configuration_t` structure to be populated with extracted values.
 */
esp_err_t conf_parse_cjson_payload(cJSON *payload, configuration_t *out_conf);

/**
 * @brief The event handler for the `CONF_APPLIED` event.
 */
void conf_applied_handler(void *handler_arg, esp_event_base_t event_base, int32_t event_id,
                          void *event_data);

/**
 * @brief The event handler for the `CONF_REJECTED` event.
 */
void conf_rejected_handler(void *handler_arg, esp_event_base_t event_base, int32_t event_id,
                           void *event_data);

/**
 * @brief The event handler for the `CONF_ERROR` event.
 */
void conf_error_handler(void *handler_arg, esp_event_base_t event_base, int32_t event_id,
                        void *event_data);

#ifdef __cplusplus
}
#endif