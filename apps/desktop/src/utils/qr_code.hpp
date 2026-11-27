/**
 * @file qr_code.hpp
 * @brief 轻量 QR Code 生成器（字节模式 / 纠错级别 M / 版本 1-3，单块）
 *
 * 自包含实现，不依赖第三方库。码串较短（6 位展示码）时版本 1 即可容纳，
 * 生成的图像是标准 QR Code Model 2，可被手机相机正常扫描。
 */
#pragma once

#include <QImage>
#include <QString>

namespace falcon::desktop {

/**
 * 生成 QR 码图像。
 * @param text    编码内容（UTF-8 字节）
 * @param border  静默区宽度（模块数，默认 4，QR 规范要求至少 4）
 * @param scale   单模块像素数（默认 6）
 * @return 黑白图像；内容过长（超过版本 3-M 容量）返回空 QImage
 */
QImage make_qr_image(const QString& text, int border = 4, int scale = 6);

} // namespace falcon::desktop
