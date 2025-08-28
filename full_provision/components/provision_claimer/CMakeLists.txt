set(srcs
  src/provision_claimer.c
  src/mqtt_worker.c
)

set(include_dirs include)

set(
  requires
    json
)

set(
  priv_requires
    mqtt
)

set(priv_include_dirs private_include)

idf_component_register(
  SRCS ${srcs}
  INCLUDE_DIRS ${include_dirs}
  PRIV_INCLUDE_DIRS ${priv_include_dirs}
  REQUIRES ${requires}
  PRIV_REQUIRES ${priv_requires}
)