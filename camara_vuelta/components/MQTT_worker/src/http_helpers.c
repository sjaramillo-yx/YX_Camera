#include "http_helpers.h"
#include "esp_crt_bundle.h"
#include <esp_timer.h>

static const char *TAG = "HTTPHelpers";

typedef struct {
  char etag[CONFIG_S3_MAX_ETAG_LEN];
} http_etag_capture_t;

// Case-insensitive string comparision
static bool str_ieq(const char *a, const char *b) {
  while (*a && *b) {
    if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
      return false;
    a++;
    b++;
  }
  return *a == '\0' && *b == '\0';
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
  http_etag_capture_t *cap = (http_etag_capture_t *)evt->user_data;
  if (!cap)
    return ESP_OK;
  if (evt->event_id == HTTP_EVENT_ON_HEADER && evt->header_key && evt->header_value) {
    if (str_ieq(evt->header_key, "ETag"))
      strncpy(cap->etag, evt->header_value, sizeof(cap->etag));
  }
  return ESP_OK;
}

esp_err_t http_put_part(const char *url, const char *file_path, size_t offset, size_t len,
                        int timeout_ms, char etag_out[CONFIG_S3_MAX_ETAG_LEN]) {
  esp_err_t ret = ESP_OK;
  FILE     *f   = NULL;
  uint8_t  *buf = NULL;
  ESP_RETURN_ON_ERROR(sdman_open_file(file_path, "rb", &f), TAG, "open file failed");

  ESP_GOTO_ON_FALSE(fseek(f, (long)offset, SEEK_SET) == 0, ESP_FAIL, cleanup, TAG, "fseek failed");

  http_etag_capture_t      cap = {0};
  esp_http_client_config_t cfg = {
      .url               = url,
      .method            = HTTP_METHOD_PUT,
      .timeout_ms        = timeout_ms,
      .event_handler     = http_event_handler,
      .user_data         = &cap,
      .crt_bundle_attach = esp_crt_bundle_attach,
      .buffer_size_tx    = 32 * 1024,
  };

  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  ESP_GOTO_ON_FALSE(client != NULL, ESP_FAIL, cleanup, TAG, "http init failed");
  ESP_LOGD(TAG, "(%s) HTTP client initialized", __func__);

  ESP_GOTO_ON_ERROR(esp_http_client_open(client, (int)len), cleanup_http, TAG, "http open failed");
  ESP_LOGD(TAG, "(%s) HTTP client open", __func__);

  buf = heap_caps_aligned_alloc(16, 128 * 1024,
                                MALLOC_CAP_CACHE_ALIGNED | MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);

  size_t remaining = len;
  ESP_LOGD(TAG, "(%s) Uploading %d bytes", __func__, len);
  int64_t start = esp_timer_get_time() / 1000000ULL;
  while (remaining > 0) {
    size_t chunk = remaining > 128 * 1024 ? 128 * 1024 : remaining;
    size_t rd    = fread(buf, 1, chunk, f);
    ESP_GOTO_ON_FALSE(rd > 0, ESP_FAIL, cleanup_http, TAG, "file read failed");
    size_t sent = 0;
    while (sent < rd) {
      int wr = esp_http_client_write(client, (const char *)buf + sent, (int)(rd - sent));
      ESP_GOTO_ON_FALSE(wr > 0, ESP_FAIL, cleanup_http, TAG, "http write failed");
      sent      += (size_t)wr;
      remaining -= (size_t)wr;
    }
    ESP_LOGD(TAG, "(%s) %d/%d bytes uploaded (%ld B/s)", __func__, len - remaining, len,
             ((int64_t)len - (int64_t)remaining) / (esp_timer_get_time() / 1000000ULL - start));
  }

  (void)esp_http_client_fetch_headers(client);
  int code = esp_http_client_get_status_code(client);
  if (code < 200 || code >= 300) {
    char body[1024] = {0};
    int  r          = esp_http_client_read_response(client, body, sizeof(body) - 1);
    if (r > 0) {
      body[r] = 0;
      ESP_LOGE(TAG, "HTTP %d, body: %s", code, body);
    } else {
      ESP_LOGE(TAG, "HTTP %d (empty body)", code);
    }
    ret = ESP_FAIL;
    goto cleanup_http;
  }
  ESP_GOTO_ON_FALSE(code >= 200 && code < 300, ESP_FAIL, cleanup_http, TAG, "HTTP status %d", code);
  ESP_GOTO_ON_FALSE(cap.etag[0] != '\0', ESP_FAIL, cleanup_http, TAG, "ETag missing");
  strncpy(etag_out, cap.etag, CONFIG_S3_MAX_ETAG_LEN);

cleanup_http:
  esp_http_client_close(client);
  esp_http_client_cleanup(client);
  if (buf)
    heap_caps_free(buf);

cleanup:
  fclose(f);
  return ret;
}