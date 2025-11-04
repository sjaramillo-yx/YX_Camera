#include "SD_manager.h"

#define ALIGN_UP(num, align) (((num) + ((align) - 1)) & ~((align) - 1))

static const char *TAG = "SD Card Manager" /**< Logging tag for this module. */;

/* ================ STRUCTS ================ */
typedef struct {
  char             *data;         // Stage buffer
  uint64_t          staged;       // Staged bytes
  SemaphoreHandle_t write_smphr;  // A semaphore for writing this buffer
} stage_t;

typedef struct {
  stage_t active;    // The active stage
  stage_t inactive;  // The inactive stage
} staging_buffers_t;

/* ================ GLOBALS ================ */
static sdmmc_card_t *card;                                          // Card handle
static const char    mount_point[]   = CONFIG_SD_CARD_MOUNT_POINT;  // Mount point string
static bool          sd_initialized  = false;                       // Initialized flag
sd_pwr_ctrl_handle_t pwr_ctrl_handle = NULL;                        // LDO power handle

/* =============== PUBLIC FUNCTIONS ===============*/

esp_err_t sdman_mount() {
  esp_err_t ret = ESP_OK;

  esp_vfs_fat_sdmmc_mount_config_t mount_config = {
#ifdef CONFIG_FORMAT_IF_MOUNT_FAILED
      .format_if_mount_failed = true,
#else
      .format_if_mount_failed = false,
#endif
      .max_files            = CONFIG_MAX_OPEN_FILES,
      .allocation_unit_size = CONFIG_ALLOCATION_UNIT_SIZE};

  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.max_freq_khz = CONFIG_SDMMC_MAX_SPEED;
  host.flags        = SDMMC_HOST_FLAG_4BIT;

  sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
  slot_config.width               = CONFIG_SDMMC_SLOT_BUS_WIDTH;

  slot_config.clk = CONFIG_PIN_CLK;
  slot_config.cmd = CONFIG_PIN_CMD;
  slot_config.d0  = CONFIG_PIN_D0;
  if (CONFIG_SDMMC_SLOT_BUS_WIDTH == 4) {
    slot_config.d1 = CONFIG_PIN_D1;
    slot_config.d2 = CONFIG_PIN_D2;
    slot_config.d3 = CONFIG_PIN_D3;
  }
  slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

  sd_pwr_ctrl_ldo_config_t ldo_config = {
      .ldo_chan_id = LDO_UNIT_4,
  };

  ret = sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &pwr_ctrl_handle);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to create a new on-chip LDO power control driver");
    return ret;
  }
  host.pwr_ctrl_handle = pwr_ctrl_handle;

  ESP_LOGI(TAG, "Mounting filesystem");
  ret = esp_vfs_fat_sdmmc_mount(mount_point, &host, &slot_config, &mount_config, &card);
  if (ret != ESP_OK) {
    if (ret == ESP_FAIL) {
      ESP_LOGE(TAG, "Failed to mount filesystem.");
    } else {
      ESP_LOGE(TAG, "Failed to initialize the card (%s).", esp_err_to_name(ret));
    }
    return ret;
  }
  sd_initialized = true;
  ESP_LOGI(TAG, "Filesystem mounted");
  host.set_cclk_always_on(card->host.slot, true);
  sdmmc_card_print_info(stdout, card);

  return ret;
}

esp_err_t sdman_umount() {
  ESP_ERROR_CHECK(esp_vfs_fat_sdcard_unmount(mount_point, card));
  ESP_LOGI(TAG, "Card unmounted");
  esp_err_t ret = sd_pwr_ctrl_del_on_chip_ldo(pwr_ctrl_handle);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to delete the on-chip LDO power control driver");
  }
  return ret;
}

esp_err_t sdman_open_file(char *filename, char *mode, FILE **file_handle) {
  if (card == NULL || sd_initialized == false) {
    ESP_LOGE(TAG, "SD Card is not initialized!");
    return ESP_ERR_INVALID_STATE;
  }

  char path[48] = {0};
  strcat(path, mount_point);
  strcat(path, "/");
  strcat(path, filename);

  ESP_LOGI(TAG, "Opening file %s", path);
  FILE *f = fopen(path, mode);
  if (f == NULL) {
    ESP_LOGE(TAG, "Failed opening file");
    return ESP_FAIL;
  }
  ESP_LOGI(TAG, "File opened");
  if (file_handle != NULL) {
    *file_handle = f;
  }

  return ESP_OK;
}

esp_err_t sdman_getJSON(cJSON **sdJSON) {
  esp_err_t ret = ESP_FAIL;
  /// TODO: Cleanup in case of error
  *sdJSON = cJSON_CreateObject();
  if (*sdJSON == NULL)
    goto end;

  cJSON *present = cJSON_CreateBool(card != NULL && sd_initialized);
  if (present == NULL)
    goto end;
  cJSON_AddItemToObject(*sdJSON, "present", present);
  if (cJSON_IsFalse(present)) {
    return ESP_OK;
  }
  uint64_t sd_total_size;
  uint64_t sd_free_size;
  esp_vfs_fat_info(mount_point, &sd_total_size, &sd_free_size);
  cJSON *size_mb = cJSON_CreateNumber(sd_total_size >> 20);
  if (size_mb == NULL)
    goto end;
  cJSON *free_mb = cJSON_CreateNumber(sd_free_size >> 20);
  if (free_mb == NULL)
    goto end;
  cJSON_AddItemToObject(*sdJSON, "sizeMB", size_mb);
  cJSON_AddItemToObject(*sdJSON, "freeMB", free_mb);

  return ESP_OK;

end:
  /// TODO: Cleanup
  return ret;
}