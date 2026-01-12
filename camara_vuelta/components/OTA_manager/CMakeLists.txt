set(srcs
  src/OTA_manager.c
)

set(include_dirs include)

set(
  requires
    app_update
    NVS_manager
)


idf_component_register(
  SRCS ${srcs}
  INCLUDE_DIRS ${include_dirs}
  REQUIRES ${requires}
)