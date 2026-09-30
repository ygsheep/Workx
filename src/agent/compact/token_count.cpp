/**
 * @file token_count.cpp
 * @brief Token 估算实现
 * @details 与 claude-code services/tokenEstimation.ts 对齐。
 *          纯启发式估算，无外部 tokenizer 依赖。
 */

#include "agent/compact/token_count.h"

#include <algorithm>
#include <cctype>

namespace agent::compact {

namespace {

/// @brief 不区分大小写比较字符串后缀
[[maybe_unused]] bool ends_with_ci(std::string_view str, std::string_view suffix) {
    if (str.size() < suffix.size()) return false;
    auto off = str.size() - suffix.size();
    return std::equal(suffix.begin(), suffix.end(), str.begin() + off, [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) ==
               std::tolower(static_cast<unsigned char>(b));
    });
}

/// @brief 提取文件扩展名（不含点）
/// @details "foo.json" -> "json", "bar.tar.gz" -> "gz"
std::string_view get_ext(std::string_view filename) {
    auto pos = filename.rfind('.');
    if (pos == std::string_view::npos) return {};
    return filename.substr(pos + 1);
}

}  // anonymous namespace

int32_t bytes_per_token_for_ext(std::string_view ext) {
    if (ext.empty()) return BYTES_PER_TOKEN_DEFAULT;
    // JSON 类内容更紧凑（符号多、关键字重复）
    if (ext == "json" || ext == "jsonl" || ext == "jsonc") {
        return BYTES_PER_TOKEN_JSON;
    }
    return BYTES_PER_TOKEN_DEFAULT;
}

int32_t rough_token_count(std::string_view text, int32_t bytes_per_token) {
    if (text.empty()) return 0;
    if (bytes_per_token <= 0) bytes_per_token = BYTES_PER_TOKEN_DEFAULT;
    // 向上取整
    auto len = static_cast<int64_t>(text.size());
    return static_cast<int32_t>((len + bytes_per_token - 1) / bytes_per_token);
}

int32_t estimate_message_tokens(const ChatMessage& msg) {
    int32_t total = 0;

    // content
    total += rough_token_count(msg.content);

    // reasoning_content（思考过程）
    total += rough_token_count(msg.reasoning_content);

    // tool_call_id / tool_name（Tool 角色）
    total += rough_token_count(msg.tool_call_id);
    total += rough_token_count(msg.tool_name);

    // tool_uses（Assistant 角色）
    for (const auto& tu : msg.tool_uses) {
        total += rough_token_count(tu.id);
        total += rough_token_count(tu.name);
        // 工具输入按 JSON 序列化后估算（JSON 比例）
        std::string input_str = tu.input.dump();
        total += rough_token_count(input_str, BYTES_PER_TOKEN_JSON);
    }

    // 对话结构开销（role 标签、分隔符等，对齐 claude-code 启发式）
    total += 4;

    return total;
}

int32_t estimate_messages_tokens(const std::vector<ChatMessage>& messages) {
    int32_t total = 0;
    for (const auto& msg : messages) {
        total += estimate_message_tokens(msg);
    }
    return total;
}

// 保留未使用的辅助函数以备未来按文件扩展名估算
namespace {
[[maybe_unused]] int32_t estimate_file_tokens(std::string_view filename, std::string_view content) {
    auto ext = get_ext(filename);
    return rough_token_count(content, bytes_per_token_for_ext(ext));
}
}  // anonymous namespace

}  // namespace agent::compact
