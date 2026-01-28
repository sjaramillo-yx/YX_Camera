#include "recording_worker.h"
#include "handler_types.h"

static const char *TAG = "RecWorker"; /**< Logging tag for this module. */

/*================= Globals =================*/
/*================== Static functions ==================*/
/*================== Event Handlers ==================*/
void rec_started_handler(void *handler_arg, esp_event_base_t event_base, int32_t event_id,
                         void *event_data) {
  handler_ctx_t    *handler_conf;
  recording_conf_t *vman_rec_params;
  cJSON            *payload = cJSON_CreateObject();
  cJSON            *qps;
  cJSON            *res;
  char             *payload_str;
  char              topic_name[1024] = "";

  handler_conf    = (handler_ctx_t *)handler_arg;
  vman_rec_params = (recording_conf_t *)event_data;
  ESP_LOGI(TAG, "Video Manager started recording with ID %s", vman_rec_params->transaction_id);
  qps = cJSON_AddArrayToObject(payload, "QPs");
  res = cJSON_AddArrayToObject(payload, "resolution");
  cJSON_AddStringToObject(payload, "status", "STARTED");
  cJSON_AddStringToObject(payload, "transactionId", vman_rec_params->transaction_id);
  cJSON_AddItemToArray(qps, cJSON_CreateNumber(vman_rec_params->qp_min));
  cJSON_AddItemToArray(qps, cJSON_CreateNumber(vman_rec_params->qp_max));
  cJSON_AddItemToArray(res, cJSON_CreateNumber(vman_rec_params->hres));
  cJSON_AddItemToArray(res, cJSON_CreateNumber(vman_rec_params->vres));
  cJSON_AddNumberToObject(payload, "targetFps", vman_rec_params->fps);
  cJSON_AddNumberToObject(payload, "targetBitrate", vman_rec_params->target_bitrate);
  cJSON_AddNumberToObject(payload, "timeout", vman_rec_params->timeout_seconds);
  snprintf(topic_name, sizeof(topic_name),
           CONFIG_PROJ_BASE_NAME "/" CONFIG_PROJ_ENV_NAME "/recordings/%s/%s/status",
           handler_conf->thing_name, vman_rec_params->transaction_id);
  topic_name[sizeof(topic_name) - 1] = '\0';
  payload_str                        = cJSON_Print(payload);
  int msg_id =
      esp_mqtt_client_publish(*handler_conf->mqtt_client, topic_name, payload_str, 0, 1, 0);
  if (payload_str)
    cJSON_free(payload_str);
  ESP_LOGD(TAG, "[%s] Sent publish successful, msg_id=%d", __func__, msg_id);
  if (payload)
    cJSON_Delete(payload);
}

void rec_done_handler(void *handler_arg, esp_event_base_t event_base, int32_t event_id,
                      void *event_data) {
  handler_ctx_t    *handler_conf;
  recording_file_t *vman_rec_file;
  cJSON            *payload = cJSON_CreateObject();
  cJSON            *res;
  char             *payload_str;
  char              topic_name[1024] = "";

  handler_conf  = (handler_ctx_t *)handler_arg;
  vman_rec_file = (recording_file_t *)event_data;
  cJSON_AddStringToObject(payload, "status", "DONE");
  cJSON_AddStringToObject(payload, "transactionId", vman_rec_file->transaction_id);
  res = cJSON_AddArrayToObject(payload, "resolution");
  cJSON_AddItemToArray(res, cJSON_CreateNumber(vman_rec_file->hres));
  cJSON_AddItemToArray(res, cJSON_CreateNumber(vman_rec_file->vres));
  cJSON_AddStringToObject(payload, "filename", vman_rec_file->filename);
  cJSON_AddNumberToObject(payload, "filesize", vman_rec_file->size);
  cJSON_AddNumberToObject(payload, "recordedSeconds", vman_rec_file->recorded_seconds);
  snprintf(topic_name, 1024,
           CONFIG_PROJ_BASE_NAME "/" CONFIG_PROJ_ENV_NAME "/recordings/%s/%s/status",
           handler_conf->thing_name, vman_rec_file->transaction_id);
  payload_str = cJSON_Print(payload);
  int msg_id =
      esp_mqtt_client_publish(*handler_conf->mqtt_client, topic_name, payload_str, 0, 1, 0);
  ESP_LOGD(TAG, "[%s] Sent publish successful, msg_id=%d", __func__, msg_id);
  // Unsubscribe from the recording topic
  snprintf(topic_name, 1024,
           CONFIG_PROJ_BASE_NAME "/" CONFIG_PROJ_ENV_NAME "/recordings/%s/%s/commands",
           handler_conf->thing_name, vman_rec_file->transaction_id);
  esp_mqtt_client_unsubscribe(*handler_conf->mqtt_client, topic_name);
  if (payload_str)
    cJSON_free(payload_str);
  if (payload)
    cJSON_Delete(payload);
}

