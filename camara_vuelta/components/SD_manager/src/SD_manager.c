#include "SD_manager.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

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

typedef struct video_candidate {
  char                    name[CONFIG_FATFS_MAX_LFN];
  time_t                  mtime;
  uint64_t                size;
  struct video_candidate *next;
} video_candidate_t;

/* ================ GLOBALS ================ */
static sdmmc_card_t *card;                                          // Card handle
static const char    mount_point[]   = CONFIG_SD_CARD_MOUNT_POINT;  // Mount point string
static bool          sd_initialized  = false;                       // Initialized flag
sd_pwr_ctrl_handle_t pwr_ctrl_handle = NULL;                        // LDO power handle
uint64_t             min_free_bytes  = CONFIG_SD_CARD_DEFAULT_MIN_FREE_KB << 10;

// Task handles
static TaskHandle_t s_sd_video_scan_task_h = NULL;

/* =============== STATIC FUNCTIONS ===============*/

static esp_err_t remove_video_if_exists(const char *video_name) {
  if (mount_point[0] == '\0' || !video_name || video_name[0] == '\0') {
    errno = EINVAL;
    return ESP_ERR_INVALID_ARG;
  }

  char path[PATH_MAX];
  int  n = snprintf(path, sizeof(path), "%s/videos/%s", mount_point, video_name);
  if (n < 0 || n >= (int)sizeof(path)) {
    errno = ENAMETOOLONG;
    return ESP_ERR_INVALID_ARG;
  }

  struct stat st;
  if (stat(path, &st) != 0) {
    if (errno == ENOENT)
      return ESP_OK;           // doesn't exist
    return ESP_ERR_NOT_FOUND;  // stat error
  }

  if (unlink(path) != 0) {
    return ESP_FAIL;
  }

  return ESP_OK;
}

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

static void sdman_prune_newest_candidates(video_candidate_t **head, uint64_t target_bytes,
                                          uint64_t *total_bytes) {
  while (*head != NULL && *total_bytes > target_bytes) {
    video_candidate_t *prev = NULL;
    // Find the tail
    video_candidate_t *tail = *head;
    while (tail->next != NULL) {
      prev = tail;
      tail = tail->next;
    }
    // If target bytes have been reached, beak the loop
    if (*total_bytes - tail->size < target_bytes)
      break;
    // Remove reference to this candidate from previous node in linked list
    if (prev == NULL)
      *head = NULL;  // This was the only candidate in the list
    else
      prev->next = NULL;
    // Remove this candidate's bytes from total bytes
    *total_bytes -= tail->size;
    free(tail);  // Free the tail
  }  // end while
}

static void sdman_insert_candidate(video_candidate_t **head, const char *name, time_t mtime,
                                   uint64_t size, uint64_t target_bytes, uint64_t *total_bytes) {
  // Allocate memory for new candidate
  video_candidate_t *entry = calloc(1, sizeof(video_candidate_t));
  if (entry == NULL) {
    ESP_LOGW(TAG, "[%s] Failed to allocate candidate for %s", __func__, name);
    return;
  }
  // Populate the candidate's fields
  strlcpy(entry->name, name, sizeof(entry->name));
  entry->mtime = mtime;
  entry->size  = size;
  // If this is the first candidate in the linked list, make it the head.
  if (*head == NULL) {
    *head = entry;
  } else {
    // If not, find the correct position for this node in the linked list.
    video_candidate_t *prev = NULL;
    video_candidate_t *curr = *head;
    while (curr != NULL) {
      if (entry->mtime < curr->mtime ||
          (entry->mtime == curr->mtime && strcmp(entry->name, curr->name) < 0)) {
        break;
      }
      prev = curr;
      curr = curr->next;
    }
    if (prev == NULL) {
      entry->next = *head;
      *head       = entry;
    } else {
      entry->next = prev->next;
      prev->next  = entry;
    }
  }
  // Add this candidate's bytes to the total byte count.
  *total_bytes += size;
  // Remove newest candidates from the list in order to delete just the necessary candidates later.
  sdman_prune_newest_candidates(head, target_bytes, total_bytes);
}

