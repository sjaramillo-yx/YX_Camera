#include "http_helpers.h"
#include "esp_crt_bundle.h"

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
  };

  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  ESP_GOTO_ON_FALSE(client != NULL, ESP_FAIL, cleanup, TAG, "http init failed");

  ESP_GOTO_ON_ERROR(esp_http_client_open(client, (int)len), cleanup_http, TAG, "http open failed");

  uint8_t buf[4096];
  size_t  remaining = len;
  while (remaining > 0) {
    size_t chunk = remaining > sizeof(buf) ? sizeof(buf) : remaining;
    size_t rd    = fread(buf, 1, chunk, f);
    ESP_GOTO_ON_FALSE(rd > 0, ESP_FAIL, cleanup_http, TAG, "file read failed");

    int wr = esp_http_client_write(client, (const char *)buf, (int)rd);
    ESP_GOTO_ON_FALSE(wr > 0, ESP_FAIL, cleanup_http, TAG, "http write failed");
    remaining -= (size_t)wr;
  }

  (void)esp_http_client_fetch_headers(client);
  int code = esp_http_client_get_status_code(client);
  ESP_GOTO_ON_FALSE(code >= 200 && code < 300, ESP_FAIL, cleanup_http, TAG, "HTTP status %d", code);
  ESP_GOTO_ON_FALSE(cap.etag[0] != '\0', ESP_FAIL, cleanup_http, TAG, "ETag missing");
  strncpy(etag_out, cap.etag, CONFIG_S3_MAX_ETAG_LEN);

cleanup_http:
  esp_http_client_close(client);
  esp_http_client_cleanup(client);

cleanup:
  fclose(f);
  return ret;
}