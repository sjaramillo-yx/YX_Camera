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

static esp_err_t nvsman_retrieve_provisioning_certs() {
  esp_err_t ret = ESP_OK;
  size_t    len = 0;

  // Open the namespace "provisioning" of the "certs" partition
  nvs_handle_t certs_handle;
  ret = nvs_open_from_partition("certs", "provisioning", NVS_READONLY, &certs_handle);
  if (ret != ESP_OK) {
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
      ESP_LOGW(TAG, "Provisioning certificates not found in certs NVS !");
    } else {
      ESP_LOGE(TAG, "Failed opening NVS namespace: %s (0x%02x)", esp_err_to_name(ret), ret);
    }
    /// NOTE: Don't go to cleanup beacuse the handle is not valid and cannot be closed
    return ret;
  }

  ESP_LOGD(TAG, "Searching for provisioning certificate");
  ret = nvs_get_str(certs_handle, "client_crt", NULL, &len);  // Query the length of the string
  ESP_GOTO_ON_FALSE(len < sizeof(cert_data.client_crt), ESP_ERR_NO_MEM, cleanup, TAG,
                    "Device certificate is too long!");
  ret = nvs_get_str(certs_handle, "client_crt", cert_data.client_crt, &len);
  ESP_LOGD(TAG, "Searching for provisioning private key");
  ret = nvs_get_str(certs_handle, "client_key", NULL, &len);  // Query the length of the string
  ESP_GOTO_ON_FALSE(len < sizeof(cert_data.client_key), ESP_ERR_NO_MEM, cleanup, TAG,
                    "Device private key is too long!");
  ret = nvs_get_str(certs_handle, "client_key", cert_data.client_key, &len);
  ESP_LOGD(TAG, "Searching for Root CA");
  ret = nvs_get_str(certs_handle, "root_ca", NULL, &len);  // Query the length of the string
  ESP_GOTO_ON_FALSE(len < sizeof(cert_data.root_ca), ESP_ERR_NO_MEM, cleanup, TAG,
                    "Root CA is too long!");
  ret = nvs_get_str(certs_handle, "root_ca", cert_data.root_ca, &len);
  ESP_LOGD(TAG, "Searching for OTA codesign cert");
  ret = nvs_get_str(certs_handle, "ota_key", NULL, &len);  // Query the length of the string
  ESP_GOTO_ON_FALSE(len < sizeof(cert_data.ota_key), ESP_ERR_NO_MEM, cleanup, TAG,
                    "OTA codesign public key is too long!");
  ret = nvs_get_str(certs_handle, "ota_key", cert_data.ota_key, &len);

  strlcpy(cert_data.cert_id, "provisioning", sizeof(cert_data.cert_id));
  cert_data.populated = true;

cleanup:
  nvs_close(certs_handle);
  return ret;
}

static esp_err_t nvsman_retreive_certs() {
  esp_err_t ret = ESP_OK;
  size_t    len = 0;

  // Open the namespace "dev_certs" of the "certs" partition
  nvs_handle_t certs_handle;
  ret = nvs_open_from_partition("certs", "dev_certs", NVS_READONLY, &certs_handle);
  if (ret != ESP_OK) {
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
      ESP_LOGW(TAG, "Device certificates not found in certs NVS !");
    } else {
      ESP_LOGE(TAG, "Failed opening NVS namespace: %s (0x%02x)", esp_err_to_name(ret), ret);
    }
    /// NOTE: Don't go to cleanup beacuse the handle is not valid and cannot be closed
    return ret;
  }

  ESP_LOGD(TAG, "Searching for device certificate");
  ret = nvs_get_str(certs_handle, "client_crt", NULL, &len);  // Query the length of the string
  ESP_GOTO_ON_FALSE(len < sizeof(cert_data.client_crt), ESP_ERR_NO_MEM, cleanup, TAG,
                    "Device certificate is too long!");
  ret = nvs_get_str(certs_handle, "client_crt", cert_data.client_crt, &len);
  ESP_LOGD(TAG, "Searching for provisioning private key");
  ret = nvs_get_str(certs_handle, "client_key", NULL, &len);  // Query the length of the string
  ESP_GOTO_ON_FALSE(len < sizeof(cert_data.client_key), ESP_ERR_NO_MEM, cleanup, TAG,
                    "Device private key is too long!");
  ret = nvs_get_str(certs_handle, "client_key", cert_data.client_key, &len);
  ESP_LOGD(TAG, "Searching for Root CA");
  ret = nvs_get_str(certs_handle, "root_ca", NULL, &len);  // Query the length of the string
  ESP_GOTO_ON_FALSE(len < sizeof(cert_data.root_ca), ESP_ERR_NO_MEM, cleanup, TAG,
                    "Root CA is too long!");
  ret = nvs_get_str(certs_handle, "root_ca", cert_data.root_ca, &len);
  ESP_LOGD(TAG, "Searching for certificate ID");
  ret = nvs_get_str(certs_handle, "cert_id", NULL, &len);  // Query the length of the string
  ESP_GOTO_ON_FALSE(len < sizeof(cert_data.cert_id), ESP_ERR_NO_MEM, cleanup, TAG,
                    "Certificate ID is too long!");
  ret = nvs_get_str(certs_handle, "cert_id", cert_data.cert_id, &len);
  ESP_LOGD(TAG, "Searching for ThingName");
  ret = nvs_get_str(certs_handle, "thing_name", NULL, &len);  // Query the length of the string
  ESP_GOTO_ON_FALSE(len < sizeof(cert_data.thing_name), ESP_ERR_NO_MEM, cleanup, TAG,
                    "ThingName is too long!");
  ret = nvs_get_str(certs_handle, "thing_name", cert_data.thing_name, &len);
  ESP_LOGD(TAG, "Searching for OTA codesign cert");
  ret = nvs_get_str(certs_handle, "ota_key", NULL, &len);  // Query the length of the string
  ESP_GOTO_ON_FALSE(len < sizeof(cert_data.ota_key), ESP_ERR_NO_MEM, cleanup, TAG,
                    "OTA codesign public key is too long!");
  ret = nvs_get_str(certs_handle, "ota_key", cert_data.ota_key, &len);

  /// TODO: Goto tag for cleanup instead of if/else
  ESP_LOGD(TAG, "Certificate correctly retreived from NVS");
  ESP_LOGD(TAG, "Certificate ID: %s", cert_data.cert_id);
  ESP_LOGD(TAG, "Thing Name: %s", cert_data.thing_name);
  cert_data.populated = true;

