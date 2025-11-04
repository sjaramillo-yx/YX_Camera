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
/*================= Globals =================*/
/**
 * @name Certificates
 * @brief Certs included in the binary image.
 * @{
 */
extern const uint8_t client_cert_pem_start[] asm("_binary_client_crt_start");
extern const uint8_t client_key_pem_start[] asm("_binary_client_key_start");
extern const uint8_t server_cert_pem_start[] asm("_binary_amazon_pem_start");

static cert_data_t mqtt_cert_data;
/** @} */

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
  esp_mqtt_event_handle_t  event  = event_data;
  esp_mqtt_client_handle_t client = event->client;
  int                      msg_id;

  // Give the initialization semaphore
  xSemaphoreGive(mqtt_init_semphr);
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
  }
  err = nvsman_get_certs(&mqtt_cert_data);
  ESP_LOGD(TAG, "ThingName is %s", mqtt_cert_data.thing_name);
  mqtt_cfg.credentials.authentication.certificate = (const char *)mqtt_cert_data.client_crt;
  mqtt_cfg.credentials.authentication.key         = (const char *)mqtt_cert_data.client_key;
  client                                          = esp_mqtt_client_init(&mqtt_cfg);

  /* Register the event handlers */
  esp_mqtt_client_register_event(client, MQTT_EVENT_CONNECTED, mqtt_connected_handler, NULL);
  /* Create the initialization semaphore */
  mqtt_init_semphr = xSemaphoreCreateBinary();
  /* Start MQTT event loop */
  esp_mqtt_client_start(client);
  /* Wait for the client to stablish a connection */
  xSemaphoreTake(mqtt_init_semphr, portMAX_DELAY);

  return err;
}