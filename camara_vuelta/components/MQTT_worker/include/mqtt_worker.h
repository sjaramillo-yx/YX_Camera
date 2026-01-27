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
#include <mbedtls/pk.h>
#include <mbedtls/x509_crt.h>
#include <mqtt_client.h>
/* Standard includes*/
#include <cJSON.h>
/* FreeRTOS includes*/
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
/* Custom includes */
#include "recording_events.h"

#pragma once

/**
 * @brief Initialize the worker.
 *
 * @param free_chunk_queue The queue used to receive free chunks from the OTA Manager.
 * @param filled_chunk_queue The queue to send filled chunks to the OTA Manager.
 */
esp_err_t mqttworker_init(QueueHandle_t free_chunk_queue, QueueHandle_t filled_chunk_queue);

/**
 * @brief Connect the worker to AWS.
 */
esp_err_t mqttworker_begin(int timeout_ms);

/**
 * @brief Stop the MQTT worker.
 */
esp_err_t mqttworker_stop(void);

/**
 * @brief Deinitialize the worker.
 */
esp_err_t mqttworker_deinit(void);

/**
 * @brief Publish initial state information to AWS
 */
esp_err_t mqttworker_publish_initial_state(cJSON *sdJSON, cJSON *vmanJSON);

/**
 * @brief Publish current state information to AWS
 *
 * @param sdJSON A `cJSON` object that contains information about the SD Card.
 * @param is_recording A `bool` indicating wether the camera is currently recording or not.
 */
esp_err_t mqttworker_publish_current_state(cJSON *sdJSON, bool is_recording);

/**
 * @brief Publish ongoing recording state information to AWS
 *
 * @param recJSON A `cJSON` object that contains information about the current recording.
 */
esp_err_t mqttworker_publish_recording_state(cJSON *recJSON);

/**
 * @brief Verify the certificates embedded into the binary image can be parsed.
 */
esp_err_t mqttworker_verify_flash_certs(void);

/**
 * @brief Get the ThingName associated to this device
 *
 * @param[out] thing_name The buffer where the ThingName string will be written to.
 */
esp_err_t mqttworker_get_thingname(char thing_name[128]);

/**
 * @brief Check for pending jobs.
 */
esp_err_t mqttworker_check_for_jobs(void);

/**
 * @brief Reconfigure the TCP keep alive parameters for the MQTT client.
 *
 * @param idle_s Seconds the connection must idle before the first keep alive probe.
 * @param interval_s Time between keepalive probes if there’s no response.
 * @param retries How many failed probes before declaring the connection dead.

 */
esp_err_t mqttworker_configure_tcp_keep_alive(int idle_s, int interval_s, int retries);