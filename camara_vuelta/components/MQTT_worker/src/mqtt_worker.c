/**
 * @file mqtt_worker.c
 * @date September 2025
 * @author Simón Jaramillo <sjaramillo@yx.cl>
 *
 * @ingroup mqtt_worker
 */

#include "mqtt_worker.h"
#include "nvs_manager.h"
#include "provision_claimer.h"

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
/**
 * @name Certificates
 * @brief Certs included in the binary image.
 * @{
 */
extern const uint8_t client_cert_pem_start[] asm("_binary_client_crt_start");
extern const uint8_t client_key_pem_start[] asm("_binary_client_key_start");
extern const uint8_t server_cert_pem_start[] asm("_binary_amazon_pem_start");
/** @} */
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
    /// TODO: Make this URL configurable in KConfig
    .broker.address.uri              = "mqtts://" CONFIG_AWS_ENDPOINT ":8883",
    .broker.verification.certificate = (const char *)server_cert_pem_start,
    .credentials = {.authentication = {.certificate = (const char *)client_cert_pem_start,
                                       .key         = (const char *)client_key_pem_start}},
    .buffer      = {.size = 8192, .out_size = 8192}};

/**
 * @brief The initialization semaphore
 */
static SemaphoreHandle_t mqtt_init_semphr = NULL;

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
  ESP_LOGD(TAG, "Connected to MQTT broker");
  esp_mqtt_event_handle_t  event            = event_data;
  esp_mqtt_client_handle_t client           = event->client;
  char                     topic_name[1024] = "";

  // Subscribe to relevant topics
  snprintf(topic_name, 1024, "yx/recordings/%s/start", mqtt_cert_data.thing_name);
  esp_mqtt_client_subscribe(client, topic_name, 0);
  // Give the initialization semaphore
  xSemaphoreGive(mqtt_init_semphr);
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
  esp_mqtt_event_handle_t event = event_data;
  cJSON                  *payload;
  char                    received_topic[1024] = "";
  char                    topic_name[1024]     = "";

  strncpy(received_topic, event->topic, event->topic_len);
  received_topic[event->topic_len] = '\0';
  ESP_LOGD(TAG, "Received data from MQTT topic %s", received_topic);

  /* -- Recording start notification --*/
  snprintf(topic_name, 1024, "yx/recordings/%s/start", mqtt_cert_data.thing_name);
  if (!strcmp(received_topic, topic_name)) {
    /// TODO: Encapsulate this in a helper function
    /// TODO: Handle payload being sent in multiple events
    payload = cJSON_ParseWithLength(event->data, event->data_len);
    cJSON *res, *qps;
    // Extract array parameters from payload
    res = cJSON_GetObjectItem(payload, "resolution");
    qps = cJSON_GetObjectItem(payload, "QPs");
    // Check that the whole payload is correctly formatted
    /// TODO: Fix this mess
    if (cJSON_IsNumber(cJSON_GetObjectItem(payload, "version")) && cJSON_IsArray(res) &&
        cJSON_GetArraySize(res) == 2 && cJSON_IsNumber(cJSON_GetArrayItem(res, 0)) &&
        cJSON_IsNumber(cJSON_GetArrayItem(res, 1)) &&
        cJSON_IsNumber(cJSON_GetObjectItem(payload, "fps")) && cJSON_IsArray(qps) &&
        cJSON_GetArraySize(qps) == 2 && cJSON_IsNumber(cJSON_GetArrayItem(qps, 0)) &&
        cJSON_IsNumber(cJSON_GetArrayItem(qps, 1)) &&
        cJSON_IsNumber(cJSON_GetObjectItem(payload, "timeout")) &&
        cJSON_IsString(cJSON_GetObjectItem(payload, "transactionId")) &&
        cJSON_IsNumber(cJSON_GetObjectItem(payload, "targetBitrate"))) {
      rec_conf.hres            = (uint16_t)cJSON_GetArrayItem(res, 0)->valuedouble;
      rec_conf.vres            = (uint16_t)cJSON_GetArrayItem(res, 1)->valuedouble;
      rec_conf.fps             = (uint16_t)cJSON_GetObjectItem(payload, "fps")->valuedouble;
      rec_conf.qp_min          = (uint8_t)cJSON_GetArrayItem(qps, 0)->valuedouble;
      rec_conf.qp_max          = (uint8_t)cJSON_GetArrayItem(qps, 1)->valuedouble;
      rec_conf.timeout_seconds = (uint32_t)cJSON_GetObjectItem(payload, "timeout")->valuedouble;
      strncpy(rec_conf.transaction_id, cJSON_GetObjectItem(payload, "transactionId")->valuestring,
              sizeof(rec_conf.transaction_id) - 1);
      rec_conf.transaction_id[sizeof(rec_conf.transaction_id) - 1] = '\0';
      rec_conf.target_bitrate =
          (uint32_t)cJSON_GetObjectItem(payload, "targetBitrate")->valuedouble;

      cJSON *job           = cJSON_GetObjectItemCaseSensitive(payload, "jobId");
      rec_conf.has_aws_job = cJSON_IsString(job);
      if (rec_conf.has_aws_job) {
        strncpy(rec_conf.aws_job_id, job->valuestring, sizeof(rec_conf.aws_job_id) - 1);
        rec_conf.aws_job_id[sizeof(rec_conf.aws_job_id) - 1] = '\0';
      } else {
        rec_conf.aws_job_id[0] = '\0';
      }

      // For now, print the values
      /// TODO: For each value, check for not NULL and data type
      ESP_LOGI(TAG, "Received recording parameters:");
      ESP_LOGI(TAG, "\t\tResolution:%dx%d", rec_conf.hres, rec_conf.vres);
      ESP_LOGI(TAG, "\t\tFPS:%d", rec_conf.fps);
      ESP_LOGI(TAG, "\t\tQPs:%d (max), %d (min)", rec_conf.qp_max, rec_conf.qp_min);
      ESP_LOGI(TAG, "\t\tTimeout:%d", rec_conf.timeout_seconds);
      ESP_LOGI(TAG, "\t\tTransaction ID:%s", rec_conf.transaction_id);
      ESP_LOGI(TAG, "\t\tTarget bitrate:%d", rec_conf.target_bitrate);
      ESP_LOGI(TAG, "\t\tJob ID:%s", rec_conf.has_aws_job ? rec_conf.aws_job_id : "N/A");
      // Post the event
      esp_event_post_to(rec_event_h, RECORDING_EVENTS, REC_BEGIN, (void *)&rec_conf,
                        sizeof(rec_conf), 100);
      // Subscribe to relevant topic
      snprintf(topic_name, 1024, "yx/recordings/%s/%s", mqtt_cert_data.thing_name,
               rec_conf.transaction_id);
      esp_mqtt_client_subscribe(client, topic_name, 0);
    }
    // cleanup
    cJSON_Delete(payload);
  }

  /* -- In progress recording topic --*/
  snprintf(topic_name, 1024, "yx/recordings/%s/%s", mqtt_cert_data.thing_name,
           rec_conf.transaction_id);
  if (!strcmp(received_topic, topic_name)) {
    // Get the payload
    payload = cJSON_ParseWithLength(event->data, event->data_len);
    // Check that the formatting is correct
    if (!cJSON_IsString(cJSON_GetObjectItem(payload, "command")) ||
        !cJSON_IsString(cJSON_GetObjectItem(payload, "transactionId"))) {
      ESP_LOGW(TAG, "Invalid payload format");
      cJSON_Delete(payload);
      return;
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
  }
}

