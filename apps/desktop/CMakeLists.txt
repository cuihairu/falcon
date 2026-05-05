cmake_minimum_required(VERSION 3.16)
project(falcon-desktop VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# macOS 特定设置
if(APPLE)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING "Minimum macOS version" FORCE)
  set(CMAKE_MACOSX_BUNDLE TRUE)
  set(CMAKE_MACOSX_BUNDLE_GUI_TYPE "FBND")
  set(MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in)
endif()

# Windows 特定设置
if(WIN32)
  # GUI 应用（无控制台窗口）
  set(WIN32_EXECUTABLE TRUE)
endif()

# 查找 Qt6
find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network Concurrent)
find_package(Qt6 OPTIONAL_COMPONENTS LinguistTools)

# 设置输出目录
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# 源文件
set(SOURCES
    src/main.cpp
    src/main_window.cpp
    src/widgets/top_bar.cpp
    src/widgets/status_bar.cpp
    src/navigation/sidebar.cpp
    src/pages/download_page.cpp
    src/pages/cloud_page.cpp
    src/pages/discovery_page.cpp
    src/pages/settings_page.cpp
    src/dialogs/add_download_dialog.cpp
    src/utils/url_detector.cpp
    src/utils/clipboard_monitor.cpp
    src/utils/theme_manager.cpp
    src/services/storage_service.cpp
    src/services/search_service.cpp
    src/ipc/http_server.cpp
)

# 头文件
set(HEADERS
    src/styles.hpp
    src/main_window.hpp
    src/widgets/top_bar.hpp
    src/widgets/status_bar.hpp
    src/navigation/sidebar.hpp
    src/pages/download_page.hpp
    src/pages/cloud_page.hpp
    src/pages/discovery_page.hpp
    src/pages/settings_page.hpp
    src/dialogs/add_download_dialog.hpp
    src/utils/url_detector.hpp
    src/utils/clipboard_monitor.hpp
    src/utils/theme_manager.hpp
    src/services/storage_service.hpp
    src/services/search_service.hpp
    src/ipc/http_server.hpp
)

# UI 文件（如果使用 Qt Designer）
set(UI_FILES
    # 暂时手动编码，不使用 .ui 文件
)

# 资源文件
set(RESOURCES
    resources.qrc
)

# Windows 资源文件（图标、版本信息）
if(WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/resources/windows/app.rc")
  list(APPEND SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/resources/windows/app.rc")
endif()

# 添加资源到可执行文件
qt_add_resources(falcon-desktop ${RESOURCES})

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
)

# 链接库
target_link_libraries(falcon-desktop PRIVATE
    Qt6::Core
    Qt6::Widgets
    Qt6::Network
    Qt6::Concurrent
    Falcon::core
    Falcon::protocols
    Falcon::storage
)

# 安装目标（macOS 使用手动打包，跳过 install）
if(NOT APPLE)
  install(TARGETS falcon-desktop
      RUNTIME DESTINATION bin
      LIBRARY DESTINATION lib
      ARCHIVE DESTINATION lib
  )

  # Windows: 安装时复制 vcpkg DLL 到输出目录
  if(WIN32)
    # 使用 vcpkg 的 applocal 脚本复制运行时 DLL
    include(GNUInstallDirs)
    install(CODE "
      execute_process(COMMAND \"\${CMAKE_COMMAND}\" -E env \
        \"VCPKG_APPLOCAL_DEPS_INSTALL=1\" \
        \"\${CMAKE_COMMAND}\" --install . --config Debug --component falcon-desktop \
        WORKING_DIRECTORY \${CMAKE_BINARY_DIR})
    " OPTIONAL)

    # 或者直接复制 vcpkg 的 bin 目录内容
    if(DEFINED VCPKG_INSTALLED_DIR)
      install(DIRECTORY \${VCPKG_INSTALLED_DIR}/x64-windows/bin/
          DESTINATION bin
          FILES_MATCHING PATTERN \"*.dll\"
      )
    endif()
  endif()
endif()
