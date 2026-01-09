#include "s3_uploader.h"
#include "http_helpers.h"

static const char *TAG = "S3Uploader";

/// TODO: turn this into a KConfig option or part of the configuration
#define S3_FILE_EXT "bin"

/*-------- Structs ---------*/
/**
 * @brief All possible commands from the backend
 */
typedef enum {
  CMD_NONE = 0,
  CMD_INIT_INFO,  // Initial info for upload
  CMD_PART_INFO,  // Information about a part (part number, URL)
  CMD_UPLOAD_COMPLETE,
} cmd_kind_t;

/**
 * @brief A "start upload" message. Contains the recording ID for the video that should be uploaded
 * (the filename matches the recording ID) and an upload ID used for the upload process.
 */
typedef struct {
  char recording_id[CONFIG_S3_MAX_RECORDING_ID];
  char upload_id[CONFIG_S3_MAX_UPLOAD_ID];
} start_msg_t;

/**
 * @brief Command message structure to parse incoming commands from AWS. Depending on the kind of
 * message (start message, part info, etc.) some fields may not be filled.
 */
typedef struct {
  cmd_kind_t kind;                                      // The command contained in this message
  char       recording_id[CONFIG_S3_MAX_RECORDING_ID];  // The recording ID to be uploaded
  char       upload_id[CONFIG_S3_MAX_UPLOAD_ID];        // The upload ID for this process

  // init_info
  char   bucket[CONFIG_S3_MAX_BUCKET_LEN];  // The bucket where the file will be stored
  char   key[CONFIG_S3_MAX_KEY_LEN];        // Path inside the bucket
  size_t part_size;                         // The size in bytes of each part
  int    total_parts;                       // Total parts for this upload

  // part_info
  int  part_number;
  char url[CONFIG_S3_MAX_URL_LEN];  // The presigned URL for this part
} cmd_msg_t;

/**
 * @brief A part (from a multipart upload) with it's corresponding E-Tag
 */
typedef struct {
  bool present;
  char etag[CONFIG_S3_MAX_ETAG_LEN];
} part_slot_t;

typedef struct {
  char         recording_id[CONFIG_S3_MAX_RECORDING_ID];
  char         upload_id[CONFIG_S3_MAX_UPLOAD_ID];
  char         bucket[CONFIG_S3_MAX_BUCKET_LEN];
  char         key[CONFIG_S3_MAX_KEY_LEN];
  size_t       part_size;
  int          total_parts;
  char         file_path[CONFIG_S3_MAX_PATH_LEN + 4];  // +4 because of extension
  size_t       file_size;
  part_slot_t *parts;
  int          parts_uploaded;
} upload_ctx_t;

/**
 * @brief Uploader state machine possible states
 */
typedef enum {
  ST_IDLE = 0,
  ST_WAIT_INIT,
  ST_WAIT_PARTS,
  ST_UPLOADING,
} uploader_state_t;

/*-------- Globals ---------*/
static esp_mqtt_client_handle_t client;
static uploader_state_t         state = ST_IDLE;
static bool                     initialized;
static bool                     mqtt_connected;

static char thing_name[CONFIG_S3_MAX_THING_NAME];
static char rec_dir[CONFIG_S3_MAX_PATH_LEN - CONFIG_S3_MAX_RECORDING_ID];

// HTTP parmeters
static int http_timeout_ms;
static int http_put_retries;

// MQTT topics
static char topic_start[CONFIG_S3_MAX_TOPIC_LEN];
static char topic_commands_active[CONFIG_S3_MAX_TOPIC_LEN];
static char topic_status_active[CONFIG_S3_MAX_TOPIC_LEN];

// Message payloads
static start_msg_t start_message_temp;

// Current upload
static upload_ctx_t current_upload;
static uint32_t     last_ready_for_parts_ms = 0;
static uint8_t      ready_for_parts_count   = 0;

