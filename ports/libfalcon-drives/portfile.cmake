vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO cuihairu/falcon
    REF 87686dac6b6361791281ffcf95517f98c40fd05d
    SHA512 6ad26158b5f76dbfcbf682c987c7db0f07e1c4357a738c19935deb37b885b5907c747df5c716f1e38ad042bfe9813a694cb3e051e451a1ffbfb0a212b3eae898
    HEAD_REF main
)

# 共享/静态由 VCPKG_LIBRARY_LINKAGE 驱动（todo vcpkg 阶段 2 解锁）

# drives 首版零 feature（候选网盘未实现，设计裁定不给虚假承诺）
vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}/packages/libfalcon-drives"
    OPTIONS
        -DFALCON_BUILD_TESTS=OFF
)

vcpkg_cmake_install()

vcpkg_cmake_config_fixup(PACKAGE_NAME FalconDrives CONFIG_PATH lib/cmake/FalconDrives)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")

file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
