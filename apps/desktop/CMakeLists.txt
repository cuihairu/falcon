cmake_minimum_required(VERSION 3.16)
project(falcon-desktop VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# macOS 特定设置
if(APPLE)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version" FORCE)
endif()

# Windows 特定设置
if(WIN32)
  # GUI 应用（无控制台窗口）
  set(WIN32_EXECUTABLE TRUE)
endif()

# 修复 vcpkg freetype BZip2 依赖问题
# freetype 引用了 BZip2::BZip2 但 vcpkg bzip2 没有提供该目标
# 必须在 find_package(Qt6) 之前设置，因为 Qt6 依赖 harfbuzz -> freetype -> bzip2
if(NOT TARGET BZip2::BZip2)
  if(EXISTS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib/libbz2.a")
    # 手动创建 BZip2::BZip2 目标
    add_library(BZip2::BZip2 STATIC IMPORTED GLOBAL)
    set_target_properties(BZip2::BZip2 PROPERTIES
      IMPORTED_LOCATION "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib/libbz2.a"
      INTERFACE_INCLUDE_DIRECTORIES "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include"
    )
  endif()
endif()

# 查找 Qt6(Svg 为 REQUIRED:内嵌 SVG 图标系统经 QSvgRenderer 渲染,
# 缺依赖在 configure 期报错好过运行时图标全空)
find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network Concurrent Svg)
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
    src/utils/icon_utils.cpp
    src/services/storage_service.cpp
    src/services/search_service.cpp
    src/services/download_backend.cpp
    src/services/download_service.cpp
    src/ipc/http_server.cpp
)

# 头文件
set(HEADERS
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
    src/utils/theme_tokens.hpp
    src/utils/icon_utils.hpp
    src/services/storage_service.hpp
    src/services/search_service.hpp
    src/services/download_backend.hpp
    src/services/download_service.hpp
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

# 创建可执行文件(resources.qrc 进 sources 由 AUTORCC 编入——资源必须在
# add_executable 之后再挂:此前 qt_add_resources 在此之前调用是无效调用,
# 资源从未进过二进制,运行时 QSS/图标读取全部落空)
add_executable(falcon-desktop ${SOURCES} ${HEADERS} ${UI_FILES} ${RESOURCES})

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
    Qt6::Svg
    Falcon::core
    Falcon::protocols
    Falcon::storage
)

# daemon RPC 后端（JSON-RPC 客户端 + aria2 快照转换）。
# 快照服务层的基础类型（TaskSnapshot 等）定义在该库中，为硬依赖。
if(NOT TARGET falcon_daemon_rpc_client)
    message(FATAL_ERROR
        "falcon-desktop requires falcon_daemon_rpc_client (needs nlohmann_json + CURL). "
        "Keep FALCON_BUILD_DAEMON=ON or install the missing dependencies.")
endif()
target_link_libraries(falcon-desktop PRIVATE falcon_daemon_rpc_client)

# 后端层测试（纯 C++，无需 Qt；见 tests/CMakeLists.txt）
if(FALCON_BUILD_TESTS)
    add_subdirectory(tests)
endif()

# 离屏 UI 截图沙盒（默认 OFF，见 dev/ui_sandbox.cpp）：无显示环境下
# QT_QPA_PLATFORM=offscreen 渲染各页面并保存 png，供设计验收/视觉回归
option(FALCON_BUILD_UI_SANDBOX "Build offscreen UI screenshot sandbox" OFF)
if(FALCON_BUILD_UI_SANDBOX)
  set(SANDBOX_SOURCES ${SOURCES})
  list(REMOVE_ITEM SANDBOX_SOURCES src/main.cpp)
  # resources.qrc 进 sources 交给 AUTORCC(与主程序同源样式/图标;
  # qrc 已不引用构建产物 .qm,无 translations 构建顺序耦合)
  add_executable(falcon-ui-sandbox
      ${SANDBOX_SOURCES} ${HEADERS} dev/ui_sandbox.cpp ${RESOURCES})
  target_include_directories(falcon-ui-sandbox PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(falcon-ui-sandbox PRIVATE
      Qt6::Core
      Qt6::Widgets
      Qt6::Network
      Qt6::Concurrent
      Qt6::Svg
      Falcon::core
      Falcon::protocols
      Falcon::storage
      falcon_daemon_rpc_client
  )
endif()

# 安装目标（macOS 使用手动打包，跳过 install）
if(NOT APPLE)
  install(TARGETS falcon-desktop
      RUNTIME DESTINATION bin
      LIBRARY DESTINATION lib
      ARCHIVE DESTINATION lib
  )

  # Windows: 自动复制依赖 DLL
  if(WIN32)
    include(GNUInstallDirs)

    # 使用 windeployqt 自动复制 Qt DLL 和依赖
    find_program(WINDEPLOYQT_EXECUTABLE windeployqt HINTS ${Qt6_DIR}/../../../bin)

    if(WINDEPLOYQT_EXECUTABLE)
      # 构建后自动运行 windeployqt
      add_custom_command(TARGET falcon-desktop POST_BUILD
        COMMAND ${WINDEPLOYQT_EXECUTABLE}
          --no-translations
          --no-opengl-sw
          --no-system-d3d-compiler
          --dir $<TARGET_FILE_DIR:falcon-desktop>
          $<TARGET_FILE:falcon-desktop>
        COMMENT "Running windeployqt to copy Qt DLLs..."
      )
    else()
      message(WARNING "windeployqt not found. Qt DLLs will not be copied automatically.")
    endif()

    # 复制 vcpkg 的运行时 DLL
    if(DEFINED VCPKG_INSTALLED_DIR)
      # 复制 vcpkg bin 目录的 DLL 到构建输出目录
      add_custom_command(TARGET falcon-desktop POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin/*.dll
          $<TARGET_FILE_DIR:falcon-desktop> 2>nul || cd .
        COMMENT "Copying vcpkg runtime DLLs..."
      )

      # 安装时也复制 vcpkg DLL
      install(DIRECTORY ${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin/
        DESTINATION bin
        FILES_MATCHING PATTERN "*.dll"
      )
    endif()
  endif()
endif()
