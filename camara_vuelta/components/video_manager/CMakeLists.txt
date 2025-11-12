set(srcs
  src/video_manager.c
  src/sensor_init.c
)

set(include_dirs include)
set(priv_include_dirs private_include)

set(
  requires
    esp_driver_cam
    esp_driver_isp
    esp_timer
    Recording_events
    SD_manager
)


idf_component_register(
  SRCS ${srcs}
  INCLUDE_DIRS ${include_dirs}
  PRIV_INCLUDE_DIRS ${priv_include_dirs}
  REQUIRES ${requires}
)