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
#include <freertos/timers.h>
/* Custom includes */
#include "OTA_events.h"
#include "command_events.h"
#include "configuration_events.h"
#include "recording_events.h"
ESP_EVENT_DEFINE_BASE(RECORDING_EVENTS);  // Event base must be declared here (not sure why)
ESP_EVENT_DEFINE_BASE(OTA_EVENTS);
ESP_EVENT_DEFINE_BASE(CONFIGURATION_EVENTS);
ESP_EVENT_DEFINE_BASE(COMMAND_EVENTS);
#include "OTA_manager.h"
#include "SD_manager.h"
#include "ethernet_manager.h"
#include "mqtt_worker.h"
#include "video_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/*=========================================== Globals ============================================*/
static const char      *TAG = "VueltaCAM";
static esp_eth_handle_t eth_handle;
// Initialization success flags
static esp_err_t ethman_inited;
static esp_err_t mqtt_w_inited;
static esp_err_t sd_mounted;
static esp_err_t vman_inited;
static esp_err_t hello_published;
static esp_err_t ota_rec_retrieved;
// Event loops
static esp_event_loop_handle_t OTA_event_h;
static esp_event_loop_handle_t conf_event_h;
static esp_event_loop_handle_t cmd_event_h;
// Peripheral JSONs
cJSON *sdJSON;
cJSON *vmanJSON;
// TCP Keep alive configurations
static int               tcp_idle_s, tcp_interval_s, tcp_retries;
static TaskHandle_t      mqtt_conf_task_h;
static SemaphoreHandle_t mqtt_conf_smphr;
// MQTT status task
static TaskHandle_t mqtt_status_task_h;
// Timers
static TimerHandle_t status_timer_h;

/*======================================= Static Functions =======================================*/
static esp_err_t main_test(char out_msg[128]) {
  if (CONFIG_AWS_ENDPOINT[0] == '\0') {
    strlcpy(out_msg, "AWS IoTCore Endpoint is empty", 128);
    return ESP_ERR_NOT_FOUND;
  }
  return ESP_OK;
}

static esp_err_t publish_rec_state() {
  esp_err_t ret       = ESP_OK;
  cJSON    *rec_state = cJSON_CreateObject();
  /// TODO: check for errors
  vman_get_rec_json(&rec_state);
  mqttworker_publish_recording_state(rec_state);  // This also frees cJSON memory
  return ret;
}

static esp_err_t deinit_peripherals() {
  ESP_RETURN_ON_ERROR(vman_deinit(), TAG, "Couldn't deinitialize Video Manager");
  ESP_RETURN_ON_ERROR(sdman_umount(), TAG, "Couldn't unmount the SD card");
  ESP_RETURN_ON_ERROR(mqttworker_stop(), TAG, "Couldn't stop the MQTT Worker");
  ESP_RETURN_ON_ERROR(mqttworker_deinit(), TAG, "Couldn't deinitialize the MQTT Worker");
  ESP_RETURN_ON_ERROR(ethman_deinit(eth_handle), TAG, "Couldn't deinitialize the Ethernet Manager");
  ESP_RETURN_ON_ERROR(otaman_deinit(), TAG, "Couldn't deinitialize the OTA Manager");
  ESP_RETURN_ON_ERROR(nvsman_deinit(), TAG, "Couldn't deinitialize the NVS Manager");
  return ESP_OK;
}

/*======================================= Timer Callbacks ========================================*/
static void publish_status_callback(TimerHandle_t xTimer) { xTaskNotifyGive(mqtt_status_task_h); }
/*======================================== FreeRTOS Tasks ========================================*/
static void mqtt_configure_task(void *args) {
  /// NOTE: This task exists only because a bigger stack is needed to reconfigure the TCP layer.
  esp_err_t err = ESP_OK;
  while (true) {
    while (xSemaphoreTake(mqtt_conf_smphr, portMAX_DELAY) != pdTRUE)
      continue;
    ESP_LOGD(TAG, "Configuring TCP keep alive values");
    err = mqttworker_configure_tcp_keep_alive(tcp_idle_s, tcp_interval_s, tcp_retries);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Couldn't configure TCP keep alive values (%s)", esp_err_to_name(err));
      goto end;
    }
    ESP_LOGI(TAG,
             "Set the TCP Keep Alive idle period to %ds, interval to %ds and retry count to %d",
             tcp_idle_s, tcp_interval_s, tcp_retries);
  end:
    xSemaphoreGive(mqtt_conf_smphr);
    vTaskDelay(10);
  }
}

