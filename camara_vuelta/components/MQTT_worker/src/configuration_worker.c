#include "configuration_worker.h"

static const char *TAG = "ConfWorker"; /**< Logging tag for this module. */

/*================= Globals =================*/
/*================== Static functions ==================*/
/*================== Event Handlers ==================*/
void conf_applied_handler(void *handler_arg, esp_event_base_t event_base, int32_t event_id,
                          void *event_data) {
  handler_ctx_t *handler_conf;
  cJSON         *payload = cJSON_CreateObject();
  char          *payload_str;
  char           topic_name[1024] = "";

  handler_conf = (handler_ctx_t *)handler_arg;
  ESP_LOGD(TAG, "Configuration applied succesfully");
  cJSON_AddBoolToObject(payload, "conf_applied", true);
  snprintf(topic_name, sizeof(topic_name),
           CONFIG_PROJ_BASE_NAME "/" CONFIG_PROJ_ENV_NAME "/cameras/%s/configure/response",
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

void conf_rejected_handler(void *handler_arg, esp_event_base_t event_base, int32_t event_id,
                           void *event_data) {
  handler_ctx_t *handler_conf;
  cJSON         *payload = cJSON_CreateObject();
  char          *payload_str;
  char           topic_name[1024] = "";

  handler_conf = (handler_ctx_t *)handler_arg;
  ESP_LOGW(TAG, "Configuration rejected");
  cJSON_AddBoolToObject(payload, "conf_applied", false);
  /// TODO: Check that `event_data` contains a valid string before adding it to payload
  cJSON_AddStringToObject(payload, "reason", event_data);
  snprintf(topic_name, sizeof(topic_name),
           CONFIG_PROJ_BASE_NAME "/" CONFIG_PROJ_ENV_NAME "/cameras/%s/configure/response",
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

void conf_error_handler(void *handler_arg, esp_event_base_t event_base, int32_t event_id,
                        void *event_data) {
  handler_ctx_t *handler_conf;
  cJSON         *payload = cJSON_CreateObject();
  char          *payload_str;
  char           topic_name[1024] = "";

  handler_conf = (handler_ctx_t *)handler_arg;
  ESP_LOGD(TAG, "Configuration rejected");
  cJSON_AddBoolToObject(payload, "conf_applied", false);
  /// TODO: Check that `event_data` contains a valid string before adding it to payload
  cJSON_AddStringToObject(payload, "error_msg", event_data);
  snprintf(topic_name, sizeof(topic_name),
           CONFIG_PROJ_BASE_NAME "/" CONFIG_PROJ_ENV_NAME "/cameras/%s/configure/response",
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
esp_err_t conf_parse_cjson_payload(cJSON *payload, configuration_t *out_conf) {
  // Check that the formatting is correct
  if (!cJSON_IsNumber(cJSON_GetObjectItem(payload, "status_period_ms")) ||
      !cJSON_IsNumber(cJSON_GetObjectItem(payload, "min_sd_free_space_kb")) ||
      !cJSON_IsNumber(cJSON_GetObjectItem(payload, "tcp_keep_alive_idle_s")) ||
      !cJSON_IsNumber(cJSON_GetObjectItem(payload, "tcp_keep_alive_interval_s")) ||
      !cJSON_IsNumber(cJSON_GetObjectItem(payload, "tcp_keep_alive_retries"))) {
    ESP_LOGW(TAG, "Invalid payload format");
    return ESP_ERR_INVALID_ARG;
  }

  // Extract values from payload
  out_conf->status_period_ms =
      cJSON_GetNumberValue(cJSON_GetObjectItem(payload, "status_period_ms"));
  out_conf->min_sd_free_space_kb =
      cJSON_GetNumberValue(cJSON_GetObjectItem(payload, "min_sd_free_space_kb"));
  out_conf->tcp_keep_alive_idle_s =
      cJSON_GetNumberValue(cJSON_GetObjectItem(payload, "tcp_keep_alive_idle_s"));
  out_conf->tcp_keep_alive_interval_s =
      cJSON_GetNumberValue(cJSON_GetObjectItem(payload, "tcp_keep_alive_interval_s"));
  out_conf->tcp_keep_alive_retries =
      cJSON_GetNumberValue(cJSON_GetObjectItem(payload, "tcp_keep_alive_retries"));

  return ESP_OK;
}