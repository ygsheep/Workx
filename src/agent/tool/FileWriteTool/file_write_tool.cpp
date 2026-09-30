/**
 * @file file_write_tool.cpp
 * @brief FileWriteTool 实现
 * @details 文件写入工具的具体实现：
 *          - 路径解析（weakly_canonical，相对路径基于 ctx.cwd）
 *          - 自动创建父目录（create_directories，mkdir -p 语义）
 *          - 判断 create/update（fs::exists）
 *          - Pre-read 强制检查 + Staleness 检测（update 模式，对齐 CC validateInput）
 *          - .bak 备份（update 模式，安全优先）
 *          - 读取旧内容（update 模式，用于 diff）
 *          - 写入文件（std::ofstream binary，不重写行尾）
 *          - 刷新 FileReadStateTracker（写后保持状态一致）
 *          - 生成行级 diff（LCS 算法，update 模式）
 *          所有 filesystem 操作使用 std::error_code 重载，避免异常抛出。
 * @author workx
 * @version 2.1.0
 * @date 2026-07
 */

#include "agent/tool/FileWriteTool/file_write_tool.h"
#include "agent/tool/FileWriteTool/diff.h"
#include "agent/tool/FileReadState/file_read_state.h"
#include "agent/tool/path_expand.h"
#include "agent/tool/path_validator.h"
#include "agent/tool/permission_ask.h"
#include "agent/tool/secret_scanner.h"
#include "agent/tool/types.h"

#include <format>
#include <fstream>
#include <iterator>
#include <sstream>
#include <system_error>
#include <algorithm>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#include <fcntl.h>
#endif

