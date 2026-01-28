/**
 * @file mqtt_worker.c
 * @date September 2025
 * @author Simón Jaramillo <sjaramillo@yx.cl>
 *
 * @ingroup mqtt_worker
 */

#include "mqtt_worker.h"
#include "NVS_manager.h"
#include "configuration_events.h"
#include "configuration_worker.h"
#include "jobs_manager.h"
#include "provision_claimer.h"
#include "recording_worker.h"
#include "s3_uploader.h"

static const char *TAG = "MQTT Worker"; /**< Logging tag for this module. */

/* ================ STRUCTS ================ */
typedef struct {
  uint8_t  version;
  uint16_t width, height;
  uint16_t fps;
  uint8_t  qmin, qmax;
  uint32_t timeout;
  char     trans_id[512];
  uint32_t target_bitrate;
  char     aws_job_id[65];
  bool     has_aws_job;
} record_params_t;

/*================= Globals =================*/
static cert_data_t mqtt_cert_data;

/* Recording events */
static esp_event_loop_handle_t rec_event_h;
static recording_conf_t        rec_conf;
static char                    rec_stop_id[128];

/**
 * @brief The MQTT client
 */
static esp_mqtt_client_handle_t client = NULL;

/**
 * @brief The MQTT client configuration
 */
static esp_mqtt_client_config_t mqtt_cfg = {
    .broker.address.uri = "mqtts://" CONFIG_AWS_ENDPOINT ":8883",
    /// TODO: Make buffer sizes configurable in KConfig
    .buffer = {.size = 8192, .out_size = 8192},
    /// TODO: Make defaults configurable in KConfig
    .network.tcp_keep_alive_cfg =
        {
            .keep_alive_enable   = true,
            .keep_alive_idle     = 45,
            .keep_alive_interval = 10,
            .keep_alive_count    = 4,
        },
};

/**
 * @brief The connected semaphore
 */
static SemaphoreHandle_t mqtt_conn_semphr = NULL;

/**
 * @brief The recording events handlers
 */
esp_event_handler_instance_t rec_started_handler_h;
esp_event_handler_instance_t rec_done_handler_h;
esp_event_handler_instance_t rec_error_handler_h;

handler_ctx_t handler_conf;

/**
 * @brief The configuration events handlers
 */
esp_event_handler_instance_t conf_applied_handler_h;

static esp_event_loop_handle_t conf_event_h;

static bool jobs_checked = false;

/*================== Statics ==================*/
esp_err_t register_event_handlers() {
  /* Get the event loop handles */
  /// TODO:  Check for errors
  ESP_LOGD(TAG, "registering recording event handlers");
  ESP_RETURN_ON_ERROR(rec_eventloop_get_handle(&rec_event_h), TAG,
                      "Couldn't get the recording event loop handle");
  ESP_RETURN_ON_ERROR(esp_event_handler_instance_register_with(
                          rec_event_h, RECORDING_EVENTS, REC_STARTED, rec_started_handler,
                          &handler_conf, &rec_started_handler_h),
                      TAG, "Couldn't register recording started event handler");
  ESP_RETURN_ON_ERROR(esp_event_handler_instance_register_with(rec_event_h, RECORDING_EVENTS,
                                                               REC_DONE, rec_done_handler,
                                                               &handler_conf, &rec_done_handler_h),
                      TAG, "Couldn't register recording done event handler");
  ESP_RETURN_ON_ERROR(esp_event_handler_instance_register_with(rec_event_h, RECORDING_EVENTS,
                                                               REC_ERROR, rec_error_handler,
                                                               &handler_conf, &rec_error_handler_h),
                      TAG, "Couldn't register recording error event handler");
  ESP_LOGD(TAG, "registering configuration event handlers");
  ESP_RETURN_ON_ERROR(conf_eventloop_get_handle(&conf_event_h), TAG,
                      "Couldn't get the configuration event loop handle");
  ESP_RETURN_ON_ERROR(esp_event_handler_instance_register_with(
                          conf_event_h, CONFIGURATION_EVENTS, CONF_APPLIED, conf_applied_handler,
                          &handler_conf, &conf_applied_handler_h),
                      TAG, "Couldn't register configuration applied event handler");
  ESP_RETURN_ON_ERROR(esp_event_handler_instance_register_with(
                          conf_event_h, CONFIGURATION_EVENTS, CONF_REJECTED, conf_rejected_handler,
                          &handler_conf, &conf_applied_handler_h),
                      TAG, "Couldn't register configuration rejected event handler");
  ESP_RETURN_ON_ERROR(esp_event_handler_instance_register_with(
                          conf_event_h, CONFIGURATION_EVENTS, CONF_ERROR, conf_error_handler,
                          &handler_conf, &conf_applied_handler_h),
                      TAG, "Couldn't register configuration error event handler");
  ESP_LOGD(TAG, "All event handlers registered");
  return ESP_OK;
}

