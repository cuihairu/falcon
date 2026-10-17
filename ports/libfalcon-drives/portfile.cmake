vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO cuihairu/falcon
    REF 5ba5843b7fdcbe9442f27add21f39eccff2d23e0
    SHA512 c8a588ed817bdcff33098ea00b1ece959f3820b1256a66a0b98e024df6d0c7256cc618aad13fada4a80b49493e480219d95862f5da8088f0f8f581126b084ab9
    HEAD_REF main
)

# 同 libfalcon-core：共享构建产出零导出符号 DLL，端口锁静态
vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

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
