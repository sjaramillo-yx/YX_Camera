#include "recording_worker.h"

static const char *TAG = "RecWorker"; /**< Logging tag for this module. */

/*================= Globals =================*/

/*================== Static functions ==================*/

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