/**
 * @file WiFi_manager.h
 * @date August 2025
 * @author Simón Jaramillo <sjaramillo@yx.cl>
 * 
 * @brief This file implements a basic WiFi manager. Most of this
 * code was adapted from the documentation examples (https://github.com/espressif/esp-idf/tree/v5.4/examples/wifi)
 */
#pragma once

/* Espressif includes */
#include <esp_err.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif_sntp.h>
#include <esp_sntp.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <lwip/err.h>
#include <lwip/sys.h>
#include <nvs_flash.h>
/* Standard includes */
#include <sys/param.h>
/* FreeRTOS includes */
#include "freertos/event_groups.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
/* Custom includes */

/**
 * @brief A wrapper function to initialize the wifi station
 */
esp_err_t wifi_init_sta();
/**
 * @brief Connect the wifi station to a network using provided credentials
 * @param ssid The SSID of the WiFi AP
 * @param pass The password for the WiFi AP
 */
esp_err_t connect_wifi(char * ssid, char * pass);