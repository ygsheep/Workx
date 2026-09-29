/**
 * @file file_read_tool.cpp
 * @brief FileReadTool 实现
 * @details 文件读取工具的具体实现：路径解析、编码检测、行号格式化、offset/limit 流式读取。
 *          所有 filesystem 操作使用 std::error_code 重载，避免异常抛出。
 *          v1.4.0：成功读取后调用 FileReadStateTracker 记录状态（pre-read/staleness 支持）。
 * @author workx
 * @version 1.4.0
 * @date 2026-07
 */

#include "agent/tool/FileReadTool/file_read_tool.h"
#include "agent/tool/constants.h"
#include "agent/tool/encoding.h"
#include "agent/tool/FileReadState/file_read_state.h"
#include "agent/tool/path_expand.h"
#include "agent/tool/path_validator.h"
#include "agent/tool/permission_ask.h"
#include "agent/tool/secret_scanner.h"
#include "core/config/config_manager.h"
#include "agent/config/app_config.h"

#include <format>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <vector>

namespace agent::tool {

namespace fs = std::filesystem;

using namespace agent::tool::constants;

// ============================================================
// 元数据方法
// ============================================================

const std::string& FileReadTool::name() const {
    static const std::string n{"Read"};
    return n;
}

const std::string& FileReadTool::description() const {
    static const std::string d{"Reads a file from the local filesystem."};
    return d;
}

const std::string& FileReadTool::prompt() const {
    static const std::string p = std::format(
        "Reads a file from the local filesystem. "
        "By default, it reads up to {} lines starting from the beginning of the file. "
        "You can optionally specify a line offset and limit (especially handy for long files), "
        "but it's recommended to read the whole file by not providing these parameters. "
        "When you already know which part of the file you need, only read that part. "
        "This can be important for larger files. "
        "Files larger than {} bytes will return an error; use offset and limit for larger files. "
        "The file_path parameter should be an absolute path (e.g., /home/user/file.txt or "
        "C:\\Users\\user\\file.txt); "
        "relative paths are resolved against the current working directory. "
        "Assume this tool is able to read all files on the machine. "
        "If the User provides a path to a file assume that path is valid. "
        "It is okay to read a file that does not exist; an error will be returned.",
        MAX_LINES_TO_READ, MAX_FILE_SIZE_BYTES);
    return p;
}

nlohmann::json FileReadTool::input_schema() const {
    return {{"type", "object"},
            {"properties",
             {{"file_path",
               {{"type", "string"}, {"description", "The absolute path to the file to read"}}},
              {"offset",
               {{"type", "integer"},
                {"minimum", 1},
                {"description",
                 "The line number to start reading from. Only provide if the file is too large to "
                 "read at once"}}},
              {"limit",
               {{"type", "integer"},
                {"minimum", 1},
                {"description",
                 "The number of lines to read. Only provide if the file is too large to read at "
                 "once."}}},
              {"pages",
               {{"type", "string"},
                {"description",
                 "Page range for PDF files (e.g., \"1-5\", \"3\", \"10-20\"). Only applicable to "
                 "PDF files. Maximum 20 pages per request."}}}}},
            {"required", {"file_path"}},
            {"additionalProperties", false}};
}

// ============================================================
// 输入验证
// ============================================================

ValidationResult FileReadTool::validate_input(const nlohmann::json& input,
                                              const ToolContext& /*ctx*/
) const {
    // 检查必填字段 file_path
    if (!input.contains("file_path") || !input["file_path"].is_string()) {
        return ValidationResult::err(Error::Code::MissingArgument,
                                     "Missing required field: file_path");
    }
    // L-1: 复用 path_str 变量，避免重复 get
    const std::string path_str = input["file_path"].get<std::string>();
    if (path_str.empty()) {
        return ValidationResult::err(Error::Code::InvalidInput, "file_path must not be empty");
    }
    // 相对路径在 call() 中由 expand_path() 基于 ctx.cwd 自动展开为绝对路径
    // （对齐 Claude Code CLI expandPath() 行为，避免弱模型/用户输入相对路径被拒）
    // 可选字段 offset：必须为正整数（1-based 行号）
    if (input.contains("offset")) {
        if (!input["offset"].is_number_integer()) {
            return ValidationResult::err(Error::Code::InvalidInput, "offset must be an integer");
        }
        if (input["offset"].get<int>() < 1) {
            return ValidationResult::err(Error::Code::InvalidInput, "offset must be >= 1");
        }
    }
    // 可选字段 limit：必须为正整数
    if (input.contains("limit")) {
        if (!input["limit"].is_number_integer()) {
            return ValidationResult::err(Error::Code::InvalidInput, "limit must be an integer");
        }
        if (input["limit"].get<int>() <= 0) {
            return ValidationResult::err(Error::Code::InvalidInput, "limit must be > 0");
        }
    }
    return ValidationResult::ok();
}

// ============================================================
// 私有辅助方法
// ============================================================

std::string FileReadTool::format_with_line_numbers(const std::vector<std::string>& lines,
                                                   int start_line) {
    if (lines.empty()) return "";

    const int last_line = start_line + static_cast<int>(lines.size());
    // 计算行号宽度（按最大行号的位数）
    int width = 1;
    for (int n = last_line; n >= 10; n /= 10) ++width;

    std::string result;
    result.reserve(lines.size() * 80);

    for (size_t i = 0; i < lines.size(); ++i) {
        const int line_num = start_line + static_cast<int>(i);
        // 右对齐行号 + → (U+2192) + 内容
        std::string num_str = std::to_string(line_num);
        if (static_cast<int>(num_str.size()) < width) {
            result.append(width - num_str.size(), ' ');
        }
        result += num_str;
        result += "\xe2\x86\x92";  // → (U+2192) UTF-8 编码
        result += lines[i];
        result += '\n';
    }

    // 移除末尾多余换行，保持输出整洁
    if (!result.empty()) {
        result.pop_back();
    }
    return result;
}

ToolResult FileReadTool::read_directory(const fs::path& dir_path) {
    std::vector<std::string> entries;

    // 使用 skip_permission_denied 跳过无权限目录，遇错不中断
    std::error_code ec;
    for (auto it =
             fs::directory_iterator(dir_path, fs::directory_options::skip_permission_denied, ec);
         it != fs::directory_iterator(); it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        const auto& entry = *it;
        std::string name = entry.path().filename().string();
        // 目录项追加 '/' 后缀，便于区分文件与目录
        if (entry.is_directory(ec)) {
            name += "/";
        }
        entries.push_back(std::move(name));
    }

    // 按字典序排序，保证输出稳定
    std::sort(entries.begin(), entries.end());

    if (entries.empty()) {
        return ToolResult::ok(std::string{"(empty directory)"});
    }

    std::string result;
    result.reserve(entries.size() * 32);
    for (const auto& e : entries) {
        result += e;
        result += '\n';
    }
    if (!result.empty()) {
        result.pop_back();
    }
    return ToolResult::ok(std::move(result));
}

// ============================================================
// 执行
// ============================================================

// ============================================================
// 权限检查
// ============================================================

PermissionResult FileReadTool::check_permissions(const nlohmann::json& input,
                                                 const ToolContext& ctx) const {
    // #36：Bypass 模式完全放行
    if (is_bypass_mode(ctx.permission_mode)) {
        return PermissionResult::ok();
    }
    // #34：路径边界 + 敏感文件拦截（Plan 模式允许只读，仍受路径校验）
    const std::string raw = input.value("file_path", input.value("path", ""));
    if (raw.empty()) return PermissionResult::ok();  // 空路径由 validate_input 拦截
    std::istringstream ss(raw);
    std::string part;
    while (std::getline(ss, part, '|')) {
        try {
            const std::string expanded = expand_path(part, ctx.cwd);
            auto res =
                validate_path_access(expanded, ctx.cwd, repo_root_allowlist(ctx.git_repo_root));
            if (res.is_err()) {
                // 评审 #2：绝对禁止（私钥/凭据）不可确认；越界/可确认敏感路径由用户确认放行
                if (!is_absolutely_forbidden_path(expanded) &&
                    ask_user_confirm(
                        ctx, std::format("Read access requires your approval:\n\n```\n{}\n```\n\n"
                                         "Allow reading this path?",
                                         part))) {
                    continue;
                }
                return PermissionResult::err(Error::Code::PermissionDenied, res.error().message);
            }
        } catch (const std::exception& e) {
            return PermissionResult::err(Error::Code::PermissionDenied,
                                         std::string("Read path error: ") + e.what());
        }
    }
    return PermissionResult::ok();
}

// ============================================================
// 执行
// ============================================================

ResultV2<ToolResult> FileReadTool::call(const nlohmann::json& input, const ToolContext& ctx) const {
    // 1. 解析输入 JSON 为 FileReadInput 结构（try-catch 防止类型不匹配抛异常）
    FileReadInput read_input;
    try {
        read_input = input.get<FileReadInput>();
    } catch (const nlohmann::json::exception& e) {
        return ResultV2<ToolResult>::err(Error::Code::InvalidInput,
                                         std::format("Input parse failed: {}", e.what()));
    }

    // 读取可配置参数（回退到 constants.h 编译期默认值）
    // prompt() 仍使用 constants.h 作为文档默认值，实际限制由此处配置决定
    constexpr int64_t MAX_FILE_SIZE_LIMIT = 100LL * 1024 * 1024;  // 100 MB 上限
    constexpr int MAX_LINES_LIMIT = 100000;                       // 10 万行上限
    constexpr int MAX_LINES_FLOOR = 1;                            // 至少 1 行

    // D-5：通过 ctx.config_manager() 解析配置管理器，支持 DI 注入
    int max_size_cfg = ctx.config_manager().get_or<int>(
        agent::keys::FILE_READ_MAX_SIZE, static_cast<int>(constants::MAX_FILE_SIZE_BYTES));
    // 校验范围：负数或超上限时回退到编译期默认值
    if (max_size_cfg < 0 || max_size_cfg > MAX_FILE_SIZE_LIMIT) {
        max_size_cfg = static_cast<int>(constants::MAX_FILE_SIZE_BYTES);
    }
    const size_t max_file_size = static_cast<size_t>(max_size_cfg);

    int max_lines = ctx.config_manager().get_or<int>(agent::keys::FILE_READ_MAX_LINES,
                                                     constants::MAX_LINES_TO_READ);
    // 校验范围：负数或超上限时回退到编译期默认值；floor 到 1
    if (max_lines < MAX_LINES_FLOOR || max_lines > MAX_LINES_LIMIT) {
        max_lines = constants::MAX_LINES_TO_READ;
    }

    // 2. 路径解析：用 expand_path 展开 ~ 与相对路径（基于 ctx.cwd），再 weakly_canonical 规范化
    std::string expanded = expand_path(read_input.file_path, ctx.cwd);
    fs::path file_path(expanded);
    std::error_code ec;
    file_path = fs::weakly_canonical(file_path, ec);
    // conditional skills：上报 touch 文件路径
    ctx.report_touch(file_path.string());
    if (ec) {
        ec.clear();  // 规范化失败不中断，继续使用原路径
    }

    // 3. 检查路径存在性
    if (!fs::exists(file_path, ec)) {
        return ResultV2<ToolResult>::err(Error::Code::ResourceNotFound,
                                         "File does not exist: " + read_input.file_path);
    }

    // 4. 目录处理：路径指向目录时列举内容
    if (fs::is_directory(file_path, ec)) {
        return ResultV2<ToolResult>::ok(read_directory(file_path));
    }

    // 5. 非常规文件（如设备文件、管道）拒绝读取
    if (!fs::is_regular_file(file_path, ec)) {
        return ResultV2<ToolResult>::err(Error::Code::InvalidInput,
                                         "Path is not a regular file: " + read_input.file_path);
    }

    // 6. 文件大小检查：超过 max_file_size 拒绝（需用 offset/limit 分段）
    const auto file_size = fs::file_size(file_path, ec);
    if (!ec && file_size > max_file_size) {
        return ResultV2<ToolResult>::err(Error::Code::InvalidInput,
                                         std::format("File size {} bytes exceeds maximum {} bytes; "
                                                     "use offset and limit for larger files",
                                                     file_size, max_file_size));
    }

    // 7. 图片文件检测：扩展名明确告知模型当前模型无图片输入能力，
    //    避免模型猜测内容或把占位错误写入文件（真实用户场景复现）
    {
        const std::string ext = file_path.extension().string();
        std::string lower_ext = ext;
        std::transform(lower_ext.begin(), lower_ext.end(), lower_ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        static constexpr const char* kImageExts[] = {".png", ".jpg", ".jpeg", ".gif", ".webp",
                                                     ".bmp", ".ico", ".tiff", ".svg"};
        for (const char* img : kImageExts) {
            if (lower_ext == img) {
                return ResultV2<ToolResult>::err(
                    Error::Code::InvalidInput,
                    std::format("File '{}' is an image ({}) which the current model "
                                "cannot read (no image input support). Inform the user "
                                "that image files cannot be processed.",
                                file_path.filename().string(), img + 1));
            }
        }
    }

    // 8. 编码检测（替代原 is_binary_file，更精确：UTF-16 不会被误判为二进制）
    const Encoding encoding = detect_encoding(file_path);
    if (encoding == Encoding::Binary) {
        return ResultV2<ToolResult>::err(
            Error::Code::InvalidInput,
            "File appears to be binary, cannot display: " + read_input.file_path);
    }

    const int offset = read_input.offset.value_or(1);
    const int limit = read_input.limit.value_or(max_lines);

    std::vector<std::string> target_lines;
    int total_lines = 0;
    bool has_more = false;

    if (encoding == Encoding::Utf8 || encoding == Encoding::Ascii ||
        encoding == Encoding::Unknown) {
        // 8a. UTF-8/ASCII：流式读取（仅存储目标范围行，避免全部加载到内存）
        std::ifstream file(file_path);
        if (!file.is_open()) {
            return ResultV2<ToolResult>::err(Error::Code::InternalError,
                                             "Failed to open file: " + read_input.file_path);
        }

        // 跳过 UTF-8 BOM（若存在）
        skip_utf8_bom(file);

        target_lines.reserve(static_cast<size_t>(std::min(limit, max_lines)));
        std::string line;

        // 单次遍历：计数所有行，仅存储 [offset, offset+limit) 范围内的行
        while (std::getline(file, line)) {
            ++total_lines;
            if (total_lines >= offset && total_lines < offset + limit) {
                normalize_eol(line);  // 剥离 CRLF 残留的 '\r'
                target_lines.push_back(std::move(line));
            }
            // 已读取到目标范围末尾，检查是否还有更多行后提前退出
            if (total_lines >= offset + limit) {
                std::string extra;
                if (std::getline(file, extra)) {
                    has_more = true;
                }
                break;
            }
        }
    } else {
        // 8b. 非 UTF-8（UTF-16/GBK）：全量读取 + 编码转换为 UTF-8
        std::vector<std::string> all_lines = read_as_utf8_lines(file_path, encoding);
        total_lines = static_cast<int>(all_lines.size());

        // 应用 offset/limit 切片
        if (offset <= total_lines) {
            const int end = std::min(offset - 1 + limit, total_lines);
            target_lines.assign(all_lines.begin() + (offset - 1), all_lines.begin() + end);
        }
    }

    // 空文件检查：返回提示信息而非空字符串（对齐 Claude Code 行为）
    if (total_lines == 0) {
        // 记录读取状态（空文件，完整视图）—— 供 FileWriteTool 做 pre-read 检查
        std::error_code mtime_ec;
        const auto mtime = fs::last_write_time(file_path, mtime_ec);
        FileReadStateTracker::instance().record_read(
            file_path.generic_string(), std::string{},
            mtime_ec ? std::filesystem::file_time_type{} : mtime,
            false,   // 空文件视为完整视图
            1, 0, 0  // offset=1, lines_read=0, total_lines=0（空文件无行）
        );
        return ResultV2<ToolResult>::ok(ToolResult::ok(
            std::string{"<system-reminder>File exists but has empty contents.</system-reminder>"}));
    }

    // offset 超出范围（不记录状态，读取实际失败）
    if (offset > total_lines) {
        return ResultV2<ToolResult>::err(
            Error::Code::InvalidInput,
            std::format("offset {} is beyond the file's {} lines", offset, total_lines));
    }

    // 记录读取状态（供 FileWriteTool 做 pre-read / staleness 检查）
    // - content：LF 规范化后的内容快照（用于 mtime 变化时的内容对比回退）
    // - is_partial_view：offset != 1 或未读完所有行时为 true（部分视图不可做内容对比）
    {
        std::error_code mtime_ec;
        const auto mtime = fs::last_write_time(file_path, mtime_ec);

        std::string content_snapshot;
        content_snapshot.reserve(target_lines.size() * 80);
        for (size_t i = 0; i < target_lines.size(); ++i) {
            if (i > 0) content_snapshot += '\n';
            content_snapshot += target_lines[i];
        }

        const int lines_read = static_cast<int>(target_lines.size());
        const bool is_partial = (offset != 1) || has_more || (lines_read < total_lines);

        FileReadStateTracker::instance().record_read(
            file_path.generic_string(), std::move(content_snapshot),
            mtime_ec ? std::filesystem::file_time_type{} : mtime, is_partial, offset, lines_read,
            total_lines  // has_more 时提前退出，total_lines 可能不完整
        );
    }

    // 9. 格式化输出（带行号，1-based）
    std::string formatted = format_with_line_numbers(target_lines, offset);

    // #36：输出脱敏，密钥内容替换为 [REDACTED:label]
    formatted = redact_secrets(formatted);

    // 10. 部分读取时附加元信息（明确已读行范围，供模型定位缺失区域）
    const int lines_read = static_cast<int>(target_lines.size());
    if (lines_read > 0) {
        const int end_line = offset + lines_read - 1;
        if (has_more) {
            // 提前退出：文件还有更多行，total_lines 不完整
            formatted += std::string("\n\n(read lines ") + std::to_string(offset) + "-" +
                         std::to_string(end_line) + ", more lines available)";
        } else if (lines_read < total_lines) {
            formatted += std::string("\n\n(read lines ") + std::to_string(offset) + "-" +
                         std::to_string(end_line) + ", total " + std::to_string(total_lines) +
                         ", truncated)";
        }
    }

    return ResultV2<ToolResult>::ok(ToolResult::ok(std::move(formatted)));
}

}  // namespace agent::tool
