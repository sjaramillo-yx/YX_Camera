/**
 * @file provision_claimer.h
 * @date September 2025
 * @author Simón Jaramillo <sjaramillo@yx.cl>
 * 
 * @brief Declarations for a state machine in charge of claiming a certificate
 */

#pragma once

/* Espressif includes */
#include <cJSON.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_system.h>
#include <mqtt_client.h>
/* Standard includes*/
/* FreeRTOS includes*/
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
/* Custom includes */

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
 * @brief The state of the state machine
 */
static ClaimerState state = UNINITIALIZED;

/**
 * @brief Provision the device by claim
 * 
 * @param client
 */
esp_err_t provision_begin(esp_mqtt_client_handle_t client);