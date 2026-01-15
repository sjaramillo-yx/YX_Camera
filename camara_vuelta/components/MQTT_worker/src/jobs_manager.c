#include "jobs_manager.h"

static const char *TAG = "AWSJobsManager"; /**< Logging tag for this module. */

/* ================ STRUCTS ================ */
/**
 * @brief Job status type
 */
typedef enum { IN_PROGRESS = 0, FAILED, SUCCEEDED, REJECTED } job_status_t;

/**
 * @brief Valid jobs for this Thing
 */
typedef enum {
  OTA_UPDATE,
  VIDEO_UPLOAD,
  INVALID = 0xFFFF,
} job_class_t;

const char *job_status_str[] = {"IN_PROGRESS", "FAILED", "SUCCEEDED", "REJECTED"};

typedef struct {
  int    file_id;       // [0-255] The ID of the file to be downloaded
  size_t block_size;    // [256-131072] The size of each block
  int    block_offset;  // [0-98304] Offset (in blocks) of requested blocks in the stream file
  int    n_blocks;      // [0-98304] Number of requested blocks (optional)
  int    block_bitmap;  // A bitmap that represents the blocks being requested (optional).
} get_stream_t;

typedef struct {
  int      file_index;
  int      index;
  size_t   len;
  uint8_t *data;
} data_block_t;

/* ================ Globals ================ */
static ota_stream_t             ota_stream = {0};
static esp_mqtt_client_handle_t mqtt_client;
static mbedtls_sha256_context   s_sha_ctx;  // Hashing context

// FreeRTOS tasks
static TaskHandle_t s_download_task_h;

// Data blocks
static uint8_t *s_data_block_buffers[CONFIG_DATA_BLOCK_COUNT];

// OTA queues
static QueueHandle_t s_free_chunk_q   = NULL;  // items: ota_chunk_t
static QueueHandle_t s_filled_chunk_q = NULL;  // items: ota_chunk_t
static QueueHandle_t s_free_stream_data_q;     // items: data_block_t
static QueueHandle_t s_filled_stream_data_q;   // items: data_block_t

/*================== MQTT Helpers ==================*/
/**
 * @brief Describe a file stream for a given streamID
 *
 * @param client An MQTT client handle for the subscribe and publish functions
 * @param client_token A pointer to a null-terminated string to use as a client token. This string
 * is arbitrary and can be used to correlate requests and responses. Can be NULL.
 * @param thing_name The name of the Thing to receive the stream description
 */
static esp_err_t file_describe_stream(esp_mqtt_client_handle_t client, char *thing_name,
                                      char *client_token, char *stream_id) {
  esp_err_t ret         = ESP_OK;
  cJSON    *payload     = cJSON_CreateObject();
  char     *payload_str = NULL;
  char      topic_name[1024];

  // Subscribe to relevant topics
  sprintf(topic_name, "$aws/things/%s/streams/%s/description/json", thing_name, stream_id);
  ESP_GOTO_ON_FALSE(esp_mqtt_client_subscribe(client, topic_name, 1) >= 0, ESP_FAIL, cleanup, TAG,
                    "Failed subscribing to stream description accepted topic");
  sprintf(topic_name, "$aws/things/%s/streams/%s/rejected/json", thing_name, stream_id);
  ESP_GOTO_ON_FALSE(esp_mqtt_client_subscribe(client, topic_name, 1) >= 0, ESP_FAIL, cleanup, TAG,
                    "Failed subscribing to stream description rejected topic");

  // Build the payload
  client_token = client_token ? client_token : "";
  cJSON_AddStringToObject(payload, "c", client_token);
  // Publish the request
  sprintf(topic_name, "$aws/things/%s/streams/%s/describe/json", thing_name, stream_id);
  ESP_GOTO_ON_FALSE(NULL != (payload_str = cJSON_Print(payload)), ESP_FAIL, cleanup, TAG,
                    "Couldn't print the payload to payload_str");
  ESP_LOGD(TAG, "Publishing to %s\nPayload: %s", topic_name, payload_str);
  ESP_GOTO_ON_FALSE(esp_mqtt_client_publish(client, topic_name, payload_str, 0, 1, 0) >= 0,
                    ESP_FAIL, cleanup, TAG, "Failed publishing stream description request");

cleanup:
  if (payload_str)
    cJSON_free(payload_str);
  cJSON_Delete(payload);
  return ret;
}