// FreeRTOS
static TaskHandle_t  task;    // The main S3 uploader task
static QueueHandle_t start_q; /* start_msg_t */
static QueueHandle_t cmd_q;   /* cmd_msg_t */

/*================== Static functions ==================*/

/*------------------ Helper functions -----------------*/
static double current_time_sec(void) {
  struct timeval system_time;
  gettimeofday(&system_time, NULL);
  return (double)system_time.tv_sec;
}

static esp_err_t publish_json(const char *topic, cJSON *obj, int qos) {
  char *payload_str = cJSON_PrintUnformatted(obj);
  ESP_RETURN_ON_FALSE(payload_str != NULL, ESP_FAIL, TAG, "cJSON print failed");

  int msg_id = esp_mqtt_client_publish(client, topic, payload_str, 0, qos, 0);
  cJSON_free(payload_str);
  ESP_RETURN_ON_FALSE(msg_id >= 0, ESP_FAIL, TAG, "mqtt publish failed");
  return ESP_OK;
}

static esp_err_t subscribe_commands_topic(void) {
  esp_err_t ret = ESP_OK;
  ESP_RETURN_ON_FALSE(mqtt_connected, ESP_ERR_INVALID_STATE, TAG, "MQTT client is not connected");
  ESP_RETURN_ON_FALSE(current_upload.recording_id[0] != '\0', ESP_ERR_INVALID_STATE, TAG,
                      "Recording ID is invalid");
  int id = esp_mqtt_client_subscribe(client, topic_commands_active, 1);
  if (id < 0)
    ESP_LOGW(TAG, "subscribe commands failed: %s", topic_commands_active);
  return ret;
}

static esp_err_t parts_alloc(int total_parts) {
  ESP_LOGD(TAG, "(%s) allocating %d parts", __func__, total_parts);
  ESP_RETURN_ON_FALSE(total_parts > 0, ESP_ERR_INVALID_ARG, TAG, "total_parts invalid");
  free(current_upload.parts);
  current_upload.parts = (part_slot_t *)calloc((size_t)total_parts, sizeof(part_slot_t));
  ESP_RETURN_ON_FALSE(current_upload.parts != NULL, ESP_ERR_NO_MEM, TAG, "OOM parts");
  current_upload.parts_uploaded = 0;
  return ESP_OK;
}

/*------------------ Status publishing -----------------*/
static esp_err_t publish_status_op(const char *op) {
  esp_err_t ret = ESP_OK;
  cJSON    *j   = cJSON_CreateObject();
  ESP_RETURN_ON_FALSE(j != NULL, ESP_ERR_NO_MEM, TAG, "(%s) No memory for JSON payload", __func__);
  cJSON_AddStringToObject(j, "op", op);
  cJSON_AddStringToObject(j, "recording_id", current_upload.recording_id);
  cJSON_AddStringToObject(j, "upload_id", current_upload.upload_id);
  cJSON_AddNumberToObject(j, "timestamp", current_time_sec());
  ret = publish_json(topic_status_active, j, 1);
  cJSON_Delete(j);
  return ret;
}

static esp_err_t publish_status_error(const char *code, const char *message_opt) {
  esp_err_t ret = ESP_OK;
  cJSON    *j   = cJSON_CreateObject();
  cJSON_AddStringToObject(j, "op", "error");
  cJSON_AddStringToObject(j, "recording_id", current_upload.recording_id);
  cJSON_AddStringToObject(j, "upload_id", current_upload.upload_id);
  cJSON_AddStringToObject(j, "error_code", code);
  if (message_opt)
    cJSON_AddStringToObject(j, "message", message_opt);
  cJSON_AddNumberToObject(j, "timestamp", current_time_sec());
  ret = publish_json(topic_status_active, j, 1);
  cJSON_Delete(j);
  return ret;
}