static void mqtt_publish_status_task(void *args) {
  while (true) {
    ulTaskNotifyTake(false, portMAX_DELAY);
    sdman_getJSON(&sdJSON);
    mqttworker_publish_current_state(sdJSON, vman_is_recording());  // This also frees cJSON memory
    if (vman_is_recording()) {
      publish_rec_state();
    }
  }
}

/*======================================== Event Handlers ========================================*/
static void ota_job_received_handler(void *handler_arg, esp_event_base_t event_base,
                                     int32_t event_id, void *event_data) {
  ota_stream_t *ota_stream;
  esp_err_t     ret;
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
  return;

start_failed:
  esp_event_post_to(OTA_event_h, OTA_EVENTS, OTA_JOB_REJECTED, NULL, 0, 100);
}

static void ota_job_error_handler(void *handler_arg, esp_event_base_t event_base, int32_t event_id,
                                  void *event_data) {
  otaman_cancel_update();
}

static void conf_received_handler(void *handler_arg, esp_event_base_t event_base, int32_t event_id,
                                  void *event_data) {
  configuration_t      *conf           = event_data;
  char                  error_msg[256] = "";
  esp_err_t             ret            = ESP_OK;  // Necessary for ESP_GOTO macros
  configuration_event_t out_event      = CONF_APPLIED;

  /* First validate inputs */
  /// TODO: Define upper and lower limits as KConfig options
  if (0 > conf->status_period_ms || conf->status_period_ms > portMAX_DELAY) {
    snprintf(error_msg, sizeof(error_msg),
             "Status period outside limits. Must be greater than %d and lower than %lu.", 0,
             portMAX_DELAY);
    out_event = CONF_REJECTED;
    goto end;
  }
  if (0 > conf->min_sd_free_space_kb || conf->min_sd_free_space_kb > (8ULL << 20)) {
    /// NOTE: This limits the maximum value to 8 GB, but it would be wiser to use SD card size.
    snprintf(error_msg, sizeof(error_msg),
             "SD minimum free space value outside limits. Must be greater than %d and lower than "
             "%llu.",
             0, (8ULL << 20));
    out_event = CONF_REJECTED;
    goto end;
  }
  if ((0 > conf->tcp_keep_alive_idle_s && conf->tcp_keep_alive_idle_s > 256) &&
      (0 > conf->tcp_keep_alive_interval_s && conf->tcp_keep_alive_interval_s > 256) &&
      (0 > conf->tcp_keep_alive_retries && conf->tcp_keep_alive_retries > 256)) {
    snprintf(error_msg, sizeof(error_msg),
             "TCP values outside limits. Must be greater than %d and lower than %d.", 0, 256);
    out_event = CONF_REJECTED;
    goto end;
  }

  /* Status message period */
  ESP_LOGD(TAG, "Setting the status message period to %d ms", conf->status_period_ms);
  if (xTimerChangePeriod(status_timer_h, pdMS_TO_TICKS(conf->status_period_ms), portMAX_DELAY) !=
      pdPASS) {
    snprintf(error_msg, sizeof(error_msg), "Couldn't set the new status message period.");
    out_event = CONF_ERROR;
    goto end;
  }
  /// TODO: Remove portMAX_DELAY
  publish_status_callback(NULL);
  xTimerReset(status_timer_h, portMAX_DELAY);
  ESP_LOGI(TAG, "New status message period is %d ms", conf->status_period_ms);

  /* SD Card free space */
  ESP_LOGD(TAG, "Setting minimum free space in SD card to %d KB", conf->min_sd_free_space_kb);
  sdman_set_free_space_target(conf->min_sd_free_space_kb);

  /* TCP configuration for MQTT */
  ESP_LOGD(TAG, "Setting the TCP Keep Alive idle period to %ds", conf->tcp_keep_alive_idle_s);
  ESP_LOGD(TAG, "Setting the TCP Keep Alive interval to %ds", conf->tcp_keep_alive_interval_s);
  ESP_LOGD(TAG, "Setting the TCP Keep Alive retry count to %d", conf->tcp_keep_alive_retries);
  tcp_idle_s     = conf->tcp_keep_alive_idle_s;
  tcp_interval_s = conf->tcp_keep_alive_interval_s;
  tcp_retries    = conf->tcp_keep_alive_retries;
  xSemaphoreGive(mqtt_conf_smphr);
  vTaskDelay(10);
  // Wait for MQTT TCP Keep Alive configuration task
  xSemaphoreTake(mqtt_conf_smphr, portMAX_DELAY);

end:
  esp_event_post_to(conf_event_h, CONFIGURATION_EVENTS, out_event, error_msg, sizeof(error_msg),
                    portMAX_DELAY);
  return;
}

