/**
 * @file NVS_manager.c
 * @date January 2026
 * @author Simón Jaramillo <sjaramillo@yx.cl>
 *
 */
#include "NVS_manager.h"

/**
 * @brief Logging tag for this module
 */
static const char *TAG = "NVS Manager";

/**
 * @brief The certificate data
 * @todo Move this to MQTT Worker
 */
static cert_data_t cert_data = {.populated = false};

static esp_err_t nvsman_retreive_certs() {
  // Open the namespace "certs"
  nvs_handle_t certs_handle;
  esp_err_t    err = nvs_open("certs", NVS_READONLY, &certs_handle);
  if (err != ESP_OK) {
    if (err == ESP_ERR_NVS_NOT_FOUND) {
      ESP_LOGW(TAG, "Certificates not found in NVS!");
    } else {
      ESP_LOGE(TAG, "Failed opening NVS namespace: %s (0x%02x)", esp_err_to_name(err), err);
    }
    return err;
  }

  ESP_LOGD(TAG, "Searching for stored certificate");
  size_t cert_data_size = sizeof(cert_data_t);
  err                   = nvs_get_blob(certs_handle, "cert_data", &cert_data, &cert_data_size);
  if (err != ESP_OK) {
    if (err == ESP_ERR_NVS_NOT_FOUND) {
      ESP_LOGW(TAG, "Certificates not found in NVS!");
    } else {
      ESP_LOGE(TAG, "Error 0x%02x", err);
    }
  } else {
    /// TODO: Goto tag for cleanup instead of if/else
    ESP_LOGD(TAG, "Certificate correctly retreived from NVS");
    ESP_LOGD(TAG, "Certificate ID: %s", cert_data.cert_id);
    ESP_LOGD(TAG, "Thing Name: %s", cert_data.thing_name);
    cert_data.populated = true;
  }

  nvs_close(certs_handle);
  return err;
}

esp_err_t nvsman_save_certs(cert_data_t *new_certs) {
  // Open the namespace "certs"
  nvs_handle_t certs_handle;
  esp_err_t    err = nvs_open("certs", NVS_READWRITE, &certs_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed opening NVS namespace (0x%02x)", err);
    return err;
  }

  ESP_LOGI(TAG, "Saving new certificates");
  /// TODO: Check if certificates are already saved and notify
  size_t cert_data_size = sizeof(cert_data_t);
  err                   = nvs_set_blob(certs_handle, "cert_data", new_certs, cert_data_size);
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
  cert_data           = *new_certs;
  cert_data.populated = true;

  nvs_close(certs_handle);
  return err;
}

esp_err_t nvsman_begin(void) {
  // Initialize NVS
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "Couldn't erase NVS");
    err = nvs_flash_init();
  }

  if (err != ESP_OK)
    return err;
  err = nvsman_retreive_certs();

  return err;
}

esp_err_t nvsman_get_certs(cert_data_t *out_certs) {
  if (!cert_data.populated) {
    ESP_LOGE(TAG, "Certificate data is not populated yet!");
    return ESP_ERR_INVALID_STATE;
  }
  *out_certs = cert_data;
  return ESP_OK;
}

esp_err_t nvsman_save_ota_record(ota_record_t *new_ota_rec) {
  // Open the namespace "ota_rec"
  nvs_handle_t ota_rec_handle;
  esp_err_t    err = nvs_open("ota_rec", NVS_READWRITE, &ota_rec_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed opening NVS namespace (0x%02x)", err);
    return err;
  }

  ESP_LOGI(TAG, "Saving new OTA record");
  size_t ota_rec_size = sizeof(ota_record_t);
  err                 = nvs_set_blob(ota_rec_handle, "ota_rec", new_ota_rec, ota_rec_size);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to write OTA record blob!");
    nvs_close(ota_rec_handle);
    return err;
  }
  // Commit
  err = nvs_commit(ota_rec_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to commit data");
  }

  nvs_close(ota_rec_handle);
  return err;
}

esp_err_t nvsman_get_ota_record(ota_record_t *out_ota_rec) {
  nvs_handle_t ota_rec_handle;
  esp_err_t    err = nvs_open("ota_rec", NVS_READONLY, &ota_rec_handle);
  if (err != ESP_OK) {
    if (err == ESP_ERR_NVS_NOT_FOUND) {
      ESP_LOGW(TAG, "OTA record not found in NVS!");
    } else {
      ESP_LOGE(TAG, "Failed opening NVS namespace: %s (0x%02x)", esp_err_to_name(err), err);
    }
    return err;
  }

  ESP_LOGD(TAG, "Searching for stored OTA record");
  size_t ota_rec_size = sizeof(ota_record_t);
  err                 = nvs_get_blob(ota_rec_handle, "ota_rec", out_ota_rec, &ota_rec_size);
  if (err != ESP_OK) {
    if (err == ESP_ERR_NVS_NOT_FOUND) {
      ESP_LOGW(TAG, "OTA record not found in NVS!");
    } else {
      ESP_LOGE(TAG, "Error 0x%02x", err);
    }
    goto cleanup;
  } else {
    // Check magic ("OTAF" in hex format)
    if (out_ota_rec->magic != CONFIG_OTA_REC_MAGIC) {
      err = ESP_ERR_NVS_NOT_FOUND;
      memset(out_ota_rec, 0, sizeof(ota_record_t));
      ESP_LOGE(TAG, "Record is invalid, magic bytes are scrambled");
      /// TODO: Maybe clear this blob
    }
  }

cleanup:
  nvs_close(ota_rec_handle);
  return err;
}
