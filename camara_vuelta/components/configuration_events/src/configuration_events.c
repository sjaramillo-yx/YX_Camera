#include "configuration_events.h"

static const char *TAG = "Configuration Events"; /**< Logging tag for this module. */

/*================= Globals =================*/
static esp_event_loop_handle_t conf_eventloop_handle = NULL;

/*================= Public functions =================*/
esp_err_t conf_eventloop_create() {
  esp_err_t             ret;
  esp_event_loop_args_t conf_eventloop_args = {.queue_size      = 5,
                                               .task_name       = "conf.eventloop",
                                               .task_priority   = 15,
                                               .task_stack_size = 4096,
                                               .task_core_id    = tskNO_AFFINITY};
  ESP_GOTO_ON_ERROR(esp_event_loop_create(&conf_eventloop_args, &conf_eventloop_handle), fail, TAG,
                    "Couldn't create the configuration event loop");
  return ESP_OK;
fail:
  return ret;
}

esp_err_t conf_eventloop_get_handle(esp_event_loop_handle_t *out_handle) {
  if (!conf_eventloop_handle) {
    ESP_LOGE(TAG, "The event loop has not been created yet!");
    return ESP_ERR_INVALID_STATE;
  }
  *out_handle = conf_eventloop_handle;
  return ESP_OK;
}