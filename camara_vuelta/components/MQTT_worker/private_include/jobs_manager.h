/**
 * @file jobs_manager.h
 * @date December 2025
 * @author Simón Jaramillo <sjaramillo@yx.cl>
 *
 * @brief This header exposes helper functions for interacting with the
 * AWS IoT Jobs API.
 * @ingroup mqtt_worker
 */

/* Espressif includes */
#include <esp_check.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_system.h>
#include <mbedtls/base64.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <mbedtls/x509_crt.h>
#include <mqtt_client.h>
/* Standard includes*/
#include <cJSON.h>
/* FreeRTOS includes*/
/* Custom includes */
#include "OTA_events.h"

/**
 * @brief Get pending jobs for this Thing
 *
 * @param client_token A pointer to a null-terminated string to use as a client token. This string
 * is arbitrary and can be used to correlate requests and responses. Can be NULL.
 * @param thing_name The name of the Thing to query pending jobs
 */
esp_err_t jobs_get_pending(char *thing_name, char *client_token);

/**
 * @brief Get a specific job's execution status. The response always includes the job document
 * @todo Make the job document optional.
 *
 * @param client_token A null-terminated string to use as a client token. This string
 * is arbitrary and can be used to correlate requests and responses.
 * @param thing_name The name of the Thing to query job status
 * @param job_id The ID of the job to be queried
 */
esp_err_t jobs_describe_job(char *thing_name, char *client_token, char *job_id);

/**
 * @brief Handle incoming MQTT data from a Jobs topic
 *
 * @param thing_name The name of this Thing in AWS IoT.
 * @param data A pointer to the payload of the received message.
 * @param data_len The length of the payload in bytes.
 */
esp_err_t jobs_data_handler(const char *thing_name, const char *data, int data_len);

/**
 * @brief Handle incoming MQTT data from a Streams topic
 *
 * @param thing_name The name of this Thing in AWS IoT.
 * @param data A pointer to the payload of the received message.
 * @param data_len The length of the payload in bytes.
 */
esp_err_t jobs_stream_data_handler(const char *thing_name, const char *data, int data_len);

/**
 * @brief Initialize AWSJobsManager.
 *
 * @param client The MQTT client used to communicate with AWS.
 * @param free_chunk_queue The queue used to receive free chunks from the OTA Manager
 * @param filled_chunk_queue The queue to send filled chunks to the OTA Manager
 */
esp_err_t jobs_init(esp_mqtt_client_handle_t client, QueueHandle_t free_chunk_queue,
                    QueueHandle_t filled_chunk_queue);