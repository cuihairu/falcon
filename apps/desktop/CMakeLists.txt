cmake_minimum_required(VERSION 3.16)
project(falcon-desktop VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# 查找 Qt6
find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network)
find_package(Qt6 OPTIONAL_COMPONENTS LinguistTools)

# 设置输出目录
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# 源文件
set(SOURCES
    src/main.cpp
    src/main_window.cpp
    src/navigation/sidebar.cpp
    src/pages/download_page.cpp
    src/pages/cloud_page.cpp
    src/pages/discovery_page.cpp
    src/pages/settings_page.cpp
    src/dialogs/add_download_dialog.cpp
    src/utils/url_detector.cpp
    src/utils/clipboard_monitor.cpp
    src/ipc/http_server.cpp
)

# 头文件
set(HEADERS
    src/styles.hpp
    src/main_window.hpp
    src/navigation/sidebar.hpp
    src/pages/download_page.hpp
    src/pages/cloud_page.hpp
    src/pages/discovery_page.hpp
    src/pages/settings_page.hpp
    src/dialogs/add_download_dialog.hpp
    src/utils/url_detector.hpp
    src/utils/clipboard_monitor.hpp
    src/ipc/http_server.hpp
)

# UI 文件（如果使用 Qt Designer）
set(UI_FILES
    # 暂时手动编码，不使用 .ui 文件
)

# 创建可执行文件
add_executable(falcon-desktop ${SOURCES} ${HEADERS} ${UI_FILES})

# Translations (optional; requires Qt Linguist Tools)
set(TRANSLATION_TS_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/i18n/falcon_desktop_zh_CN.ts
)
if (Qt6LinguistTools_FOUND)
    qt_add_translations(falcon-desktop TS_FILES ${TRANSLATION_TS_FILES})
endif()

# 包含目录
target_include_directories(falcon-desktop PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/packages/libfalcon/include
)

# 链接库
target_link_libraries(falcon-desktop PRIVATE
    Qt6::Core
    Qt6::Widgets
    Qt6::Network
    falcon
)

# 安装目标
install(TARGETS falcon-desktop
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/falcon_desktop_zh_CN.qm
    DESTINATION bin/i18n
    OPTIONAL
)
