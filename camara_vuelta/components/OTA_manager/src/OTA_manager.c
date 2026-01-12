#include "OTA_manager.h"

static const char *TAG = "OTAManager";

esp_err_t otaman_run_test(component_test test_function) {
  esp_err_t          ret      = ESP_OK;
  ota_fail_record_t *fail_rec = (ota_fail_record_t *)calloc(sizeof(ota_fail_record_t), 1);
  // Run the test
  ret = test_function(fail_rec->detail);
  if (ret == ESP_OK) {
    free(fail_rec);
    return ESP_OK;
  }
  // Something failed
  ESP_LOGE(TAG, "Component test failed with code %s", esp_err_to_name(ret));
  // Populate the failure record
  fail_rec->esp_err = (int32_t)ret;
  fail_rec->magic   = 0x4F544146;  // "OTAF" in hex format
  ESP_RETURN_ON_ERROR(nvsman_save_ota_fail(fail_rec), TAG,
                      "Couldn't save OTA failure record to NVS");
  // Mark app as invalid and rollback
  ret = esp_ota_mark_app_invalid_rollback_and_reboot();
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "Rollback failed! (%s)", esp_err_to_name(ret));
  }
  return ret;
}