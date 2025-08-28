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
  switch(state) {
    case UNINITIALIZED:
      state = SUSCRIBED_CERT_CREATION;
      // Attempt to create a new certificate
      msg_id = esp_mqtt_client_publish(client, "$aws/certificates/create/json", NULL, 0, 0, 0);
      ESP_LOGD(TAG, "sent publish successful, msg_id=%d", msg_id);
    break;
    case GOT_CERTIFICATE:
      state = SUSCRIBED_THING_CREATION;
      // Attempt to create a new Thing
      cJSON_AddStringToObject(payload, "certificateOwnershipToken", ownership_tkn);
      msg_id = esp_mqtt_client_publish( client, "$aws/provisioning-templates/" CONFIG_TEMPLATE_NAME "/provision/json", 
                                        cJSON_Print(payload), 0, 0, 0);
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
  cJSON *token;
  switch(state) {
    case SUSCRIBED_CERT_CREATION:
      state = GOT_CERTIFICATE;
      // Extract certificate from payload
      token = cJSON_GetObjectItem(payload, "certificateOwnershipToken");
      snprintf(ownership_tkn, sizeof(ownership_tkn), "%s", token->valuestring);
      ESP_LOGI(TAG, "certificateOwnershipToken=%s", ownership_tkn);
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
}
