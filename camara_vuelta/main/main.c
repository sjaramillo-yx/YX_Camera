/* Espressif includes */
#include <esp_err.h>
#include <esp_log.h>
#include <esp_system.h>
/* Standard includes*/
/* FreeRTOS includes*/
/* Custom includes */
#include "ethernet_manager.h"
#include "mqtt_worker.h"

static const char *TAG = "full-provision";

void app_main(void)
{
  ESP_LOGI(TAG, "[APP] Startup..");
  ESP_LOGI(TAG, "[APP] Free memory: %" PRIu32 " bytes", esp_get_free_heap_size());
  ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());

  // Set log levels
  esp_log_level_set("*", ESP_LOG_INFO);
  esp_log_level_set("mqtt_client", ESP_LOG_VERBOSE);
  esp_log_level_set("transport_base", ESP_LOG_VERBOSE);
  esp_log_level_set("transport", ESP_LOG_VERBOSE);
  esp_log_level_set("Provision Claimer", ESP_LOG_VERBOSE);
  esp_log_level_set("MQTT Worker", ESP_LOG_VERBOSE);
  esp_log_level_set("NVS Manager", ESP_LOG_VERBOSE);

  // Initialize ethernet
  esp_eth_handle_t eth_handle;
  ESP_ERROR_CHECK(ethman_init(&eth_handle));

  mqttworker_begin();
}