static esp_err_t publish_status_part_uploaded(int part_number, const char *etag) {
  esp_err_t ret = ESP_OK;
  cJSON    *j   = cJSON_CreateObject();
  cJSON_AddStringToObject(j, "op", "part_uploaded");
  cJSON_AddStringToObject(j, "recording_id", current_upload.recording_id);
  cJSON_AddStringToObject(j, "upload_id", current_upload.upload_id);
  cJSON_AddNumberToObject(j, "part_number", part_number);
  cJSON_AddStringToObject(j, "etag", etag);
  cJSON_AddNumberToObject(j, "timestamp", current_time_sec());
  ret = publish_json(topic_status_active, j, 1);
  cJSON_Delete(j);
  return ret;
}

static esp_err_t publish_status_all_parts_uploaded(void) {
  esp_err_t ret = ESP_OK;
  cJSON    *j   = cJSON_CreateObject();
  cJSON_AddStringToObject(j, "op", "all_parts_uploaded");
  cJSON_AddStringToObject(j, "recording_id", current_upload.recording_id);
  cJSON_AddStringToObject(j, "upload_id", current_upload.upload_id);
  cJSON_AddStringToObject(j, "bucket", current_upload.bucket);
  cJSON_AddStringToObject(j, "key", current_upload.key);

  cJSON *parts = cJSON_AddArrayToObject(j, "parts");
  for (int i = 0; i < current_upload.total_parts; i++) {
    if (!parts || !current_upload.parts[i].present)
      continue;
    cJSON *p = cJSON_CreateObject();
    cJSON_AddNumberToObject(p, "part_number", i + 1);
    cJSON_AddStringToObject(p, "etag", current_upload.parts[i].etag);
    cJSON_AddItemToArray(parts, p);
  }

  cJSON_AddNumberToObject(j, "timestamp", current_time_sec());
  ret = publish_json(topic_status_active, j, 1);
  cJSON_Delete(j);
  return ret;
}

/*------------------ Tasks helpers -----------------*/
static void clear_session(void) {
  ESP_LOGD(TAG, "Clearing session, state is 0x%01x", state);
  // If the client is connected and subscribed, unsubscribe
  if (mqtt_connected && topic_commands_active[0] != '\0') {
    int id = esp_mqtt_client_unsubscribe(client, topic_commands_active);
    if (id < 0)
      ESP_LOGW(TAG, "unsubscribe commands failed: %s", topic_commands_active);
  }

  // Clear context
  memset(current_upload.recording_id, 0, sizeof(current_upload.recording_id));
  memset(current_upload.upload_id, 0, sizeof(current_upload.upload_id));
  memset(topic_commands_active, 0, sizeof(topic_commands_active));
  memset(topic_status_active, 0, sizeof(topic_status_active));

  memset(current_upload.bucket, 0, sizeof(current_upload.bucket));
  memset(current_upload.key, 0, sizeof(current_upload.key));
  current_upload.part_size   = 0;
  current_upload.total_parts = 0;

  current_upload.parts_uploaded = 0;
  free(current_upload.parts);
  current_upload.parts = NULL;

  current_upload.file_size = 0;
  memset(current_upload.file_path, 0, sizeof(current_upload.file_path));

  // Go back to idle state
  state = ST_IDLE;
}

