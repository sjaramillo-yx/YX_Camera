#include "command_worker.h"

static const char *TAG = "CmdWorker"; /**< Logging tag for this module. */

/*================= Globals =================*/
/*================== Static functions ==================*/
/*================== Event Handlers ==================*/
void cmd_done_handler(void *handler_arg, esp_event_base_t event_base, int32_t event_id,
                      void *event_data) {
  handler_ctx_t *handler_conf;
  cJSON         *payload = cJSON_CreateObject();
  char          *payload_str;
  char           topic_name[1024] = "";

  handler_conf = (handler_ctx_t *)handler_arg;
  ESP_LOGD(TAG, "Command executed succesfully");
  cJSON_AddBoolToObject(payload, "cmd_executed", true);
  snprintf(topic_name, sizeof(topic_name),
           CONFIG_PROJ_BASE_NAME "/" CONFIG_PROJ_ENV_NAME "/cameras/%s/commands/response",
           handler_conf->thing_name);
  topic_name[sizeof(topic_name) - 1] = '\0';
  payload_str                        = cJSON_Print(payload);
  int msg_id =
      esp_mqtt_client_publish(*(handler_conf->mqtt_client), topic_name, payload_str, 0, 1, 0);
  if (payload_str)
    cJSON_free(payload_str);
  ESP_LOGD(TAG, "[%s] Sent publish successful, msg_id=%d", __func__, msg_id);
  if (payload)
    cJSON_Delete(payload);
}

void cmd_error_handler(void *handler_arg, esp_event_base_t event_base, int32_t event_id,
                       void *event_data) {
  handler_ctx_t *handler_conf;
  cJSON         *payload = cJSON_CreateObject();
  char          *payload_str;
  char           topic_name[1024] = "";

  handler_conf = (handler_ctx_t *)handler_arg;
  ESP_LOGD(TAG, "Command execution failed");
  cJSON_AddBoolToObject(payload, "cmd_executed", false);
  /// TODO: Check that `event_data` contains a valid string before adding it to payload
  cJSON_AddStringToObject(payload, "error_msg", event_data);
  snprintf(topic_name, sizeof(topic_name),
           CONFIG_PROJ_BASE_NAME "/" CONFIG_PROJ_ENV_NAME "/cameras/%s/commands/response",
           handler_conf->thing_name);
  topic_name[sizeof(topic_name) - 1] = '\0';
  payload_str                        = cJSON_Print(payload);
  int msg_id =
      esp_mqtt_client_publish(*(handler_conf->mqtt_client), topic_name, payload_str, 0, 1, 0);
  if (payload_str)
    cJSON_free(payload_str);
  ESP_LOGD(TAG, "[%s] Sent publish successful, msg_id=%d", __func__, msg_id);
  if (payload)
    cJSON_Delete(payload);
}
/*================== Public Functions ==================*/
esp_err_t cmd_parse_cjson_payload(cJSON *payload, esp_event_loop_handle_t event_loop_handle) {
  // Check that the formatting is correct
  if (!cJSON_IsString(cJSON_GetObjectItem(payload, "command"))) {
    ESP_LOGW(TAG, "Invalid payload format");
    return ESP_ERR_INVALID_ARG;
  }

  if (!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(payload, "command")), "reboot")) {
    ESP_LOGI(TAG, "Reboot command received");
    esp_event_post_to(event_loop_handle, COMMAND_EVENTS, CMD_REBOOT, NULL, 0, portMAX_DELAY);
  } else if (!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(payload, "command")), "format_sd")) {
    esp_event_post_to(event_loop_handle, COMMAND_EVENTS, CMD_FORMAT_SD, NULL, 0, portMAX_DELAY);
  }
  return ESP_OK;
}