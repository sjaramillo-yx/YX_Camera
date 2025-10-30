set(srcs
  src/SD_manager.c
)

set(include_dirs include)

set(
  requires
    esp_driver_sdmmc
    esp_mm
    fatfs
    vfs
)


idf_component_register(
  SRCS ${srcs}
  INCLUDE_DIRS ${include_dirs}
  REQUIRES ${requires}
)