static esp_err_t file_get_stream(char *thing_name, char *client_token, get_stream_t get_conf) {
  esp_err_t ret         = ESP_OK;
  cJSON    *payload     = cJSON_CreateObject();
  char     *payload_str = NULL;
  char      topic_name[1024];
  char      bitmap_str[1024];

  /// TODO: Validate ota_stream
  // Build the payload
  client_token = client_token ? client_token : "";
  cJSON_AddStringToObject(payload, "c", client_token);
  cJSON_AddNumberToObject(payload, "f", get_conf.file_id);
  cJSON_AddNumberToObject(payload, "l", get_conf.block_size);
  cJSON_AddNumberToObject(payload, "o", get_conf.block_offset);
  if (get_conf.n_blocks >= 0) {
    cJSON_AddNumberToObject(payload, "n", get_conf.n_blocks);
  } else if (get_conf.block_bitmap >= 0) {
    snprintf(bitmap_str, 1024, "%x", get_conf.block_bitmap);
    cJSON_AddStringToObject(payload, "b", bitmap_str);
  } else {
    ESP_LOGE(TAG, "Either bitmap or number of blocks must be defined for stream get request");
    ret = ESP_ERR_INVALID_ARG;
    goto cleanup;
  }

  // Publish the request
  sprintf(topic_name, "$aws/things/%s/streams/%s/get/json", thing_name, ota_stream.stream_id);
  ESP_GOTO_ON_FALSE(NULL != (payload_str = cJSON_Print(payload)), ESP_FAIL, cleanup, TAG,
                    "Couldn't print the payload to payload_str");
  ESP_GOTO_ON_FALSE(esp_mqtt_client_publish(mqtt_client, topic_name, payload_str, 0, 1, 0) >= 0,
                    ESP_FAIL, cleanup, TAG, "Failed publishing stream get request");

cleanup:
  if (payload_str)
    cJSON_free(payload_str);
  cJSON_Delete(payload);
  return ret;
}  // end file_get_stream

static esp_err_t jobs_update_job_status(char *job_id, job_status_t new_status, char *client_token) {
  esp_err_t ret         = ESP_OK;
  cJSON    *payload     = cJSON_CreateObject();
  char     *payload_str = NULL;
  char      topic_name[1024];

  ESP_GOTO_ON_FALSE(job_id != NULL, ESP_ERR_INVALID_ARG, cleanup, TAG, "Job ID can't be NULL!");
  ESP_GOTO_ON_FALSE(ota_stream.thing_name != NULL, ESP_ERR_INVALID_ARG, cleanup, TAG,
                    "Thing name can't be NULL!");
  ESP_GOTO_ON_FALSE(new_status > IN_PROGRESS || new_status < REJECTED, ESP_ERR_INVALID_ARG, cleanup,
                    TAG, "Invalid new_status value");
  ESP_GOTO_ON_FALSE(payload != NULL, ESP_ERR_NO_MEM, cleanup, TAG, "Couldn't allocate cJSON root");

  client_token = client_token ? client_token : "";
  cJSON_AddStringToObject(payload, "clientToken", client_token);
  cJSON_AddStringToObject(payload, "status", job_status_str[new_status]);

  /// TODO: statusDetails to include information about status update
  /*
  if (status_details_json && status_details_json[0] != '\0') {
    cJSON *details = cJSON_Parse(status_details_json);
    ESP_GOTO_ON_FALSE(details != NULL, ESP_ERR_INVALID_ARG, cleanup, TAG,
                      "Couldn't parse status_details_json");
    ESP_GOTO_ON_FALSE(cJSON_IsObject(details), ESP_ERR_INVALID_ARG, cleanup, TAG,
                      "status_details_json must be a JSON object");
    cJSON_AddItemToObject(payload, "statusDetails", details);  // payload now owns details
  }
  */

  ESP_GOTO_ON_FALSE(NULL != (payload_str = cJSON_Print(payload)), ESP_FAIL, cleanup, TAG,
                    "Couldn't print the payload to payload_str");
  sprintf(topic_name, "$aws/things/%s/jobs/%s/update", ota_stream.thing_name, job_id);
  ESP_LOGW(TAG, "DEBUG: publishing to %s", topic_name);
  ESP_LOGW(TAG, "%s", payload_str);
  ESP_GOTO_ON_FALSE(esp_mqtt_client_publish(mqtt_client, topic_name, payload_str, 0, 1, 0) >= 0,
                    ESP_FAIL, cleanup, TAG, "Failed publishing payload to topic %s", topic_name);

cleanup:
  if (payload_str)
    cJSON_free(payload_str);
  cJSON_Delete(payload);
  return ret;
}