static void mqttworker_rec_handler(void *handler_arg, esp_event_base_t event_base, int32_t event_id,
                                   void *event_data) {
  ESP_LOGI(TAG, "Received event %s:%d", (char *)event_base, event_id);
  recording_conf_t *vman_rec_params  = (recording_conf_t *)event_data;
  cJSON            *payload          = cJSON_CreateObject();
  char              topic_name[1024] = "";
  int               msg_id;
  switch (event_id) {
  case REC_STARTED:
    ESP_LOGI(TAG, "Video Manager started recording with ID %s", vman_rec_params->transaction_id);
    snprintf(topic_name, 1024, "yx/recordings/%s/%s", mqtt_cert_data.thing_name,
             rec_conf.transaction_id);
    /// TODO: Build payload for this message
    cJSON_AddStringToObject(payload, "status", "STARTED");
    cJSON_AddStringToObject(payload, "transactionId", vman_rec_params->transaction_id);
    /// TODO: Add the rest of the attributes here
    msg_id = esp_mqtt_client_publish(client, topic_name, cJSON_Print(payload), 0, 1, 0);
    ESP_LOGD(TAG, "sent publish successful, msg_id=%d", msg_id);
    cJSON_Delete(payload);
    break;

  case REC_ERROR:
    ESP_LOGI(TAG, "Video Manager reported error in recording with ID %s",
             vman_rec_params->transaction_id);
    break;
  }
}

