/**
 * @file NVS_manager.h
 * @date January 2026
 * @author Simón Jaramillo <sjaramillo@yx.cl>
 *
 * @brief This header manages Non-Volatile Storage.
 */

/* Espressif includes */
#include <esp_check.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_system.h>
#include <nvs_flash.h>
/* Standard includes*/
/* FreeRTOS includes*/
/* Custom includes */

/**
 * @brief A data structure to store certificate data in a blob.
 */
typedef struct {
  char client_crt[4096];
  char client_key[4096];
  char cert_id[512];
  char thing_name[512];
  bool populated;
} cert_data_t;

/**
 * @brief A data structure to store OTA fail records in a blob.
 */
/// TODO: Turn the contents of magic into a Kconfig option
typedef struct {
  uint32_t magic;        // helps verify integrity of the blob
  int32_t  esp_err;      // last error code (esp_err_t fixed to 32 bits)
  char     detail[128];  // short message (truncate)
} ota_fail_record_t;

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
 * @brief Save an OTA fail record to the NVS partition
 */
esp_err_t nvsman_save_ota_fail(ota_fail_record_t *new_fail_rec);

/**
 * @brief Get the last saved OTA failure record
 */
esp_err_t nvsman_get_ota_fail(ota_fail_record_t *out_fail_rec);