/*================== Static Functions ==================*/
/**
 * @brief Verify a SHA-256 hash against a Base64 signature
 *
 * @todo Decide how to deliver the OTA certificate to the device
 */
/*
static esp_err_t verify_signature(const unsigned char *sha_digest, const char *signature_b64) {
  esp_err_t ret = ESP_OK;
  // Decode the base64 signature into binary
  unsigned char sig_bin[128];
  size_t        sig_bin_len;

  ESP_LOGI(TAG, "Signature in base64 is %s", signature_b64);
  ESP_GOTO_ON_ERROR(mbedtls_base64_decode(sig_bin, 128, &sig_bin_len,
                                          (unsigned char *)signature_b64, strlen(signature_b64)),
                    cleanup, TAG, "Couldn't decode signature");

  // Load the certificate
  mbedtls_x509_crt cert;
  mbedtls_x509_crt_init(&cert);
  ESP_GOTO_ON_ERROR(mbedtls_x509_crt_parse(&cert, (const unsigned char *)ota_cert_pem_start,
                                           strlen((char *)ota_cert_pem_start) + 1),  // +1 for '\0'
                    cleanup, TAG, "Couldn't patse the OTA certificate");

  // Verify the digest
  ESP_GOTO_ON_ERROR(
      mbedtls_pk_verify(&cert.pk, MBEDTLS_MD_SHA256, sha_digest, 32, sig_bin, sig_bin_len), cleanup,
      TAG, "Verification failed (%s)");
  ESP_LOGI(TAG, "SHA-256 hash verified successfully");
cleanup:
  return ret;
}
*/

static esp_err_t file_process_ota_data(char *buf, int buflen) {
  esp_err_t    ret = ESP_OK;
  data_block_t data_block;
  cJSON       *ota_data;

  /// TODO: Remove portMAX_DELAY and fail accordingly
  while (xQueueReceive(s_free_stream_data_q, &data_block, portMAX_DELAY) != pdTRUE) {
    continue;
  }

  ota_data = cJSON_ParseWithLength(buf, buflen);
  ESP_GOTO_ON_FALSE(ota_data != NULL, ESP_ERR_INVALID_ARG, cleanup, TAG,
                    "Couldn't parse stream data");

  ESP_GOTO_ON_FALSE((cJSON_GetObjectItem(ota_data, "f") != NULL) &&
                        (cJSON_GetObjectItem(ota_data, "i") != NULL) &&
                        (cJSON_GetObjectItem(ota_data, "l") != NULL) &&
                        (cJSON_GetObjectItem(ota_data, "p") != NULL),
                    ESP_ERR_INVALID_ARG, cleanup, TAG, "Invalid stream data");

  data_block.file_index = cJSON_GetObjectItem(ota_data, "f")->valueint;
  data_block.index      = cJSON_GetObjectItem(ota_data, "i")->valueint;
  int payload_len       = cJSON_GetObjectItem(ota_data, "l")->valueint;
  payload_len           = (payload_len * 4 + 3 - 1) / 3;

  ESP_GOTO_ON_ERROR(
      mbedtls_base64_decode(data_block.data, CONFIG_DATA_BLOCK_SIZE, &(data_block.len),
                            (const unsigned char *)cJSON_GetObjectItem(ota_data, "p")->valuestring,
                            payload_len + payload_len % 4),
      cleanup, TAG, "Couldn't decode data block");

  ESP_LOGV(TAG, "sending data block with buffer at %p to queue at %p", data_block.data,
           s_filled_stream_data_q);
  xQueueSendToBack(s_filled_stream_data_q, &data_block, portMAX_DELAY);

cleanup:
  cJSON_Delete(ota_data);
  return ret;
}

