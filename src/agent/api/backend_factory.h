/**
 * @file backend_factory.h
 * @brief 后端工厂
 * @version 2.0.0
 * @date 2026-07
 */

#pragma once

#include <memory>
#include "agent/api/backend_types.h"

namespace agent {

class IBackend;
class IEventBus;

/// @brief 后端工厂
/// @details 根据 BackendConfig 创建对应的后端实例
class BackendFactory {
   public:
    /// @brief 创建后端
    /// @param config 后端配置
    /// @param event_bus 事件总线（H-1 DI：透传给 RemoteBackend，nullptr 时不发布状态事件；
    ///                     M-2：移除默认实参，调用方必须显式传入）
    /// @return 后端实例，失败返回 nullptr
    static std::unique_ptr<IBackend> create(const BackendConfig& config, IEventBus* event_bus);
};

}  // namespace agent
