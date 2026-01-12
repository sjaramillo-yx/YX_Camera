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
/* Custom includes */
#include "NVS_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef esp_err_t (*component_test)(char *);

/**
 * @brief Run a component test. If the test fails, this function will write the resulting error
 * message to NVS before rolling back to previous valid partition.
 *
 * @param test_function A function pointer to the component test to run. This function must receive
 * a char pointer as it's only parameter (where the error message must be written to) and return an
 * `esp_err_t` type value.
 */
esp_err_t otaman_run_test(component_test test_function);

#ifdef __cplusplus
}
#endif