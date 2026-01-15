#include "OTA_manager.h"
#include "OTA_events.h"
#include "esp_task_wdt.h"

static const char *TAG = "OTAManager";

/*================= Globals =================*/
// Event loop
static esp_event_loop_handle_t OTA_event_h;
static OTA_state_t             otaman_state = OTA_MAN_UNITIALIZED;

// OTA variables
static esp_ota_handle_t s_ota_handle;

// OTA chunks
static uint8_t *s_ota_chunks[CONFIG_OTA_CHUNK_COUNT];

// Queues
static QueueHandle_t s_free_chunk_q   = NULL;  // items: uint8_t* (ota_chunk)
static QueueHandle_t s_filled_chunk_q = NULL;  // items: uint8_t* (ota_chunk)

// Tasks
static TaskHandle_t s_ota_task_h = NULL;

/*================== Event handlers ==================*/

/*========================= Bootstrapping =========================*/
static void allocate_pools(void) {
  // Allocate OTA Chunks (word-aligned, DRAM)
  for (int i = 0; i < CONFIG_OTA_CHUNK_COUNT; ++i) {
    s_ota_chunks[i] = (uint8_t *)heap_caps_aligned_alloc(64, CONFIG_OTA_CHUNK_SIZE,
                                                         MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    if (s_ota_chunks[i] == NULL) {
      ESP_LOGE(TAG, "Failed to alloc OTA chunk %d (%u bytes)", i, (unsigned)CONFIG_OTA_CHUNK_SIZE);
      abort();
    }
    ESP_LOGV(TAG, "Allocated s_ota_chunks[%d] at %p", i, s_ota_chunks[i]);
  }
}

static void create_queues(void) {
  s_free_chunk_q   = xQueueCreate(CONFIG_OTA_CHUNK_COUNT + 2, sizeof(ota_chunk_t));
  s_filled_chunk_q = xQueueCreate(CONFIG_OTA_CHUNK_COUNT, sizeof(ota_chunk_t));
  if (!s_free_chunk_q || !s_filled_chunk_q) {
    ESP_LOGE(TAG, "Queue creation failed");
    abort();
  }
}

/*================== FreeRTOS Tasks ==================*/
static void ota_task(void *arg) {
  esp_err_t        ret           = ESP_OK;
  ota_chunk_t      curr_chunk    = {0};
  esp_partition_t *ota_partition = arg;

  if (s_ota_handle == NULL) {
    ESP_LOGE(TAG, "[%s] Task called with NULL OTA handle", pcTaskGetName(NULL));
    vTaskDelete(NULL);
  }
  if (otaman_state != OTA_MAN_READY) {
    ESP_LOGE(TAG, "[%s] Task called with invalid OTA Manager state (0x%02x)", pcTaskGetName(NULL),
             otaman_state);
    vTaskDelete(NULL);
  }
  // Seed the queues
  for (int i = 0; i < CONFIG_OTA_CHUNK_COUNT; ++i) {
    uint8_t *p      = s_ota_chunks[i];
    curr_chunk.data = p;
    curr_chunk.len  = CONFIG_OTA_CHUNK_SIZE;
    ESP_LOGD(TAG, "Chunk %d with buffer at %p", i, curr_chunk.data);
    /// TODO: Remove portMAX_DELAY and fail accordingly
    xQueueSendToBack(s_free_chunk_q, &curr_chunk, portMAX_DELAY);
  }
  // Move to downloading state
  otaman_state = OTA_MAN_DOWNLOADING;
  // Receive filled chunks and write them to the OTA partition
  ESP_LOGD(TAG, "[%s] Ready, entering loop", pcTaskGetName(NULL));
  while (true) {
    memset(&curr_chunk, 0, sizeof(ota_chunk_t));
    if (xQueueReceive(s_filled_chunk_q, &curr_chunk, portMAX_DELAY) != pdTRUE) {
      continue;
    }
    ESP_LOGD(TAG, "Received filled chunk with buffer at %p", curr_chunk.data);
    /// TODO: Validate the chunk
    ESP_GOTO_ON_ERROR(esp_ota_write(s_ota_handle, curr_chunk.data, curr_chunk.len), end, TAG,
                      "Couldn't write chunk to OTA partition");
    if (curr_chunk.last) {
      ESP_LOGI(TAG, "Received last chunk, closing");
      /// TODO: Turn all this into a static function
      /// TODO: Drain free queue
      // End the OTA flow
      esp_ota_end(s_ota_handle);
      // Update boot partition
      ESP_LOGI(TAG, "Setting updated partition as boot");
      esp_ota_set_boot_partition(ota_partition);
      /// TODO: Check for errors and move to OTA_MAN_ERROR state.
      otaman_state = OTA_MAN_DONE;
      esp_event_post_to(OTA_event_h, OTA_EVENTS, OTA_CTRL_DONE, NULL, 0, portMAX_DELAY);
      vTaskDelete(NULL);
    }
    // Return the chunk to the MQTT Worker for writing
    /// TODO: Remove portMAX_DELAY and fail accordingly
    xQueueSendToBack(s_free_chunk_q, &curr_chunk, portMAX_DELAY);
  }  // end while(true)
end:
  vTaskDelete(NULL);
}

/*================== Public Functions ==================*/
esp_err_t otaman_run_test(component_test test_function) {
  esp_err_t          ret      = ESP_OK;
  ota_fail_record_t *fail_rec = (ota_fail_record_t *)calloc(sizeof(ota_fail_record_t), 1);
  // Run the test
  ret = test_function(fail_rec->detail);
  if (ret == ESP_OK) {
    free(fail_rec);
    return ESP_OK;
  }
  // Something failed
  ESP_LOGE(TAG, "Component test failed with code %s", esp_err_to_name(ret));
  // Populate the failure record
  fail_rec->esp_err = (int32_t)ret;
  fail_rec->magic   = 0x4F544146;  // "OTAF" in hex format
  ESP_RETURN_ON_ERROR(nvsman_save_ota_fail(fail_rec), TAG,
                      "Couldn't save OTA failure record to NVS");
  // Mark app as invalid and rollback
  ret = esp_ota_mark_app_invalid_rollback_and_reboot();
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "Rollback failed! (%s)", esp_err_to_name(ret));
  }
  return ret;
}