/// TODO: Receive a pointer to a certificate string or use the mqtt client conf
/*
static esp_err_t mqttworker_verify_flash_certs(void) {
  int                ret = ESP_OK;
  mbedtls_x509_crt   client;
  mbedtls_pk_context key;

  // Parse client certificate
  mbedtls_x509_crt_init(&client);
  ret = mbedtls_x509_crt_parse(&client, (const unsigned char *)client_cert_pem_start,
                               strlen((const char *)client_cert_pem_start) + 1);
  if (ret != 0) {
    ESP_LOGE(TAG, "client cert parse failed: -0x%04X", (unsigned)(-ret));
    mbedtls_x509_crt_free(&client);
    return ESP_FAIL;
  }
  ESP_LOGI(TAG, "Client certificate in binary correctly parsed");

  // Parse private key
  mbedtls_pk_init(&key);
  ret = mbedtls_pk_parse_key(&key, (const unsigned char *)client_key_pem_start,
                             strlen((const char *)client_key_pem_start) + 1, NULL, 0, NULL, NULL);
  if (ret != 0) {
    ESP_LOGE(TAG, "client key parse failed: -0x%04X", (unsigned)(-ret));
    mbedtls_pk_free(&key);
    mbedtls_x509_crt_free(&client);
    return ESP_FAIL;
  }
  ESP_LOGI(TAG, "Client key in binary correctly parsed");

  mbedtls_pk_free(&key);
  mbedtls_x509_crt_free(&client);
  return ESP_OK;
}
  */

/*================== Event Handlers ==================*/
/**
 * @brief The handler for the `MQTT_EVENT_CONNECTED` event
 *
 * @param handler_args user data registered to the event.
 * @param base Event base for the handler(always MQTT Base in this case).
 * @param event_id The id for the received event.
 * @param event_data The data for the event, esp_mqtt_event_handle_t.
 */
static void mqtt_connected_handler(void *handler_args, esp_event_base_t base, int32_t event_id,
                                   void *event_data) {
  ESP_LOGI(TAG, "Connected to MQTT broker");
  esp_mqtt_event_handle_t  event            = event_data;
  esp_mqtt_client_handle_t mqtt_client      = event->client;
  char                     topic_name[1024] = "";

  /*----- Subscribe to relevant topics -----*/
  // Recording start topic
  snprintf(topic_name, sizeof(topic_name),
           CONFIG_PROJ_BASE_NAME "/" CONFIG_PROJ_ENV_NAME "/recordings/%s/start",
           mqtt_cert_data.thing_name);
  esp_mqtt_client_subscribe(mqtt_client, topic_name, 0);
  // Jobs notify topics
  snprintf(topic_name, sizeof(topic_name), "$aws/things/%s/jobs/notify-next",
           mqtt_cert_data.thing_name);
  ESP_LOGD(TAG, "Subscribing to topic: %s", topic_name);
  esp_mqtt_client_subscribe(mqtt_client, topic_name, 0);
  // Camera configuration topic
  snprintf(topic_name, sizeof(topic_name),
           CONFIG_PROJ_BASE_NAME "/" CONFIG_PROJ_ENV_NAME "/cameras/%s/configure",
           mqtt_cert_data.thing_name);
  ESP_LOGD(TAG, "Subscribing to topic: %s", topic_name);
  esp_mqtt_client_subscribe(mqtt_client, topic_name, 0);
  // Connect the S3 uploader
  s3_uploader_on_connected();
  // Check for Jobs (only once)
  if (!jobs_checked) {
    jobs_get_pending(mqtt_cert_data.thing_name, "getJobs");
    jobs_checked = true;
  }
  // Give the connected semaphore
  xSemaphoreGive(mqtt_conn_semphr);
}

