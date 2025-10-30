/**
 * @file nvs_manager.h
 * @date August 2025
 * @author Simón Jaramillo <sjaramillo@yx.cl>
 *
 * @brief This header manages Non-Volatile Storage of client certificates and
 * private keys for the authentication of the MQTT Worker.
 * @ingroup mqtt_worker
 */

/* Espressif includes */
#include <esp_err.h>
#include <esp_log.h>
#include <esp_system.h>
#include <nvs_flash.h>
/* Standard includes*/
/* FreeRTOS includes*/
/* Custom includes */

/**
 * @brief A data structure to store certificate data in a blob
 */
typedef struct {
  char client_crt[4096];
  char client_key[4096];
  char cert_id[512];
  char thing_name[512];
} cert_data_t;

/**
 * @brief Initialize the NVS partition
 */
esp_err_t nvsman_begin(void);

/**
 * @brief Save certificate data to the NVS partition
 */
esp_err_t nvsman_save_certs(cert_data_t *new_certs);