/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "driver/i2c_master.h"
#include "esp_cam_sensor.h"
#include "hal/color_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Configuration of SCCB interface and sensor
 */
typedef struct {
  int                   i2c_port_num;   /* SCCB: i2c port */
  int                   i2c_sda_io_num; /* SCCB: i2c SDA IO number */
  int                   i2c_scl_io_num; /* SCCB: i2c SCL IO number */
  esp_cam_sensor_port_t port;           /* Sensor: interface of the camera sensor */
  const char           *format_name;    /* Sensor: format to be set for the camera sensor */
} example_sensor_config_t;

/**
 * @brief SCCB Interface and Sensor Init
 *
 * @param[in]  sensor_config         Camera sensor configuration
 * @param[out] out_sensor_device     Camera sensor handle
 */
void sensor_init(example_sensor_config_t  *sensor_config,
                 esp_cam_sensor_device_t **out_sensor_device);

/**
 * @brief SCCB Interface and Sensor Deinit
 */
void sensor_deinit(void);

/**
 * @brief Configure the sensor's resolution and FPS
 */
esp_err_t set_sensor_format(uint16_t hres, uint16_t vres, uint16_t *fps,
                            esp_cam_sensor_device_t *dev);

#ifdef __cplusplus
}
#endif