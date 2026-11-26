vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO cuihairu/falcon
    REF 87686dac6b6361791281ffcf95517f98c40fd05d
    SHA512 6ad26158b5f76dbfcbf682c987c7db0f07e1c4357a738c19935deb37b885b5907c747df5c716f1e38ad042bfe9813a694cb3e051e451a1ffbfb0a212b3eae898
    HEAD_REF main
)

# 共享/静态由 VCPKG_LIBRARY_LINKAGE 驱动（toolchain 注入
# BUILD_SHARED_LIBS）；共享形态经 WINDOWS_EXPORT_ALL_SYMBOLS 导出符号
# （todo vcpkg 阶段 2 已接线，monorepo 内外行为一致）

# core 自足块（阶段 1）在独立 configure 下自行完成依赖查找，无需注入项
vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}/libs/libfalcon-core"
)

vcpkg_cmake_install()

vcpkg_cmake_config_fixup(PACKAGE_NAME FalconCore CONFIG_PATH lib/cmake/FalconCore)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")

file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
