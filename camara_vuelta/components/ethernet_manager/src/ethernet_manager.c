/**
 * @file ethernet_manager.c
 * @date September 2025
 * @author Simón Jaramillo <sjaramillo@yx.cl>
 *
 * @ingroup ethernet_manager
 */

#include "ethernet_manager.h"

static const char *TAG = "Ethernet Manager" /**< Logging tag for this module. */;

/*-------- Globals ---------*/
static bool              initialized = false;
static bool              got_ip      = false;
static SemaphoreHandle_t ip_semphr   = NULL;

/*================== Static functions ==================*/
/**
 * @brief A simple handler for the IP obtained event that simply gives the ip semaphore.
 */
static void got_ip_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id,
                                 void *event_data) {
  ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
  ESP_LOGI(TAG, "Got IP " IPSTR, IP2STR(&event->ip_info.ip));
  got_ip = true;
  xSemaphoreGiveFromISR(ip_semphr, NULL);
}

/**
 * @brief A simple handler for the IP lost event that simply takes the ip semaphore.
 */
static void lost_ip_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id,
                                  void *event_data) {
  got_ip = false;
  ESP_LOGW(TAG, "Lost IP");
  xSemaphoreTakeFromISR(ip_semphr, NULL);
}

/**
 * @brief Internal ESP32 Ethernet initialization
 *
 * @param[out] mac_out optionally returns Ethernet MAC object
 * @param[out] phy_out optionally returns Ethernet PHY object
 * @return
 *          - esp_eth_handle_t if init succeeded
 *          - NULL if init failed
 */
static esp_eth_handle_t eth_init_internal(esp_eth_mac_t **mac_out, esp_eth_phy_t **phy_out) {
  esp_eth_handle_t ret = NULL;

  // Init common MAC and PHY configs to default
  eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
  eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();

  // Update PHY config based on board specific configuration
  phy_config.phy_addr       = -1;
  phy_config.reset_gpio_num = CONFIG_ETH_RST_GPIO;
  // Init vendor specific MAC config to default
  eth_esp32_emac_config_t esp32_emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
  // Update vendor specific MAC config based on board configuration
  esp32_emac_config.smi_gpio.mdc_num  = CONFIG_ETH_MDC_GPIO;
  esp32_emac_config.smi_gpio.mdio_num = CONFIG_ETH_MDIO_GPIO;
  // Create new ESP32 Ethernet MAC instance
  esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&esp32_emac_config, &mac_config);
  esp_eth_phy_t *phy = esp_eth_phy_new_ip101(&phy_config);
  // Init Ethernet driver to default and install it
  esp_eth_handle_t eth_handle = NULL;
  esp_eth_config_t config     = ETH_DEFAULT_CONFIG(mac, phy);
  ESP_GOTO_ON_FALSE(esp_eth_driver_install(&config, &eth_handle) == ESP_OK, NULL, err, TAG,
                    "Ethernet driver install failed");
  if (mac_out != NULL) {
    *mac_out = mac;
  }
  if (phy_out != NULL) {
    *phy_out = phy;
  }
  return eth_handle;

err:
  // Cleanup on error
  if (eth_handle != NULL) {
    esp_eth_driver_uninstall(eth_handle);
  }
  if (mac != NULL) {
    mac->del(mac);
  }
  if (phy != NULL) {
    phy->del(phy);
  }
  return ret;
}

/*================== Public functions ==================*/
esp_err_t ethman_wait_ip(int ms_to_wait) {
  ESP_RETURN_ON_FALSE(initialized == true, ESP_ERR_INVALID_STATE, TAG,
                      "Ethernet manager has not been initialized yet");
  if (got_ip)
    return ESP_OK;
  ESP_LOGI(TAG, "Waiting for IP address");
  got_ip = xSemaphoreTake(ip_semphr, ms_to_wait > 0 ? pdMS_TO_TICKS(ms_to_wait) : portMAX_DELAY) ==
           pdTRUE;
  if (!got_ip)
    ESP_LOGW(TAG, "Failed to obtain IP address within %d ms timeout", ms_to_wait);
  ESP_LOGI(TAG, "IP%sobtained", got_ip ? " " : " not  ");
  return got_ip ? ESP_OK : ESP_FAIL;
}

