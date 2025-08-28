#include "WiFi_manager.h"

/* FreeRTOS event group to signal when we are connected */
static EventGroupHandle_t s_wifi_event_group;

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static const char *TAG = "wifi station";

static bool wifi_initialized = false;
static int s_retry_num = 0;

static void event_handler(void* arg, esp_event_base_t event_base,
                          int32_t event_id, void* event_data){
  static int ap_idx = 1;
  if (event_base == WIFI_EVENT) {
    switch(event_id) {
      case WIFI_EVENT_STA_START:
        // Do nothing
        break;
      case WIFI_EVENT_STA_DISCONNECTED:
        // If we got disconnected, check if we still have retries available
        if (s_retry_num < CONFIG_MAX_RETRY) {
          esp_wifi_connect();
          s_retry_num++;
          ESP_LOGD(TAG, "retrying connection to selected AP");
        } else {
          // If we are out of retries, set the failbit of the event group
          xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        break;
      }
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    // If we obtained an IP, log it and reset fail bit and retry count
    ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
    ESP_LOGD(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
    s_retry_num = 0;
    xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    /* Sync internal clock with NTP server*/
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&config);
  } // end if
  return;
}

esp_err_t wifi_init_sta() {
  esp_err_t ret = ESP_OK;                                  // Default to no errors
  if (!wifi_initialized) {
    s_wifi_event_group = xEventGroupCreate();             // Create the wifi event group
    ESP_ERROR_CHECK(esp_netif_init());                    // Attempt to initialize the network interface
    esp_event_loop_create_default();                      // Create the default event loop *Note: this should only be done once!
    esp_netif_create_default_wifi_sta();                  // Create the wifi station using default values
    // Initialize the WiFi interface
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();  // Use default configuration
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    // Event handles
    esp_event_handler_instance_t instance_any_id;         // Any ID for WIFI_EVENT
    esp_event_handler_instance_t instance_got_ip;         // Only care for "got ID" event
    // Register our custom event handler (and check for errors)
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id) );
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip) );
  }
  wifi_initialized = true;
  ESP_LOGD(TAG, "wifi_init_sta finished.");
  return ret;
}

esp_err_t connect_wifi(char * ssid, char * pass){
  esp_err_t ret = ESP_OK;      // Default to no errors
  // Clear the event bits for a fresh connection attempt
  xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
  // Configure the AP settings
  /// TODO: Allow selection of authmode via KConfig
  wifi_ap_record_t ap_info;
  if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK && !strcmp((char *)ap_info.ssid, (char *)ssid)){
    ESP_LOGI(TAG, "Already connected to %s", ssid);
    return ESP_OK;
  }
  esp_wifi_disconnect();
  s_retry_num = 0;
  wifi_config_t wifi_config = {0};                          // Start with an empty configuration
  strncpy((char *)wifi_config.sta.ssid, (const char*)ssid, 32);          // Copy SSID to configuration
  strncpy((char *)wifi_config.sta.password, (const char*)pass, 64);      // Copy password to configuration
  wifi_config.sta.ssid[31] = '\0';                          // Null-terminate SSID string
  wifi_config.sta.password[63] = '\0';                      // Null-terminate password string
  wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;  // Set the authentication mode to WPA2-PSK
  // Start the WiFi station with selected configuration
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
  ESP_ERROR_CHECK(esp_wifi_start() );
  esp_wifi_connect();
  // Wait for the connection to be succesfully established
  EventBits_t bits = xEventGroupWaitBits( s_wifi_event_group,
                                          WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                          pdFALSE, pdFALSE, portMAX_DELAY);
  // Check that the WIFI_CONNECTED_BIT was set by the event handler
  /// TODO: Better error checking
  ret = ESP_FAIL;
  if (bits & WIFI_CONNECTED_BIT) {
    ESP_LOGI(TAG, "connected to AP %s", ssid);
  } else if (bits & WIFI_FAIL_BIT) {
    ESP_LOGW(TAG, "Failed to connect to AP %s", ssid);
  } else {
    ESP_LOGE(TAG, "UNEXPECTED EVENT");
  }

  return ret;
}