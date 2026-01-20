/**
 * @defgroup OTA_manager OTA Updates Manager Component
 * @file OTA_manager.h
 * @date January 2026
 * @author Simón Jaramillo <sjaramillo@yx.cl>
 *
 * @brief
 *
 * @ingroup
 */
#pragma once

/* Espressif includes */
#include <esp_check.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
/* Standard includes*/
/* FreeRTOS includes*/
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
/* Custom includes */
#include "NVS_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef esp_err_t (*component_test)(char *);

/**
 * @brief OTA Manager state machine
 */
typedef enum {
  OTA_MAN_UNITIALIZED,
  OTA_MAN_READY,
  OTA_MAN_DOWNLOADING,
  OTA_MAN_DONE,
  OTA_MAN_ERROR
} OTA_state_t;

/**
 * @brief Run a component test. If the test fails, this function will write the resulting error
 * message to NVS before rolling back to previous valid partition.
 *
 * @param test_function A function pointer to the component test to run. This function must receive
 * a char pointer as it's only parameter (where the error message must be written to) and return an
 * `esp_err_t` type value.
 * @param out_ota_rec A pointer to an `ota_record_t` structure already populated with the last OTA
 * flow result information.
 */
esp_err_t otaman_run_test(component_test test_function, ota_record_t *in_ota_rec);

/**
 * @brief Check if OTA Manager can start a new OTA update process.
 *
 * @param image_size The size of the new binary image.
 */
esp_err_t otaman_can_start(uint32_t image_size);

/**
 * @brief Begin the OTA Update process
 */
esp_err_t otaman_start_update(uint32_t image_size);

/**
 * @brief Initalize the OTA Manager
 */
esp_err_t otaman_init(QueueHandle_t *free_chunk_queue, QueueHandle_t *filled_chunk_queue);

#ifdef __cplusplus
}
#endif