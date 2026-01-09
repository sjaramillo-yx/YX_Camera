/* Espressif includes */
#include <esp_err.h>
#include <esp_log.h>
#include <esp_system.h>
/* Standard includes*/
#include <cJSON.h>
/* FreeRTOS includes*/
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
/* Custom includes */
#include "recording_events.h"
ESP_EVENT_DEFINE_BASE(RECORDING_EVENTS);  // Event base must be declared here (not sure why)
#include "SD_manager.h"
#include "ethernet_manager.h"
#include "mqtt_worker.h"
#include "video_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

static const char *TAG = "VueltaCAM";

static esp_err_t publish_rec_state() {
  esp_err_t ret       = ESP_OK;
  cJSON    *rec_state = cJSON_CreateObject();
  /// TODO: check for errors
  vman_get_rec_json(&rec_state);
  mqttworker_publish_recording_state(rec_state);  // This also frees cJSON memory
  return ret;
}

void app_main(void) {
  ESP_LOGI(TAG, "[APP] Free memory: %" PRIu32 " bytes", esp_get_free_heap_size());
  ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());
  ESP_LOGI(TAG, "[APP] Firmware version: %s", esp_app_get_description()->version);

  // Set log levels
  esp_log_level_set("*", ESP_LOG_INFO);
  esp_log_level_set("mqtt_client", ESP_LOG_INFO);
  esp_log_level_set("transport_base", ESP_LOG_INFO);
  esp_log_level_set("transport", ESP_LOG_INFO);
  esp_log_level_set("Provision Claimer", ESP_LOG_INFO);
  esp_log_level_set("MQTT Worker", ESP_LOG_INFO);
  esp_log_level_set("S3Uploader", ESP_LOG_INFO);
  esp_log_level_set("NVS Manager", ESP_LOG_INFO);
  esp_log_level_set("Video Manager", ESP_LOG_INFO);
  esp_log_level_set("SDManager", ESP_LOG_INFO);
  esp_log_level_set("HTTPHelpers", ESP_LOG_INFO);

  // Create the recording event loop
  rec_eventloop_create();

  // Initialize ethernet
  esp_eth_handle_t eth_handle;
  ESP_ERROR_CHECK(ethman_init(&eth_handle));

  if (mqttworker_begin() != ESP_OK) {
    /// TODO: Handle errors
  }

  // Mount the SD Card
  esp_err_t sd_mounted = sdman_mount();
  // Initialize the Video Manager
  esp_err_t vman_inited = vman_init();

  // Get information about the SD Card
  cJSON *sdJSON;
  sdman_getJSON(&sdJSON);
  /// Get information about the Video Manager
  cJSON *vmanJSON;
  vman_getJSON(&vmanJSON);
  // Publish initial state
  mqttworker_publish_initial_state(sdJSON, vmanJSON);  // This also frees cJSON memory

  while (true) {
    vTaskDelay(pdMS_TO_TICKS(30000));
    sdman_getJSON(&sdJSON);
    mqttworker_publish_current_state(sdJSON, vman_is_recording());  // This also frees cJSON memory
    if (vman_is_recording()) {
      publish_rec_state();
    }
  }
}

#ifdef __cplusplus
}
#endif