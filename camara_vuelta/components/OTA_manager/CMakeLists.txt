set(srcs
  src/OTA_manager.c
)

set(include_dirs include)

set(
  requires
    app_update
    NVS_manager
)

set(
  priv_requires
    OTA_events
)

idf_component_register(
  SRCS ${srcs}
  INCLUDE_DIRS ${include_dirs}
  REQUIRES ${requires}
  PRIV_REQUIRES ${priv_requires}
)