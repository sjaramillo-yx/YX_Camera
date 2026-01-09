#include "SD_manager.h"
#include "esp_check.h"
#include <errno.h>
#include <sys/stat.h>

#define ALIGN_UP(num, align) (((num) + ((align) - 1)) & ~((align) - 1))

static const char *TAG = "SDManager" /**< Logging tag for this module. */;

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

/* =============== STATIC FUNCTIONS ===============*/
static esp_err_t make_dir(const char *path) {
  if (mkdir(path, 0775) == 0) {
    ESP_LOGI(TAG, "Created dir: %s", path);
    return ESP_OK;
  }
  if (errno == EEXIST) {
    ESP_LOGD(TAG, "Dir already exists: %s", path);
    return ESP_OK;
  }
  ESP_LOGE(TAG, "mkdir(%s) failed: errno=%d (%s)", path, errno, strerror(errno));
  return ESP_FAIL;
}

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

  ESP_LOGI(TAG, "Ensuring directories");
  ESP_RETURN_ON_ERROR(make_dir(CONFIG_SD_CARD_MOUNT_POINT "/videos"), TAG,
                      "Couldn't create videos directory");
  ESP_RETURN_ON_ERROR(make_dir(CONFIG_SD_CARD_MOUNT_POINT "/logs"), TAG,
                      "Couldn't create logs directory");
  ESP_RETURN_ON_ERROR(make_dir(CONFIG_SD_CARD_MOUNT_POINT "/firmware"), TAG,
                      "Couldn't create firmware directory");

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

esp_err_t sdman_open_file(const char *path_in, const char *mode, FILE **file_handle) {
  if (file_handle) {
    *file_handle = NULL;
  }

  if (card == NULL || sd_initialized == false) {
    ESP_LOGE(TAG, "SD Card is not initialized!");
    return ESP_ERR_INVALID_STATE;
  }

  if (path_in == NULL || path_in[0] == '\0' || mode == NULL || mode[0] == '\0') {
    ESP_LOGE(TAG, "(%s) Invalid args (path/mode)", __func__);
    return ESP_ERR_INVALID_ARG;
  }

  // fopen() expects VFS-style paths (e.g. "/sdcard/..."), not FatFs "0:/..."
  if (strchr(path_in, ':') != NULL) {
    ESP_LOGE(TAG, "(%s) Invalid VFS path (looks like FatFs drive path): %s", __func__, path_in);
    return ESP_ERR_INVALID_ARG;
  }

  char full_path[256];

  if (path_in[0] == '/') {
    // Caller provided an absolute VFS path like "/sdcard/..."
    int n = snprintf(full_path, sizeof(full_path), "%s", path_in);
    if (n < 0 || n >= (int)sizeof(full_path)) {
      ESP_LOGE(TAG, "(%s) Path too long: %s", __func__, path_in);
      return ESP_ERR_INVALID_SIZE;
    }
  } else {
    // Build: mount_point + "/" + path_in
    const char *mp = mount_point;  // e.g. "/sdcard"
    if (mp == NULL || mp[0] == '\0') {
      ESP_LOGE(TAG, "(%s) mount_point is not set", __func__);
      return ESP_ERR_INVALID_STATE;
    }

    bool mp_has_trailing_slash = (mp[strlen(mp) - 1] == '/');

    int n = snprintf(full_path, sizeof(full_path), mp_has_trailing_slash ? "%s%s" : "%s/%s", mp,
                     path_in);

    if (n < 0 || n >= (int)sizeof(full_path)) {
      ESP_LOGE(TAG, "(%s) Full path too long: %s + %s", __func__, mp, path_in);
      return ESP_ERR_INVALID_SIZE;
    }
  }

  ESP_LOGI(TAG, "Opening file %s (mode=%s)", full_path, mode);

  FILE *f = fopen(full_path, mode);
  if (f == NULL) {
    int err = errno;
    ESP_LOGE(TAG, "Failed opening file: errno=%d (%s)", err, strerror(err));
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "File opened");
  if (file_handle != NULL) {
    *file_handle = f;
  }
  return ESP_OK;
}

esp_err_t sdman_stat_file(char *filename, size_t *file_size) {
  ESP_LOGD(TAG, "Stating file %s", filename);
  // Validate filename
  if (filename == NULL || filename[0] == '\0') {
    ESP_LOGE(TAG, "(%s) Invalid filename (null/empty)", __func__);
    return ESP_ERR_INVALID_ARG;
  }

  char full_path[256];
  if (filename[0] == '/') {
    // Caller already gave an absolute VFS path like "/sdcard/..."
    if (snprintf(full_path, sizeof(full_path), "%s", filename) >= (int)sizeof(full_path)) {
      ESP_LOGE(TAG, "(%s) Path too long: %s", __func__, filename);
      return ESP_ERR_INVALID_SIZE;
    }
  } else {
    // Ensure exactly one slash between mount point and relative path
    const char *mp                    = CONFIG_SD_CARD_MOUNT_POINT;
    bool        mp_has_trailing_slash = (mp[0] != '\0' && mp[strlen(mp) - 1] == '/');

    int n = snprintf(full_path, sizeof(full_path), mp_has_trailing_slash ? "%s%s" : "%s/%s", mp,
                     filename);

    if (n < 0 || n >= (int)sizeof(full_path)) {
      ESP_LOGE(TAG, "(%s) Full path too long: %s + %s", __func__, mp, filename);
      return ESP_ERR_INVALID_SIZE;
    }
  }

  // Stat the file
  ESP_LOGD(TAG, "Built full path %s", full_path);
  struct stat st;
  if (stat(full_path, &st) == 0) {
    if (file_size != NULL) {
      *file_size = (size_t)st.st_size;
    }
    return ESP_OK;
  }
  // stat() failed — map errno to esp_err_t
  int err = errno;
  if (err == ENOENT) {
    ESP_LOGE(TAG, "(%s) File not found: %s", __func__, filename);
    return ESP_ERR_NOT_FOUND;
  }
  if (err == ENOTDIR) {
    // A path component isn't a directory (still effectively "not found" for your use)
    ESP_LOGE(TAG, "(%s) Path component is not a directory: %s", __func__, filename);
    return ESP_ERR_NOT_FOUND;
  }
  if (err == EINVAL || err == ENAMETOOLONG) {
    ESP_LOGE(TAG, "(%s) Invalid filename: %s", __func__, filename);
    return ESP_ERR_INVALID_ARG;
  }

  ESP_LOGE(TAG, "(%s) stat(%s) failed: errno=%d (%s)", __func__, filename, err, strerror(err));
  return ESP_FAIL;
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