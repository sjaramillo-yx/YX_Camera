/**
 * @defgroup ethernet_manager Ethernet Manager Component
 * @file ethernet_manager.h
 * @date September 2025
 * @author Simón Jaramillo <sjaramillo@yx.cl>
 *
 * @brief This header provides helper functions to create and bind the ethernet
 * interface.
 *
 * @ingroup ethernet_manager
 */
#pragma once

/* Espressif includes */
#include <esp_check.h>
#include <esp_err.h>
#include <esp_eth_driver.h>
#include <esp_eth_netif_glue.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_netif_sntp.h>
#include <esp_sntp.h>
#include <esp_system.h>
/* Standard includes*/
/* FreeRTOS includes*/
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
/* Custom includes */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the Ethernet driver.
 *
 * @param[out] eth_handle_out A pointer to an `esp_eth_handle_t` where the handle should be stored.
 *
 * @return
 *          - `ESP_OK` on success
 *          - `ESP_ERR_INVALID_ARG` when passed invalid pointers
 *          - `ESP_ERR_NO_MEM` when there is no memory to allocate for Ethernet driver handles array
 *          - `ESP_FAIL` on any other failure
 */
esp_err_t ethman_init(esp_eth_handle_t *eth_handle_out);

/**
 * @brief Deinitialize the Ethernet driver
 *
 * @param eth_handle An `esp_eth_handle_t` handle to deinitialize.
 */
esp_err_t ethman_deinit(esp_eth_handle_t eth_handle);

/**
 * @brief Wait for the "Got IP" event.
 */
esp_err_t ethman_wait_ip(int ms_to_wait);

/**
 * @brief Wait for SNTP synchronization
 */
esp_err_t ethman_wait_sntp(int ms_to_wait);

#ifdef __cplusplus
}
#endif
