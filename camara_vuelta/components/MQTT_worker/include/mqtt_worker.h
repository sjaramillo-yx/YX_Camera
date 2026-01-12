/**
 * @defgroup mqtt_worker MQTT Worker Component
 * @file mqtt_worker.h
 * @date September 2025
 * @author Simón Jaramillo <sjaramillo@yx.cl>
 *
 * @brief This header defines a simple MQTT worker for AWS IoT Core
 * @ingroup mqtt_worker
 */

/* Espressif includes */
#include <esp_app_desc.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_system.h>
#include <mqtt_client.h>
/* Standard includes*/
#include <cJSON.h>
#include <sys/time.h>
/* FreeRTOS includes*/
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
/* Custom includes */
#include "recording_events.h"

#pragma once

/**
 * @brief Initialize the worker
 */
esp_err_t mqttworker_begin(void);

/**
 * @brief Publish initial state information to AWS
 */
esp_err_t mqttworker_publish_initial_state(cJSON *sdJSON, cJSON *vmanJSON);

/**
 * @brief Publish current state information to AWS
 */
esp_err_t mqttworker_publish_current_state(cJSON *sdJSON, bool is_recording);

/**
 * @brief Publish ongoing recording state information to AWS
 */
esp_err_t mqttworker_publish_recording_state(cJSON *recJSON);