static esp_err_t file_parse_stream_description(char *buf, size_t buflen, ota_stream_t *out_stream) {
  esp_err_t ret         = ESP_OK;
  cJSON    *description = NULL;
  cJSON    *file_list   = NULL;
  cJSON    *file        = NULL;

  description = cJSON_ParseWithLength(buf, buflen);
  ESP_GOTO_ON_FALSE(description != NULL, ESP_ERR_INVALID_ARG, cleanup, TAG,
                    "Couldn't parse stream description");
  ESP_GOTO_ON_FALSE((cJSON_GetObjectItem(description, "s") != NULL) &&
                        (cJSON_GetObjectItem(description, "r") != NULL),
                    ESP_ERR_INVALID_ARG, cleanup, TAG, "Invalid description");

  out_stream->stream_version = cJSON_GetObjectItem(description, "s")->valueint;
  file_list                  = cJSON_GetObjectItem(description, "r");

  file = cJSON_GetArrayItem(file_list, 0);
  // Confirm file index and filesize
  if (out_stream->file_index != cJSON_GetObjectItem(file, "f")->valueint) {
    ESP_LOGW(TAG, "Missmatched file index!");
  }
  if (out_stream->filesize != cJSON_GetObjectItem(file, "z")->valueint) {
    ESP_LOGW(TAG, "Missmatched filesize! stream has %d bytes and should be %d bytes",
             out_stream->filesize, cJSON_GetObjectItem(file, "z")->valueint);
  }
cleanup:
  if (description)
    cJSON_Delete(description);
  return ret;
}  // end file_parse_stream_description

/**
 * @brief Parse a DescribeJob payload to find out the job class and if it's
 * supported by this Thing.
 *
 * @param job_description Pointer to the payload.
 */
static job_class_t jobs_get_job_class(const char *job_description) {
  // Find the job id inside the job description
  char *job_id = strstr(job_description, "jobId");
  if (!job_id)
    return INVALID;
  // Compare with known job id prefixes
  job_id += strlen("\"jobId\":");
  if (strstr(job_id, "AFR_OTA-") != NULL)
    return OTA_UPDATE;
  if (strstr(job_id, "UPLOAD_VID-") != NULL)
    return VIDEO_UPLOAD;
  // If no match was found, this job is not supported
  return INVALID;
}

/**
 * @brief Parse JobDocument for an OTA Update Job
 *
 * @param job_document A buffer containing the job document
 * @param doc_len The length of the job_document in the buffer
 * @param out_stream A pointer to an ota_stream_t object to be populated during parsing. Can't be
 * NULL.
 */
static esp_err_t jobs_parse_ota_job(const char *job_document, size_t doc_len,
                                    ota_stream_t *out_stream) {
  esp_err_t ret = ESP_OK;
  // Parse the job document into cJSON
  ESP_LOGD(TAG, "job_document: %.*s", doc_len, job_document);
  cJSON *job_json     = cJSON_ParseWithLength(job_document, doc_len);
  cJSON *afr_ota_json = cJSON_GetObjectItem(job_json, "afr_ota");
  ESP_GOTO_ON_FALSE(job_json != NULL, ESP_ERR_INVALID_ARG, cleanup, TAG,
                    "Couldn't parse job document");
  ESP_GOTO_ON_FALSE((cJSON_GetObjectItem(afr_ota_json, "streamname") != NULL) &&
                        (cJSON_GetObjectItem(afr_ota_json, "files") != NULL),
                    ESP_ERR_INVALID_ARG, cleanup, TAG, "Invalid job document");
  // Populate the out_stream object
  ESP_GOTO_ON_FALSE(out_stream != NULL, ESP_ERR_INVALID_ARG, cleanup, TAG,
                    "%s out_stream can't be NULL!", __func__);
  // Copy the streamID
  strncpy(out_stream->stream_id,
          cJSON_GetStringValue(cJSON_GetObjectItem(afr_ota_json, "streamname")),
          sizeof(out_stream->stream_id));
  // Copy the file information to the stream struct
  cJSON *file = cJSON_GetArrayItem(cJSON_GetObjectItem(afr_ota_json, "files"), 0);
  ESP_GOTO_ON_FALSE(file != NULL, ESP_ERR_INVALID_ARG, cleanup, TAG,
                    "Couldn't find the file in the job document filelist");
  ESP_GOTO_ON_FALSE((cJSON_GetObjectItem(file, "filesize") != NULL) &&
                        (cJSON_GetObjectItem(file, "fileid") != NULL),
                    ESP_ERR_INVALID_ARG, cleanup, TAG, "Invalid file");
  out_stream->filesize   = cJSON_GetNumberValue(cJSON_GetObjectItem(file, "filesize"));
  out_stream->file_index = cJSON_GetNumberValue(cJSON_GetObjectItem(file, "fileid"));
  ESP_LOGD(TAG, "fileid: %d, filesize: %d", out_stream->file_index, out_stream->filesize);
  strncpy(out_stream->file_signature,
          cJSON_GetStringValue(cJSON_GetObjectItem(file, "sig-sha256-ecdsa")),
          sizeof(out_stream->file_signature));

cleanup:
  if (job_json)
    cJSON_Delete(job_json);
  return ret;
}  // end jobs_parse_ota_job

