/**
 * @file path_expand.h
 * @brief 路径展开工具 — 将用户输入路径规范化为绝对路径
 * @details 对齐 Claude Code CLI 的 expandPath() 行为：
 *          - `~` / `~/path` 展开为用户 home 目录
 *          - 绝对路径 normalize 后返回
 *          - 相对路径基于 base_dir（通常为 ctx.cwd）解析为绝对路径
 *          - 安全检查：拒绝含 null 字节的路径
 *
 *          设计意图：prompt/schema 仍建议模型使用绝对路径（引导强模型），
 *          但代码层容忍相对路径（兜底弱模型与用户直接输入），避免
 *          Issue #13 的"强制拒绝相对路径"导致用户输入 main.py 被拒。
 * @author workx
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include <string_view>

namespace agent::tool {

/// @brief 将路径展开为绝对路径
/// @param path      用户输入路径（可能为相对路径、~ 开头、绝对路径）
/// @param base_dir  相对路径解析基准目录（通常为 ctx.cwd）；为空时使用进程 cwd
/// @return 规范化后的绝对路径
/// @throws std::invalid_argument 路径含 null 字节
///
/// @note 不做文件存在性检查（纯字符串/路径操作），调用方负责后续 exists() 等。
///       与 fs::weakly_canonical 的区别：本函数不解析符号链接、不要求路径存在，
///       仅做词法规范化（fs::absolute + fs::normalize 语义）。
std::string expand_path(std::string_view path, std::string_view base_dir = {});

}  // namespace agent::tool