/*================== Statics ==================*/
/**
 * @brief initialize the MQTT client with default certificates (pre-provisioning)
 */
static esp_err_t mqttworker_defaults(void) {
  mqtt_cfg.credentials.authentication.certificate = (const char *)client_cert_pem_start;
  mqtt_cfg.credentials.authentication.key         = (const char *)client_key_pem_start;
  if (client != NULL)
    return esp_mqtt_set_config(client, &mqtt_cfg);
  client = esp_mqtt_client_init(&mqtt_cfg);

  return client != NULL ? ESP_OK : ESP_FAIL;
}

/*================== Public Functions ==================*/
esp_err_t mqttworker_begin(void) {
  /* Create the initialization semaphore */
  mqtt_init_semphr = xSemaphoreCreateBinary();
  /* Check if certificates are in NVS */
  esp_err_t err = nvsman_begin();
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    /* If certificates not present, load the default configuration */
    err = mqttworker_defaults();
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Couldn't configure MQTT client!");
      /// TODO: Handle errors
    }
    err = provision_begin(client);
    xSemaphoreTake(mqtt_init_semphr, portMAX_DELAY);
  }
  err = nvsman_get_certs(&mqtt_cert_data);
  ESP_LOGD(TAG, "ThingName is %s", mqtt_cert_data.thing_name);
  mqtt_cfg.credentials.authentication.certificate = (const char *)mqtt_cert_data.client_crt;
  mqtt_cfg.credentials.authentication.key         = (const char *)mqtt_cert_data.client_key;
  client                                          = esp_mqtt_client_init(&mqtt_cfg);

  /* Register the event handlers */
  esp_mqtt_client_register_event(client, MQTT_EVENT_CONNECTED, mqtt_connected_handler, NULL);
  esp_mqtt_client_register_event(client, MQTT_EVENT_DATA, mqtt_data_handler, NULL);
  /* Get the recording event loop handle */
  /// TODO:  Check for errors
  rec_eventloop_get_handle(&rec_event_h);
  ESP_ERROR_CHECK(esp_event_handler_instance_register_with(
      rec_event_h, RECORDING_EVENTS, ESP_EVENT_ANY_ID, mqttworker_rec_handler, NULL, NULL));
  /* Start MQTT event loop */
  esp_mqtt_client_start(client);
  /* Wait for the client to stablish a connection */
  xSemaphoreTake(mqtt_init_semphr, portMAX_DELAY);

  return err;
}

esp_err_t mqttworker_publish_initial_state(cJSON *sdJSON) {
  esp_err_t      ret     = ESP_OK;
  cJSON         *payload = cJSON_CreateObject();
  struct timeval system_time;

  /* Get information */
  gettimeofday(&system_time, NULL);
  /* Build the payload */
  cJSON_AddStringToObject(payload, "thingName", mqtt_cert_data.thing_name);
  cJSON_AddNumberToObject(payload, "deviceTime", (uint64_t)system_time.tv_sec);
  cJSON_AddItemToObject(payload, "sdCardInfo", sdJSON);
  cJSON_AddStringToObject(payload, "firmwareVersion", esp_app_get_description()->version);
  int msg_id = esp_mqtt_client_publish(client, "yx/cameras/hello", cJSON_Print(payload), 0, 1, 0);
  ESP_LOGD(TAG, "sent publish successful, msg_id=%d", msg_id);
  cJSON_Delete(payload);
  return ret;
  /// TODO: Handle errors
}