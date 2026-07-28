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
  # 打包脚本会手动创建 .app，这里禁用自动创建
  # set(CMAKE_MACOSX_BUNDLE TRUE)
  # set(CMAKE_MACOSX_BUNDLE_GUI_TYPE "FBND")
  # set(MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in)

  # Qt6 在 macOS 上可能需要 AGL 框架，但在 macOS 11+ 中已被弃用
  # 设置环境变量禁用 AGL 依赖
  set(ENV{QT_MAC_NO_AGL} "1")
  set(QT_MAC_NO_AGL ON CACHE BOOL "Disable AGL framework dependency on macOS" FORCE)

  # 创建一个虚拟的 AGL 框架目标，避免链接错误
  # AGL 在 macOS 11+ 已被移除，但 Qt6 仍可能引用它
  if(NOT TARGET AGL::AGL)
    add_library(AGL::AGL INTERFACE IMPORTED)
  endif()
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

# macOS: 移除 Qt6 添加的 AGL 框架依赖（在 macOS 11+ 中已弃用/移除）
if(APPLE)
  # 获取当前的链接库列表
  get_target_property(_libs falcon-desktop INTERFACE_LINK_LIBRARIES)
  if(_libs)
    # 移除 AGL 相关的链接
    list(FILTER _libs EXCLUDE REGEX ".*AGL.*")
    set_target_properties(falcon-desktop PROPERTIES INTERFACE_LINK_LIBRARIES "${_libs}")
  endif()

  # 也检查 LINK_LIBRARIES
  get_target_property(_link_libs falcon-desktop LINK_LIBRARIES)
  if(_link_libs)
    list(FILTER _link_libs EXCLUDE REGEX ".*AGL.*")
    set_target_properties(falcon-desktop PROPERTIES LINK_LIBRARIES "${_link_libs}")
  endif()
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