namespace agent::tool {

namespace fs = std::filesystem;

namespace {

/// @brief 调用 fsync/_commit 把文件数据刷到磁盘
/// @details 用于原子写入前的持久化保证。失败时通过 ec 返回（非致命）。
void do_sync(const fs::path& path, std::error_code& ec) {
    ec.clear();
#ifdef _WIN32
    FILE* f = _wfopen(path.c_str(), L"rb+");
#else
    FILE* f = std::fopen(path.c_str(), "rb+");
#endif
    if (!f) {
        ec = std::error_code(errno, std::system_category());
        return;
    }
    int fd = fileno(f);
#ifdef _WIN32
    if (_commit(fd) != 0) {
        ec = std::error_code(errno, std::system_category());
    }
#else
    if (::fsync(fd) != 0) {
        ec = std::error_code(errno, std::system_category());
    }
#endif
    std::fclose(f);
}

/// @brief 将字符串 LF 规范化
/// @details 用于 staleness 内容对比与 FileReadState 状态刷新：
///          1. CRLF (\r\n) → LF (\n)
///          2. 孤立 \r → LF（旧 Mac 风格）
///          3. 移除末尾单个 \n（对齐 FileReadTool 的 getline 行为：
///             std::getline 剥离 \n 分隔符，最后一行无 \n 时也直接返回）
/// @param content 待规范化的内容（按值传递，避免拷贝）
/// @return LF 规范化后的内容
std::string lf_normalize(std::string content) {
    std::string normalized;
    normalized.reserve(content.size());
    for (size_t i = 0; i < content.size(); ++i) {
        if (content[i] == '\r') {
            normalized += '\n';
            if (i + 1 < content.size() && content[i + 1] == '\n') {
                ++i;
            }
        } else {
            normalized += content[i];
        }
    }
    if (!normalized.empty() && normalized.back() == '\n') {
        normalized.pop_back();
    }
    return normalized;
}

/// @brief 读取文件并 LF 规范化
/// @details 复用 lf_normalize 的规范化逻辑，保证读取与写入路径行为一致。
/// @param path 文件路径
/// @return LF 规范化后的内容；读取失败返回空字符串
std::string read_file_lf_normalized(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return {};

    // 花括号初始化避免 most vexing parse（圆括号会被解析为函数声明）
    std::string raw{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};

    return lf_normalize(std::move(raw));
}

}  // anonymous namespace

// ============================================================
// 元数据方法
// ============================================================

const std::string& FileWriteTool::name() const {
    static const std::string n{"Write"};
    return n;
}

const std::string& FileWriteTool::description() const {
    // 对齐 Claude Code DESCRIPTION
    static const std::string d{"Write a file to the local filesystem."};
    return d;
}

const std::string& FileWriteTool::prompt() const {
    // 对齐 Claude Code getWriteToolDescription() + getPreReadInstruction()
    // issue #13: 明确绝对路径要求并提供示例，避免弱模型反复纠结
    static const std::string p{
        "Writes a file to the local filesystem.\n"
        "\n"
        "Usage:\n"
        "- This tool will overwrite the existing file if there is one at the provided path.\n"
        "- If this is an existing file, you MUST use the Read tool first to read the file's "
        "contents. "
        "This tool will fail if you did not read the file first.\n"
        "- Prefer the Edit tool for modifying existing files \xe2\x80\x94 it only sends the diff. "
        "Only use this tool to create new files or for complete rewrites.\n"
        "- NEVER create documentation files (*.md) or README files unless explicitly requested by "
        "the User.\n"
        "- Only use emojis if the user explicitly requests it. Avoid writing emojis to files "
        "unless asked.\n"
        "- The file_path parameter should be an absolute path (e.g., /home/user/file.txt or "
        "C:\\Users\\user\\file.txt); "
        "relative paths are resolved against the current working directory."};
    return p;
}

nlohmann::json FileWriteTool::input_schema() const {
    // 对齐 Claude Code z.strictObject → additionalProperties: false
    return {
        {"type", "object"},
        {"properties",
         {{"file_path",
           {{"type", "string"}, {"description", "The absolute path to the file to write"}}},
          {"content", {{"type", "string"}, {"description", "The content to write to the file"}}}}},
        {"required", {"file_path", "content"}},
        {"additionalProperties", false}};
}

// ============================================================
// 输入验证
// ============================================================

ValidationResult FileWriteTool::validate_input(const nlohmann::json& input,
                                               const ToolContext& /*ctx*/
) const {
    // file_path 校验
    if (!input.contains("file_path") || !input["file_path"].is_string()) {
        return ValidationResult::err(Error::Code::MissingArgument,
                                     "Missing required field: file_path");
    }
    const std::string path_str = input["file_path"].get<std::string>();
    if (path_str.empty()) {
        return ValidationResult::err(Error::Code::InvalidInput, "file_path must not be empty");
    }
    // 相对路径在 call() 中由 expand_path() 基于 ctx.cwd 自动展开为绝对路径
    // （对齐 Claude Code CLI expandPath() 行为，避免弱模型/用户输入相对路径被拒）
    // content 校验（允许空字符串，空文件合法）
    if (!input.contains("content") || !input["content"].is_string()) {
        return ValidationResult::err(Error::Code::MissingArgument,
                                     "Missing required field: content");
    }
    // #40：写入内容密钥扫描（拒绝把密钥写进文件）
    const std::string content = input["content"].get<std::string>();
    const std::string secret_err = scan_for_secret_error(content);
    if (!secret_err.empty()) {
        return ValidationResult::err(Error::Code::PermissionDenied, secret_err);
    }
    return ValidationResult::ok();
}

// ============================================================
// 权限检查
// ============================================================

PermissionResult FileWriteTool::check_permissions(const nlohmann::json& input,
                                                  const ToolContext& ctx) const {
    // #36：Bypass 模式完全放行
    if (is_bypass_mode(ctx.permission_mode)) {
        return PermissionResult::ok();
    }
    // #36：Plan 模式禁止写文件
    if (deny_write_by_mode(ctx.permission_mode)) {
        return PermissionResult::err(Error::Code::PermissionDenied,
                                     "Write operations are not allowed in plan mode. "
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
                    ctx, std::format("Write access requires your approval:\n\n```\n{}\n```\n\n"
                                     "Allow writing to this path?",
                                     path_str))) {
                return PermissionResult::ok();
            }
            return PermissionResult::err(Error::Code::PermissionDenied, res.error().message);
        }
    } catch (const std::exception& e) {
        return PermissionResult::err(Error::Code::PermissionDenied,
                                     std::string("Write path error: ") + e.what());
    }
    return PermissionResult::ok();
}