/**
 * @brief The handler for the `MQTT_EVENT_DATA` event
 *
 * @param handler_args user data registered to the event.
 * @param base Event base for the handler(always MQTT Base in this case).
 * @param event_id The id for the received event.
 * @param event_data The data for the event, esp_mqtt_event_handle_t.
 */
static void mqtt_data_handler(void *handler_args, esp_event_base_t base, int32_t event_id,
                              void *event_data) {
  esp_err_t               ret                  = ESP_OK;
  esp_mqtt_event_handle_t event                = event_data;
  cJSON                  *payload              = NULL;
  char                    received_topic[1024] = "";
  char                    topic_name[1024]     = "";
  configuration_t         cam_conf             = {0};

  strncpy(received_topic, event->topic, event->topic_len);
  received_topic[event->topic_len] = '\0';
  ESP_LOGD(TAG, "Received data from MQTT topic %s", received_topic);

  /* -- Recording start command --*/
  snprintf(topic_name, 1024, CONFIG_PROJ_BASE_NAME "/" CONFIG_PROJ_ENV_NAME "/recordings/%s/start",
           mqtt_cert_data.thing_name);
  if (!strcmp(received_topic, topic_name)) {
    /// TODO: Handle payload being sent in multiple events
    payload = cJSON_ParseWithLength(event->data, event->data_len);
    ESP_GOTO_ON_ERROR(parse_recording_start(payload, &rec_conf), cleanup, TAG,
                      "Couldn't parse recording start payload");
    // For now, print the values
    rec_print_config(&rec_conf);
    // Post the event
    esp_event_post_to(rec_event_h, RECORDING_EVENTS, REC_BEGIN, (void *)&rec_conf, sizeof(rec_conf),
                      100);
    // Subscribe to relevant topic
    snprintf(topic_name, 1024,
             CONFIG_PROJ_BASE_NAME "/" CONFIG_PROJ_ENV_NAME "/recordings/%s/%s/commands",
             mqtt_cert_data.thing_name, rec_conf.transaction_id);
    esp_mqtt_client_subscribe(client, topic_name, 0);

    goto cleanup;
  }

  /* -- In progress recording topic --*/
  snprintf(topic_name, 1024,
           CONFIG_PROJ_BASE_NAME "/" CONFIG_PROJ_ENV_NAME "/recordings/%s/%s/commands",
           mqtt_cert_data.thing_name, rec_conf.transaction_id);
  if (!strcmp(received_topic, topic_name)) {
    // Get the payload
    payload = cJSON_ParseWithLength(event->data, event->data_len);
    // Check that the formatting is correct
    if (!cJSON_IsString(cJSON_GetObjectItem(payload, "command")) ||
        !cJSON_IsString(cJSON_GetObjectItem(payload, "transactionId"))) {
      ESP_LOGW(TAG, "Invalid payload format");
      goto cleanup;
    }
    // Extract the command from the payload
    char *command = cJSON_GetObjectItem(payload, "command")->valuestring;
    if (!strcmp(command, "STOP")) {
      // Store the transaction ID
      strncpy(rec_stop_id, cJSON_GetObjectItem(payload, "transactionId")->valuestring,
              sizeof(rec_stop_id) - 1);
      // Post the event
      esp_event_post_to(rec_event_h, RECORDING_EVENTS, REC_STOP, (void *)rec_stop_id,
                        sizeof(rec_stop_id), 100);
    }
    goto cleanup;
  }

  /* -- Upload commands --*/
  snprintf(topic_name, 1024, CONFIG_PROJ_BASE_NAME "/" CONFIG_PROJ_ENV_NAME "/uploads/%s/",
           mqtt_cert_data.thing_name);
  if (strstr(received_topic, topic_name) != NULL) {
    /// TODO: Handle payload being sent in multiple events
    ESP_LOGD(TAG, "Passing %s to s3 uploader handler", received_topic);
    s3_uploader_handler(received_topic, event->data, event->data_len);

    goto cleanup;
  }

  /* -- AWS Jobs commands -- */
  sprintf(topic_name, "$aws/things/%s/jobs/", mqtt_cert_data.thing_name);
  if (strstr(received_topic, topic_name) != NULL) {
    /// TODO: Handle payload being sent in multiple events
    ESP_LOGV(TAG, "Passing data from %s to AWS jobs handler", received_topic);
    jobs_data_handler(mqtt_cert_data.thing_name, event->data, event->data_len);
    goto cleanup;
  }
  /* -- AWS MQTT file streams -- */
  sprintf(topic_name, "$aws/things/%s/streams/", mqtt_cert_data.thing_name);
  if (strstr(received_topic, topic_name) != NULL) {
    /// TODO: Handle payload being sent in multiple events
    ESP_LOGV(TAG, "Passing data from %s to AWS streams handler", received_topic);
    jobs_stream_data_handler(mqtt_cert_data.thing_name, event->data, event->data_len);
    goto cleanup;
  }

  /* -- Camera configuration -- */
  sprintf(topic_name, CONFIG_PROJ_BASE_NAME "/" CONFIG_PROJ_ENV_NAME "/cameras/%s/configure",
          mqtt_cert_data.thing_name);
  if (!strcmp(received_topic, topic_name)) {
    ESP_LOGI(TAG, "Received configuration message");
    // Get the payload
    payload = cJSON_ParseWithLength(event->data, event->data_len);
    // Parse the payload
    conf_parse_cjson_payload(payload, &cam_conf);
    // Publish the configuration event
    //// TODO: Remove portMAX_DELAY and fail accordingly
    esp_event_post_to(conf_event_h, CONFIGURATION_EVENTS, CONF_RECEIVED, &cam_conf,
                      sizeof(configuration_t), portMAX_DELAY);
    goto cleanup;
  }

cleanup:
  if (payload)
    cJSON_Delete(payload);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Encountered error: %s", esp_err_to_name(ret));
    /// TODO: publish error event
  }
}

