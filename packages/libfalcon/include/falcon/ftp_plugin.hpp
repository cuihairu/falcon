/**
 * @file ftp_plugin.hpp
 * @brief FTP协议插件头文件
 * @author Falcon Team
 * @date 2025-12-21
 */

#pragma once

#include <falcon/plugin_interface.hpp>
#include <memory>

namespace falcon {

// Forward declaration
class DownloadTask;
class DownloadOptions;
class IEventListener;

// FTP协议处理器接口
class IFTPProtocolHandler : public IProtocolHandler {
public:
    virtual ~IFTPProtocolHandler() = default;

    // FTP特有方法
    virtual void set_passive_mode(bool passive) = 0;
    virtual void set_credentials(const std::string& username, const std::string& password) = 0;
};

} // namespace falcon