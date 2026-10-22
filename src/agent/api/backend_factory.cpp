/**
 * @file backend_factory.cpp
 * @brief 后端工厂实现
 * @version 2.0.0
 * @date 2026-07
 */

#include "agent/api/backend_factory.h"
#include "agent/api/i_backend.h"
#include "agent/api/remote/remote_backend.h"

namespace agent {

std::unique_ptr<IBackend> BackendFactory::create(const BackendConfig& config,
                                                 IEventBus* event_bus) {
    switch (config.type) {
        case BackendConfig::Type::Remote:
            return std::make_unique<RemoteBackend>(event_bus);
        case BackendConfig::Type::Local:
            // Phase 5: LocalBackend
            return nullptr;
        default:
            return nullptr;
    }
}

}  // namespace agent
