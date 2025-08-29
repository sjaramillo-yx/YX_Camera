#include "provision_claimer.h"
#include "mqtt_worker.h"

/**
 * @brief A common logging tag, makes debugging easier
 */
static const char *TAG = "ProvisionClaimer";

/**
 * @brief The token that proves ownership of the received certificate
 */
static char ownership_tkn[1024];
/**
 * @brief Byffers for certificate and private key
 */
static char device_certificate[4096];
static char private_key[4096];

/**
 * @brief The Thing creation task handle
 */
static TaskHandle_t create_thing_task = NULL;

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
  switch(state) {
    case UNINITIALIZED:
      state = SUSCRIBED_CERT_CREATION;
      // Attempt to create a new certificate
      msg_id = esp_mqtt_client_publish(client, "$aws/certificates/create/json", NULL, 0, 1, 0);
      ESP_LOGD(TAG, "sent publish successful, msg_id=%d", msg_id);
    break;
    case GOT_CERTIFICATE:
      state = SUSCRIBED_THING_CREATION;
      // Attempt to create a new Thing
      cJSON_AddStringToObject(payload, "certificateOwnershipToken", ownership_tkn);
      cJSON_AddStringToObject(parameters, "SerialNumber", "123");
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
  int msg_id;
  cJSON *payload = cJSON_Parse(event->data);
  cJSON *token, *cert, *key, *thing_name;
  switch(state) {
    case SUSCRIBED_CERT_CREATION:
      state = GOT_CERTIFICATE;
      // Extract certificate from payload
      token = cJSON_GetObjectItem(payload, "certificateOwnershipToken");
      cert  = cJSON_GetObjectItem(payload, "certificatePem");
      key   = cJSON_GetObjectItem(payload, "privateKey");
      snprintf(ownership_tkn, sizeof(ownership_tkn), "%s", token->valuestring);
      ESP_LOGD(TAG, "certificateOwnershipToken=%s", ownership_tkn);
      // Update client configuration
      snprintf(device_certificate, sizeof(device_certificate), "%s", cert->valuestring);
      snprintf(private_key, sizeof(private_key), "%s", key->valuestring);
      // Notify the Thing creation task
      xTaskNotifyGive( create_thing_task );
      break;
    case SUSCRIBED_THING_CREATION:
      state = CREATED_THING;
      // Extract thing name
      thing_name = cJSON_GetObjectItem(payload, "thingName");
      ESP_LOGI(TAG, "Thing Name = %s", thing_name->valuestring);
      vTaskDelete(create_thing_task);
      break;
    default:
    break;
  }
  cJSON_Delete(payload);
}

void provisioner_start(void)
{
  esp_mqtt_client_handle_t client = mqtt_app_start();
  esp_mqtt_client_register_event(client, MQTT_EVENT_CONNECTED, mqtt_connected_handler, NULL);
  esp_mqtt_client_register_event(client, MQTT_EVENT_SUBSCRIBED, mqtt_subscribed_handler, NULL);
  esp_mqtt_client_register_event(client, MQTT_EVENT_DATA, mqtt_data_handler, NULL);
  xTaskCreate(  createThing,
                "CreateThing",
                2048,
                (void *)client,
                5,
                &create_thing_task
              );
}

/*--------------- FreeRTOS Tasks ---------------*/
void createThing(void *p) {
  esp_err_t conf_updated;
  while (true) {
    // Wait for this task to be notified
    ulTaskNotifyTake( pdTRUE, portMAX_DELAY );
    esp_mqtt_client_handle_t client = p;
    // Reconnect with new certificate
    esp_mqtt_client_stop(client);
    //conf_updated = update_cert(device_certificate, private_key);
    //ESP_LOGD(TAG, "%s", conf_updated == ESP_OK ? "" : "Failed to update config");
    esp_mqtt_client_start(client);
  }
}