static void restart_handler(void *handler_arg, esp_event_base_t event_base, int32_t event_id,
                            void *event_data) {
  deinit_peripherals();
  esp_restart();
}
/*================================================================================================*/
/*                                      App entry point                                           */
/*================================================================================================*/
void app_main(void) {
  esp_err_t err = ESP_OK;
  // Set log levels
  esp_log_level_set("*", ESP_LOG_INFO);
  esp_log_level_set(TAG, ESP_LOG_DEBUG);
  esp_log_level_set("mqtt_client", ESP_LOG_INFO);
  esp_log_level_set("transport_base", ESP_LOG_INFO);
  esp_log_level_set("transport", ESP_LOG_INFO);
  esp_log_level_set("Provision Claimer", ESP_LOG_INFO);
  esp_log_level_set("MQTT Worker", ESP_LOG_DEBUG);
  esp_log_level_set("MQTT ConfWorker", ESP_LOG_DEBUG);
  esp_log_level_set("S3Uploader", ESP_LOG_INFO);
  esp_log_level_set("NVS Manager", ESP_LOG_INFO);
  esp_log_level_set("Video Manager", ESP_LOG_INFO);
  esp_log_level_set("SDManager", ESP_LOG_DEBUG);
  esp_log_level_set("OTAManager", ESP_LOG_DEBUG);
  esp_log_level_set("HTTPHelpers", ESP_LOG_INFO);
  esp_log_level_set("AWSJobsManager", ESP_LOG_DEBUG);
  esp_log_level_set("esp-x509-crt-bundle", ESP_LOG_WARN);

  ESP_LOGI(TAG, "[APP] Free memory: %" PRIu32 " bytes", esp_get_free_heap_size());
  ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());
  ESP_LOGI(TAG, "[APP] Firmware version: %s", esp_app_get_description()->version);

  // Initialize NVS manager
  nvsman_init();
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

  // Create the event loopsç
  conf_eventloop_create();
  rec_eventloop_create();
  OTA_eventloop_create();
  cmd_eventloop_create();
  ESP_LOGI(TAG, "Event loops created");

  // Initialize OTAManager
  QueueHandle_t free_chunk_queue, filled_chunk_queue;
  otaman_init(&free_chunk_queue, &filled_chunk_queue);

  /* Register OTA event loop handlers*/
  ESP_ERROR_CHECK(OTA_eventloop_get_handle(&OTA_event_h));
  ESP_ERROR_CHECK(esp_event_handler_instance_register_with(
      OTA_event_h, OTA_EVENTS, OTA_JOB_RECEIVED, ota_job_received_handler, NULL, NULL));
  ESP_ERROR_CHECK(esp_event_handler_instance_register_with(OTA_event_h, OTA_EVENTS, OTA_CTRL_DONE,
                                                           restart_handler, NULL, NULL));
  ESP_ERROR_CHECK(esp_event_handler_instance_register_with(OTA_event_h, OTA_EVENTS, OTA_JOB_ERROR,
                                                           ota_job_error_handler, NULL, NULL));
  /* Register configuration event loop handlers */
  ESP_ERROR_CHECK(conf_eventloop_get_handle(&conf_event_h));
  ESP_ERROR_CHECK(esp_event_handler_instance_register_with(
      conf_event_h, CONFIGURATION_EVENTS, CONF_RECEIVED, conf_received_handler, NULL, NULL));

  // Initialize ethernet
  ethman_inited = ethman_init(&eth_handle);

  mqtt_w_inited = mqttworker_init(free_chunk_queue, filled_chunk_queue);

  // Mount the SD Card
  sd_mounted = sdman_mount();
  // Initialize the Video Manager
  vman_inited = vman_init();

  // Test the OTA partition
  esp_err_t part_test = otaman_run_test(main_test, ota_rec);

  ota_result_t ota_res = {.err_code = ota_rec->esp_err};
  strlcpy(ota_res.job_id, ota_rec->job_id, sizeof(ota_res.job_id));
  strlcpy(ota_res.detail, ota_rec->detail, sizeof(ota_res.detail));
  mqttworker_get_thingname(ota_res.thing_name);
  // If OTA partition is in failure state, tell the MQTT manager to update the Job
  if (last_bad != NULL) {
    /// TODO: Post an event including the neccesary details in the event data pointer
    /// TODO: Remove portMAX_DELAY and fail accordingly
    esp_event_post_to(OTA_event_h, OTA_EVENTS, OTA_JOB_ERROR, &ota_res, sizeof(ota_result_t),
                      portMAX_DELAY);
    /// TODO: If no connection is available, retry on connected
    /// TODO: Clear the OTA record after this.
  } else if (strcmp(running->label, "factory") && part_test != ESP_ERR_INVALID_STATE) {
    /// TODO: Publish OTA partition test results.
    esp_event_post_to(OTA_event_h, OTA_EVENTS, OTA_JOB_DONE, &ota_res, sizeof(ota_result_t),
                      portMAX_DELAY);
  }

  // Connect to AWS
  if (ethman_wait_ip(1000) == ESP_OK)
    ethman_wait_sntp(10000);
  if (mqtt_w_inited == ESP_OK) {
    if ((err = mqttworker_begin(10000)) != ESP_OK)
      ESP_LOGE(TAG, "MQTT Worker couldn't begin: %s (0x%02x)", esp_err_to_name(err), err);
  } else
    ESP_LOGW(TAG, "The MQTT worker couldn't be initialized. No connection will be attempted");

  // Get information about the SD Card
  sdman_getJSON(&sdJSON);
  /// Get information about the Video Manager
  vman_getJSON(&vmanJSON);
  // Publish initial state
  hello_published = mqttworker_publish_initial_state(sdJSON, vmanJSON);  // Also frees cJSON memory

  // Create the MQTT TCP Keep Alive configuration semaphore
  mqtt_conf_smphr = xSemaphoreCreateBinary();
  // Create the MQTT TCP Keep Alive configuration task
  if (xTaskCreate(mqtt_configure_task, "mqtt.conf.task", 4096, NULL, 5, &mqtt_conf_task_h) !=
      pdPASS) {
    ESP_LOGE(TAG, "Couldn't create the MQTT TCP Keep Alive configuration task!");
  }

  // Create the status task
  if (xTaskCreate(mqtt_publish_status_task, "mqtt.status.task", 4096, NULL, 5,
                  &mqtt_status_task_h) != pdPASS) {
    ESP_LOGE(TAG, "Couldn't create the MQTT status task!");
  }
  // Create the status timer
  /// TODO: Turn defaults into KConfig options
  status_timer_h = xTimerCreate("status.timer", 30000, true, NULL, publish_status_callback);
  status_timer_h =
      xTimerCreate("status.timer", pdMS_TO_TICKS(30000), true, NULL, publish_status_callback);
  xTimerStart(status_timer_h, portMAX_DELAY);
}  // end app_main

#ifdef __cplusplus
}
#endif