/*================== Event Handlers ==================*/
static void ota_event_handler(void *handler_arg, esp_event_base_t event_base, int32_t event_id,
                              void *event_data) {
  ESP_LOGI(TAG, "Received event %s:%d", (char *)event_base, event_id);
  switch (event_id) {
  case OTA_JOB_REJECTED:
    ESP_LOGW(TAG, "OTA Update rejected by OTA Manager, aborting");
    jobs_update_job_status(ota_stream.job_id, REJECTED, "updateJob");
    /// TODO: Mark the AWS Job as failed and include reason.
    break;
  case OTA_CTRL_START:
    file_describe_stream(mqtt_client, ota_stream.thing_name, "describeStream",
                         ota_stream.stream_id);
    break;
  case OTA_CTRL_DONE:
    jobs_update_job_status(ota_stream.job_id, SUCCEEDED, "updateJob");
    /// TODO: Update Job status in AWS and end ota task
    break;
  default:
    break;
  }
}  // end ota_event_handler

/*================== FreeRTOS Tasks ==================*/
void download_task(void *arg) {
  ota_chunk_t  curr_chunk = {0};
  esp_err_t    ret;
  int          block_n;
  get_stream_t get_conf        = {0};
  uint8_t      received_blocks = 0b0000;
  data_block_t data_block      = {0};

  // Subscribe to relevant topics
  char topic_name[1024];
  sprintf(topic_name, "$aws/things/%s/streams/%s/data/json", ota_stream.thing_name,
          ota_stream.stream_id);
  ESP_GOTO_ON_FALSE(esp_mqtt_client_subscribe(mqtt_client, topic_name, 1) >= 0, ESP_FAIL, cleanup,
                    TAG, "Failed subscribing to stream description accepted topic");
  sprintf(topic_name, "$aws/things/%s/streams/%s/rejected/json", ota_stream.thing_name,
          ota_stream.stream_id);
  ESP_GOTO_ON_FALSE(esp_mqtt_client_subscribe(mqtt_client, topic_name, 1) >= 0, ESP_FAIL, cleanup,
                    TAG, "Failed subscribing to stream description rejected topic");

  // Update the Job status
  jobs_update_job_status(ota_stream.job_id, IN_PROGRESS, "updateJob");

  // Initialize the hashing context
  ESP_LOGD(TAG, "Initializing SHA256 hashing context...");
  mbedtls_sha256_init(&s_sha_ctx);
  mbedtls_sha256_starts(&s_sha_ctx, 0);
  ESP_LOGD(TAG, "Hashing context initialized");

  // Get an OTA chunk
  memset(&curr_chunk, 0, sizeof(ota_chunk_t));
  while (xQueueReceive(s_free_chunk_q, &curr_chunk, portMAX_DELAY) != pdTRUE) {
    continue;
  }

  // Configurate the get request
  get_conf.file_id    = ota_stream.file_index;
  get_conf.block_size = curr_chunk.len / CONFIG_DATA_BLOCK_COUNT;

  // Calculate required blocks
  /// TODO: Validate ota_stream
  ESP_LOGD(TAG, "Computing required blocks for filesize=%d", ota_stream.filesize);
  block_n = (ota_stream.filesize + get_conf.block_size - 1) / get_conf.block_size;
  ESP_LOGD(TAG, "Required %d bytes blocks: %d", get_conf.block_size, block_n);
  ESP_LOGD(TAG, "Required iterations: %d", (block_n + 4 - 1) / 4);

  // Seed the queues
  data_block_t new_data_block = {0};
  uint8_t     *p;
  for (int i = 0; i < CONFIG_DATA_BLOCK_COUNT; ++i) {
    p                   = s_data_block_buffers[i];
    new_data_block.data = p;
    ESP_LOGD(TAG, "Data block %d with buffer at %p", i, new_data_block.data);
    /// TODO: Remove portMAX_DELAY and fail accordingly
    xQueueSendToBack(s_free_stream_data_q, &new_data_block, portMAX_DELAY);
  }

  for (int i = 0; i < (block_n + CONFIG_DATA_BLOCK_COUNT - 1) / CONFIG_DATA_BLOCK_COUNT; i++) {
    get_conf.block_offset = CONFIG_DATA_BLOCK_COUNT * i;
    get_conf.n_blocks     = block_n - CONFIG_DATA_BLOCK_COUNT * i < CONFIG_DATA_BLOCK_COUNT
                                ? block_n - CONFIG_DATA_BLOCK_COUNT * i
                                : CONFIG_DATA_BLOCK_COUNT;
    get_conf.block_bitmap = -1;
    ESP_LOGD(TAG, "Getting blocks %d-%d", CONFIG_DATA_BLOCK_COUNT * i,
             CONFIG_DATA_BLOCK_COUNT * i + (CONFIG_DATA_BLOCK_COUNT - 1));
    ESP_LOGD(TAG, "ThingName: %s, StreamID: %s", ota_stream.thing_name, ota_stream.stream_id);
    file_get_stream(ota_stream.thing_name, "getOtaStreamTest", get_conf);
    ESP_LOGD(TAG, "Attempting to receive blocks %d-%d", CONFIG_DATA_BLOCK_COUNT * i,
             CONFIG_DATA_BLOCK_COUNT * i + (get_conf.n_blocks - 1));
    // Attempt to receive the blocks
    curr_chunk.len = 0;
    for (int j = 0; j < get_conf.n_blocks; j++) {
      /// TODO: Handle receive timeouts
      ESP_LOGD(TAG, "Receiving block %d", CONFIG_DATA_BLOCK_COUNT * i + j);
      xQueueReceive(s_filled_stream_data_q, &data_block, portMAX_DELAY);
      // Add this block buffer to the hash
      mbedtls_sha256_update(&s_sha_ctx, (unsigned char *)data_block.data, data_block.len);
      ESP_LOGV(TAG, "Received chunk with buffer at %p", curr_chunk.data);
      // Fill the chunk buffer
      memcpy(curr_chunk.data + (data_block.index % CONFIG_DATA_BLOCK_COUNT) * get_conf.block_size,
             data_block.data, data_block.len);
      curr_chunk.len += data_block.len;
      // Send the data block to the free queue
      xQueueSendToBack(s_free_stream_data_q, &data_block, portMAX_DELAY);
      // Check for last block received
      if (data_block.index + 1 >= block_n)
        curr_chunk.last = true;
    }  // end for
    // Send the chunk back to the OTA Manager
    ESP_LOGD(TAG, "Sending chunk %d to the OTA Manager", i);
    xQueueSendToBack(s_filled_chunk_q, &curr_chunk, portMAX_DELAY);
    // Get next OTA chunk
    memset(&curr_chunk, 0, sizeof(ota_chunk_t));
    while (xQueueReceive(s_free_chunk_q, &curr_chunk, portMAX_DELAY) != pdTRUE) {
      continue;
    }
  }  // end for

  // Close the hasher
  uint8_t resulting_sha256[32];
  mbedtls_sha256_finish(&s_sha_ctx, resulting_sha256);
  ESP_LOGI(TAG, "Download complete, SHA256 is:");
  ESP_LOG_BUFFER_HEX(TAG, resulting_sha256, 32);

cleanup:
  mbedtls_sha256_free(&s_sha_ctx);
  // Delete the task
  vTaskDelete(NULL);

}  // end download_task

