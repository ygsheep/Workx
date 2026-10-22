/**
 * @file provider_preset.cpp
 * @brief Provider 预设注册表实现
 */

#include "agent/model/provider_preset.h"
#include <algorithm>

namespace agent {

/// @brief 内置预设表（中国顶级模型 + 自定义 URL）
/// @note 仅保留中国模型提供商：DeepSeek / GLM / Kimi / Qwen / MiniMax。
///       openai-compatible 是特殊预设：无默认 URL/Model，用户必须提供 --remote。
/// @note F.9：默认模型版本会随厂商发布过时，定期更新并标注日期。
///       用户可通过 config.json 的 backend.model_name 覆盖默认值。
static const ProviderPreset s_presets[] = {
    {
        .name = "deepseek",
        .display_name = "DeepSeek",
        .type = ProviderType::OpenAI,
        .default_url = "https://api.deepseek.com",
        .default_model =
            "deepseek-v4-flash",  // DeepSeek V4-Flash (2026-04-24) 非思考模式，1M context
        .api_path = "/v1/chat/completions",
        .default_context_length = 1000000  // DeepSeek V4 系列 1M
    },
    {
        .name = "glm",
        .display_name = "智谱 GLM",
        .type = ProviderType::OpenAI,
        .default_url = "https://open.bigmodel.cn/api/paas/v4",
        .default_model = "glm-5.2",  // GLM-5.2 (2026-06-13) 旗舰编码模型，1M context
        .api_path = "/chat/completions",
        .default_context_length = 1000000  // GLM-5.2 系列 1M
    },
    {
        .name = "kimi",
        .display_name = "Kimi (Moonshot)",
        .type = ProviderType::OpenAI,
        .default_url = "https://api.moonshot.cn/v1",
        .default_model = "kimi-k3",  // Kimi K3 (2026) 1M context，Agent/编码旗舰
        .api_path = "/chat/completions",
        .default_context_length = 1000000  // Kimi K3 1M
    },
    {
        .name = "qwen",
        .display_name = "通义千问 (Qwen)",
        .type = ProviderType::OpenAI,
        .default_url = "https://dashscope.aliyuncs.com/compatible-mode/v1",
        .default_model = "qwen-plus",  // Qwen Plus 商业版 (阿里云百炼)，128K context
        .api_path = "/chat/completions",
        .default_context_length = 128000  // Qwen Plus 128K
    },
    {
        .name = "minimax",
        .display_name = "MiniMax",
        .type = ProviderType::OpenAI,
        .default_url = "https://api.minimaxi.com/v1",
        .default_model = "MiniMax-M3",  // MiniMax M3 (2026) 1M context，Agent/代码优化
        .api_path = "/chat/completions",
        .default_context_length = 1000000  // MiniMax M3 1M
    },
    {.name = "openai-compatible",
     .display_name = "Custom URL",
     .type = ProviderType::OpenAI,
     .default_url = "",
     .default_model = "",
     .api_path = "/v1/chat/completions"}};

const ProviderPreset* find_preset(std::string_view name) {
    auto it = std::find_if(std::begin(s_presets), std::end(s_presets),
                           [&](const auto& preset) { return preset.name == name; });
    return it != std::end(s_presets) ? &*it : nullptr;
}

std::vector<std::string_view> list_preset_names() {
    std::vector<std::string_view> names;
    names.reserve(std::size(s_presets));
    // 注意：lambda 必须显式返回 string_view，而非 std::string
    // 若返回 std::string（值拷贝临时对象），string_view 会指向临时对象的内部 buffer，
    // 临时对象在 push_back 后析构，string_view 悬空 → 读取到无效内容
    std::transform(std::begin(s_presets), std::end(s_presets), std::back_inserter(names),
                   [](const auto& preset) -> std::string_view { return preset.name; });
    return names;
}

std::optional<std::string> build_preset_url(const ProviderPreset* preset) {
    if (!preset || preset->default_url.empty()) {
        return std::nullopt;
    }

    std::string url = preset->default_url;
    // 去重尾部 /
    while (!url.empty() && url.back() == '/') {
        url.pop_back();
    }

    if (!preset->api_path.empty()) {
        url += preset->api_path;
    }

    return url;
}

}  // namespace agent
