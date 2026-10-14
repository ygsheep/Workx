/**
 * @file env_setup_hint.h
 * @brief 环境自建提示（Issue #176 关联）
 * @details 评测题目镜像常常「刻意极简」（如裸 `ubuntu:24.04`），依赖要靠 agent
 *          自己 `apt-get` / `pip` 装 —— 官方参考解也是这么做的。实测这会把
 *          **大量墙钟预算**花在「搭环境」而不是「解题」上（caffe 题有 98% 的
 *          工具耗时是装/编依赖；adaptive 题 100%）。
 *
 *          本模块把「环境里缺哪些常用工具」显式告诉模型，并给出搭环境策略：
 *          - 缺什么就装什么，只装任务真正需要的，别一把梭
 *          - 安装/构建走**后台任务**（`run_in_background=true`），与后续工作重叠
 *          - 批量成一条非交互命令，别拆成很多条小命令逐个等
 *          - 盯住剩余时间，别让搭环境吃掉全部预算
 *
 * @note 全部为纯函数：不读配置、不碰时钟、不起子进程，便于直接单测。
 *       工具存在性由调用方（`probe_common_tools()`，只查 PATH 不查版本）传入。
 * @version 0.1.0
 * @date 2026-10
 */

#pragma once

#include <string>
#include <vector>

#include "agent/prompt/environment_probe.h"

namespace agent::prompt {

/// @brief 环境自建提示中最多列出的缺失工具数（避免刷屏）
inline constexpr std::size_t kEnvSetupMaxListed = 12;

/// @brief 生成「环境自建」提示
/// @param common_tools 常用工具的探测结果（见 probe_common_tools；只用到 found 字段）
/// @return 提示文本；**没有任何缺失工具时返回空串**（调用方据此跳过注入，
///         避免给模型塞一段无意义的空标题）
std::string format_env_setup_hint(const std::vector<ToolchainInfo>& common_tools);

}  // namespace agent::prompt
