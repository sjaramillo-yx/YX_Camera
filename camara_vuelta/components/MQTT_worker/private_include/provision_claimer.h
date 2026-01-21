/**
 * @file provision_claimer.h
 * @date September 2025
 * @author Simón Jaramillo <sjaramillo@yx.cl>
 *
 * @brief Declarations for a state machine in charge of claiming a certificate.
 * @ingroup mqtt_worker
 */

#pragma once

/* Espressif includes */
#include <cJSON.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_system.h>
#include <mqtt_client.h>
/* Standard includes*/
/* FreeRTOS includes*/
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
/* Custom includes */
#include "NVS_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief A simple structure to pair an MQTT client and configuration
 */
typedef struct {
  esp_mqtt_client_handle_t client;
  esp_mqtt_client_config_t conf;
} mqtt_args_t;

/**
 * @brief The states that the state machine will
 * cycle through during the provisioning process
 */
typedef enum {
  UNINITIALIZED,
  SUSCRIBED_CERT_CREATION,
  GOT_CERTIFICATE,
  SUSCRIBED_THING_CREATION,
  CREATED_THING,
  DONE,
  ERROR
} ClaimerState;

/**
 * @brief Provision the device by claim
 *
 * @param client The MQTT Client used to connect to AWS.
 * @param cert_data A pointer to a `cert_data_t` structure to be populated and then written to NVS.
 */
esp_err_t provision_begin(esp_mqtt_client_handle_t client, cert_data_t *cert_data);

#ifdef __cplusplus
}
#endif
