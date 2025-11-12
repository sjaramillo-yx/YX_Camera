#include "recording_events.h"

static const char *TAG = "Recording Events"; /**< Logging tag for this module. */

/*================= Globals =================*/
static esp_event_loop_handle_t rec_eventloop_handle = NULL;

/*================= Public functions =================*/
esp_err_t rec_eventloop_create() {
  esp_err_t             ret;
  esp_event_loop_args_t rec_eventloop_args = {.queue_size      = 5,
                                              .task_name       = "recording_loop_task",
                                              .task_priority   = uxTaskPriorityGet(NULL),
                                              .task_stack_size = 3072,
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