/*================== Public Functions ==================*/
esp_err_t mqttworker_publish_initial_state(cJSON *sdJSON, cJSON *vmanJSON) {
  esp_err_t      ret     = ESP_OK;
  cJSON         *payload = cJSON_CreateObject();
  char          *payload_str;
  struct timeval system_time;

  /* Get information */
  gettimeofday(&system_time, NULL);
  /* Build the payload */
  cJSON_AddStringToObject(payload, "thingName", mqtt_cert_data.thing_name);
  cJSON_AddNumberToObject(payload, "deviceTime", (uint64_t)system_time.tv_sec);
  cJSON_AddItemToObject(payload, "sdCardInfo", sdJSON);
  cJSON_AddItemToObject(payload, "videoInfo", vmanJSON);
  cJSON_AddStringToObject(payload, "firmwareVersion", esp_app_get_description()->version);
  payload_str = cJSON_Print(payload);
  ESP_GOTO_ON_FALSE(payload_str != NULL, ESP_ERR_NO_MEM, cleanup, TAG,
                    "No memory for the payload string");
  int msg_id = esp_mqtt_client_publish(
      client, CONFIG_PROJ_BASE_NAME "/" CONFIG_PROJ_ENV_NAME "/cameras/hello", payload_str, 0, 1,
      0);
  ESP_GOTO_ON_FALSE(msg_id >= 0, ESP_FAIL, cleanup, TAG, "Couldn't publish initial state (%d)",
                    msg_id);
  ESP_LOGD(TAG, "sent publish successful, msg_id=%d", msg_id);
cleanup:
  if (payload_str)
    cJSON_free(payload_str);
  if (payload)
    cJSON_Delete(payload);
  return ret;
  /// TODO: Handle errors
}