void rec_error_handler(void *handler_arg, esp_event_base_t event_base, int32_t event_id,
                       void *event_data) {
  handler_ctx_t     *handler_conf;
  recording_error_t *vman_rec_error;
  cJSON             *payload = cJSON_CreateObject();
  char              *payload_str;
  char               topic_name[1024] = "";
  int                msg_id;

  handler_conf   = (handler_ctx_t *)handler_arg;
  vman_rec_error = (recording_error_t *)event_data;
  ESP_LOGI(TAG, "[%s] Module %s reported error 0x%02x (%s)", __func__,
           vman_rec_error->errored_module, vman_rec_error->error_code,
           vman_rec_error->error_message);
  struct timeval system_time;
  gettimeofday(&system_time, NULL);
  cJSON_AddStringToObject(payload, "status", "ERROR");
  cJSON_AddStringToObject(payload, "thingName", handler_conf->thing_name);
  cJSON_AddStringToObject(payload, "errorCode", esp_err_to_name(vman_rec_error->error_code));
  cJSON_AddStringToObject(payload, "errorMessage", vman_rec_error->error_message);
  cJSON_AddStringToObject(payload, "errorOrigin", vman_rec_error->errored_module);
  cJSON_AddNumberToObject(payload, "timestamp", (uint64_t)system_time.tv_sec);
  payload_str = cJSON_Print(payload);
  if (strcmp(vman_rec_error->transaction_id, "")) {
    snprintf(topic_name, 1024,
             CONFIG_PROJ_BASE_NAME "/" CONFIG_PROJ_ENV_NAME "/recordings/%s/%s/status",
             handler_conf->thing_name, vman_rec_error->transaction_id);
  } else {
    strncpy(topic_name, CONFIG_PROJ_BASE_NAME "/" CONFIG_PROJ_ENV_NAME "/cameras/error",
            sizeof(topic_name));
  }
  msg_id = esp_mqtt_client_publish(*handler_conf->mqtt_client, topic_name, payload_str, 0, 1, 0);

  ESP_LOGD(TAG, "[%s] Sent publish successful, msg_id=%d", __func__, msg_id);
  if (payload_str)
    cJSON_free(payload_str);

  if (payload)
    cJSON_Delete(payload);
}

/*================== Public Functions ==================*/
esp_err_t parse_recording_start(cJSON *payload, recording_conf_t *out_rec_conf) {
  esp_err_t ret = ESP_OK;
  cJSON    *res, *qps;
  // Extract array parameters from payload
  res = cJSON_GetObjectItem(payload, "resolution");
  qps = cJSON_GetObjectItem(payload, "QPs");
  /*------------ Check for valid schema ------------*/
  // Check version
  ESP_GOTO_ON_FALSE(cJSON_IsNumber(cJSON_GetObjectItem(payload, "version")),
                    ESP_ERR_INVALID_VERSION, cleanup, TAG, "Invalid schema version");
  // Check valid resolution format
  ESP_GOTO_ON_FALSE(cJSON_IsArray(res) && cJSON_GetArraySize(res) == 2 &&
                        cJSON_IsNumber(cJSON_GetArrayItem(res, 0)) &&
                        cJSON_IsNumber(cJSON_GetArrayItem(res, 1)),
                    ESP_ERR_INVALID_ARG, cleanup, TAG, "Invalid resolution array");
  // Check valid QP format
  ESP_GOTO_ON_FALSE(cJSON_IsArray(qps) && cJSON_GetArraySize(qps) == 2 &&
                        cJSON_IsNumber(cJSON_GetArrayItem(qps, 0)) &&
                        cJSON_IsNumber(cJSON_GetArrayItem(qps, 1)),
                    ESP_ERR_INVALID_ARG, cleanup, TAG, "Invalid QP array");
  // Check the rest of the fields
  ESP_GOTO_ON_FALSE(cJSON_IsNumber(cJSON_GetObjectItem(payload, "fps")), ESP_ERR_INVALID_ARG,
                    cleanup, TAG, "Invalid fps");
  ESP_GOTO_ON_FALSE(cJSON_IsNumber(cJSON_GetObjectItem(payload, "timeout")), ESP_ERR_INVALID_ARG,
                    cleanup, TAG, "Invalid timeout");
  ESP_GOTO_ON_FALSE(cJSON_IsString(cJSON_GetObjectItem(payload, "transactionId")),
                    ESP_ERR_INVALID_ARG, cleanup, TAG, "Invalid transactionId");
  ESP_GOTO_ON_FALSE(cJSON_IsNumber(cJSON_GetObjectItem(payload, "targetBitrate")),
                    ESP_ERR_INVALID_ARG, cleanup, TAG, "Invalid targetBitrate");
  // Write values to output
  out_rec_conf->hres            = (uint16_t)cJSON_GetArrayItem(res, 0)->valuedouble;
  out_rec_conf->vres            = (uint16_t)cJSON_GetArrayItem(res, 1)->valuedouble;
  out_rec_conf->fps             = (uint16_t)cJSON_GetObjectItem(payload, "fps")->valuedouble;
  out_rec_conf->qp_min          = (uint8_t)cJSON_GetArrayItem(qps, 0)->valuedouble;
  out_rec_conf->qp_max          = (uint8_t)cJSON_GetArrayItem(qps, 1)->valuedouble;
  out_rec_conf->timeout_seconds = (uint32_t)cJSON_GetObjectItem(payload, "timeout")->valuedouble;
  out_rec_conf->target_bitrate =
      (uint32_t)cJSON_GetObjectItem(payload, "targetBitrate")->valuedouble;
  // Copy transaction ID
  strncpy(out_rec_conf->transaction_id, cJSON_GetObjectItem(payload, "transactionId")->valuestring,
          sizeof(out_rec_conf->transaction_id) - 1);
  out_rec_conf->transaction_id[sizeof(out_rec_conf->transaction_id) - 1] = '\0';
  // Check if recording has an associated Job
  cJSON *job                = cJSON_GetObjectItemCaseSensitive(payload, "jobId");
  out_rec_conf->has_aws_job = cJSON_IsString(job);
  if (out_rec_conf->has_aws_job) {
    strncpy(out_rec_conf->aws_job_id, job->valuestring, sizeof(out_rec_conf->aws_job_id) - 1);
    out_rec_conf->aws_job_id[sizeof(out_rec_conf->aws_job_id) - 1] = '\0';
  } else {
    out_rec_conf->aws_job_id[0] = '\0';
  }

cleanup:
  return ret;
}