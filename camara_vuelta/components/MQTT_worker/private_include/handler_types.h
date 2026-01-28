/**
 * @file handler_types.h
 * @date January 2026
 * @author Simón Jaramillo <sjaramillo@yx.cl>
 *
 * @brief This header declares common types for event handlers. It's meant to be included by source
 * files that define event handlers.
 * @ingroup mqtt_worker
 */
#pragma once

/* Espressif includes */
#include <mqtt_client.h>
/* Standard includes*/
/* FreeRTOS includes*/
/* Custom includes */
/**
 * @brief Context for the event handlers.
 */
typedef struct handler_ctx_t {
  char                      thing_name[128];
  esp_mqtt_client_handle_t *mqtt_client;
} handler_ctx_t;