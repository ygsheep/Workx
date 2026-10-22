/**
 * @file model_catalog.cpp
 * @brief models.dev 模型目录实现
 */

#include "agent/model/model_catalog.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace agent {

namespace {

/// @brief 转小写
std::string to_lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    });
    return out;
}

/// @brief 不区分大小写的子串包含检查
bool contains_ci(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) return true;
    if (haystack.size() < needle.size()) return false;
    auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
                          [](char a, char b) {
                              return std::tolower(static_cast<unsigned char>(a)) ==
                                     std::tolower(static_cast<unsigned char>(b));
                          });
    return it != haystack.end();
}

/// @brief 官方 provider 白名单（模型厂商直营，context 值最可信）
/// @details 同名模型在聚合/代理 provider 下可能给出缩水的 context
///          （如 venice/scaleway）。官方 provider 的值优先；其余仅补缺。
///          数组顺序即优先级：靠前的 provider 先插入（first-wins）。
///          注意 nlohmann::json 的 object 按 key 字典序迭代，必须显式排序。
const std::vector<std::string>& official_provider_order() {
    static const std::vector<std::string> s = {
        // 国产模型厂商直营（核心优先）
        "deepseek",
        "zhipuai",
        "zai",
        "moonshotai",
        "moonshotai-cn",
        "alibaba",
        "alibaba-cn",
        "minimax",
        "minimax-cn",
        // 国际模型厂商直营
        "anthropic",
        "openai",
        "google",
        "google-vertex",
        "meta",
        "mistral",
        "cohere",
        "xai",
        "amazon-bedrock",
    };
    return s;
}

/// @brief provider 优先级：0=官方（按数组顺序），其余=非官方（字典序）
/// @return 排序键，小者优先处理
std::pair<int, std::string> provider_rank(const std::string& provider_id) {
    const auto& order = official_provider_order();
    std::string key = to_lower(provider_id);
    for (std::size_t i = 0; i < order.size(); ++i) {
        // P2: std::to_string 字典序 bug（"10" < "2"）→ 补零定宽，数组顺序即优先级
        if (key == order[i]) {
            std::string rank = std::to_string(i);
            if (rank.size() < 2) rank.insert(rank.begin(), '0');
            return {0, rank};
        }
    }
    return {1, key};
}

/// @brief 读取整个文件内容
std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // anonymous namespace

ResultV2<ModelCatalog> ModelCatalog::from_api_json(std::string_view json_text) {
    ModelCatalog catalog;

    nlohmann::json root;
    try {
        root = nlohmann::json::parse(json_text);
    } catch (const nlohmann::json::exception& e) {
        return ResultV2<ModelCatalog>::err(
            Error::Code::ConfigParseFailed,
            std::string("models.dev catalog parse failed: ") + e.what());
    }

    if (!root.is_object()) {
        return ResultV2<ModelCatalog>::err(Error::Code::ConfigParseFailed,
                                           "models.dev catalog: root must be a JSON object");
    }

    // 结构：{ providerId: { models: { modelId: { limit: { context, output } } } } }
    // 合并策略：官方 provider 优先（同名模型官方值优先），其余仅补缺（first-wins）。
    // nlohmann::json 的 object 按 key 字典序迭代，故先按 provider_rank 显式排序。
    std::vector<std::string> provider_ids;
    provider_ids.reserve(root.size());
    for (auto it = root.begin(); it != root.end(); ++it) provider_ids.push_back(it.key());
    std::stable_sort(provider_ids.begin(), provider_ids.end(),
                     [](const std::string& a, const std::string& b) {
                         return provider_rank(a) < provider_rank(b);
                     });

    for (const auto& provider_id : provider_ids) {
        const auto& provider = root[provider_id];
        if (!provider.is_object()) continue;
        auto models_it = provider.find("models");
        if (models_it == provider.end() || !models_it->is_object()) continue;

        for (auto mit = models_it->begin(); mit != models_it->end(); ++mit) {
            const std::string& model_id = mit.key();
            const auto& info = mit.value();
            if (!info.is_object()) continue;

            int32_t context = 0;
            int32_t output = 0;
            if (auto limit_it = info.find("limit");
                limit_it != info.end() && limit_it->is_object()) {
                if (auto ctx = limit_it->find("context");
                    ctx != limit_it->end() && ctx->is_number_integer())
                    context = ctx->get<int32_t>();
                if (auto out = limit_it->find("output");
                    out != limit_it->end() && out->is_number_integer())
                    output = out->get<int32_t>();
            }

            if (context > 0) {
                ModelInfo mi;
                mi.context_window = context;
                mi.max_output_tokens = output;
                // emplace：仅插入不存在的 key，保证官方优先 + first-wins
                catalog.m_models.try_emplace(to_lower(model_id), mi);
            }
        }
    }

    return ResultV2<ModelCatalog>::ok(std::move(catalog));
}

