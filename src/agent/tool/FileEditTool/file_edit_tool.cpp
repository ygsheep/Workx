/**
 * @file file_edit_tool.cpp
 * @brief FileEditTool 实现
 * @details 文件编辑工具的具体实现：
 *          - 路径解析（weakly_canonical，相对路径基于 ctx.cwd）
 *          - validate_input 检查（错误码 0-10，覆盖 CC Phase 2 全部 P0/P1/P2/P3）
 *            - 0: secret 扫描（ConfigManager: tool.edit.scan_secrets 开关）
 *            - 1: old/new 相等
 *            - 2: deny 规则（ConfigManager: tool.edit.deny_patterns 配置）
 *            - 3/4/5/6/7/8/9/10: 存在性 / .ipynb / 预读 / staleness / 匹配 / 唯一性 / 大小
 *          - 新建模式：创建父目录 + 写入 + 状态刷新
 *          - 更新模式：Pre-read/Staleness + LF 规范化匹配 + .bak 备份 + 写入 + diff 生成
 *          所有 filesystem 操作使用 std::error_code 重载，避免异常抛出。
 * @author workx
 * @version 1.2.0
 * @date 2026-07
 */

#include "agent/tool/FileEditTool/file_edit_tool.h"
#include "agent/tool/FileWriteTool/diff.h"
#include "agent/tool/FileReadState/file_read_state.h"
#include "agent/tool/path_expand.h"
#include "agent/tool/path_validator.h"
#include "agent/tool/permission_ask.h"
#include "agent/tool/types.h"
#include "agent/tool/path_matcher.h"
#include "agent/tool/secret_scanner.h"
#include "agent/tool/line_endings.h"
#include "agent/tool/quote_normalizer.h"
#include "agent/tool/encoding.h"
#include "agent/tool/file_history.h"
#include "core/config/config_manager.h"
#include "agent/config/app_config.h"

#include <format>
#include <fstream>
#include <iterator>
#include <system_error>
#include <algorithm>
#include <sstream>