static void sdman_scan_oldest_videos(void) {
  uint64_t sd_total_size = 0;
  uint64_t sd_free_size  = 0;
  if (esp_vfs_fat_info(mount_point, &sd_total_size, &sd_free_size) != ESP_OK) {
    ESP_LOGW(TAG, "[%s] Failed to read SD card info, skipping scan", __func__);
    return;
  }
  if (sd_free_size >= min_free_bytes) {
    ESP_LOGD(TAG, "[%s] Free space already sufficient (%llu bytes free, target %llu)", __func__,
             (unsigned long long)sd_free_size, (unsigned long long)min_free_bytes);
    return;
  }
  uint64_t bytes_needed = min_free_bytes - sd_free_size;

  char   dir_path[256];
  size_t dir_len = snprintf(dir_path, sizeof(dir_path), "%s/videos", mount_point);
  if (dir_len == 0 || dir_len >= sizeof(dir_path)) {
    ESP_LOGE(TAG, "[%s] Videos directory path too long", __func__);
    return;
  }

  ESP_LOGD(TAG, "[%s] Opening videos directory: %s", __func__, dir_path);
  DIR *dir = opendir(dir_path);
  if (dir == NULL) {
    ESP_LOGW(TAG, "[%s] Failed to open videos directory (%s)", __func__, strerror(errno));
    return;
  }

  video_candidate_t *candidates  = NULL;
  uint64_t           total_bytes = 0;
  uint32_t           file_count  = 0;
  struct dirent     *entry;
  // Scan the whole videos directory
  while ((entry = readdir(dir)) != NULL) {
    if (entry->d_name[0] == '.') {
      continue;
    }
    char file_path[256];
    int  written = snprintf(file_path, sizeof(file_path), "%s/%s", dir_path, entry->d_name);
    if (written < 0 || written >= (int)sizeof(file_path)) {
      ESP_LOGW(TAG, "[%s] Skipping long filename: %s", __func__, entry->d_name);
      continue;
    }
    struct stat st;
    if (stat(file_path, &st) != 0) {
      ESP_LOGD(TAG, "[%s] stat(%s) failed: %s", __func__, file_path, strerror(errno));
      continue;
    }
    if (!S_ISREG(st.st_mode)) {
      continue;  // Not a regular file
    }
    file_count++;
    sdman_insert_candidate(&candidates, entry->d_name, st.st_mtime, (uint64_t)st.st_size,
                           bytes_needed, &total_bytes);
  }

  closedir(dir);

  if (candidates == NULL) {
    ESP_LOGI(TAG, "[%s] No video files found", __func__);
    return;
  }

  if (total_bytes < bytes_needed) {
    ESP_LOGW(TAG, "[%s] Only %llu bytes found (%u files), below %llu bytes needed", __func__,
             (unsigned long long)total_bytes, (unsigned)file_count,
             (unsigned long long)bytes_needed);
  } else {
    ESP_LOGI(TAG, "[%s] Found %llu bytes across oldest videos (%u files scanned)", __func__,
             (unsigned long long)total_bytes, (unsigned)file_count);
  }

  uint64_t           running = 0;
  int                idx     = 0;
  video_candidate_t *curr    = candidates;
  while (curr != NULL) {
    running += curr->size;
    ESP_LOGD(TAG, "[%s] Candidate %d causing %llu bytes total: %s (size=%llu, mtime=%lld)",
             __func__, idx++, (unsigned long long)running, curr->name,
             (unsigned long long)curr->size, (long long)curr->mtime);
    remove_video_if_exists(curr->name);
    curr = curr->next;
  }

  // Free the linked list
  while (candidates != NULL) {
    video_candidate_t *next = candidates->next;
    free(candidates);
    candidates = next;
  }
}

