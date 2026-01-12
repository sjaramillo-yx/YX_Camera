/**
 * @file s3_uploader.h
 * @date December 2025
 * @author Simón Jaramillo <sjaramillo@yx.cl>
 *
 * @brief Multipart S3 uploader driven by the recordings topic pattern and presigned URLs.
 *
 * Device subscription rule:
 *   - Camera subscribes to START always
 *   - Camera subscribes to COMMANDS only for the currently active recordingId
 *
 * High-level workflow:
 *   1) Backend publishes to START with {recordingId, uploadId} to begin upload.
 *   2) Camera checks local recording presence:
 *        - If missing: publish STATUS error and do not proceed.
 *        - If present: publish STATUS "waiting_init".
 *   3) Backend publishes init info to COMMANDS for that recordingId:
 *        - bucket, key, part_size, total_parts
 *   4) Backend publishes each part info to COMMANDS:
 *        - part_number, url
 *      Camera uploads the part via HTTP PUT and publishes STATUS part_uploaded with ETag.
 *   5) After last part, camera publishes STATUS all_parts_uploaded (including parts+etags).
 *
 * @ingroup mqtt_worker
 */
#pragma once

/* Espressif includes */
#include <esp_check.h>
#include <esp_err.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <mqtt_client.h>
/* Standard includes*/
#include <cJSON.h>
/* FreeRTOS includes*/
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
/* Custom includes */
#include "SD_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief A configuration structure for the S3 uploader
 */
typedef struct {
  const char *thing_name;       /*!< Thing name used to build topics (required) */
  const char *rec_dir;          /*!< Directory where <recordingId>.bin is stored (required) */
  int         http_timeout_ms;  /*!< HTTP timeout per part PUT */
  int         http_put_retries; /*!< Retries for HTTP PUT per part */
} s3uploader_cfg_t;

/**
 * @brief Initialize the S3 uploader. This will configure it's static variables and configure the
 * HTTP client.
 *
 * @param mqtt_client The MQTT client used to receive commands and publish status messages
 * @param cfg The configuration for the S3 uploader.
 */
esp_err_t s3uploader_init(esp_mqtt_client_handle_t mqtt_client, const s3uploader_cfg_t *cfg);

/**
 * @brief Handle incoming MQTT messages for the S3 Uploader.
 *
 * @param topic The topic where the message was received, null terminated.
 * @param data A pointer to the payload.
 * @param data_len The length of the payload buffer
 */
esp_err_t s3_uploader_handler(const char *topic, const char *data, int data_len);

/**
 * @brief Subscribe to relevant topics on MQTT client connection.
 */
esp_err_t s3_uploader_on_connected(void);

/**
 * @brief Simple helper to check if an upload is currently in progress
 */
bool s3_uploader_is_busy(void);

#ifdef __cplusplus
}
#endif
