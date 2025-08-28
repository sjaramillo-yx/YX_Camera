/**
 * @file provision_claimer.h
 * @date August 2025
 * @author Simón Jaramillo <sjaramillo@yx.cl>
 * 
 * @brief Declarations for a state machine
 */

#pragma once

/* Espressif includes */
#include <esp_err.h>
#include <esp_log.h>
#include <esp_system.h>
#include <cJSON.h>
/* Standard includes*/
/* FreeRTOS includes*/
/* Custom includes */


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
  DONE
} ClaimerState;

/**
 * @brief The state of the state machine
 */
static ClaimerState state = UNINITIALIZED;

/**
 * @brief Resets the provision claimer to its initial state
 */
void reset_claimer();

/**
 * @brief Initialize the MQTT client and register handlers
 */
void provisioner_start(void);