namespace agent::tool {

namespace fs = std::filesystem;

namespace {

/// @brief 1 GiB 文件大小上限（对齐 CC constants.ts）
/// @details 防止 OOM；C++ 虽无 V8 字符串长度限制，但为对齐源码行为保留。
constexpr size_t MAX_EDIT_FILE_SIZE_BYTES = 1ull * 1024 * 1024 * 1024;

/// @brief 读取文件并 LF 规范化（保留末尾换行）
/// @details 用于 old_string 匹配与写回内容生成：
///          1. 检测文件编码（UTF-8/UTF-16LE/BE/GBK），解码为 UTF-8
///          2. CRLF (\r\n) → LF (\n)
///          3. 孤立 \r → LF（旧 Mac 风格）
///          4. 保留末尾 \n（写回时保持原文件行尾结构）
///          注意：staleness 对比前需调用 strip_trailing_newline 规整为
///          FileReadStateTracker 存储格式（无末尾 \n）。
/// @param path 文件路径
/// @return LF 规范化后的内容；读取失败返回空字符串
std::string read_file_lf_normalized(const fs::path& path) {
    Encoding encoding = detect_encoding(path);
    std::string utf8_content = read_file_as_utf8(path, encoding);
    return normalize_to_lf(utf8_content);
}

/// @brief 移除末尾单个 \n（对齐 FileReadStateTracker 存储约定）
/// @details FileReadTool 使用 getline 读取，状态中 content 不含末尾 \n。
///          staleness 对比前需对两侧内容统一剥离末尾 \n，避免云同步/杀毒
///          等场景下 mtime 变化但内容实际相同时的误判。
std::string strip_trailing_newline(std::string s) {
    if (!s.empty() && s.back() == '\n') {
        s.pop_back();
    }
    return s;
}

/// @brief 将字符串 LF 规范化并剥离末尾 \n
/// @details 用于写入后刷新 FileReadStateTracker 时规范化内容，
///          保证与 FileReadStateTracker 存储约定一致（无末尾 \n）。
std::string lf_normalize(std::string content) {
    std::string normalized = normalize_to_lf(content);
    if (!normalized.empty() && normalized.back() == '\n') {
        normalized.pop_back();
    }
    return normalized;
}

/// @brief 统计非重叠子串出现次数
/// @param haystack 被搜索的字符串
/// @param needle 待统计的子串（空串返回 0）
/// @return 非重叠匹配次数
size_t count_substring_occurrences(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return 0;
    size_t count = 0;
    size_t pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

/// @brief 替换所有非重叠匹配
/// @param content 原始内容
/// @param from 待替换子串（非空）
/// @param to 替换后子串
/// @return 替换后的内容
std::string replace_all_occurrences(std::string content, const std::string& from,
                                    const std::string& to) {
    if (from.empty()) return content;
    std::string result;
    result.reserve(content.size());
    size_t pos = 0;
    size_t last = 0;
    while ((pos = content.find(from, last)) != std::string::npos) {
        result.append(content, last, pos - last);
        result.append(to);
        last = pos + from.size();
    }
    result.append(content, last, std::string::npos);
    return result;
}

/// @brief 解析换行分隔的 glob 模式列表
/// @details 跳过空行与首尾空白；对每条模式做 ~ 展开（家目录）。
/// @param raw 原始字符串（换行分隔）
/// @return 解析后的模式列表（POSIX 风格）
std::vector<std::string> parse_deny_patterns(const std::string& raw) {
    std::vector<std::string> patterns;
    if (raw.empty()) return patterns;

    std::istringstream stream(raw);
    std::string line;
    while (std::getline(stream, line)) {
        // 去除首尾空白
        auto first = line.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) continue;  // 空行
        auto last = line.find_last_not_of(" \t\r\n");
        std::string trimmed = line.substr(first, last - first + 1);

        // 注释行跳过
        if (trimmed[0] == '#') continue;

        // ~ 展开为家目录
        std::string expanded = expand_home(trimmed);
        // 转 POSIX 风格（\ → /）
        patterns.push_back(to_posix_path(expanded));
    }
    return patterns;
}

}  // anonymous namespace

// ============================================================
// 元数据方法
// ============================================================

const std::string& FileEditTool::name() const {
    static const std::string n{"Edit"};
    return n;
}

const std::string& FileEditTool::description() const {
    static const std::string d{"Performs exact string replacements in files."};
    return d;
}

const std::string& FileEditTool::prompt() const {
    // 对齐 Claude Code prompt.ts 的 7 条 Usage（第 7 条 USER_TYPE=ant 跳过，Phase 4）
    static const std::string p{
        "Performs exact string replacements in files.\n"
        "\n"
        "Usage:\n"
        "- You must use your Read tool at least once in the conversation before editing. "
        "This tool will error if you attempt to edit a file without reading it first.\n"
        "- When editing text from Read tool output, ensure you preserve the exact formatting "
        "(indentation, tabs, newlines) of the original source.\n"
        "- ALWAYS prefer editing existing files in the current working directory. "
        "NEVER create new files unless explicitly required by the user.\n"
        "- Avoid using emojis in files unless explicitly requested by the user.\n"
        "- old_string must be unique within the file unless replace_all is true. "
        "If the string is not unique, the edit will fail. "
        "To make it unique, include more surrounding context in old_string.\n"
        "- Use replace_all to replace all occurrences of old_string with new_string. "
        "This is useful for renaming variables or making sweeping changes across a file.\n"
        "- The file_path parameter should be an absolute path (e.g., /home/user/file.txt or "
        "C:\\Users\\user\\file.txt); "
        "relative paths are resolved against the current working directory."};
    return p;
}

nlohmann::json FileEditTool::input_schema() const {
    // 对齐 Claude Code z.strictObject → additionalProperties: false
    return {
        {"type", "object"},
        {"properties",
         {{"file_path",
           {{"type", "string"}, {"description", "The absolute path to the file to modify"}}},
          {"old_string", {{"type", "string"}, {"description", "The text to replace"}}},
          {"new_string",
           {{"type", "string"},
            {"description", "The text to replace it with (must be different from old_string)"}}},
          {"replace_all",
           {{"type", "boolean"},
            {"description", "Replace all occurrences of old_string (default false)"},
            {"default", false}}}}},
        {"required", {"file_path", "old_string", "new_string"}},
        {"additionalProperties", false}};
}

// ============================================================
// 输入验证
// ============================================================

// ============================================================
// 权限检查
// ============================================================

PermissionResult FileEditTool::check_permissions(const nlohmann::json& input,
                                                 const ToolContext& ctx) const {
    // #36：Bypass 模式完全放行
    if (is_bypass_mode(ctx.permission_mode)) {
        return PermissionResult::ok();
    }
    // #36：Plan 模式禁止编辑文件
    if (deny_write_by_mode(ctx.permission_mode)) {
        return PermissionResult::err(Error::Code::PermissionDenied,
                                     "Edit operations are not allowed in plan mode. "
                                     "Switch to default mode to edit files.");
    }
    // #34：路径边界 + 敏感路径拦截
    const std::string path_str = input.value("file_path", "");
    if (path_str.empty()) return PermissionResult::ok();  // 空路径由 validate_input 拦截
    try {
        const std::string expanded = expand_path(path_str, ctx.cwd);
        auto res = validate_path_access(expanded, ctx.cwd, repo_root_allowlist(ctx.git_repo_root));
        if (res.is_err()) {
            // 评审 #2：绝对禁止（私钥/凭据）不可确认；越界/可确认敏感路径由用户确认放行
            if (!is_absolutely_forbidden_path(expanded) &&
                ask_user_confirm(
                    ctx, std::format("Edit access requires your approval:\n\n```\n{}\n```\n\n"
                                     "Allow editing this path?",
                                     path_str))) {
                return PermissionResult::ok();
            }
            return PermissionResult::err(Error::Code::PermissionDenied, res.error().message);
        }
    } catch (const std::exception& e) {
        return PermissionResult::err(Error::Code::PermissionDenied,
                                     std::string("Edit path error: ") + e.what());
    }
    return PermissionResult::ok();
}

// ============================================================
// 输入验证
// ============================================================

ValidationResult FileEditTool::validate_input(const nlohmann::json& input,
                                              const ToolContext& ctx) const {
    // 字段存在性与类型检查
    if (!input.contains("file_path") || !input["file_path"].is_string()) {
        return ValidationResult::err(Error::Code::MissingArgument,
                                     "Missing required field: file_path");
    }
    if (!input.contains("old_string") || !input["old_string"].is_string()) {
        return ValidationResult::err(Error::Code::MissingArgument,
                                     "Missing required field: old_string");
    }
    if (!input.contains("new_string") || !input["new_string"].is_string()) {
        return ValidationResult::err(Error::Code::MissingArgument,
                                     "Missing required field: new_string");
    }

    const auto& file_path_str = input["file_path"].get<std::string>();
    const auto& old_string = input["old_string"].get<std::string>();
    const auto& new_string = input["new_string"].get<std::string>();

    if (file_path_str.empty()) {
        return ValidationResult::err(Error::Code::InvalidInput, "file_path must not be empty");
    }
    // 相对路径由 expand_path() 基于 ctx.cwd 自动展开为绝对路径
    // （对齐 Claude Code CLI expandPath() 行为，避免弱模型/用户输入相对路径被拒）

    // 路径解析（提前做，用于后续 deny / 存在性检查）
    std::string expanded = expand_path(file_path_str, ctx.cwd);
    fs::path file_path(expanded);
    std::error_code ec;
    auto canonical = fs::weakly_canonical(file_path, ec);
    if (ec) {
        ec.clear();
        canonical = file_path;
    }
    const std::string posix_path = to_posix_path(canonical.generic_string());

    // === 错误码 0: secret 扫描（ConfigManager 开关控制） ===
    // 对齐 CC checkTeamMemSecrets，但 WorkX 没有 team memory 概念，
    // 通过 tool.edit.scan_secrets 开关让用户自行启用（默认关闭以兼容现有行为）
    // D-5：通过 ctx.config_manager() 解析配置管理器，支持 DI 注入
    const bool scan_secrets =
        ctx.config_manager().get_or<bool>(agent::keys::EDIT_SCAN_SECRETS, false);
    if (scan_secrets) {
        std::string secret_error = scan_for_secret_error(new_string);
        if (!secret_error.empty()) {
            return ValidationResult::err(Error::Code::InvalidInput, secret_error);
        }
    }

    // === 错误码 1: old_string === new_string ===
    if (old_string == new_string) {
        return ValidationResult::err(
            Error::Code::InvalidInput,
            "No changes to make: old_string and new_string are exactly the same.");
    }

    // === 错误码 2: deny 规则（ConfigManager 配置） ===
    // 对齐 CC matchingRuleForInput，但 WorkX 简化为单一 deny 列表（无多源合并）
    // D-5：通过 ctx.config_manager() 解析配置管理器，支持 DI 注入
    const std::string deny_raw =
        ctx.config_manager().get_or<std::string>(agent::keys::EDIT_DENY_PATTERNS, std::string{});
    if (!deny_raw.empty()) {
        auto patterns = parse_deny_patterns(deny_raw);
        if (!patterns.empty() && matches_any_pattern(posix_path, patterns)) {
            return ValidationResult::err(
                Error::Code::PermissionDenied,
                "File is in a directory that is denied by your permission settings.");
        }
    }

    // replace_all 类型检查（可选字段，存在则必须是布尔）
    if (input.contains("replace_all") && !input["replace_all"].is_boolean()) {
        return ValidationResult::err(Error::Code::InvalidInput, "replace_all must be a boolean");
    }

    // 错误码 5: .ipynb 后缀拒绝
    if (file_path_str.size() >= 6 &&
        file_path_str.compare(file_path_str.size() - 6, 6, ".ipynb") == 0) {
        return ValidationResult::err(
            Error::Code::InvalidInput,
            "File is a Jupyter Notebook. Use the NotebookEditTool instead.");
    }

    const bool exists = fs::exists(canonical, ec);

    // 错误码 10: 文件 > 1 GiB
    if (exists) {
        auto file_size = fs::file_size(canonical, ec);
        if (!ec && file_size > MAX_EDIT_FILE_SIZE_BYTES) {
            return ValidationResult::err(
                Error::Code::InvalidInput,
                std::format("File is too large to edit (size: {} bytes, max: {} bytes).", file_size,
                            MAX_EDIT_FILE_SIZE_BYTES));
        }
    }

    // 错误码 4: 文件不存在且 old_string != ""
    if (!exists && !old_string.empty()) {
        return ValidationResult::err(
            Error::Code::ResourceNotFound,
            std::format("File does not exist: {}. Use the Write tool to create new files.",
                        file_path_str));
    }

    // 错误码 3: old_string == "" 但文件已存在非空
    if (exists && old_string.empty()) {
        auto file_size = fs::file_size(canonical, ec);
        if (!ec && file_size > 0) {
            return ValidationResult::err(
                Error::Code::InvalidInput,
                "Cannot create new file - file already exists: " + file_path_str);
        }
    }

    // 错误码 6/7: 预读检查 + staleness 检测（仅在文件存在时）
    if (exists) {
        auto check =
            check_pre_read_and_staleness(canonical.generic_string(), canonical, old_string);
        if (check.is_err()) {
            return check.error();
        }
    }

    // 错误码 8/9: 匹配检查 + 唯一性检查（仅在文件存在且 old_string 非空时）
    // 使用 find_actual_string 支持引号规范化匹配（弯引号 ↔ 直引号）
    if (exists && !old_string.empty()) {
        auto content = read_file_lf_normalized(canonical);
        const bool replace_all = input.value("replace_all", false);

        size_t match_count = count_actual_occurrences(content, old_string);

        // 错误码 8: 未匹配（精确匹配 + 引号规范化匹配均失败）
        if (match_count == 0) {
            return ValidationResult::err(
                Error::Code::ResourceNotFound,
                "String to replace not found in file: " + file_path_str +
                    "\n"
                    "Ensure the old_string matches exactly, including whitespace and newlines.");
        }

        // 错误码 9: 多匹配但 replace_all=false
        if (match_count > 1 && !replace_all) {
            return ValidationResult::err(
                Error::Code::InvalidInput,
                std::format(
                    "Found {} matches for old_string in file, but replace_all is false. "
                    "Either provide a more specific old_string with more surrounding context, "
                    "or set replace_all=true to replace all occurrences.",
                    match_count));
        }
    }

    return ValidationResult::ok();
}

// ============================================================
// 私有辅助：Pre-read + Staleness 检查
// ============================================================

ValidationResult FileEditTool::check_pre_read_and_staleness(const std::string& canonical_path,
                                                            const fs::path& file_path,
                                                            const std::string& old_string) {
    // 1. Pre-read 强制检查
    auto state = FileReadStateTracker::instance().get_state(canonical_path);
    if (!state.has_value()) {
        return ValidationResult::err(
            Error::Code::PermissionDenied,
            "File has not been read yet. Read it first before editing it.");
    }

    // 1b. 部分视图：仅当 old_string 起始行落在已读范围内时放行
    //     （支持分段读取后编辑已读区域；未读区域仍拒绝，防止盲改）
    if (state->is_partial_view && !old_string.empty()) {
        const auto current_content = read_file_lf_normalized(file_path);
        const auto pos = current_content.find(old_string);
        if (pos != std::string::npos) {
            // 计算 old_string 的起始行号（1-based）
            int start_line = 1;
            for (size_t i = 0; i < pos; ++i) {
                if (current_content[i] == '\n') ++start_line;
            }
            if (!state->covers_line(start_line)) {
                return ValidationResult::err(
                    Error::Code::PermissionDenied,
                    std::format("File was only partially read (edit target line {} is outside the "
                                "read ranges). "
                                "Read the relevant section before editing it.",
                                start_line));
            }
        }
        // old_string 未找到：放行，由后续匹配检查给出更准确的错误
    }

    // 2. Staleness 检查：mtime 对比
    std::error_code ec;
    const auto current_mtime = fs::last_write_time(file_path, ec);
    if (ec) {
        // 无法获取 mtime（容错放行，让后续写入自然失败）
        return ValidationResult::ok();
    }

    if (current_mtime > state->mtime) {
        // mtime 变化，做内容对比回退（对齐 CC call() 中的 fallback）：
        // Windows 下云同步/杀毒等可能改 mtime 但内容未变，避免误判。
        // 两侧统一剥离末尾 \n 后比较（FileReadStateTracker 约定不含末尾 \n）。
        const auto current_content = read_file_lf_normalized(file_path);
        if (strip_trailing_newline(current_content) != strip_trailing_newline(state->content)) {
            return ValidationResult::err(
                Error::Code::InternalError,
                "File has been modified since read, either by the user or by a linter. "
                "Read it again before attempting to edit it.");
        }
        // 内容相同：放行（不更新 state.mtime，让下次走相同对比路径）
    }

    return ValidationResult::ok();
}

// ============================================================
// 私有辅助：.bak 备份
// ============================================================

ValidationResult FileEditTool::create_backup(const fs::path& file_path) {
    fs::path bak_path = file_path;
    bak_path += ".bak";

    std::error_code ec;
    fs::copy_file(file_path, bak_path, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        return ValidationResult::err(
            Error::Code::InternalError,
            std::format("Failed to create backup '{}': {}", bak_path.string(), ec.message()));
    }
    return ValidationResult::ok();
}

// ============================================================
// 执行管道
// ============================================================

ResultV2<ToolResult> FileEditTool::call(const nlohmann::json& input, const ToolContext& ctx) const {
    // 1. 解析输入（已在 validate_input 中校验，此处安全访问）
    FileEditInput edit_input;
    try {
        edit_input = input.get<FileEditInput>();
    } catch (const nlohmann::json::exception& e) {
        return ResultV2<ToolResult>::err(Error::Code::InvalidInput,
                                         std::format("Input parse failed: {}", e.what()));
    }

    // 2. 路径解析：用 expand_path 展开 ~ 与相对路径（基于 ctx.cwd），再 weakly_canonical 规范化
    std::string expanded = expand_path(edit_input.file_path, ctx.cwd);
    fs::path file_path(expanded);
    std::error_code ec;
    file_path = fs::weakly_canonical(file_path, ec);
    if (ec) {
        ec.clear();
    }
    // conditional skills：上报 touch 文件路径
    ctx.report_touch(file_path.string());

    const std::string canonical_key = file_path.generic_string();

    // 3. 判断新建/更新
    const bool is_update = fs::exists(file_path, ec);

    // 4. 新建模式（old_string 必须为空，validate_input 已检查）
    if (!is_update) {
        // 创建父目录（mkdir -p 语义，幂等）
        const fs::path parent_dir = file_path.parent_path();
        if (!parent_dir.empty()) {
            bool parent_exists = fs::exists(parent_dir, ec);
            if (ec) {
                ec.clear();
                parent_exists = true;
            }
            if (!parent_exists) {
                fs::create_directories(parent_dir, ec);
                if (ec) {
                    return ResultV2<ToolResult>::err(
                        Error::Code::InternalError,
                        std::format("Failed to create directory '{}': {}", parent_dir.string(),
                                    ec.message()));
                }
            }
        }

        // 写入文件（原子写 + 取消点，#23 P2：临时文件 + rename，中断不留半写）
        if (ctx.is_cancelled()) {
            return ResultV2<ToolResult>::err(
                Error::Code::Cancelled,
                std::format("File creation cancelled for: {}", edit_input.file_path));
        }
        if (!write_file_with_encoding(file_path, edit_input.new_string, Encoding::Utf8,
                                      [&ctx]() { return ctx.is_cancelled(); })) {
            // 取消发生在写临时文件之后：报告 Cancelled 而非写入失败
            if (ctx.is_cancelled()) {
                return ResultV2<ToolResult>::err(
                    Error::Code::Cancelled,
                    std::format("File creation cancelled for: {}", edit_input.file_path));
            }
            return ResultV2<ToolResult>::err(
                Error::Code::InternalError,
                std::format("Failed to write file: {}", edit_input.file_path));
        }

        // 刷新 FileReadStateTracker（写后保持状态一致，允许连续编辑）
        std::error_code mtime_ec;
        const auto new_mtime = fs::last_write_time(file_path, mtime_ec);
        FileReadStateTracker::instance().update_after_write(
            canonical_key, lf_normalize(edit_input.new_string),
            mtime_ec ? std::filesystem::file_time_type{} : new_mtime, false);

        return ResultV2<ToolResult>::ok(
            ToolResult::ok(std::format("File created successfully at: {}", file_path.string())));
    }

    // 5. 更新模式
    // 5a. Pre-read + Staleness 检查（validate_input 已检查，这里再查一次以防 call 直接调用）
    auto check = check_pre_read_and_staleness(canonical_key, file_path, edit_input.old_string);
    if (check.is_err()) {
        return check.error();
    }

    // 5b. 检测编码 + 读取 UTF-8 内容 + 检测原文件行尾风格（LF / CRLF / CR）
    //     匹配/替换在 LF 规范化版本上完成，写回时转换为原行尾风格 + 原编码。
    Encoding original_encoding = detect_encoding(file_path);
    std::string utf8_content = read_file_as_utf8(file_path, original_encoding);
    LineEnding original_ending = detect_line_ending(utf8_content);
    std::string old_content = normalize_to_lf(utf8_content);

    // 5c. 匹配与替换（在 LF 规范化版本上操作）
    //     使用 find_actual_string 支持引号规范化匹配（弯引号 ↔ 直引号）
    //     使用 preserve_quote_style 写回时保留原文件引号风格
    std::string new_content;
    if (edit_input.old_string.empty()) {
        // old_string == "" + 文件为空（validate_input 已检查非空时拒绝）
        // 视为写入新内容
        new_content = edit_input.new_string;
    } else {
        // 查找文件中的实际子串（支持引号规范化）
        auto actual_old_opt = find_actual_string(old_content, edit_input.old_string);

        // 错误码 8: 未匹配（精确匹配 + 引号规范化匹配均失败）
        if (!actual_old_opt.has_value()) {
            return ResultV2<ToolResult>::err(
                Error::Code::ResourceNotFound,
                "String to replace not found in file: " + file_path.string() +
                    "\n"
                    "Ensure the old_string matches exactly, including whitespace and newlines.");
        }

        const std::string& actual_old = *actual_old_opt;

        // 统计实际子串的非重叠出现次数
        size_t match_count = count_substring_occurrences(old_content, actual_old);

        // 错误码 9: 多匹配但 replace_all=false
        if (match_count > 1 && !edit_input.replace_all) {
            return ResultV2<ToolResult>::err(
                Error::Code::InvalidInput,
                std::format(
                    "Found {} matches for old_string in file, but replace_all is false. "
                    "Either provide a more specific old_string with more surrounding context, "
                    "or set replace_all=true to replace all occurrences.",
                    match_count));
        }

        // 保留原文件引号风格（若文件使用弯引号，将 new_string 的直引号转换为弯引号）
        std::string actual_new =
            preserve_quote_style(edit_input.old_string, actual_old, edit_input.new_string);

        if (edit_input.replace_all) {
            new_content = replace_all_occurrences(old_content, actual_old, actual_new);
        } else {
            // 单次替换：替换第一个匹配
            const auto pos = old_content.find(actual_old);
            new_content = old_content.substr(0, pos) + actual_new +
                          old_content.substr(pos + actual_old.size());
        }
    }

    // 5d. .bak 备份（安全优先，失败则中止写入）
    auto backup = create_backup(file_path);
    if (backup.is_err()) {
        return backup.error();
    }

    // 5d-bis. 保存版本到 FileHistory（多版本备份，支持 undo/多步回滚）
    //         保存的是 LF 规范化版本（与 FileReadStateTracker 约定一致）
    FileHistory::instance().save_version(canonical_key, old_content, "before_edit");

    // 5e-bis. 取消点（#23 P2）：备份已完成、尚未触碰原文件，可安全中止
    //                 （.bak 保留供人工回滚，原文件未受影响）
    if (ctx.is_cancelled()) {
        return ResultV2<ToolResult>::err(
            Error::Code::Cancelled,
            std::format("File edit cancelled for: {}", edit_input.file_path));
    }

    // 5e. 写入文件（保留原行尾风格 + 原编码 + BOM；原子写 #23 P2：
    //     临时文件 + rename，写入中断/取消不会破坏原文件）
    std::string write_content = apply_line_ending(new_content, original_ending);
    if (!write_file_with_encoding(file_path, write_content, original_encoding,
                                  [&ctx]() { return ctx.is_cancelled(); })) {
        // 取消发生在写临时文件之后：报告 Cancelled 而非写入失败
        if (ctx.is_cancelled()) {
            return ResultV2<ToolResult>::err(
                Error::Code::Cancelled,
                std::format("File edit cancelled for: {}", edit_input.file_path));
        }
        return ResultV2<ToolResult>::err(
            Error::Code::InternalError,
            std::format("Failed to write file: {}", edit_input.file_path));
    }

    // 5f. 刷新 FileReadStateTracker（写后保持状态一致，允许连续编辑）
    //     state 存储的是 LF 规范化并剥离末尾 \n 的内容（与 FileReadTool 约定一致）
    {
        std::error_code mtime_ec;
        const auto new_mtime = fs::last_write_time(file_path, mtime_ec);
        FileReadStateTracker::instance().update_after_write(
            canonical_key, lf_normalize(new_content),
            mtime_ec ? std::filesystem::file_time_type{} : new_mtime, false);
    }

    // 5g. 生成 diff 并返回结果（diff 在 LF 版本上生成，便于阅读）
    auto diff_lines = generate_line_diff(old_content, new_content);
    std::string diff_text = format_diff(file_path.string(), diff_lines);

    std::string result_text =
        std::format("The file {} has been updated successfully.\n", file_path.string());
    if (!diff_text.empty()) {
        result_text += "\n" + diff_text;
    } else {
        result_text += "(no content changes)";
    }
    return ResultV2<ToolResult>::ok(ToolResult::ok(std::move(result_text)));
}

}  // namespace agent::tool