esp_err_t otaman_can_start(uint32_t image_size) {
  ESP_RETURN_ON_FALSE(otaman_state == OTA_MAN_READY, ESP_ERR_INVALID_STATE, TAG, "Busy (0x%02x)",
                      otaman_state);
  // Check that current partition is valid
  const esp_partition_t *running = esp_ota_get_running_partition();
  if (running) {
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    if (esp_ota_get_state_partition(running, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
      return ESP_ERR_INVALID_STATE;
    }
  }

  esp_partition_t *target_partition = esp_ota_get_next_update_partition(NULL);
  ESP_RETURN_ON_FALSE(target_partition != NULL, ESP_ERR_NOT_FOUND, TAG,
                      "Couldn't find next available partition");
  ESP_RETURN_ON_FALSE(
      target_partition->size >= image_size, ESP_ERR_NO_MEM, TAG,
      "Image is too large for partition %s (image is %d bytes and partition is %d bytes)",
      target_partition->label, image_size, target_partition->size);
  return ESP_OK;
}

esp_err_t otaman_start_update(uint32_t image_size) {
  esp_partition_t *target_partition = esp_ota_get_next_update_partition(NULL);
  ESP_RETURN_ON_FALSE(target_partition != NULL, ESP_ERR_NOT_FOUND, TAG,
                      "Couldn't find next available partition");
  ESP_RETURN_ON_ERROR(esp_ota_begin(target_partition, image_size, &s_ota_handle), TAG,
                      "Couldn't begin OTA flow");
  // Create the OTA task
  if (s_ota_task_h != NULL) {
    ESP_RETURN_ON_FALSE(eTaskGetState(s_ota_task_h) == eDeleted, ESP_ERR_INVALID_STATE, TAG,
                        "OTA Task is already running");
  }
  xTaskCreate(ota_task, "ota.task", 4096, target_partition, 10, &s_ota_task_h);
  return ESP_OK;
}

esp_err_t otaman_init(QueueHandle_t *free_chunk_queue, QueueHandle_t *filled_chunk_queue) {
  esp_err_t ret = ESP_OK;
  ESP_LOGI(TAG, "Initializing buffers and queues.");
  /// TODO: Unify both functions
  allocate_pools();
  create_queues();
  // Get the OTA events handle
  ESP_RETURN_ON_ERROR(OTA_eventloop_get_handle(&OTA_event_h), TAG,
                      "Couldn't get the OTA event loop handle");
  // Output the queue handles
  *free_chunk_queue   = s_free_chunk_q;
  *filled_chunk_queue = s_filled_chunk_q;
  // Move to ready state and return
  otaman_state = OTA_MAN_READY;
  return ESP_OK;
}