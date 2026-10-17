vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO cuihairu/falcon
    REF 5ba5843b7fdcbe9442f27add21f39eccff2d23e0
    SHA512 c8a588ed817bdcff33098ea00b1ece959f3820b1256a66a0b98e024df6d0c7256cc618aad13fada4a80b49493e480219d95862f5da8088f0f8f581126b084ab9
    HEAD_REF main
)

# Windows DLL 路径未接线：FALCON_EXPORTS/FALCON_IMPORTS 宏无任何头文件
# 消费点（零 dllexport 标注），共享构建产出零导出符号的坏 DLL；monorepo
# 亦默认静态（根未开 BUILD_SHARED_LIBS）——端口按 vcpkg 惯例锁静态
vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

# core 自足块（阶段 1）在独立 configure 下自行完成依赖查找，无需注入项
vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}/packages/libfalcon-core"
)

vcpkg_cmake_install()

vcpkg_cmake_config_fixup(PACKAGE_NAME FalconCore CONFIG_PATH lib/cmake/FalconCore)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")

file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
