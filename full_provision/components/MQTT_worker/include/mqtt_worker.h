/**
 * @file mqtt_worker.h
 * @date September 2025
 * @author Simón Jaramillo <sjaramillo@yx.cl>
 * 
 * @brief This header defines a simple MQTT worker for AWS IoT Core
 */

/* Espressif includes */
#include <esp_err.h>
#include <esp_log.h>
#include <esp_system.h>
#include <mqtt_client.h>
/* Standard includes*/
/* FreeRTOS includes*/
/* Custom includes */

/**
 * @brief Initialize the worker
 */
esp_err_t mqttworker_begin(void);