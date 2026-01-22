/**
 * @file NVS_manager.h
 * @date January 2026
 * @author Simón Jaramillo <sjaramillo@yx.cl>
 *
 * @brief This header manages Non-Volatile Storage.
 */
#pragma once

/* Espressif includes */
#include <esp_check.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_system.h>
#include <nvs_flash.h>
/* Standard includes*/
/* FreeRTOS includes*/
/* Custom includes */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief A data structure to store certificate data in a blob.
 */
typedef struct cert_data_t {
  char client_crt[4096];
  char client_key[4096];
  char root_ca[4096];
  char cert_id[512];
  char thing_name[128];
  bool populated;
} cert_data_t;

/**
 * @brief A data structure to store OTA records in a blob.
 */
/// TODO: Turn the contents of magic into a Kconfig option
typedef struct ota_record_t {
  uint32_t magic;        // Helps verify integrity of the blob.
  int32_t  esp_err;      // Last error code (esp_err_t fixed to 32 bits).
  char     detail[128];  // Short message (truncate).
  char     job_id[128];  // The job ID for the corresponding AWS OTA Job.
} ota_record_t;

/**
 * @brief Initialize the NVS partition
 */
esp_err_t nvsman_begin(void);

/**
 * @brief Save certificate data to the NVS partition
 */
esp_err_t nvsman_save_certs(cert_data_t *new_certs);

/**
 * @brief Get certificate data pointer
 */
esp_err_t nvsman_get_certs(cert_data_t *out_certs);

/**
 * @brief Save an OTA record to the NVS partition
 */
esp_err_t nvsman_save_ota_record(ota_record_t *new_ota_rec);

/**
 * @brief Get the last saved OTA record
 */
esp_err_t nvsman_get_ota_record(ota_record_t *out_ota_rec);

#ifdef __cplusplus
}
#endif