cleanup:
  nvs_close(certs_handle);
  return ret;
}

esp_err_t nvsman_save_certs(cert_data_t *new_certs) {
  esp_err_t ret = ESP_OK;
  size_t    len = 0;

  // Open the namespace "dev_certs" of the "certs" partition
  nvs_handle_t certs_handle;
  ret = nvs_open_from_partition("certs", "dev_certs", NVS_READWRITE, &certs_handle);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed opening NVS namespace: %s (0x%02x)", esp_err_to_name(ret), ret);
    /// NOTE: Don't go to cleanup beacuse the handle is not valid and cannot be closed
    return ret;
  }

  ESP_LOGI(TAG, "Saving new certificates");
  ESP_RETURN_ON_ERROR(nvs_set_str(certs_handle, "client_crt", new_certs->client_crt), TAG,
                      "Couldn't write client certificate");
  ESP_RETURN_ON_ERROR(nvs_set_str(certs_handle, "client_key", new_certs->client_key), TAG,
                      "Couldn't write client key");
  ESP_RETURN_ON_ERROR(nvs_set_str(certs_handle, "root_ca", new_certs->root_ca), TAG,
                      "Couldn't write root CA");
  ESP_RETURN_ON_ERROR(nvs_set_str(certs_handle, "cert_id", new_certs->cert_id), TAG,
                      "Couldn't write certificate ID");
  ESP_RETURN_ON_ERROR(nvs_set_str(certs_handle, "thing_name", new_certs->thing_name), TAG,
                      "Couldn't write ThingName");
  ESP_RETURN_ON_ERROR(nvs_set_str(certs_handle, "ota_key", new_certs->ota_key), TAG,
                      "Couldn't write OTA codesign public key");

  // Commit
  ret = nvs_commit(certs_handle);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to commit data");
  }
  cert_data           = *new_certs;
  cert_data.populated = true;

  nvs_close(certs_handle);
  return ret;
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

/*================== Initalize, Begin, Stop and Deinitialize ==================*/
esp_err_t nvsman_init(void) {
  // Initialize NVS
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "Couldn't erase NVS");
    err = nvs_flash_init();
  }
  /// TODO: Make the certificates partition label a KConfig option.
  err = nvs_flash_init_partition("certs");
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_RETURN_ON_ERROR(nvs_flash_erase_partition("certs"), TAG, "Couldn't erase certs NVS");
    err = nvs_flash_init_partition("certs");
  }

  if (err != ESP_OK)
    return err;
  err = nvsman_retreive_certs();

  if (err == ESP_ERR_NVS_NOT_FOUND) {
    ESP_LOGW(TAG, "Using provisioning certificates");
    ESP_RETURN_ON_ERROR(err = nvsman_retrieve_provisioning_certs(), TAG,
                        "Couldn't retrieve provisioning certificates");
    return ESP_ERR_NOT_FOUND;
  }

  return err;
}

esp_err_t nvsman_deinit(void) {
  nvs_flash_deinit_partition("certs");
  nvs_flash_deinit();

  return ESP_OK;
}