// ============================================================
// 私有辅助：Pre-read + Staleness 检查
// ============================================================

ValidationResult FileWriteTool::check_pre_read_and_staleness(const std::string& canonical_path,
                                                             const fs::path& file_path) {
    // 1. Pre-read 强制检查
    auto state = FileReadStateTracker::instance().get_state(canonical_path);
    if (!state.has_value()) {
        return ValidationResult::err(
            Error::Code::PermissionDenied,
            "File has not been read yet. Read it first before writing to it.");
    }
    if (state->is_partial_view) {
        return ValidationResult::err(
            Error::Code::PermissionDenied,
            "File was only partially read. Read the full file before writing to it.");
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
        // Windows 下云同步/杀毒等可能改 mtime 但内容未变，避免误判
        const auto current_content = read_file_lf_normalized(file_path);
        if (current_content != state->content) {
            return ValidationResult::err(
                Error::Code::InternalError,
                "File has been modified since read, either by the user or by a linter. "
                "Read it again before attempting to write it.");
        }
        // 内容相同：放行（不更新 state.mtime，让下次走相同对比路径）
    }

    return ValidationResult::ok();
}

// ============================================================
// 私有辅助：.bak 备份
// ============================================================

ValidationResult FileWriteTool::create_backup(const fs::path& file_path) {
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

ResultV2<ToolResult> FileWriteTool::call(const nlohmann::json& input,
                                         const ToolContext& ctx) const {
    // 1. 解析输入（已在 validate_input 中校验，此处安全访问）
    FileWriteInput write_input;
    try {
        write_input = input.get<FileWriteInput>();
    } catch (const nlohmann::json::exception& e) {
        return ResultV2<ToolResult>::err(Error::Code::InvalidInput,
                                         std::format("Input parse failed: {}", e.what()));
    }

    // 2. 路径解析：用 expand_path 展开 ~ 与相对路径（基于 ctx.cwd），再 weakly_canonical 规范化
    std::string expanded = expand_path(write_input.file_path, ctx.cwd);
    fs::path file_path(expanded);
    std::error_code ec;
    file_path = fs::weakly_canonical(file_path, ec);
    if (ec) {
        ec.clear();  // 规范化失败不中断，继续使用原路径
    }
    // conditional skills：上报 touch 文件路径
    ctx.report_touch(file_path.string());

    // 3. 创建父目录（mkdir -p 语义，幂等）
    const fs::path parent_dir = file_path.parent_path();
    if (!parent_dir.empty()) {
        bool parent_exists = fs::exists(parent_dir, ec);
        if (ec) {
            ec.clear();
            parent_exists = true;  // 不确定时跳过创建，让后续写入报错
        }
        if (!parent_exists) {
            fs::create_directories(parent_dir, ec);
            if (ec) {
                return ResultV2<ToolResult>::err(Error::Code::InternalError,
                                                 std::format("Failed to create directory '{}': {}",
                                                             parent_dir.string(), ec.message()));
            }
        }
    }

    // 4. 判断 create/update（fs::exists 失败时返回错误，避免绕过 pre-read 检查）
    bool is_update = fs::exists(file_path, ec);
    if (ec) {
        return ResultV2<ToolResult>::err(Error::Code::InternalError,
                                         std::format("Failed to check file existence '{}': {}",
                                                     file_path.string(), ec.message()));
    }

    // 5. 安全检查（update 模式）：Pre-read + Staleness + .bak 备份
    if (is_update) {
        // 5a. Pre-read 强制检查 + Staleness 检测
        auto check = check_pre_read_and_staleness(file_path.generic_string(), file_path);
        if (check.is_err()) {
            return check.error();
        }

        // 5b. 创建 .bak 备份（安全优先，失败则中止写入）
        auto backup = create_backup(file_path);
        if (backup.is_err()) {
            return backup.error();
        }
    }

    // 6. 读取旧内容（update 模式，用于生成 diff）
    std::string old_content;
    if (is_update) {
        std::ifstream old_file(file_path, std::ios::binary);
        if (old_file.is_open()) {
            old_content = std::string(std::istreambuf_iterator<char>(old_file),
                                      std::istreambuf_iterator<char>());
        }
        // 读取失败不中断：old_content 为空，diff 会显示全部新增
    }

    // 7. 原子写入：写临时文件 → fsync → rename
    //    防止写入失败导致原文件损坏（直接 trunc 写入会破坏原文件）
    //    Windows rename 跨卷或目标存在时可能失败，失败时回退到直接写入
    fs::path tmp_path = file_path;
    tmp_path += ".workx.tmp";

    bool atomic_ok = false;
    {
        std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            // 临时文件创建失败，回退到直接写入
        } else {
            out << write_input.content;
            out.flush();
            out.close();

            if (out.fail()) {
                // 写入失败，删除临时文件
                std::error_code rm_ec;
                fs::remove(tmp_path, rm_ec);
                return ResultV2<ToolResult>::err(
                    Error::Code::InternalError,
                    std::format("Failed to write file: {}", write_input.file_path));
            }
            atomic_ok = true;
        }
    }

    if (atomic_ok) {
        // 取消点（#23 P2）：已取消则删除临时文件，原文件保持完好
        if (ctx.is_cancelled()) {
            std::error_code rm_ec;
            fs::remove(tmp_path, rm_ec);
            return ResultV2<ToolResult>::err(
                Error::Code::Cancelled,
                std::format("File write cancelled for: {}", write_input.file_path));
        }

        // fsync（POSIX 用 fsync，Windows 用 _commit）
        std::error_code sync_ec;
        do_sync(tmp_path, sync_ec);
        // sync 失败不致命，继续 rename

        // 原子 rename 覆盖原文件
        std::error_code rename_ec;
        fs::rename(tmp_path, file_path, rename_ec);
        if (rename_ec) {
            // rename 失败（如 Windows 跨卷或目标被占用），回退到直接写入
            std::error_code rm_ec;
            fs::remove(tmp_path, rm_ec);
            atomic_ok = false;
        }
    }

    if (!atomic_ok) {
        // 回退路径：直接写入（保留原 .bak 备份兜底）
        // 取消点：fallback 直接 trunc 原文件，已取消则避免触碰
        if (ctx.is_cancelled()) {
            return ResultV2<ToolResult>::err(
                Error::Code::Cancelled,
                std::format("File write cancelled for: {}", write_input.file_path));
        }

        std::ofstream out(file_path, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            return ResultV2<ToolResult>::err(
                Error::Code::InternalError,
                std::format("Failed to open file for writing: {}", write_input.file_path));
        }
        out << write_input.content;
        out.flush();
        out.close();

        if (out.fail()) {
            return ResultV2<ToolResult>::err(
                Error::Code::InternalError,
                std::format("Failed to write file: {}", write_input.file_path));
        }
    }

    // 8. 刷新 FileReadStateTracker（写后保持状态一致，允许连续写入）
    {
        std::error_code mtime_ec;
        const auto new_mtime = fs::last_write_time(file_path, mtime_ec);
        FileReadStateTracker::instance().update_after_write(
            file_path.generic_string(), lf_normalize(write_input.content),
            mtime_ec ? std::filesystem::file_time_type{} : new_mtime,
            false  // 写后视为完整视图
        );
    }

    // 8b. 通知宿主文件系统已变更（如 TUI @ 补全索引下次触发时会重建）
    //     覆盖 create/update 两种模式；无宿主注入回调时 no-op
    if (ctx.on_file_system_changed) {
        ctx.on_file_system_changed();
    }

    // 9. 生成 diff（update 模式）并返回结果
    if (is_update) {
        auto diff_lines = generate_line_diff(old_content, write_input.content);
        std::string diff_text = format_diff(file_path.string(), diff_lines);

        std::string result_text = std::format("File {} has been updated.\n", file_path.string());
        if (!diff_text.empty()) {
            result_text += "\n" + diff_text;
        } else {
            // 内容无变化
            result_text += "(no content changes)";
        }
        return ResultV2<ToolResult>::ok(ToolResult::ok(std::move(result_text)));
    }

    // create 模式
    return ResultV2<ToolResult>::ok(
        ToolResult::ok(std::format("File created successfully at: {}", file_path.string())));
}

}  // namespace agent::tool