/*========================= Bootstrapping =========================*/
static void allocate_pools(void) {
  // Allocate OTA Chunks (word-aligned, DRAM)
  for (int i = 0; i < CONFIG_DATA_BLOCK_COUNT; ++i) {
    s_data_block_buffers[i] = (uint8_t *)heap_caps_aligned_alloc(
        64, CONFIG_DATA_BLOCK_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    if (s_data_block_buffers[i] == NULL) {
      ESP_LOGE(TAG, "Failed to alloc data block %d (%u bytes)", i, (unsigned)CONFIG_OTA_CHUNK_SIZE);
      abort();
    }
    ESP_LOGV(TAG, "Allocated s_data_blocks[%d] at %p", i, s_data_block_buffers[i]);
  }
}

static void create_queues(void) {
  s_free_stream_data_q   = xQueueCreate(CONFIG_DATA_BLOCK_COUNT + 2, sizeof(data_block_t));
  s_filled_stream_data_q = xQueueCreate(CONFIG_DATA_BLOCK_COUNT, sizeof(data_block_t));
  if (!s_free_stream_data_q || !s_filled_stream_data_q) {
    ESP_LOGE(TAG, "Queue creation failed");
    abort();
  }
}

/*================== Public Functions ==================*/
esp_err_t jobs_get_pending(char *thing_name, char *client_token) {
  esp_err_t ret         = ESP_OK;
  cJSON    *payload     = cJSON_CreateObject();
  char     *payload_str = NULL;
  char      topic_name[1024];

  client_token = client_token ? client_token : "";
  cJSON_AddStringToObject(payload, "clientToken", client_token);
  payload_str = cJSON_Print(payload);
  sprintf(topic_name, "$aws/things/%s/jobs/get", thing_name);
  ESP_GOTO_ON_FALSE(esp_mqtt_client_publish(mqtt_client, topic_name, payload_str, 0, 1, 0) >= 0,
                    ESP_FAIL, cleanup, TAG, "Failed publishing payload to topic %s", topic_name);

cleanup:
  if (payload_str)
    cJSON_free(payload_str);
  cJSON_Delete(payload);
  return ret;
}

esp_err_t jobs_describe_job(char *thing_name, char *client_token, char *job_id) {
  esp_err_t ret         = ESP_OK;
  cJSON    *payload     = cJSON_CreateObject();
  char     *payload_str = NULL;
  char      topic_name[1024];

  ESP_GOTO_ON_FALSE(job_id != NULL, ESP_ERR_INVALID_ARG, cleanup, TAG, "Job ID can't be NULL!");
  ESP_GOTO_ON_FALSE(thing_name != NULL, ESP_ERR_INVALID_ARG, cleanup, TAG,
                    "Thing name can't be NULL!");
  client_token = client_token ? client_token : "";
  cJSON_AddStringToObject(payload, "clientToken", client_token);
  cJSON_AddStringToObject(payload, "jobId", job_id);
  cJSON_AddStringToObject(payload, "thingName", thing_name);
  cJSON_AddBoolToObject(payload, "includeJobDocument", true);
  payload_str = cJSON_Print(payload);
  sprintf(topic_name, "$aws/things/%s/jobs/%s/get", thing_name, job_id);
  ESP_GOTO_ON_FALSE(esp_mqtt_client_publish(mqtt_client, topic_name, payload_str, 0, 1, 0) >= 0,
                    ESP_FAIL, cleanup, TAG, "Failed publishing payload to topic %s", topic_name);

cleanup:
  cJSON_free(payload_str);
  cJSON_Delete(payload);
  return ret;
}  // end jobs_describe_job

esp_err_t jobs_data_handler(const char *thing_name, const char *data, int data_len) {
  static esp_event_loop_handle_t OTA_event_h = NULL;
  if (OTA_event_h == NULL)
    OTA_eventloop_get_handle(&OTA_event_h);
  esp_err_t ret = ESP_OK;
  char      topic_name[256];

  job_class_t job_class = jobs_get_job_class(data);
  ESP_LOGD(TAG, "This Job is a%s job",
           job_class == OTA_UPDATE ? "n OTA_UPDATE"
                                   : (job_class == VIDEO_UPLOAD ? " VIDEO_UPLOAD" : "n INVALID"));

  if (job_class == OTA_UPDATE) {
    // Search for the Job Document
    char *job_document = strstr(data, "jobDocument");
    /// TODO: Use the clientToken for this
    if (job_document == NULL) {
      memset(&ota_stream, 0, sizeof(ota_stream_t));  // Reset the OTA stream structure
      // If the payload doesn't contain a Job Document, search for the Job ID
      char *job_id = strstr(data, "jobId");
      /// TODO: Check if job_id is NULL
      job_id             += strlen("\"jobID\":");
      size_t job_id_len   = strstr(job_id, "\"") - job_id;
      job_id[job_id_len]  = '\0';
      strlcpy(ota_stream.job_id, job_id,
              sizeof(ota_stream.job_id) <= job_id_len ? sizeof(ota_stream.job_id) : job_id_len + 1);
      ESP_LOGD(TAG, "job_id is %s", ota_stream.job_id);
      // Ask for the Job Document
      ESP_RETURN_ON_ERROR(jobs_describe_job(thing_name, "getJob", ota_stream.job_id), TAG,
                          "Couldn't describe job");
    } else {
      /// TODO: Turn this into a function
      job_document +=
          strlen("\"jobDocument\":") - 1;  // strlen will return 1 more than expected (\0)
      // Find the end of the job_document
      size_t job_document_size  = data_len - (int)(job_document - data);
      job_document_size        -= 2;  // Job Document is inside two nesting levels
      ESP_LOGD(TAG, "Event data starts at %p and contains %d bytes", data, data_len);
      ESP_LOGD(TAG, "Job document starts at %p and contains %d bytes", job_document,
               job_document_size);
      ESP_LOGD(TAG, "Job document: %.*s", 20, job_document);
      ESP_RETURN_ON_ERROR(jobs_parse_ota_job(job_document, job_document_size, &ota_stream), TAG,
                          "Couldn't parse OTA job");
      ESP_LOGD(TAG, "JobDocument parsed. StreamID is %s", ota_stream.stream_id);
      // Subscribe to Job status update topics
      sprintf(topic_name, "$aws/things/%s/jobs/%s/update/accepted", thing_name, ota_stream.job_id);
      ESP_RETURN_ON_FALSE(esp_mqtt_client_subscribe(mqtt_client, topic_name, 1) >= 0, ESP_FAIL, TAG,
                          "Couldn't subscribe to %s", topic_name);
      sprintf(topic_name, "$aws/things/%s/jobs/%s/update/rejected", thing_name, ota_stream.job_id);
      ESP_RETURN_ON_FALSE(esp_mqtt_client_subscribe(mqtt_client, topic_name, 1) >= 0, ESP_FAIL, TAG,
                          "Couldn't subscribe to %s", topic_name);
      // Begin the download
      strlcpy(ota_stream.thing_name, thing_name, sizeof(ota_stream.thing_name));
      ota_stream.client = mqtt_client;
      // Post the event to the OTA loop
      esp_event_post_to(OTA_event_h, OTA_EVENTS, OTA_JOB_RECEIVED, &ota_stream,
                        sizeof(ota_stream_t), 100);
    }  // end if (job_document == NULL)
  }  // end if(job_class == OTA_UPDATE)

  return ret;
}  // end jobs_data_handler

esp_err_t jobs_stream_data_handler(const char *thing_name, const char *data, int data_len) {
  esp_err_t ret = ESP_OK;
  /// TODO: Write a "get clientToken" function for this
  /// TODO: Make clientTokens KConfig options
  if (strstr(data, "describeStream")) {
    // If the "describeStream" clientToken was found
    ESP_LOGD(TAG, "%.*s", data_len, data);
    ESP_LOGD(TAG, "This is a Stream description message, parsing");
    ESP_RETURN_ON_ERROR(file_parse_stream_description(data, data_len, &ota_stream), TAG,
                        "Couldn't parse stream description");
    ESP_LOGD(TAG, "Stream version:%d, File index:%d, File size: %lu", ota_stream.stream_version,
             ota_stream.file_index, ota_stream.filesize);
    /// TODO: Begin OTA download
    // Create the download task
    if (s_download_task_h != NULL) {
      ESP_LOGD(TAG, "Download task already exists, checking state");
      ESP_RETURN_ON_FALSE(eTaskGetState(s_download_task_h) == eDeleted, ESP_ERR_INVALID_STATE, TAG,
                          "Download Task is already running");
    }
    ESP_LOGD(TAG, "Creating the download task");
    xTaskCreate(download_task, "down.task", 5120, NULL, 10, &s_download_task_h);
    ESP_LOGD(TAG, "Download task created");
    // file_process_ota_stream(&ota_stream);
  } else if (strstr(data, "getOtaStream")) {
    // If the "describeStream" clientToken was found
    ESP_LOGD(TAG, "This is a Stream data message");
    file_process_ota_data(data, data_len);
  } else if (strstr(data, "updateJob")) {
    ESP_LOGD(TAG, "This is a Job status update response");
    ESP_LOGD(TAG, "%.*s", data_len, data);
  }
  return ret;
}

esp_err_t jobs_init(esp_mqtt_client_handle_t client, QueueHandle_t free_chunk_queue,
                    QueueHandle_t filled_chunk_queue) {
  mqtt_client      = client;
  s_free_chunk_q   = free_chunk_queue;
  s_filled_chunk_q = filled_chunk_queue;
  // Get the OTA event loop handles
  esp_event_loop_handle_t OTA_event_h = NULL;
  OTA_eventloop_get_handle(&OTA_event_h);
  ESP_RETURN_ON_ERROR(esp_event_handler_instance_register_with(
                          OTA_event_h, OTA_EVENTS, ESP_EVENT_ANY_ID, ota_event_handler, NULL, NULL),
                      TAG, "Couldn't register OTA events handler");
  // Initialize the buffers and queues
  /// TODO: Change function names
  ESP_LOGI(TAG, "Initializing buffers and queues.");
  allocate_pools();
  create_queues();
  // Create the steam data queue
  s_free_stream_data_q   = xQueueCreate(5, sizeof(data_block_t));
  s_filled_stream_data_q = xQueueCreate(5, sizeof(data_block_t));
  /// TODO: Check for errors
  return ESP_OK;
}