static esp_err_t start_session(const start_msg_t *m) {
  esp_err_t ret = ESP_OK;
  cJSON    *j   = NULL;

  ESP_LOGD(TAG, "Starting session, state is 0x%01x", state);
  // If busy: respond error on the requested recording status topic without adopting session
  if (state != ST_IDLE) {
    char tmp_status[CONFIG_S3_MAX_TOPIC_LEN];
    snprintf(tmp_status, sizeof(tmp_status),
             CONFIG_PROJ_BASE_NAME "/" CONFIG_PROJ_ENV_NAME "/uploads/%s/%s/status", thing_name,
             m->recording_id);

    j = cJSON_CreateObject();
    ESP_GOTO_ON_FALSE(j != NULL, ESP_ERR_NO_MEM, exit, TAG, "OOM in %s", __func__);
    ESP_GOTO_ON_FALSE(cJSON_AddStringToObject(j, "op", "error") != NULL, ESP_ERR_NO_MEM, exit, TAG,
                      "OOM in %s", __func__);
    ESP_GOTO_ON_FALSE(cJSON_AddStringToObject(j, "recording_id", m->recording_id) != NULL,
                      ESP_ERR_NO_MEM, exit, TAG, "OOM in %s", __func__);
    ESP_GOTO_ON_FALSE(cJSON_AddStringToObject(j, "upload_id", m->upload_id) != NULL, ESP_ERR_NO_MEM,
                      exit, TAG, "OOM in %s", __func__);
    ESP_GOTO_ON_FALSE(cJSON_AddStringToObject(j, "error_code", "busy") != NULL, ESP_ERR_NO_MEM,
                      exit, TAG, "OOM in %s", __func__);
    ESP_GOTO_ON_FALSE(cJSON_AddNumberToObject(j, "ts_ms", (double)(esp_timer_get_time() / 1000)) !=
                          NULL,
                      ESP_ERR_NO_MEM, exit, TAG, "OOM in %s", __func__);
    ESP_GOTO_ON_ERROR(publish_json(tmp_status, j, 1), exit, TAG, "Couldn't publish in %s",
                      __func__);
    goto exit;
  }

  // Build context strings
  strlcpy(current_upload.recording_id, m->recording_id, sizeof(current_upload.recording_id));
  strlcpy(current_upload.upload_id, m->upload_id, sizeof(current_upload.upload_id));
  snprintf(topic_commands_active, sizeof(topic_commands_active),
           CONFIG_PROJ_BASE_NAME "/" CONFIG_PROJ_ENV_NAME "/uploads/%s/%s/commands", thing_name,
           current_upload.upload_id);
  snprintf(topic_status_active, sizeof(topic_status_active),
           CONFIG_PROJ_BASE_NAME "/" CONFIG_PROJ_ENV_NAME "/uploads/%s/%s/status", thing_name,
           current_upload.upload_id);
  snprintf(current_upload.file_path, sizeof(current_upload.file_path), "%s/%s." S3_FILE_EXT,
           rec_dir, current_upload.recording_id);
  ESP_LOGD(TAG, "Context strings built");

  // Reset the part counter and move into "waiting upload initial message" state
  current_upload.parts_uploaded = 0;
  state                         = ST_WAIT_INIT;

  ESP_LOGD(TAG, "Checking file existance for path %s", current_upload.file_path);
  // Check that file exists
  if (sdman_stat_file(current_upload.file_path, &current_upload.file_size) != ESP_OK) {
    publish_status_error("recording_not_found", NULL);
    ESP_LOGI(TAG, "(%s) Clearing session with id %s", __func__, current_upload.upload_id);
    clear_session();
    goto exit;
  }
  ESP_LOGD(TAG, "File exists, file_size=%d", current_upload.file_size);

  subscribe_commands_topic();

  // Tell AWS we are ready for bucket/parts info
  publish_status_op("waiting_init");
exit:
  if (j)
    cJSON_Delete(j);
  return ret;
}

static esp_err_t parts_set_uploaded(int part_number, const char *etag) {
  ESP_RETURN_ON_FALSE(current_upload.parts != NULL, ESP_ERR_INVALID_STATE, TAG,
                      "parts not allocated");
  ESP_RETURN_ON_FALSE(part_number > 0 && part_number <= current_upload.total_parts,
                      ESP_ERR_INVALID_ARG, TAG, "bad part_number");
  part_slot_t *slot = &current_upload.parts[part_number - 1];
  if (!slot->present) {
    slot->present = true;
    current_upload.parts_uploaded++;
  }
  strlcpy(slot->etag, etag, CONFIG_S3_MAX_ETAG_LEN);
  return ESP_OK;
}

