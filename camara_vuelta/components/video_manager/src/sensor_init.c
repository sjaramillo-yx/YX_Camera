#include "sensor_init.h"
#include "driver/i2c_master.h"
#include "esp_attr.h"
#include "esp_cam_sensor_detect.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_sccb_i2c.h"
#include "esp_sccb_intf.h"
#include "sdkconfig.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "sensor_init";

void sensor_init(example_sensor_config_t  *sensor_config,
                 esp_cam_sensor_device_t **out_sensor_device) {
  esp_err_t ret = ESP_FAIL;

  //---------------I2C Init------------------//
  i2c_master_bus_config_t i2c_bus_conf = {
      .clk_source                   = I2C_CLK_SRC_DEFAULT,
      .sda_io_num                   = sensor_config->i2c_sda_io_num,
      .scl_io_num                   = sensor_config->i2c_scl_io_num,
      .i2c_port                     = sensor_config->i2c_port_num,
      .flags.enable_internal_pullup = true,
  };
  i2c_master_bus_handle_t i2c_bus_handle = NULL;
  ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_conf, &i2c_bus_handle));

  //---------------SCCB Init------------------//
  esp_cam_sensor_config_t cam_config = {
      .reset_pin = -1,
      .pwdn_pin  = -1,
      .xclk_pin  = -1,
  };

  esp_cam_sensor_device_t *cam = NULL;
  for (esp_cam_sensor_detect_fn_t *p = &__esp_cam_sensor_detect_fn_array_start;
       p < &__esp_cam_sensor_detect_fn_array_end; ++p) {
    sccb_i2c_config_t i2c_config = {
        .scl_speed_hz    = 10 * 1000,
        .device_address  = p->sccb_addr,
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
    };
    ESP_ERROR_CHECK(sccb_new_i2c_io(i2c_bus_handle, &i2c_config, &cam_config.sccb_handle));

    cam_config.sensor_port = p->port;

    cam = (*(p->detect))(&cam_config);
    if (cam) {
      if (p->port != sensor_config->port) {
        ESP_LOGE(TAG, "detect a camera sensor with mismatched interface");
        return;
      }
      break;
    }
    ESP_ERROR_CHECK(esp_sccb_del_i2c_io(cam_config.sccb_handle));
  }

  if (!cam) {
    ESP_LOGE(TAG, "failed to detect camera sensor");
    return;
  }

  esp_cam_sensor_format_array_t cam_fmt_array = {0};
  esp_cam_sensor_query_format(cam, &cam_fmt_array);
  const esp_cam_sensor_format_t *parray = cam_fmt_array.format_array;
  for (int i = 0; i < cam_fmt_array.count; i++) {
    ESP_LOGD(TAG, "fmt[%d].name:%s", i, parray[i].name);
  }

  esp_cam_sensor_format_t *cam_cur_fmt = NULL;
  for (int i = 0; i < cam_fmt_array.count; i++) {
    if (!strcmp(parray[i].name, sensor_config->format_name)) {
      cam_cur_fmt = (esp_cam_sensor_format_t *)&(parray[i]);
    }
  }
  if (!cam_cur_fmt) {
    ESP_LOGE(TAG, "Unsupported format");
    ESP_ERROR_CHECK(ESP_ERR_INVALID_ARG);
  }

  ret = esp_cam_sensor_set_format(cam, (const esp_cam_sensor_format_t *)cam_cur_fmt);

  int enable_flag = 1;
  // Set sensor output stream
  ret = esp_cam_sensor_ioctl(cam, ESP_CAM_SENSOR_IOC_S_STREAM, &enable_flag);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Start stream fail");
  }
  ESP_ERROR_CHECK(ret);

  *out_sensor_device = cam;
}

void example_sensor_deinit(example_sensor_handle_t sensor_handle) {
  ESP_ERROR_CHECK(esp_sccb_del_i2c_io(sensor_handle.sccb_handle));
  ESP_ERROR_CHECK(i2c_del_master_bus(sensor_handle.i2c_bus_handle));
}

esp_err_t set_sensor_format(uint16_t hres, uint16_t vres, uint16_t *fps,
                            esp_cam_sensor_device_t *dev) {

  // Retrieve compatible formats
  esp_cam_sensor_format_array_t cam_fmt_array = {0};
  esp_cam_sensor_query_format(dev, &cam_fmt_array);
  const esp_cam_sensor_format_t *match = NULL;
  for (int i = 0; i < cam_fmt_array.count; i++) {
    // Check if dimensions match
    if (cam_fmt_array.format_array[i].width == hres &&
        cam_fmt_array.format_array[i].height == vres) {
      // Record the matching format
      match = &cam_fmt_array.format_array[i];
      // Check if the FPS value matches as well
      if (*fps == 0 || cam_fmt_array.format_array[i].fps == *fps) {
        break;
      }
    }
  }

  // If no match was found, report error and return
  ESP_RETURN_ON_FALSE(match != NULL, ESP_ERR_INVALID_ARG, TAG,
                      "Sensor doesn't support requested format %ux%u", hres, vres);

  // If the caller requested a specific FPS, ensure the sensor offers it for this resolution
  ESP_RETURN_ON_FALSE(*fps == 0 || match->fps == *fps, ESP_ERR_INVALID_ARG, TAG,
                      "Sensor doesn't support requested FPS %u for %ux%u", *fps, hres, vres);

  // Try to set the matching format
  ESP_RETURN_ON_ERROR(esp_cam_sensor_set_format(dev, match), TAG,
                      "Failed to set sensor format to %ux%u", hres, vres);

  // If caller didn't request a particular FPS, expose the sensor's default for this format
  if (*fps == 0)
    *fps = match->fps;

  return ESP_OK;
}