ResultV2<ModelCatalog> ModelCatalog::load_cache(const std::filesystem::path& path) {
    std::string content = read_file(path);
    if (content.empty()) {
        return ResultV2<ModelCatalog>::err(Error::Code::ResourceNotFound, "models cache not found",
                                           path.string());
    }

    nlohmann::json root;
    try {
        root = nlohmann::json::parse(content);
    } catch (const nlohmann::json::exception& e) {
        return ResultV2<ModelCatalog>::err(Error::Code::ConfigParseFailed,
                                           std::string("models cache parse failed: ") + e.what(),
                                           path.string());
    }

    if (!root.is_object()) {
        return ResultV2<ModelCatalog>::err(Error::Code::ConfigParseFailed,
                                           "models cache: root must be a JSON object",
                                           path.string());
    }

    ModelCatalog catalog;
    for (auto it = root.begin(); it != root.end(); ++it) {
        const auto& info = it.value();
        if (!info.is_object()) continue;
        ModelInfo mi;
        if (auto ctx = info.find("context"); ctx != info.end() && ctx->is_number_integer())
            mi.context_window = ctx->get<int32_t>();
        if (auto out = info.find("output"); out != info.end() && out->is_number_integer())
            mi.max_output_tokens = out->get<int32_t>();
        if (mi.context_window > 0) {
            catalog.m_models[it.key()] = mi;
        }
    }

    // 防御：若解析结果为空（例如文件是原始 api.json 而非扁平化缓存，
    // 或缓存损坏被部分写入），按失败处理，让后台线程重新拉取生成。
    if (catalog.m_models.empty()) {
        return ResultV2<ModelCatalog>::err(Error::Code::ConfigParseFailed,
                                           "models cache: no entries parsed (unexpected format)",
                                           path.string());
    }

    return ResultV2<ModelCatalog>::ok(std::move(catalog));
}

ResultV2<void> ModelCatalog::save_cache(const std::filesystem::path& path) const {
    // 扁平化：{ model_id: {context, output} }
    nlohmann::json root = nlohmann::json::object();
    for (const auto& [name, info] : m_models) {
        root[name] = {
            {"context", info.context_window},
            {"output", info.max_output_tokens},
        };
    }

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return ResultV2<void>::err(Error::Code::PermissionDenied, "cannot write models cache",
                                   path.string());
    }
    out << root.dump(2);
    out.flush();
    if (!out) {
        return ResultV2<void>::err(Error::Code::InternalError, "failed to write models cache",
                                   path.string());
    }
    return ResultV2<void>::ok();
}

int32_t ModelCatalog::context_window_for(std::string_view model_name) const {
    const ModelInfo* info = find(model_name);
    return info ? info->context_window : 0;
}

int32_t ModelCatalog::max_output_tokens_for(std::string_view model_name) const {
    const ModelInfo* info = find(model_name);
    return info ? info->max_output_tokens : 0;
}

bool ModelCatalog::contains(std::string_view model_name) const {
    return find(model_name) != nullptr;
}

const ModelCatalog::ModelInfo* ModelCatalog::find(std::string_view model_name) const {
    if (model_name.empty() || m_models.empty()) return nullptr;

    // 1. 精确匹配（不区分大小写）
    if (auto it = m_models.find(to_lower(model_name)); it != m_models.end()) {
        return &it->second;
    }

    // 2. 去 provider 前缀匹配：取最后一段 '/' 之后
    //    例如 model_name = "kimi/kimi-k3" → "kimi-k3"
    if (auto slash = model_name.rfind('/'); slash != std::string_view::npos) {
        auto tail = model_name.substr(slash + 1);
        if (auto it = m_models.find(to_lower(tail)); it != m_models.end()) {
            return &it->second;
        }
    }

    // 3. canonical 名本身带 provider 前缀：如索引为 "kimi/kimi-k3"，
    //    查询 "kimi-k3" 时对 canonical 去前缀后精确匹配
    {
        std::string key = to_lower(model_name);
        for (const auto& [canonical, info] : m_models) {
            if (canonical == key) return &info;
            if (auto slash = canonical.rfind('/'); slash != std::string::npos) {
                if (canonical.substr(slash + 1) == key) return &info;
            }
        }
    }

    // 4. 最长子串匹配（如 "deepseek-v4-flash-20260424" 匹配 "deepseek-v4-flash"）
    //    对齐 find_model_capability 的规则：model_name 包含 canonical_name 即可
    const ModelInfo* best = nullptr;
    std::size_t best_len = 0;
    for (const auto& [canonical, info] : m_models) {
        if (canonical.size() > best_len && contains_ci(model_name, canonical)) {
            best = &info;
            best_len = canonical.size();
        }
    }
    return best;
}

}  // namespace agent
