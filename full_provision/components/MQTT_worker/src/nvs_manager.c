#include "nvs_manager.h"

/**
 * @brief Logging tag for this module
 */
static const char *TAG = "NVS Manager";

/**
 * @brief The certificate data
 */
static cert_data_t cert_data;

static esp_err_t nvsman_retreive_certs() {
  // Open the namespace "certs"
  nvs_handle_t certs_handle;
  esp_err_t err = nvs_open("certs", NVS_READONLY, &certs_handle);
  if (err != ESP_OK){
    if (err == ESP_ERR_NVS_NOT_FOUND) {
      ESP_LOGW(TAG, "Certificates not found in NVS!");
    } else {
      ESP_LOGE(TAG, "Failed opening NVS namespace: %s (0x%02x)", esp_err_to_name(err), err);
    }
    return err;
  }

  ESP_LOGI(TAG, "Reading stored certificate");
  size_t cert_data_size = sizeof(cert_data_t);
  err = nvs_get_blob(certs_handle, "cert_data", &cert_data, &cert_data_size);
  if (err != ESP_OK) {
    if (err == ESP_ERR_NVS_NOT_FOUND) {
      ESP_LOGW(TAG, "Certificates not found in NVS!");
      /// TODO: Begin provisioning
    } else {
      ESP_LOGE(TAG, "Error 0x%02x", err);
    }
  } else {
    /// TODO: Goto tag for cleanup instead of if/else
    ESP_LOGD(TAG, "Certificate ID: %s", cert_data.cert_id);
    ESP_LOGD(TAG, "Thing Name: %s", cert_data.thing_name);
  }

  nvs_close(certs_handle);
  return err;
}

esp_err_t nvsman_save_certs(cert_data_t *new_certs) {
  // Open the namespace "certs"
  nvs_handle_t certs_handle;
  esp_err_t err = nvs_open("certs", NVS_READWRITE, &certs_handle);
  if (err != ESP_OK){
    ESP_LOGE(TAG, "Failed opening NVS namespace (0x%02x)", err);
    return err;
  }

  ESP_LOGI(TAG, "Saving new certificates");
  /// TODO: Check if certificates are already saved and notify
  size_t cert_data_size = sizeof(cert_data_t);
  err = nvs_set_blob(certs_handle, "cert_data", new_certs, cert_data_size);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to write certificate data blob!");
    nvs_close(certs_handle);
    return err;
  }
  // Commit
  err = nvs_commit(certs_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to commit data");
  }

  nvs_close(certs_handle);
  return err;
}

esp_err_t nvsman_begin(void) {
  // Initialize NVS
  /// TODO: Define a specific partition in flash for this
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }

  if (err != ESP_OK) return err;
  err = nvsman_retreive_certs();

  return err;
}