esp_err_t mqttworker_publish_current_state(cJSON *sdJSON, bool is_recording) {
  esp_err_t      ret     = ESP_OK;
  cJSON         *payload = cJSON_CreateObject();
  char          *payload_str;
  struct timeval system_time;

  /* Get information */
  gettimeofday(&system_time, NULL);
  /* Build the payload */
  cJSON_AddStringToObject(payload, "thingName", mqtt_cert_data.thing_name);
  cJSON_AddNumberToObject(payload, "deviceTime", (uint64_t)system_time.tv_sec);
  cJSON_AddItemToObject(payload, "sdCardInfo", sdJSON);
  cJSON_AddBoolToObject(payload, "isRecording", is_recording);
  cJSON_AddBoolToObject(payload, "isUploading", s3_uploader_is_busy());
  payload_str = cJSON_Print(payload);
  int msg_id  = esp_mqtt_client_publish(
      client, CONFIG_PROJ_BASE_NAME "/" CONFIG_PROJ_ENV_NAME "/cameras/status", payload_str, 0, 1,
      0);
  if (msg_id >= 0) {
    ESP_LOGV(TAG, "[%s] Sent publish successful, msg_id=%d", __func__, msg_id);
    ESP_LOGD(TAG, "[%s] Published current status", __func__);
  }
  if (payload_str)
    cJSON_free(payload_str);
  if (payload)
    cJSON_Delete(payload);
  return ret;
  /// TODO: Handle errors
}

esp_err_t mqttworker_publish_recording_state(cJSON *recJSON) {
  esp_err_t ret = ESP_OK;
  char     *payload_str;
  char      topic_name[1024] = "";

  payload_str = cJSON_Print(recJSON);
  snprintf(topic_name, 1024,
           CONFIG_PROJ_BASE_NAME "/" CONFIG_PROJ_ENV_NAME "/recordings/%s/%s/status",
           mqtt_cert_data.thing_name, rec_conf.transaction_id);
  int msg_id = esp_mqtt_client_publish(client, topic_name, payload_str, 0, 1, 0);
  ESP_LOGD(TAG, "sent publish successful, msg_id=%d", msg_id);
  if (payload_str)
    cJSON_free(payload_str);
  if (recJSON)
    cJSON_Delete(recJSON);
  return ret;
  /// TODO: Handle errors
}

esp_err_t mqttworker_get_thingname(char thing_name[128]) {
  ESP_RETURN_ON_FALSE(mqtt_cert_data.populated, ESP_ERR_INVALID_STATE, TAG,
                      "MQTT Cert data has not been populated, can't retrieve ThingName");
  strlcpy(thing_name, mqtt_cert_data.thing_name, 128);
  return ESP_OK;
}

esp_err_t mqttworker_check_for_jobs() {
  // Ask for pending jobs
  ESP_LOGD(TAG, "Getting pending jobs for %s", mqtt_cert_data.thing_name);
  jobs_get_pending(mqtt_cert_data.thing_name, "getJobs");
  jobs_checked = true;
  return ESP_OK;
}