static esp_err_t handle_part_info(const cmd_msg_t *m) {
  esp_err_t ret = ESP_OK;
  ESP_RETURN_ON_FALSE(state == ST_WAIT_PARTS, ESP_ERR_INVALID_STATE, TAG,
                      "Uploader is not waiting for parts");
  // Validate part number
  int part = m->part_number;
  if (part <= 0 || part > current_upload.total_parts) {
    publish_status_error("bad_part_number", NULL);
    goto cleanup;
  }

  // Check that remaining file bytes fit in the part
  size_t offset = (size_t)(part - 1) * current_upload.part_size;
  size_t len    = current_upload.part_size;
  if (offset + len > current_upload.file_size)  // This will be true for the last part
    len = current_upload.file_size - offset;

  // Upload process variables
  char      etag[CONFIG_S3_MAX_ETAG_LEN] = {0};       // ETag for this part
  esp_err_t put_ret                      = ESP_FAIL;  // Wether the part could be uploaded

  // Attempt to upload the part
  ESP_LOGI(TAG, "(%s) Uploading part %d/%d for session with id %s", __func__, part,
           current_upload.total_parts, current_upload.upload_id);
  for (int attempt = 1; attempt <= http_put_retries; attempt++) {
    ESP_LOGV(TAG, "(%s) Putting part to %s", __func__, m->url);
    put_ret = http_put_part(m->url, current_upload.file_path, offset, len, http_timeout_ms, etag);
    if (put_ret == ESP_OK)
      break;
  }

  if (put_ret != ESP_OK) {
    publish_status_error("http_put_failed", NULL);
    goto cleanup;
  }

  ESP_LOGI(TAG, "(%s) Part %d/%d uploaded for session with id %s", __func__, part,
           current_upload.total_parts, current_upload.upload_id);

  // Mark this part as uplaoded and store it's ETag
  if (parts_set_uploaded(part, etag) != ESP_OK) {
    publish_status_error("etag_store_failed", NULL);
    goto cleanup;
  }

  publish_status_part_uploaded(part, etag);

  // Check if all parts where uploaded
  if (current_upload.parts_uploaded >= current_upload.total_parts) {
    publish_status_all_parts_uploaded();
    ESP_LOGI(TAG, "All parts uploaded");
    goto cleanup;
  }

  return ret;

cleanup:
  ESP_LOGI(TAG, "(%s) Clearing session with ID: %s", __func__, current_upload.upload_id);
  clear_session();
  return ret;
}

static esp_err_t handle_init_info(const cmd_msg_t *m) {
  esp_err_t ret = ESP_OK;
  // Confirm the uploader is waiting for initial info
  ESP_RETURN_ON_FALSE(state == ST_WAIT_INIT, ESP_ERR_INVALID_STATE, TAG,
                      "Uploader is not waiting for initial info, current state is 0x%01x", state);

  // Extract bucket and key from the message
  strlcpy(current_upload.bucket, m->bucket, sizeof(current_upload.bucket));
  strlcpy(current_upload.key, m->key, sizeof(current_upload.key));

  // Extract part size from the message
  current_upload.part_size = (m->part_size > 0) ? m->part_size : CONFIG_S3_DEFAULT_PART_SIZE;
  ESP_LOGD(TAG, "(%s) Part size set to %d", __func__, current_upload.part_size);
  // Compute total parts from the filesize and part size
  int computed_parts =
      (int)((current_upload.file_size + current_upload.part_size - 1) / current_upload.part_size);
  // Check that received total parts match the part size
  if (computed_parts != m->total_parts) {
    ESP_LOGW(TAG, "(%s) Computed total parts (%d) don't match received total parts (%d)", __func__,
             computed_parts, current_upload.total_parts);
    /// TODO: Signal AWS to change total parts to the computed value
    // current_upload.total_parts = computed_parts;
  }
  current_upload.total_parts = m->total_parts;
  ESP_LOGD(TAG, "(%s) Total parts: %d", __func__, current_upload.total_parts);
  // Try to allocate and publish out of memory message on failure
  if (parts_alloc(current_upload.total_parts) != ESP_OK) {
    publish_status_error("oom_parts", NULL);
    ESP_LOGI(TAG, "(%s) Clearing session with ID %s", __func__, current_upload.upload_id);
    clear_session();
    return ESP_ERR_NO_MEM;
  }

  // Move to the next state and inform AWS
  state = ST_WAIT_PARTS;
  publish_status_op("ready_for_parts");
  last_ready_for_parts_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
  ready_for_parts_count   = 0;
  return ret;
}

