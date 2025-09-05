#include "provision_claimer.h"
#include "nvs_manager.h"

/*-------- Globals ---------*/
/**
 * @brief Logging tag for this module
 */
static const char *TAG = "Provision Claimer";
/**
 * @brief A structure for MQTT arguments passed to the provisioning tasks
 */
static mqtt_args_t mqtt_args = {0};
/**
 * @brief The token that proves ownership of the received certificate
 */
static char ownership_tkn[1024];
/**
 * @brief The obtained ThingName
 */
static char thing_name[512];
/**
 * @brief Byffers for certificate and private key
 */
static char device_certificate[4096];
static char private_key[4096];
static char certificate_id[1024];
/**
 * @brief The Task handle for the provisioning task
 */
static TaskHandle_t provision_handle;

/* ---------- MQTT handlers ---------- */
/**
 * @brief The handler for the `MQTT_EVENT_CONNECTED` event
 * 
 * @param handler_args user data registered to the event.
 * @param base Event base for the handler(always MQTT Base in this case).
 * @param event_id The id for the received event.
 * @param event_data The data for the event, esp_mqtt_event_handle_t.
 */
static void mqtt_connected_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
  ESP_LOGD(TAG, "Connected to MQTT broker");
  esp_mqtt_event_handle_t event = event_data;
  esp_mqtt_client_handle_t client = event->client;
  switch(state) {
    case UNINITIALIZED:
      esp_mqtt_client_subscribe(client, "$aws/certificates/create/json/accepted", 0);
      esp_mqtt_client_subscribe(client, "$aws/certificates/create/json/rejected", 0);
    break;
    case GOT_CERTIFICATE:
      esp_mqtt_client_subscribe(client, "$aws/provisioning-templates/" CONFIG_TEMPLATE_NAME "/provision/json/accepted", 0);
      esp_mqtt_client_subscribe(client, "$aws/provisioning-templates/" CONFIG_TEMPLATE_NAME "/provision/json/rejected", 0);
    break;
    default:
    break;
  }
}

/**
 * @brief The handler for the `MQTT_EVENT_SUBSCRIBED` event
 * 
 * @param handler_args user data registered to the event.
 * @param base Event base for the handler(always MQTT Base in this case).
 * @param event_id The id for the received event.
 * @param event_data The data for the event, esp_mqtt_event_handle_t.
 */
static void mqtt_subscribed_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
  ESP_LOGD(TAG, "Subscribed to MQTT topic");
  esp_mqtt_event_handle_t event = event_data;
  esp_mqtt_client_handle_t client = event->client;
  int msg_id;
  cJSON *payload = cJSON_CreateObject();
  cJSON *parameters = cJSON_CreateObject();
  uint8_t mac_addr[6] = {0};
  char serial_num[7];
  switch(state) {
    case UNINITIALIZED:
      state = SUSCRIBED_CERT_CREATION;
      // Attempt to create a new certificate
      msg_id = esp_mqtt_client_publish(client, "$aws/certificates/create/json", NULL, 0, 1, 0);
      ESP_LOGD(TAG, "sent publish successful, msg_id=%d", msg_id);
    break;
    case GOT_CERTIFICATE:
      state = SUSCRIBED_THING_CREATION;
      /* get MAC address */
      esp_efuse_mac_get_default(mac_addr);
      ESP_LOGD(TAG, "Ethernet MAC Address: %02x:%02x:%02x:%02x:%02x:%02x",
         mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
      snprintf(serial_num, sizeof(serial_num), "%02x%02x%02x", mac_addr[3], mac_addr[4], mac_addr[5]);
      ESP_LOGD(TAG, "Serial number from MAC: %s", serial_num);
      // Attempt to create a new Thing
      cJSON_AddStringToObject(payload, "certificateOwnershipToken", ownership_tkn);
      cJSON_AddStringToObject(parameters, "SerialNumber", serial_num);
      cJSON_AddItemToObjectCS(payload, "parameters", parameters);
      msg_id = esp_mqtt_client_publish( client, "$aws/provisioning-templates/" CONFIG_TEMPLATE_NAME "/provision/json", 
                                        cJSON_Print(payload), 0, 1, 0);
      ESP_LOGD(TAG, "sent publish successful, msg_id=%d", msg_id);
    break;
    default:
    break;
  }
  cJSON_Delete(payload);
}

