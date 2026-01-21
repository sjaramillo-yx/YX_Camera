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
// Initialization success flags
static esp_err_t ethman_inited;
static esp_err_t mqtt_w_inited;
static esp_err_t sd_mounted;
static esp_err_t vman_inited;
static esp_err_t hello_published;
static esp_err_t ota_rec_retrieved;
// Event loops
static esp_event_loop_handle_t OTA_event_h;

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
  esp_err_t err = ESP_OK;
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
  ota_record_t *ota_rec = (ota_record_t *)calloc(1, sizeof(ota_record_t));
  ota_rec_retrieved     = nvsman_get_ota_record(ota_rec);

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
    if (ota_rec_retrieved != ESP_OK) {
      ESP_LOGE(TAG, "Couldn't retrieve OTA record");
    } else {
      ESP_LOGW(TAG, "[APP] Failed partition error code: %s",
               esp_err_to_name((esp_err_t)ota_rec->esp_err));
      ESP_LOGW(TAG, "[APP] Failed partition error message: %s", ota_rec->detail);
    }
  }

  // Create the event loops
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
  if (mqtt_w_inited == ESP_OK) {
    if ((err = mqttworker_begin(10000)) != ESP_OK)
      ESP_LOGE(TAG, "MQTT Worker couldn't begin: %s (0x%02x)", esp_err_to_name(err), err);
  } else
    ESP_LOGW(TAG, "The MQTT worker couldn't be initialized. No connection will be attempted");

  // Get information about the SD Card
  cJSON *sdJSON;
  sdman_getJSON(&sdJSON);
  /// Get information about the Video Manager
  cJSON *vmanJSON;
  vman_getJSON(&vmanJSON);
  // Publish initial state
  hello_published = mqttworker_publish_initial_state(sdJSON, vmanJSON);  // Also frees cJSON memory

  // If OTA partition is in failure state, tell the MQTT manager to update the Job
  if (last_bad != NULL) {
    /// TODO: Post an event including the neccesary details in the event data pointer
    ota_result_t ota_res = {.err_code = ota_rec->esp_err};
    strlcpy(ota_res.job_id, ota_rec->job_id, sizeof(ota_res.job_id));
    strlcpy(ota_res.detail, ota_rec->detail, sizeof(ota_res.detail));
    mqttworker_get_thingname(ota_res.thing_name);
    /// TODO: Remove portMAX_DELAY and fail accordingly
    esp_event_post_to(OTA_event_h, OTA_EVENTS, OTA_JOB_ERROR, &ota_res, sizeof(ota_result_t),
                      portMAX_DELAY);
    /// TODO: If no connection is available, retry on connected
    /// TODO: Clear the OTA record after this.
  }

  while (true) {
    vTaskDelay(pdMS_TO_TICKS(30000));
    sdman_getJSON(&sdJSON);
    mqttworker_publish_current_state(sdJSON, vman_is_recording());  // This also frees cJSON memory
    if (vman_is_recording()) {
      publish_rec_state();
    }  // end if
    mqttworker_check_for_jobs();
  }  // end while
}  // end app_main

#ifdef __cplusplus
}
#endif