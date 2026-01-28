/**
 * @defgroup command_events Configuration Events Component
 * @file command_events.h
 * @date January 2026
 * @author Simón Jaramillo <sjaramillo@yx.cl>
 *
 * @brief This header provides a simple event loop for communicating command events.
 *
 * @ingroup command_events
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
ESP_EVENT_DECLARE_BASE(COMMAND_EVENTS);

/**
 * @brief All the possible command events
 */
typedef enum command_event_t {
  CMD_REBOOT,     // Reboot the camera.
  CMD_FORMAT_SD,  // Format the SD card.
  CMD_ERROR,      // An error occurred when executing the command.
  CMD_DONE,       // Command successfully executed.
} command_event_t;

/**
 * @brief Create the commands event loop
 */
esp_err_t cmd_eventloop_create(void);

/**
 * @brief Get the commands event loop handle
 *
 * @param[out] out_handle A pointer to an `esp_event_loop_handle_t` where the eventloop handle will
 * be stored.
 */
esp_err_t cmd_eventloop_get_handle(esp_event_loop_handle_t *out_handle);

#ifdef __cplusplus
}
#endif
