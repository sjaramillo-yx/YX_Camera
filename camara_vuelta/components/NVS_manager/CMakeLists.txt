set(srcs
  src/NVS_manager.c
)

set(include_dirs include)

set(
  requires
    nvs_flash
)


idf_component_register(
  SRCS ${srcs}
  INCLUDE_DIRS ${include_dirs}
  REQUIRES ${requires}
)