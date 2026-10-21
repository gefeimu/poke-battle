# poke-battle 固件构建（ESP-IDF / ESP32-C3）
#
# PC 侧仍走 Makefile（gcc + 显式源文件列表）；这里只是把同一份源码
# 交给 ESP-IDF 的组件系统，两条路径共用 src/ 与 gen/，没有第二份实现。
#
# 构建：idf.py build    烧录：idf.py -p <PORT> flash monitor
cmake_minimum_required(VERSION 3.16)

# src/ 与 gen/ 各自是一个组件（目录里的 CMakeLists.txt 就是组件定义）。
# 不把它们放进 components/：gen/ 全是 tools/ 生成的数据，src/ 是与
# 平台无关的游戏逻辑，混进 components/ 反而看不出边界。
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/src"
    "${CMAKE_CURRENT_LIST_DIR}/gen")

# 本游戏不联网、也不使用蓝牙/802.15.4，但本机拿不到 components/esp_wifi/lib
# 这个子模块（见 README 的固件构建说明）。把整条无线电依赖链一起排除：
#   esp_wifi ← esp_phy ← ieee802154 / bt ← wifi_provisioning / protocomm / esp_hid
#   esp_coex ← openthread
# 显示、ADC、FreeRTOS 不受影响。
set(EXCLUDE_COMPONENTS
    esp_wifi esp_phy esp_coex bt
    ieee802154 esp_hal_ieee802154
    wifi_provisioning wpa_supplicant
    esp_hid protocomm esp_local_ctrl openthread)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# 本机 IDF 5.5.3 的 idf_build_process() 只用 __COMPONENTS 做裁剪，却没有把
# COMPONENTS 参数（_COMPONENTS）拷进去。这里直接给出起始组件，其余由 IDF 按
# REQUIRES 自动展开 —— 与官方 COMPONENTS 的语义相同。
set(__COMPONENTS "main;bsp;src;gen")
project(poke_battle)
