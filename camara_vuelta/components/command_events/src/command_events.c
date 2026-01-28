#include "command_events.h"

static const char *TAG = "Command Events"; /**< Logging tag for this module. */

/*================= Globals =================*/
static esp_event_loop_handle_t cmd_eventloop_handle = NULL;

/*================= Public functions =================*/
esp_err_t cmd_eventloop_create() {
  esp_err_t             ret;
  esp_event_loop_args_t cmd_eventloop_args = {.queue_size      = 5,
                                              .task_name       = "cmd.eventloop",
                                              .task_priority   = 15,
                                              .task_stack_size = 4096,
                                              .task_core_id    = tskNO_AFFINITY};
  ESP_GOTO_ON_ERROR(esp_event_loop_create(&cmd_eventloop_args, &cmd_eventloop_handle), fail, TAG,
                    "Couldn't create the command event loop");
  return ESP_OK;
fail:
  return ret;
}

esp_err_t cmd_eventloop_get_handle(esp_event_loop_handle_t *out_handle) {
  if (!cmd_eventloop_handle) {
    ESP_LOGE(TAG, "The event loop has not been created yet!");
    return ESP_ERR_INVALID_STATE;
  }
  *out_handle = cmd_eventloop_handle;
  return ESP_OK;
}