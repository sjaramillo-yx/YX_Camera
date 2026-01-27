/**
 * @defgroup configuration_events Configuration Events Component
 * @file configuration_events.h
 * @date January 2026
 * @author Simón Jaramillo <sjaramillo@yx.cl>
 *
 * @brief This header provides a simple event loop for communicating configuration events.
 *
 * @ingroup configuration_events
 */
#pragma once

/* Espressif includes */
#include <esp_check.h>
#include <esp_err.h>
#include <esp_event.h>
#include <esp_log.h>
/* Standard includes*/
/* FreeRTOS includes*/
/* Custom includes */

#ifdef __cplusplus
extern "C" {
#endif

/* Declare the event base */
ESP_EVENT_DECLARE_BASE(CONFIGURATION_EVENTS);

/**
 * @brief All the possible recording events
 */
typedef enum {
  CONF_RECEIVED,  // A new configuration message has been received.
  CONF_APPLIED,   // The received configuration has been successfully applied.
  CONF_REJECTED,  // The configuration values have been rejected.
  CONF_ERROR,     // An error occured when applying the new configuration.
} configuration_event_t;

typedef struct configuration_t {
  int status_period_ms;       // Period between camera status messages.
  int min_sd_free_space_kb;   // Minimum space to keep free in SD Card, in KB.
  int tcp_keep_alive_idle_s;  // Seconds the connection must idle before the first keep alive probe.
  int tcp_keep_alive_interval_s;  // Time between keepalive probes if there’s no response.
  int tcp_keep_alive_retries;     // How many failed probes before declaring the connection dead.
} configuration_t;

/**
 * @brief Create the configuration event loop
 */
esp_err_t conf_eventloop_create(void);

/**
 * @brief Get the configuration event loop handle
 *
 * @param[out] out_handle A pointer to an `esp_event_loop_handle_t` where the eventloop handle will
 * be stored.
 */
esp_err_t conf_eventloop_get_handle(esp_event_loop_handle_t *out_handle);

#ifdef __cplusplus
}
#endif