/* Espressif includes */
#include <esp_err.h>
#include <esp_log.h>
#include <esp_system.h>
/* Standard includes*/
#include <cJSON.h>
/* FreeRTOS includes*/
/* Custom includes */
#include "recording_events.h"
ESP_EVENT_DEFINE_BASE(RECORDING_EVENTS);  // Event base must be declared here (not sure why)
#include "SD_manager.h"
#include "ethernet_manager.h"
#include "mqtt_worker.h"
#include "video_manager.h"

static const char *TAG = "VueltaCAM";

void app_main(void) {
  ESP_LOGI(TAG, "[APP] Free memory: %" PRIu32 " bytes", esp_get_free_heap_size());
  ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());
  /// TODO: Add firmware version

  // Set log levels
  esp_log_level_set("*", ESP_LOG_INFO);
  esp_log_level_set("mqtt_client", ESP_LOG_DEBUG);
  esp_log_level_set("transport_base", ESP_LOG_INFO);
  esp_log_level_set("transport", ESP_LOG_INFO);
  esp_log_level_set("Provision Claimer", ESP_LOG_INFO);
  esp_log_level_set("MQTT Worker", ESP_LOG_DEBUG);
  esp_log_level_set("NVS Manager", ESP_LOG_INFO);
  esp_log_level_set("Video Manager", ESP_LOG_DEBUG);

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
  // Publish initial state
  mqttworker_publish_initial_state(sdJSON);  // This also frees the memory for sdJSON
}