/**
 * @brief The handler for the `MQTT_EVENT_DATA` event
 * 
 * @param handler_args user data registered to the event.
 * @param base Event base for the handler(always MQTT Base in this case).
 * @param event_id The id for the received event.
 * @param event_data The data for the event, esp_mqtt_event_handle_t.
 */
static void mqtt_data_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
  ESP_LOGD(TAG, "Received data");
  esp_mqtt_event_handle_t event = event_data;
  esp_mqtt_client_handle_t client = event->client;
  cJSON *payload = cJSON_Parse(event->data);
  cJSON *token, *cert, *certId, *key, *new_thing_name;
  switch(state) {
    case SUSCRIBED_CERT_CREATION:
      state = GOT_CERTIFICATE;
      // Extract certificate from payload
      token  = cJSON_GetObjectItem(payload, "certificateOwnershipToken");
      cert   = cJSON_GetObjectItem(payload, "certificatePem");
      key    = cJSON_GetObjectItem(payload, "privateKey");
      certId = cJSON_GetObjectItem(payload, "certificateId");
      snprintf(ownership_tkn, sizeof(ownership_tkn), "%s", token->valuestring);
      ESP_LOGD(TAG, "certificateOwnershipToken=%s", ownership_tkn);
      // Update client configuration
      snprintf(device_certificate, sizeof(device_certificate), "%s", cert->valuestring);
      snprintf(certificate_id, sizeof(certificate_id), "%s", certId->valuestring);
      snprintf(private_key, sizeof(private_key), "%s", key->valuestring);
      // Suscribe to Thing creation reserved topics
      esp_mqtt_client_subscribe(client, "$aws/provisioning-templates/" CONFIG_TEMPLATE_NAME "/provision/json/accepted", 0);
      esp_mqtt_client_subscribe(client, "$aws/provisioning-templates/" CONFIG_TEMPLATE_NAME "/provision/json/rejected", 0);
      break;
    case SUSCRIBED_THING_CREATION:
      state = CREATED_THING;
      // Extract thing name
      new_thing_name = cJSON_GetObjectItem(payload, "thingName");
      ESP_LOGI(TAG, "Thing Name = %s", new_thing_name->valuestring);
      snprintf(thing_name, sizeof(thing_name), "%s", new_thing_name->valuestring);
      xTaskNotifyGive(provision_handle);
      break;
    default:
    break;
  }
  cJSON_Delete(payload);
}


/*--------------- FreeRTOS Tasks ---------------*/
static void provision_task(void *p) {
  esp_mqtt_client_handle_t client = p;
  if (client == NULL) {
    ESP_LOGE(TAG, "MQTT client handle is empty!");
    abort();
  }
  /* Register the event handlers */
  esp_mqtt_client_register_event(client, MQTT_EVENT_CONNECTED, mqtt_connected_handler, NULL);
  esp_mqtt_client_register_event(client, MQTT_EVENT_SUBSCRIBED, mqtt_subscribed_handler, NULL);
  esp_mqtt_client_register_event(client, MQTT_EVENT_DATA, mqtt_data_handler, NULL);
  /* Start MQTT event loop */
  esp_mqtt_client_start(client);

  /* Wait for this task to be notified */
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

  /* Write to NVS */
  cert_data_t *obtained_certificates = malloc(sizeof(cert_data_t));
  if (obtained_certificates == NULL) {
    ESP_LOGE(TAG, "Unsufficient memory!");
  }
  /// TODO: Pass this strings as arguments instead of a structure to save memory
  snprintf(obtained_certificates->client_crt, sizeof(device_certificate), device_certificate);
  snprintf(obtained_certificates->client_key, sizeof(private_key), private_key);
  snprintf(obtained_certificates->cert_id, sizeof(certificate_id), certificate_id);
  snprintf(obtained_certificates->thing_name, sizeof(thing_name), thing_name);
  nvsman_save_certs(obtained_certificates);
  free(obtained_certificates);

  /* Reboot */
  esp_restart();

  /* Delete this task */
  vTaskDelete(NULL);
}

/*--------- Functions ----------*/
esp_err_t provision_begin(esp_mqtt_client_handle_t mqtt_client) {
  BaseType_t task_created = xTaskCreate(provision_task,
                                        "BeginProvision",
                                        4096, (void*)mqtt_client, 5,
                                        &provision_handle);
  return (task_created == pdTRUE ? ESP_OK : ESP_FAIL);
}