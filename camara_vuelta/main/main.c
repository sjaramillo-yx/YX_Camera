/* Espressif includes */
#include <esp_err.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
/* Standard includes*/
#include <cJSON.h>
/* FreeRTOS includes*/
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
/* Custom includes */
#include "OTA_events.h"
#include "recording_events.h"
ESP_EVENT_DEFINE_BASE(RECORDING_EVENTS);  // Event base must be declared here (not sure why)
ESP_EVENT_DEFINE_BASE(OTA_EVENTS);
#include "OTA_manager.h"
#include "SD_manager.h"
#include "ethernet_manager.h"
#include "mqtt_worker.h"
#include "video_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================ Globals ================ */
static const char *TAG = "VueltaCAM";
/*================== Static Functions ==================*/

static esp_err_t publish_rec_state() {
  esp_err_t ret       = ESP_OK;
  cJSON    *rec_state = cJSON_CreateObject();
  /// TODO: check for errors
  vman_get_rec_json(&rec_state);
  mqttworker_publish_recording_state(rec_state);  // This also frees cJSON memory
  return ret;
}

/*================== Event Handlers ==================*/
static void ota_event_handler(void *handler_arg, esp_event_base_t event_base, int32_t event_id,
                              void *event_data) {
  ota_stream_t *ota_stream;
  esp_err_t     ret;
  ESP_LOGI(TAG, "Received event %s:%d", (char *)event_base, event_id);
  switch (event_id) {
  case OTA_JOB_RECEIVED:
    ota_stream          = event_data;
    bool vman_recording = vman_is_recording();
    ESP_LOGD(TAG, "VideoManager is %srecording", vman_recording ? "" : "not ");
    ESP_GOTO_ON_FALSE(!vman_recording, ESP_ERR_INVALID_STATE, start_failed, TAG,
                      "Peripherals are busy, can't begin OTA update");
    /// TODO: Add other checks here before accepting OTA job
    /// TODO: Check that MQTT is connected
    ESP_GOTO_ON_ERROR(otaman_can_start(ota_stream->filesize), start_failed, TAG,
                      "OTAMan can't start");
    ESP_GOTO_ON_ERROR(otaman_start_update(ota_stream->filesize), start_failed, TAG,
                      "OTAMan couldn't start the OTA update");
    esp_event_post_to(OTA_event_h, OTA_EVENTS, OTA_CTRL_START, NULL, 0, 100);
    break;

  default:
    break;
  start_failed:
    esp_event_post_to(OTA_event_h, OTA_EVENTS, OTA_JOB_REJECTED, NULL, 0, 100);
    break;
  }
}

void app_main(void) {
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
  esp_log_level_set("OTAManager", ESP_LOG_DEBUG);
  esp_log_level_set("HTTPHelpers", ESP_LOG_INFO);
  esp_log_level_set("AWSJobsManager", ESP_LOG_DEBUG);
  esp_log_level_set("esp-x509-crt-bundle", ESP_LOG_WARN);

  ESP_LOGI(TAG, "[APP] Free memory: %" PRIu32 " bytes", esp_get_free_heap_size());
  ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());
  ESP_LOGI(TAG, "[APP] Firmware version: %s", esp_app_get_description()->version);

  // Initialize NVS manager
  nvsman_begin();

  const esp_partition_t *running  = esp_ota_get_running_partition();
  const esp_partition_t *boot     = esp_ota_get_boot_partition();
  const esp_partition_t *last_bad = esp_ota_get_last_invalid_partition();
  ESP_LOGI(TAG, "[APP] Running partition label: %s", running->label);
  ESP_LOGI(TAG, "[APP] Running partition offset: 0x%08x", running->address);
  // Check for OTA update errors
  if (running != boot) {
    ESP_LOGW(TAG, "[APP] Running partition is different from boot!");
    ESP_LOGW(TAG, "[APP] Boot partition label: %s", boot->label);
    ESP_LOGW(TAG, "[APP] Boot partition offset: 0x%08x", boot->address);
  }
  if (last_bad != NULL) {
    ESP_LOGW(TAG, "[APP] A partition is in failure state");
    ESP_LOGW(TAG, "[APP] Failed partition label: %s", last_bad->label);
    ESP_LOGW(TAG, "[APP] Failed partition offset: 0x%08x", last_bad->address);
    /// TODO: Print failure message
    ota_fail_record_t *fail_rec = (ota_fail_record_t *)calloc(sizeof(ota_fail_record_t), 1);
    esp_err_t          ret      = nvsman_get_ota_fail(fail_rec);
    if (ret != ESP_OK) {
      ESP_LOGE(TAG, "Couldn't retrieve OTA failure record");
    } else {
      ESP_LOGW(TAG, "[APP] Failed partition error code: %s",
               esp_err_to_name((esp_err_t)fail_rec->esp_err));
      ESP_LOGW(TAG, "[APP] Failed partition error message: %s", fail_rec->detail);
    }
  }

  rec_eventloop_create();
  OTA_eventloop_create();
  ESP_LOGI(TAG, "Event loops created");

  // Initialize OTAManager
  QueueHandle_t free_chunk_queue, filled_chunk_queue;
  otaman_init(&free_chunk_queue, &filled_chunk_queue);

  /* Register OTA event loop and handler*/
  ESP_ERROR_CHECK_WITHOUT_ABORT(OTA_eventloop_get_handle(&OTA_event_h));
  ESP_ERROR_CHECK_WITHOUT_ABORT(esp_event_handler_instance_register_with(
      OTA_event_h, OTA_EVENTS, ESP_EVENT_ANY_ID, ota_event_handler, NULL, NULL));

  // Initialize ethernet
  esp_eth_handle_t eth_handle;
  ethman_inited = ethman_init(&eth_handle);

  mqtt_w_inited = mqttworker_init(free_chunk_queue, filled_chunk_queue);

  // Mount the SD Card
  sd_mounted = sdman_mount();
  // Initialize the Video Manager
  vman_inited = vman_init();

  // Connect to AWS
  if (ethman_wait_ip(1000) == ESP_OK)
    ethman_wait_sntp(10000);
  if (mqtt_w_inited == ESP_OK)
    mqttworker_begin(1000);
  else
    ESP_LOGW(TAG, "The MQTT worker couldn't be initialized. No connection will be attempted");

  // Get information about the SD Card
  cJSON *sdJSON;
  sdman_getJSON(&sdJSON);
  /// Get information about the Video Manager
  cJSON *vmanJSON;
  vman_getJSON(&vmanJSON);
  // Publish initial state
  hello_published = mqttworker_publish_initial_state(sdJSON, vmanJSON);  // Also frees cJSON memory

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