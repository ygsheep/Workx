/**
 * @file image_view.h
 * @brief 图片预览：/view 及项目树点击图片时，stb_image 解码 + 半块 truecolor 渲染
 * @details 半块字符（▀）每个终端单元格显示 2 个像素（上=前景色、下=背景色），
 *          保持宽高比缩放到可用区域，超高时垂直滚动。
 */

#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <ftxui/dom/elements.hpp>

#include "vm/view_model.h"

namespace ftxtui {

/// @brief 是否为可预览的图片文件（按扩展名：png/jpg/jpeg/bmp/gif/webp/tga）
bool is_image_file(const std::string& path);

/// @brief 读取并解码图片（stb_image；加载时下采样，最大边 1024 像素控制内存）
/// @param path 图片绝对路径
/// @param[out] err 失败原因（可空，失败时写入）
/// @return 解码数据；失败返回 nullptr
std::shared_ptr<ImageData> decode_image_file(const std::string& path, std::string* err);

/// @brief 图片在 (avail_width × avail_height) 视口内完整显示所需的终端行数
/// @details 保持宽高比、不放大；返回半块渲染下的行数（= ceil(像素高 / 2)）
int image_view_rows(const ImageData& img, int avail_width, int avail_height);

/// @brief 构建图片内容元素（不含路径栏/状态栏；内部处理缩放与滚动偏移）
/// @param scroll 垂直滚动偏移（终端行，0=顶部）；调用方按 image_view_rows 钳制
ftxui::Element build_image_content(const ImageData& img, int avail_width, int avail_height,
                                   int scroll);

}  // namespace ftxtui