esp_err_t ethman_wait_sntp(int ms_to_wait) {
  ESP_RETURN_ON_FALSE(initialized, ESP_ERR_INVALID_STATE, TAG,
                      "Ethernet manager has not been initialized yet");
  ESP_RETURN_ON_FALSE(got_ip, ESP_ERR_INVALID_STATE, TAG, "No IP address has been received yet");
  esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
  esp_netif_sntp_init(&config);
  ESP_LOGI(TAG, "Waiting for SNTP synchronization");
  if (esp_netif_sntp_sync_wait(ms_to_wait > 0 ? pdMS_TO_TICKS(ms_to_wait) : portMAX_DELAY) !=
      ESP_OK) {
    ESP_LOGW(TAG, "Failed to update system time within %d ms timeout", ms_to_wait);
    return ESP_ERR_TIMEOUT;
  }
  return ESP_OK;
}

esp_err_t ethman_init(esp_eth_handle_t *eth_handle_out) {
  /* Function variables */
  esp_err_t         ret         = ESP_OK;
  esp_eth_handle_t *eth_handle  = NULL;
  esp_eth_mac_t    *mac         = NULL;
  esp_eth_phy_t    *phy         = NULL;
  uint8_t           mac_addr[6] = {0};
  /* Create IP semaphore */
  ip_semphr = xSemaphoreCreateBinary();
  /* Initialize Ethernet */
  ESP_GOTO_ON_FALSE(eth_handle_out != NULL, ESP_ERR_INVALID_ARG, err, TAG,
                    "Invalid arguments: ethernet handle is already initialized!");
  eth_handle = calloc(1, sizeof(esp_eth_handle_t));
  ESP_GOTO_ON_FALSE(eth_handle != NULL, ESP_ERR_NO_MEM, err, TAG, "no memory");
  eth_handle = eth_init_internal(&mac, &phy);
  ESP_GOTO_ON_FALSE(eth_handle, ESP_FAIL, err, TAG, "Internal ethernet init failed");
  /* Initialize netif and event loop */
  ESP_GOTO_ON_FALSE(esp_netif_init() == ESP_OK, ESP_FAIL, err, TAG, "Couldn't initialize netif!");
  ret = esp_event_loop_create_default();
  ESP_GOTO_ON_FALSE(ret == ESP_OK || ret == ESP_ERR_INVALID_STATE, ret, err, TAG,
                    "Couldn't create default event loop!");
  /* Glue the netif and ethernet */
  esp_netif_config_t          cfg            = ESP_NETIF_DEFAULT_ETH();
  esp_netif_t                *eth_netif      = esp_netif_new(&cfg);
  esp_eth_netif_glue_handle_t eth_netif_glue = esp_eth_new_netif_glue(eth_handle);
  /* Attach Ethernet driver to TCP/IP stack */
  ESP_GOTO_ON_ERROR(esp_netif_attach(eth_netif, eth_netif_glue), err, TAG,
                    "Couldn't attach netif!");
  /* Start the Ethernet state machine*/
  ret = esp_eth_start(eth_handle);
  ESP_GOTO_ON_FALSE(ret == ESP_OK || ret == ESP_ERR_INVALID_STATE, ret, err, TAG,
                    "Couldn't start the Ethernate state machine!");
  /* Register the IP obtained handler */
  ESP_GOTO_ON_ERROR(
      esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &got_ip_event_handler, NULL), err,
      TAG, "Couldn't register the IP obtained handler");
  ESP_GOTO_ON_ERROR(
      esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_LOST_IP, &lost_ip_event_handler, NULL), err,
      TAG, "Couldn't register the IP lost handler");
  /* Output and return */
  ESP_GOTO_ON_ERROR(esp_efuse_mac_get_default(mac_addr), err, TAG, "Couldn't ger MAC from efuse");
  ESP_LOGD(TAG, "Created Ethernet interface with MAC Address: %02x:%02x:%02x:%02x:%02x:%02x",
           mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
  *eth_handle_out = eth_handle;
  initialized     = true;
  return ESP_OK;

err:
  free(eth_handle);
  if (mac != NULL)
    mac->del(mac);
  if (phy != NULL)
    phy->del(phy);
  /// TODO: Make sure all resources are freed on error

  return ret;
}