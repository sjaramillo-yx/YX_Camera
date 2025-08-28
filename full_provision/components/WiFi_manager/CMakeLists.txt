# get IDF version for comparison
set(idf_version "${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}")

set(srcs 
  src/WiFi_manager.c)

set(requires
  esp_wifi
  nvs_flash
  wpa_supplicant
)

set(include_dirs include)

idf_component_register(
  SRCS ${srcs}
  INCLUDE_DIRS ${include_dirs}
  REQUIRES  ${requires}
)