/*--------------- FreeRTOS Tasks ---------------*/
static void uploader_task(void *p) {
  start_msg_t sm = {0};
  cmd_msg_t   cm = {0};

  clear_session();

  while (true) {
    // Wait 50 ticks for a start message
    if (xQueueReceive(start_q, &sm, pdMS_TO_TICKS(50)) == pdTRUE) {
      ESP_LOGD(TAG, "Popped start message from queue");
      start_session(&sm);
      ESP_LOGI(TAG, "(%s) Session started with ID %s", __func__, current_upload.upload_id);
    }

    // Check for pending commands
    while (xQueueReceive(cmd_q, &cm, 0) == pdTRUE) {
      // Check if there's an ongoing upload
      if (current_upload.recording_id[0] == '\0')
        /// TODO: Post an error message (no upload in course)
        continue;
      if (strcmp(cm.recording_id, current_upload.recording_id) != 0)
        /// TODO: Post an error message (mismatching recording id)
        continue;
      if (strcmp(cm.upload_id, current_upload.upload_id) != 0)
        /// TODO: Post an error message (mismatching upload id)
        continue;

      // Identify command and handle accordingly
      switch (cm.kind) {
      case CMD_INIT_INFO:
        ESP_LOGD(TAG, "Handling initial info command");
        handle_init_info(&cm);
        break;
      case CMD_PART_INFO:
        ESP_LOGD(TAG, "Handling part info command");
        uint32_t now_ms         = xTaskGetTickCount() * portTICK_PERIOD_MS;
        last_ready_for_parts_ms = now_ms;
        handle_part_info(&cm);
        break;
      case CMD_UPLOAD_COMPLETE:
        ESP_LOGD(TAG, "Upload complete");
        /// TODO: Maybe sent an ack message for this command
        break;
      default:
        break;
      }
    }

    // Heartbeat: if we're waiting for parts and haven't heard back, re-publish ready_for_parts
    if (state == ST_WAIT_PARTS) {
      uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
      if (last_ready_for_parts_ms == 0 ||
          (now_ms - last_ready_for_parts_ms) > CONFIG_S3_READY_FOR_PARTS_MS) {
        ESP_LOGW(TAG, "Re-publishing ready_for_parts (heartbeat)");
        publish_status_op("ready_for_parts");
        last_ready_for_parts_ms = now_ms;
        ready_for_parts_count++;
      }
      if (ready_for_parts_count > CONFIG_S3_MAX_READY_FOR_PARTS) {
        ESP_LOGE(TAG, "(%s) Waiting for parts timed out", __func__);
        publish_status_error("ready_for_parts_timeout", NULL);
        ESP_LOGI(TAG, "(%s) Clearing session with ID %s", __func__, current_upload.upload_id);
        clear_session();
      }
    }

    // Yield scheduler
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

/* ---------------- Parsing incoming MQTT ---------------- */
static bool parse_start_payload(const char *data, int len, start_msg_t *out) {
  cJSON *j = cJSON_ParseWithLength(data, len);
  if (!j)
    return false;

  const cJSON *rid = cJSON_GetObjectItem(j, "recordingId");
  if (!cJSON_IsString(rid))
    rid = cJSON_GetObjectItem(j, "recording_id");

  const cJSON *uid = cJSON_GetObjectItem(j, "uploadId");
  if (!cJSON_IsString(uid))
    uid = cJSON_GetObjectItem(j, "upload_id");

  bool ok = cJSON_IsString(rid) && cJSON_IsString(uid);
  if (ok) {
    strlcpy(out->recording_id, rid->valuestring, sizeof(out->recording_id));
    strlcpy(out->upload_id, uid->valuestring, sizeof(out->upload_id));
  }
  cJSON_Delete(j);
  return ok;
}

static bool parse_commands_payload(const char *data, int len, cmd_msg_t *out) {
  cJSON *j = cJSON_ParseWithLength(data, len);
  if (!j)
    return false;

  // Look for the "op" field in the payload
  const cJSON *op = cJSON_GetObjectItem(j, "op");
  if (!cJSON_IsString(op)) {
    cJSON_Delete(j);
    return false;
  }

  // Clear the output for parsing and build the recording and upload ids
  memset(out, 0, sizeof(*out));
  out->kind = CMD_NONE;
  strlcpy(out->recording_id, current_upload.recording_id, sizeof(out->recording_id));
  strlcpy(out->upload_id, current_upload.upload_id, sizeof(out->upload_id));

  if (strcmp(op->valuestring, "init_info") == 0) {
    // If the command is the initial info for an upload extract the information from the payload
    out->kind           = CMD_INIT_INFO;
    const cJSON *bucket = cJSON_GetObjectItem(j, "bucket");
    const cJSON *key    = cJSON_GetObjectItem(j, "key");
    const cJSON *psize  = cJSON_GetObjectItem(j, "part_size");
    const cJSON *tparts = cJSON_GetObjectItem(j, "total_parts");

    // validate the bucket and key
    if (!cJSON_IsString(bucket) || !cJSON_IsString(key)) {
      cJSON_Delete(j);
      return false;
    }
    strlcpy(out->bucket, bucket->valuestring, sizeof(out->bucket));
    strlcpy(out->key, key->valuestring, sizeof(out->key));
    // Get the part size and total parts
    out->part_size =
        (cJSON_IsNumber(psize) && psize->valuedouble > 0) ? (size_t)psize->valuedouble : 0;
    ESP_LOGD(TAG, "(%s) Received part size: %d", __func__, out->part_size);
    out->total_parts = (cJSON_IsNumber(tparts) && tparts->valueint > 0) ? tparts->valueint : 0;
    ESP_LOGD(TAG, "(%s) Received total parts: %d", __func__, out->total_parts);
  } else if (strcmp(op->valuestring, "part") == 0 || strcmp(op->valuestring, "part_info") == 0) {
    // If the incoming command is a part upload command, extract the part number and presigned url
    out->kind        = CMD_PART_INFO;
    const cJSON *pn  = cJSON_GetObjectItem(j, "part_number");
    const cJSON *url = cJSON_GetObjectItem(j, "url");
    // Validate part number and url
    if (!cJSON_IsNumber(pn) || !cJSON_IsString(url)) {
      cJSON_Delete(j);
      return false;
    }
    out->part_number = pn->valueint;
    strlcpy(out->url, url->valuestring, sizeof(out->url));
  } else if (strcmp(op->valuestring, "upload_complete") == 0) {
    // If the command is an upload complete command, simply mark it as such
    out->kind = CMD_UPLOAD_COMPLETE;
  }

  cJSON_Delete(j);
  return out->kind != CMD_NONE;
}

/*--------------- Public functions ---------------*/
esp_err_t s3uploader_init(esp_mqtt_client_handle_t mqtt_client, const s3uploader_cfg_t *cfg) {
  esp_err_t ret = ESP_OK;
  ESP_RETURN_ON_FALSE(mqtt_client != NULL, ESP_ERR_INVALID_ARG, TAG, "client is NULL");
  ESP_RETURN_ON_FALSE(cfg != NULL, ESP_ERR_INVALID_ARG, TAG, "cfg is NULL");
  ESP_RETURN_ON_FALSE(cfg->thing_name && cfg->thing_name[0] != '\0', ESP_ERR_INVALID_ARG, TAG,
                      "thing_name required");
  ESP_RETURN_ON_FALSE(cfg->rec_dir && cfg->rec_dir[0] != '\0', ESP_ERR_INVALID_ARG, TAG,
                      "rec_dir required");
  /// TODO: Check if SD card is mounted

  // General uploader configuration
  client = mqtt_client;
  strlcpy(thing_name, cfg->thing_name, sizeof(thing_name));
  strlcpy(rec_dir, cfg->rec_dir, sizeof(rec_dir));

  // HTTP configuration
  http_timeout_ms = (cfg->http_timeout_ms > 0) ? cfg->http_timeout_ms : CONFIG_S3_HTTP_TIMEOUT_MS;
  http_put_retries =
      (cfg->http_put_retries > 0) ? cfg->http_put_retries : CONFIG_S3_HTTP_PUT_RETRIES;

  // Build "upload start" MQTT topic
  snprintf(topic_start, sizeof(topic_start),
           CONFIG_PROJ_BASE_NAME "/" CONFIG_PROJ_ENV_NAME "/uploads/%s/start", thing_name);
  ESP_LOGD(TAG, "Built start topic: %s", topic_start);

  // Create the queues
  start_q = xQueueCreate(4, sizeof(start_msg_t));
  cmd_q   = xQueueCreate(8, sizeof(cmd_msg_t));
  ESP_GOTO_ON_FALSE(start_q && cmd_q, ESP_ERR_NO_MEM, cleanup, TAG, "Queue create failed");

  // Create the uploader task
  BaseType_t ok = xTaskCreate(uploader_task, "s3.uploader", 10240, NULL, 8, &task);
  ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_FAIL, TAG, "Task create failed");

  // Mark the S3 uploader as initialized
  initialized    = true;
  mqtt_connected = false;
  return ESP_OK;

cleanup:
  if (start_q)
    vQueueDelete(start_q);
  if (cmd_q)
    vQueueDelete(cmd_q);
  return ret;
}

esp_err_t s3_uploader_on_connected(void) {
  ESP_RETURN_ON_FALSE(initialized, ESP_ERR_INVALID_STATE, TAG, "Not initialized");
  ESP_RETURN_ON_FALSE(client != NULL, ESP_ERR_INVALID_STATE, TAG, "MQTT client missing");

  mqtt_connected = true;

  ESP_RETURN_ON_FALSE(esp_mqtt_client_subscribe(client, topic_start, 1) >= 0, ESP_FAIL, TAG,
                      "Subscribe failed start");

  // If a session is active during reconnect, resubscribe to its commands topic
  subscribe_commands_topic();
  return ESP_OK;
}

esp_err_t s3_uploader_handler(const char *topic, const char *data, int data_len) {
  // Check that the uploader is initialized
  ESP_RETURN_ON_FALSE(initialized, ESP_ERR_INVALID_STATE, TAG, "Not initialized");
  ESP_RETURN_ON_FALSE(topic && data && data_len >= 0, ESP_ERR_INVALID_ARG, TAG, "Bad args");

  // Check if the received topic matches the start upload topic
  if (!strcmp(topic, topic_start)) {
    ESP_LOGD(TAG, "Received a start upload command");
    // If the payload matches a start command, send it to the back of the start queue
    memset(&start_message_temp, 0, sizeof(start_msg_t));
    if (parse_start_payload(data, data_len, &start_message_temp)) {
      ESP_LOGD(TAG, "Message correctly parsed");
      BaseType_t sent = xQueueSendToBack(start_q, &start_message_temp, 0);
      ESP_LOGD(TAG, "Message %s to the start queue", sent == pdTRUE ? "sent" : "not sent");
    }
    return ESP_OK;
  }

  // Check if the received topic matches the command topic for the current upload
  if ((current_upload.recording_id[0] != '\0') && (strcmp(topic, topic_commands_active) == 0)) {
    cmd_msg_t cm = {0};
    // Parse the payload and if it matches a command message send it to the command queue
    if (parse_commands_payload(data, data_len, &cm)) {
      (void)xQueueSendToBack(cmd_q, &cm, 0);
    }
    return ESP_OK;
  }

  return ESP_OK;
}
