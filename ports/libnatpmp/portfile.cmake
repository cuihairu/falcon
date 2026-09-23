vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO miniupnp/libnatpmp
    REF 134fc89e2781e154e40042641f4d8bcbe42579f1
    SHA512 4021646ef21cd2501e92c71f261d9a5859bafb59ebb6728e372cf69383792acb0839133beff10d638d9447efeec49d87b5f39d1f5fe7ee371f1f140b3e322969
    HEAD_REF master
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DBUILD_SHARED_LIBS=OFF
)

vcpkg_cmake_install()

# 可执行工具（natpmpc / testgetgateway）非库消费面，清出 bin 目录
vcpkg_clean_executables_in_bin(FILE_NAMES natpmpc testgetgateway)

vcpkg_fixup_pkgconfig()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
