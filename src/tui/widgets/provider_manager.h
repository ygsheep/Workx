/**
 * @file provider_manager.h
 * @brief 供应商管理面板（两层：配置列表 + 字段表单）
 * @details 取代原 provider 切换面板（SearchPalette）：
 *          - 列表层：展示可用供应商，● 标记当前使用中；底部操作按钮行
 *            「设为使用中 / 编辑 / 添加 / 删除」，支持点击与 a/e/d 快捷键；
 *            Enter/空格/Tab 设为使用中。
 *          - 表单层：5 字段（名称 / Base URL / 模型 ID / 上下文窗口 / API Key），
 *            名称框带内置预设 + Custom URL 建议列表；模型 ID 输入时自动匹配
 *            上下文窗口（ModelCatalog → 静态能力表）。
 *          - 删除二次确认；Esc 可放弃未保存的表单编辑。
 * @date 2026-08
 */

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <ftxui/component/component.hpp>

#include "agent/model/model_catalog.h"
#include "agent/model/provider_config.h"

namespace ftxtui {

/// @brief 供应商管理面板参数
struct ProviderManagerOptions {
    std::vector<agent::ProviderConfigEntry>&
        providers;           ///< 面板持有的供应商列表（引用，改动即时生效）
    std::string& active_id;  ///< 当前使用中供应商 id（行首 ● 标记）
    std::shared_ptr<const agent::ModelCatalog>
        catalog;  ///< models.dev 目录（模型→上下文自动填充，可空）
    std::function<void(int)> on_activate;  ///< 设为使用中（参数为 providers 下标，面板会先关闭）
    std::function<void()> on_commit;  ///< 列表变更后持久化（app 写 backend.providers）
    std::function<void()> on_close;   ///< 关闭回调（恢复焦点等，可空）
    std::string title;                ///< 面板标题（如「供应商管理」）
};

/// @brief 构建供应商管理面板（打开后自动聚焦；esc/关闭置 open=false）
ftxui::Component make_provider_manager(ProviderManagerOptions&& opts, bool& open);

}  // namespace ftxtui