esp_err_t mqttworker_configure_tcp_keep_alive(int idle_s, int interval_s, int retries) {
  int old_idle    = mqtt_cfg.network.tcp_keep_alive_cfg.keep_alive_idle;
  int old_inteval = mqtt_cfg.network.tcp_keep_alive_cfg.keep_alive_interval;
  int old_retries = mqtt_cfg.network.tcp_keep_alive_cfg.keep_alive_count;

  if (client != NULL) {
    ESP_RETURN_ON_ERROR(esp_mqtt_client_stop(client), TAG, "Failed stopping the MQTT client");
    ESP_RETURN_ON_ERROR(esp_mqtt_client_destroy(client), TAG, "Failed destroying the MQTT client");
  }
  mqtt_cfg.network.tcp_keep_alive_cfg.keep_alive_idle     = idle_s;
  mqtt_cfg.network.tcp_keep_alive_cfg.keep_alive_interval = interval_s;
  mqtt_cfg.network.tcp_keep_alive_cfg.keep_alive_count    = retries;

  client = esp_mqtt_client_init(&mqtt_cfg);
  if (client == NULL) {
    ESP_LOGE(TAG, "Failed configuring the MQTT client with new TCP values, rolling back");
    mqtt_cfg.network.tcp_keep_alive_cfg.keep_alive_idle     = old_idle;
    mqtt_cfg.network.tcp_keep_alive_cfg.keep_alive_interval = old_inteval;
    mqtt_cfg.network.tcp_keep_alive_cfg.keep_alive_count    = old_retries;
    client                                                  = esp_mqtt_client_init(&mqtt_cfg);
    /* Register the event handlers */
    esp_mqtt_client_register_event(client, MQTT_EVENT_CONNECTED, mqtt_connected_handler, NULL);
    esp_mqtt_client_register_event(client, MQTT_EVENT_DATA, mqtt_data_handler, NULL);
    esp_mqtt_client_start(client);
    return ESP_ERR_INVALID_ARG;
  }
  ESP_LOGD(TAG, "Registering event handlers for MQTT client");
  esp_mqtt_client_register_event(client, MQTT_EVENT_CONNECTED, mqtt_connected_handler, NULL);
  esp_mqtt_client_register_event(client, MQTT_EVENT_DATA, mqtt_data_handler, NULL);
  // Update MQTT client in submodules
  s3_update_mqtt_client(client);
  jobs_update_mqtt_client(client);
  ESP_LOGD(TAG, "Restarting MQTT client");
  return esp_mqtt_client_start(client);
}

/*================== Initalize, Begin, Stop and Deinitialize ==================*/
esp_err_t mqttworker_init(QueueHandle_t free_chunk_queue, QueueHandle_t filled_chunk_queue) {
  esp_err_t err;
  /* Create the connected semaphore */
  mqtt_conn_semphr = xSemaphoreCreateBinary();

  /* Check if certificates are in NVS */
  ESP_RETURN_ON_ERROR(nvsman_get_certs(&mqtt_cert_data), TAG, "Couldn't get certificates from NVS");
  // Configure the MQTT client
  mqtt_cfg.credentials.authentication.certificate = (const char *)mqtt_cert_data.client_crt;
  mqtt_cfg.credentials.authentication.key         = (const char *)mqtt_cert_data.client_key;
  mqtt_cfg.broker.verification.certificate        = (const char *)mqtt_cert_data.root_ca;
  if (!strcmp(mqtt_cert_data.cert_id, "provisioning")) {
    // This means the provisioning certs will be used
    // Initialize the client for the provision claimer
    if (client != NULL)
      esp_mqtt_set_config(client, &mqtt_cfg);
    else
      client = esp_mqtt_client_init(&mqtt_cfg);
    ESP_LOGI(TAG, "Entering provisioning flow");
    err = provision_begin(client, &mqtt_cert_data);
  }
  /// TODO: Catch other errors
  err = nvsman_get_certs(&mqtt_cert_data);
  ESP_LOGD(TAG, "ThingName is %s", mqtt_cert_data.thing_name);
  mqtt_cfg.credentials.client_id = (const char *)mqtt_cert_data.thing_name;
  client                         = esp_mqtt_client_init(&mqtt_cfg);
  handler_conf.mqtt_client       = &client;
  strlcpy(handler_conf.thing_name, mqtt_cert_data.thing_name, sizeof(handler_conf.thing_name));
  // Register MQTT event handlers
  esp_mqtt_client_register_event(client, MQTT_EVENT_CONNECTED, mqtt_connected_handler, NULL);
  esp_mqtt_client_register_event(client, MQTT_EVENT_DATA, mqtt_data_handler, NULL);
  register_event_handlers();
  /* Initialize the jobs manager */
  ESP_RETURN_ON_ERROR(
      jobs_init(client, mqtt_cert_data.ota_key, free_chunk_queue, filled_chunk_queue), TAG,
      "Couldn't initialize the jobs manager");
  /* Initialize the uploader */
  s3uploader_cfg_t up_cfg = {
      .thing_name       = mqtt_cert_data.thing_name,
      .rec_dir          = "videos",
      .http_timeout_ms  = 20000,
      .http_put_retries = 3,
  };
  ESP_RETURN_ON_ERROR(s3uploader_init(client, &up_cfg), TAG,
                      "Couldn't initialize the S3 uploader component");

  return err;
}

