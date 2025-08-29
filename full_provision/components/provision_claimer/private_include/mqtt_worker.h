/**
 * @file mqtt_worker.h
 * @date August 2025
 * @author Simón Jaramillo <sjaramillo@yx.cl>
 * 
 * @brief Private header for MQTT functionality of provision-claimer
 */

#pragma once

/* Espressif includes */
#include <esp_err.h>
#include <esp_log.h>
#include <esp_system.h>
#include <mqtt_client.h>
/* Standard includes*/
/* FreeRTOS includes*/
/* Custom includes */

/**
 * @brief Update the MQTT client certificate
 */
esp_err_t update_cert(char * cert, char * key);


/**
 * @brief Initialize MQTT logic
 */
esp_mqtt_client_handle_t mqtt_app_start(void);