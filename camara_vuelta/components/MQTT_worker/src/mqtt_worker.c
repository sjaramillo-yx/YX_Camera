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

/**
 * @name Certificates
 * @brief Certs included in the binary image.
 * @{
 */
extern const uint8_t client_cert_pem_start[] asm("_binary_client_crt_start");
extern const uint8_t client_key_pem_start[] asm("_binary_client_key_start");
extern const uint8_t server_cert_pem_start[] asm("_binary_amazon_pem_start");
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
  return err;
}