esp_err_t mqttworker_begin(int timeout_ms) {
  esp_err_t ret = ESP_OK;
  /* Start MQTT event loop */
  ESP_LOGI(TAG, "Connecting to endpoint: %s", CONFIG_AWS_ENDPOINT);
  ESP_RETURN_ON_ERROR(esp_mqtt_client_start(client), TAG, "Couldn't start the MQTT client");
  /* Wait for the client to stablish a connection */
  ret = xSemaphoreTake(mqtt_conn_semphr,
                       timeout_ms > 0 ? pdMS_TO_TICKS(timeout_ms) : portMAX_DELAY) == pdTRUE
            ? ESP_OK
            : ESP_ERR_TIMEOUT;
  return ret;
}

esp_err_t mqttworker_stop() {
  ESP_RETURN_ON_ERROR(esp_mqtt_client_stop(client), TAG, "Couldn't stop the MQTT client");
  return ESP_OK;
}

esp_err_t mqttworker_deinit(void) {
  ESP_RETURN_ON_ERROR(jobs_deinit(), TAG, "Coudln't deinitialize the Jobs Manager");
  ESP_RETURN_ON_ERROR(s3uploader_deinit(), TAG, "Couldn't deinitialize the S3 Uploader");
  ESP_RETURN_ON_ERROR(esp_event_handler_instance_unregister_with(
                          rec_event_h, RECORDING_EVENTS, REC_STARTED, rec_started_handler_h),
                      TAG, "Couldn't unregister recording started event handler");
  ESP_RETURN_ON_ERROR(esp_event_handler_instance_unregister_with(rec_event_h, RECORDING_EVENTS,
                                                                 REC_DONE, rec_done_handler_h),
                      TAG, "Couldn't unregister recording done event handler");
  ESP_RETURN_ON_ERROR(esp_event_handler_instance_unregister_with(rec_event_h, RECORDING_EVENTS,
                                                                 REC_ERROR, rec_error_handler_h),
                      TAG, "Couldn't unregister recording error event handler");
  ESP_RETURN_ON_ERROR(esp_event_handler_instance_unregister_with(
                          conf_event_h, CONFIGURATION_EVENTS, CONF_APPLIED, conf_applied_handler_h),
                      TAG, "Couldn't unregister configuration applied event handler");
  ESP_RETURN_ON_ERROR(esp_mqtt_client_unregister_event(client, MQTT_EVENT_DATA, mqtt_data_handler),
                      TAG, "Couldn't unregister MQTT data handler");
  ESP_RETURN_ON_ERROR(
      esp_mqtt_client_unregister_event(client, MQTT_EVENT_CONNECTED, mqtt_connected_handler), TAG,
      "Couldn't unregister MQTT connected handler");
  ESP_RETURN_ON_ERROR(esp_mqtt_client_stop(client), TAG, "Couldn't stop the MQTT client");
  ESP_RETURN_ON_ERROR(esp_mqtt_client_destroy(client), TAG, "Couldn't destroy the MQTT client");

  return ESP_OK;
}