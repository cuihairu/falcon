vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO cuihairu/falcon
    REF 87686dac6b6361791281ffcf95517f98c40fd05d
    SHA512 6ad26158b5f76dbfcbf682c987c7db0f07e1c4357a738c19935deb37b885b5907c747df5c716f1e38ad042bfe9813a694cb3e051e451a1ffbfb0a212b3eae898
    HEAD_REF main
)

# 共享/静态由 VCPKG_LIBRARY_LINKAGE 驱动（todo vcpkg 阶段 2 解锁；
# 协议插件恒 STATIC，随 falcon_protocols 一起被吸收，无独立 DSO 面）

set(FALCON_FEATURE_OPTIONS -DFALCON_BUILD_TESTS=OFF)
foreach(_pair
        "http:FALCON_ENABLE_HTTP"
        "ftp:FALCON_ENABLE_FTP"
        "bittorrent:FALCON_ENABLE_BITTORRENT"
        "sftp:FALCON_ENABLE_SFTP"
        "webdav:FALCON_ENABLE_WEBDAV"
        "metalink:FALCON_ENABLE_METALINK"
        "nat:FALCON_ENABLE_NAT")
    string(REPLACE ":" ";" _pair "${_pair}")
    list(GET _pair 0 _feat)
    list(GET _pair 1 _opt)
    if(_feat IN_LIST FEATURES)
        list(APPEND FALCON_FEATURE_OPTIONS "-D${_opt}=ON")
    else()
        list(APPEND FALCON_FEATURE_OPTIONS "-D${_opt}=OFF")
    endif()
endforeach()

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}/libs/protocols"
    OPTIONS
        ${FALCON_FEATURE_OPTIONS}
)

vcpkg_cmake_install()

vcpkg_cmake_config_fixup(PACKAGE_NAME FalconProtocols CONFIG_PATH lib/cmake/FalconProtocols)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")

file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
