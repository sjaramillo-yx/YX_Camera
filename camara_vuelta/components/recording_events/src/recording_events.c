#include "recording_events.h"

static const char *TAG = "Recording Events"; /**< Logging tag for this module. */

/*================= Globals =================*/
static esp_event_loop_handle_t rec_eventloop_handle = NULL;

/*================= Public functions =================*/
esp_err_t rec_eventloop_create() {
  esp_err_t             ret;
  esp_event_loop_args_t rec_eventloop_args = {.queue_size      = 5,
                                              .task_name       = "rec.eventloop",
                                              .task_priority   = 15,
                                              .task_stack_size = 4096,
                                              .task_core_id    = tskNO_AFFINITY};
  ESP_GOTO_ON_ERROR(esp_event_loop_create(&rec_eventloop_args, &rec_eventloop_handle), fail, TAG,
                    "Couldn't create the recording event loop");
  return ESP_OK;
fail:
  return ret;
}

esp_err_t rec_eventloop_get_handle(esp_event_loop_handle_t *out_handle) {
  if (!rec_eventloop_handle) {
    ESP_LOGE(TAG, "The event loop has not been created yet!");
    return ESP_ERR_INVALID_STATE;
  }
  *out_handle = rec_eventloop_handle;
  return ESP_OK;
}

esp_err_t rec_print_config(recording_conf_t *conf) {
  /// TODO: For each value, check for not NULL and data type
  ESP_LOGI(TAG, "Received recording parameters:");
  ESP_LOGI(TAG, "\t\tResolution:%dx%d", conf->hres, conf->vres);
  ESP_LOGI(TAG, "\t\tFPS:%d", conf->fps);
  ESP_LOGI(TAG, "\t\tQPs:%d (max), %d (min)", conf->qp_max, conf->qp_min);
  ESP_LOGI(TAG, "\t\tTimeout:%d", conf->timeout_seconds);
  ESP_LOGI(TAG, "\t\tTransaction ID:%s", conf->transaction_id);
  ESP_LOGI(TAG, "\t\tTarget bitrate:%d", conf->target_bitrate);
  ESP_LOGI(TAG, "\t\tJob ID:%s", conf->has_aws_job ? conf->aws_job_id : "N/A");
  return ESP_OK;
}