/**
 * @file resume_control.hpp
 * @brief V2 引擎断点续传控制文件（aria2 .aria2 控制文件同思路）
 * @author Falcon Team
 * @date 2026-09-13
 *
 * 下载启动时建立控制文件，记录 URL/总长/ETag/分段计划与各段已落盘
 * 进度；失败/中断后临时文件与控制文件保留，重新添加同一任务时据此
 * 重建分段计划、从各段断点继续。完成后控制文件删除。
 *
 * 格式：行式文本（protocols 库不引 JSON 依赖）：
 *   falcon-resume-v1
 *   url=<原始 URL>
 *   total=<文件总字节数>
 *   etag=<ETag 原文，可为空行值>
 *   last_modified=<Last-Modified 原文，可为空行值>
 *   segments=<段数>
 *   seg=<序号> <段起始偏移> <段长度> <段已落盘字节数>
 */

#pragma once

#include <falcon/types.hpp>

#include <string>
#include <vector>

namespace falcon {

/// 控制文件固定后缀（不随 temp_extension 变化，跨配置变更仍可定位）
inline const char* kResumeControlExtension = ".falcon.ctrl";

/// 单个分段的续传进度
struct ResumeSegment {
    Bytes offset = 0;       ///< 段起始偏移（含）
    Bytes length = 0;       ///< 段长度
    Bytes downloaded = 0;   ///< 已确认落盘的字节数（≤ length）
};

/// 一个下载任务的续传控制状态
struct ResumeControl {
    std::string url;                        ///< 原始下载 URL（恢复时严格匹配）
    Bytes total = 0;                        ///< 文件总字节数（0 = 未知，不可续传）
    std::string etag;                       ///< ETag（If-Range 验证值，可空）
    std::string last_modified;              ///< Last-Modified（If-Range 备选，可空）
    std::vector<ResumeSegment> segments;    ///< 分段计划与进度
};

/// If-Range 请求头取值：ETag 优先，否则 Last-Modified（两者都空返回空串）
std::string resume_if_range_value(const ResumeControl& control);

/// 序列化并原子写入控制文件（先写临时文件再 rename，半截文件不会顶名出现）
/// @return false 写盘失败（调用方按 best-effort 处理，不影响下载本身）
bool save_resume_control(const std::string& path, const ResumeControl& control);

/// 解析控制文件
/// @return false 文件不存在/魔数不符/字段缺失/段计划不恰好覆盖
/// [0, total)（连续、每段 length>0、downloaded ≤ length）
bool load_resume_control(const std::string& path, ResumeControl& control);

/// 删除控制文件（不存在时静默成功）
void remove_resume_control(const std::string& path);

} // namespace falcon