/*======================================== FreeRTOS Tasks ========================================*/
static void sdman_oldest_video_scan_task(void *arg) {
  (void)arg;
  ESP_LOGD(TAG, "[%s] Ready, entering loop", pcTaskGetName(NULL));
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    ESP_LOGI(TAG, "[%s] Notification received, scanning oldest videos", pcTaskGetName(NULL));
    if (!sd_initialized || card == NULL) {
      ESP_LOGW(TAG, "[%s] SD card not ready, skipping scan", pcTaskGetName(NULL));
      continue;
    }
    sdman_scan_oldest_videos();
  }
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

  if (s_sd_video_scan_task_h != NULL) {
    ESP_RETURN_ON_FALSE(eTaskGetState(s_sd_video_scan_task_h) == eDeleted, ESP_ERR_INVALID_STATE,
                        TAG, "Oldest video scan task already running");
  }
  ESP_RETURN_ON_FALSE(xTaskCreate(sdman_oldest_video_scan_task, "sd.video.scan", 4096, NULL, 5,
                                  &s_sd_video_scan_task_h),
                      ESP_FAIL, TAG, "Failed creating video scan task");

  sdman_notify_oldest_video_scan_task();
  return ret;
}

esp_err_t sdman_umount() {
  ESP_RETURN_ON_ERROR(esp_vfs_fat_sdcard_unmount(mount_point, card), TAG,
                      "Couldn't unmount SD card");
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

  // Ensure there's enough space in the SD card after opening this file.
  sdman_notify_oldest_video_scan_task();
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
  esp_err_t ret     = ESP_FAIL;
  cJSON    *size_mb = NULL;
  cJSON    *present = NULL;
  cJSON    *free_mb = NULL;

  ESP_GOTO_ON_FALSE((*sdJSON = cJSON_CreateObject()) != NULL, ESP_ERR_NO_MEM, cleanup, TAG,
                    "No memory for SD card cJSON object");
  ESP_GOTO_ON_FALSE((present = cJSON_CreateBool(card != NULL && sd_initialized)) != NULL,
                    ESP_ERR_NO_MEM, cleanup, TAG, "No memory for SD card presence");
  cJSON_AddItemToObject(*sdJSON, "present", present);
  if (cJSON_IsFalse(present)) {
    return ESP_OK;
  }
  uint64_t sd_total_size;
  uint64_t sd_free_size;
  esp_vfs_fat_info(mount_point, &sd_total_size, &sd_free_size);
  ESP_GOTO_ON_FALSE((size_mb = cJSON_CreateNumber(sd_total_size >> 20)) != NULL, ESP_ERR_NO_MEM,
                    cleanup, TAG, "No memory for SD card size");
  ESP_GOTO_ON_FALSE((free_mb = cJSON_CreateNumber(sd_free_size >> 20)) != NULL, ESP_ERR_NO_MEM,
                    cleanup, TAG, "No memory for SD free space");
  cJSON_AddItemToObject(*sdJSON, "sizeMB", size_mb);
  cJSON_AddItemToObject(*sdJSON, "freeMB", free_mb);

  return ESP_OK;

cleanup:
  if (*sdJSON)
    cJSON_Delete(*sdJSON);
  return ret;
}

esp_err_t sdman_notify_oldest_video_scan_task(void) {
  ESP_RETURN_ON_FALSE(s_sd_video_scan_task_h != NULL, ESP_ERR_INVALID_STATE, TAG,
                      "Oldest video scan task not started");
  ESP_RETURN_ON_FALSE(eTaskGetState(s_sd_video_scan_task_h) != eDeleted, ESP_ERR_INVALID_STATE, TAG,
                      "Oldest video scan task not running");
  xTaskNotifyGive(s_sd_video_scan_task_h);
  return ESP_OK;
}

esp_err_t sdman_set_free_space_target(uint64_t free_kb) {
  //// TODO: Validate `free_kb` value
  min_free_bytes = free_kb << 20;  // To bytes
  return sdman_notify_oldest_video_scan_task();
}
