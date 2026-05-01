/**
 * @file private_protocols_demo.cpp
 * @brief 演示如何使用私有下载协议
 * @author Falcon Team
 * @date 2025-12-21
 */

#include <falcon/falcon.hpp>
#include <iostream>

int main() {
    try {
        // 创建下载引擎（插件会自动加载）
        falcon::DownloadEngine engine;

        std::cout << "支持的协议：\n";
        auto protocols = engine.get_supported_protocols();
        for (const auto& protocol : protocols) {
            std::cout << "  - " << protocol << "\n";
        }

        // 配置下载选项
        falcon::DownloadOptions options;
        options.max_connections = 5;
        options.timeout_seconds = 30;
        options.output_path = "./downloads";

        // 示例1：迅雷链接
        std::cout << "\n=== 迅雷链接示例 ===\n";
        std::string thunderUrl = "thunder://QUFodHRwOi8vZXhhbXBsZS5jb20vZmlsZS56aXAuWg==";
        if (engine.is_url_supported(thunderUrl)) {
            std::cout << "支持迅雷链接，开始下载...\n";
            auto task1 = engine.add_task(thunderUrl, options);
            // task1->wait(); // 等待下载完成
        } else {
            std::cout << "不支持该迅雷链接\n";
        }

        // 示例2：QQ旋风链接
        std::cout << "\n=== QQ旋风链接示例 ===\n";
        std::string qqUrl = "qqlink://aHR0cDovL2V4YW1wbGUuY29tL3ZpZGVvLm1wNA==";
        if (engine.is_url_supported(qqUrl)) {
            std::cout << "支持QQ旋风链接，开始下载...\n";
            auto task2 = engine.add_task(qqUrl, options);
            // task2->wait();
        } else {
            std::cout << "不支持该QQ旋风链接\n";
        }

        // 示例3：快车链接
        std::cout << "\n=== 快车链接示例 ===\n";
        std::string flashgetUrl = "flashget://W10=";
        if (engine.is_url_supported(flashgetUrl)) {
            std::cout << "支持快车链接，开始下载...\n";
            auto task3 = engine.add_task(flashgetUrl, options);
            // task3->wait();
        } else {
            std::cout << "不支持该快车链接\n";
        }

        // 示例4：ED2K链接
        std::cout << "\n=== ED2K链接示例 ===\n";
        std::string ed2kUrl = "ed2k://|file|example.zip|1048576|A1B2C3D4E5F67890|/";
        if (engine.is_url_supported(ed2kUrl)) {
            std::cout << "支持ED2K链接，开始下载...\n";
            auto task4 = engine.add_task(ed2kUrl, options);
            // task4->wait();
        } else {
            std::cout << "不支持该ED2K链接\n";
        }

        // 示例5：HLS流媒体
        std::cout << "\n=== HLS流媒体示例 ===\n";
        std::string hlsUrl = "https://example.com/playlist.m3u8";
        if (engine.is_url_supported(hlsUrl)) {
            std::cout << "支持HLS流媒体，开始下载...\n";
            auto task5 = engine.add_task(hlsUrl, options);
            // task5->wait();
        } else {
            std::cout << "不支持该HLS流媒体\n";
        }

        // 示例6：DASH流媒体
        std::cout << "\n=== DASH流媒体示例 ===\n";
        std::string dashUrl = "https://example.com/manifest.mpd";
        if (engine.is_url_supported(dashUrl)) {
            std::cout << "支持DASH流媒体，开始下载...\n";
            auto task6 = engine.add_task(dashUrl, options);
            // task6->wait();
        } else {
            std::cout << "不支持该DASH流媒体\n";
        }

        std::cout << "\n所有示例演示完成！\n";

    } catch (const std::exception& e) {
        std::cerr << "错误: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
