cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_p4_c5_lyrics)

# C5 首次 SDIO 烧录必须先独占 SDMMC Host。ESP-Hosted 2.12.9 默认通过构造函数
# 提前初始化传输层，因此从组件目标中移除该自动入口，改由 app_main 在烧录后
# 显式调用 esp_hosted_init()，这样初始化顺序完全可控。
idf_component_get_property(esp_hosted_lib espressif__esp_hosted COMPONENT_LIB)
get_target_property(esp_hosted_sources ${esp_hosted_lib} SOURCES)
list(FILTER esp_hosted_sources EXCLUDE REGEX "port_esp_hosted_host_init\\.c$")
set_property(TARGET ${esp_hosted_lib} PROPERTY SOURCES "${esp_hosted_sources}")
