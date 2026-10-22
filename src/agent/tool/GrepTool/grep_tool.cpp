/**
 * @file grep_tool.cpp
 * @brief GrepTool 实现
 * @details 调用捆绑的 ripgrep (rg) 进行内容正则搜索：
 *          1. ToolRegistry 解析 rg 路径（配置/bundled/PATH 三级回退）
 *          2. subprocess::exec 启动 rg，捕获 stdout
 *          3. rg 退出码：0=有匹配，1=无匹配，2=错误
 * @version 1.1.0
 * @date 2026-08
 */

#include "agent/tool/GrepTool/grep_tool.h"

#include "agent/tool/path_expand.h"      // expand_path
#include "agent/tool/path_validator.h"   // validate_path_access
#include "agent/tool/permission_ask.h"   // is_bypass_mode, ask_user_confirm
#include "core/process/tool_registry.h"  // ToolRegistry::resolve_ripgrep()
#include "core/process/subprocess.h"     // process::exec()
#include "core/utils/error.h"

#include <algorithm>
#include <filesystem>
#include <format>
#include <sstream>

namespace agent::tool {

namespace fs = std::filesystem;

// ============================================================
// 元数据方法
// ============================================================

const std::string& GrepTool::name() const {
    static const std::string n{"Grep"};
    return n;
}

const std::string& GrepTool::description() const {
    static const std::string d{
        "Searches file contents using regex or literal patterns. "
        "Backed by ripgrep for high performance."};
    return d;
}

const std::string& GrepTool::prompt() const {
    static const std::string p{
        "Searches file contents for matching patterns using ripgrep (rg).\n\n"
        "## When to use\n"
        "- Finding where a function/variable is used or defined\n"
        "- Searching for error messages or log patterns\n"
        "- Locating code by intent (e.g. 'where do we encrypt passwords')\n"
        "- Any content-based search across files\n\n"
        "## Parameters\n"
        "- `pattern` (required): The search pattern. Regex by default; "
        "set `regex=false` for literal matching.\n"
        "- `path` (optional): Directory or file to search in. "
        "Defaults to the current working directory.\n"
        "- `case_insensitive` (optional, default false): Ignore case.\n"
        "- `regex` (optional, default true): Treat `pattern` as regex. "
        "Set to false for literal string matching.\n"
        "- `glob` (optional): File name glob filter, e.g. '*.cpp' or "
        "'*.{h,cpp}'. Only files matching this glob are searched.\n"
        "- `max_matches` (optional, default 200): Maximum number of matches "
        "to return. Additional matches are truncated with a notice.\n\n"
        "## Output format\n"
        "Each match line is formatted as:\n"
        "  <relative_path>:<line_number>: <line_content>\n"
        "If no matches are found, returns 'No matches found for pattern: ...'.\n\n"
        "## Guidelines\n"
        "- Prefer this tool over BashTool+grep for content search: it respects "
        ".gitignore, is faster, and handles binary files correctly.\n"
        "- Use `glob` to narrow the search scope (e.g. '*.cpp' to skip headers).\n"
        "- For literal strings (like error messages), set `regex=false` to avoid "
        "accidental regex metacharacter issues.\n"
        "- The search is anchored at the `path` directory; results use paths "
        "relative to it.\n"};
    return p;
}

nlohmann::json GrepTool::input_schema() const {
    return {{"type", "object"},
            {"properties",
             {{"pattern",
               {{"type", "string"}, {"description", "The search pattern (regex or literal)"}}},
              {"path",
               {{"type", "string"},
                {"description", "The directory or file to search in (defaults to cwd)"}}},
              {"case_insensitive",
               {{"type", "boolean"}, {"description", "Ignore case"}, {"default", false}}},
              {"regex",
               {{"type", "boolean"},
                {"description", "Treat pattern as regex (true) or literal (false)"},
                {"default", true}}},
              {"glob",
               {{"type", "string"},
                {"description", "File name glob filter, e.g. '*.cpp' or '*.{h,cpp}'"}}},
              {"max_matches",
               {{"type", "integer"},
                {"description", "Maximum matches to return"},
                {"default", 200},
                {"minimum", 1}}}}},
            {"required", {"pattern"}},
            {"additionalProperties", false}};
}

// ============================================================
// 内部辅助
// ============================================================

namespace {

/// @brief 默认最大返回匹配数
constexpr int kDefaultMaxMatches = 200;

/// @brief 为 Windows 路径转义反斜杠，避免 rg 解析为转义符
/// @details rg 在 Windows 上接受正斜杠路径，这里统一转换
std::string normalize_path_sep(const std::string& path) {
    std::string r = path;
    std::replace(r.begin(), r.end(), '\\', '/');
    return r;
}

/// @brief 从 rg 输出中截取前 N 个匹配行
/// @details rg --no-heading 输出格式为：
///          <path>:<line>:<content>
///          <path>:<line>:<content>
///          按行计数，超出 max_matches 的部分截断
std::string truncate_matches(std::string_view output, int max_matches) {
    if (max_matches <= 0) return std::string{output};

    std::string result;
    result.reserve(output.size());
    int line_count = 0;
    size_t pos = 0;

    while (pos < output.size()) {
        size_t nl = output.find('\n', pos);
        if (nl == std::string_view::npos) nl = output.size();

        // 追加当前行（含换行）
        result.append(output.data() + pos, nl - pos);
        if (nl < output.size()) result.push_back('\n');

        ++line_count;
        if (line_count >= max_matches) {
            pos = (nl < output.size()) ? nl + 1 : nl;
            // 检查是否还有更多行
            if (pos < output.size()) {
                result += std::format("\n... (truncated, showing first {} matches)", max_matches);
            }
            break;
        }
        pos = (nl < output.size()) ? nl + 1 : nl;
    }
    return result;
}

}  // namespace

// ============================================================
// 权限检查（#60：路径边界校验，与 Read 工具一致）
// ============================================================

PermissionResult GrepTool::check_permissions(const nlohmann::json& input,
                                             const ToolContext& ctx) const {
    if (is_bypass_mode(ctx.permission_mode)) {
        return PermissionResult::ok();
    }
    // path 未指定时默认 cwd，已在边界内，无需校验
    const std::string raw = input.value("path", "");
    if (raw.empty()) return PermissionResult::ok();

    std::istringstream ss(raw);
    std::string part;
    while (std::getline(ss, part, '|')) {
        try {
            const std::string expanded = expand_path(part, ctx.cwd);
            auto res =
                validate_path_access(expanded, ctx.cwd, repo_root_allowlist(ctx.git_repo_root));
            if (res.is_err()) {
                if (!is_absolutely_forbidden_path(expanded) &&
                    ask_user_confirm(
                        ctx, std::format("Search access requires your approval:\n\n```\n{}\n```\n\n"
                                         "Allow searching this path?",
                                         part))) {
                    continue;
                }
                return PermissionResult::err(Error::Code::PermissionDenied, res.error().message);
            }
        } catch (const std::exception& e) {
            return PermissionResult::err(Error::Code::PermissionDenied,
                                         std::string("Grep path error: ") + e.what());
        }
    }
    return PermissionResult::ok();
}

// ============================================================
// 执行
// ============================================================

ResultV2<ToolResult> GrepTool::call(const nlohmann::json& input, const ToolContext& ctx) const {
    // 1. 解析输入
    if (!input.contains("pattern") || !input["pattern"].is_string()) {
        return ResultV2<ToolResult>::err(Error::Code::InvalidInput,
                                         "Grep: missing or invalid 'pattern'");
    }
    std::string pattern = input["pattern"].get<std::string>();
    if (pattern.empty()) {
        return ResultV2<ToolResult>::err(Error::Code::InvalidInput,
                                         "Grep: 'pattern' must not be empty");
    }

    bool case_insensitive = input.value("case_insensitive", false);
    bool use_regex = input.value("regex", true);
    int max_matches = input.value("max_matches", kDefaultMaxMatches);
    if (max_matches < 1) max_matches = kDefaultMaxMatches;

    std::string glob_filter;
    if (input.contains("glob") && input["glob"].is_string()) {
        glob_filter = input["glob"].get<std::string>();
    }

    // 2. 确定搜索路径
    std::string search_path = ctx.cwd;
    if (input.contains("path") && input["path"].is_string()) {
        search_path = input["path"].get<std::string>();
    }
    if (search_path.empty()) search_path = ".";
    search_path = normalize_path_sep(search_path);

    std::error_code ec;
    if (!fs::exists(search_path, ec)) {
        return ResultV2<ToolResult>::err(Error::Code::ResourceNotFound,
                                         "Grep: path does not exist: " + search_path);
    }

    // 3. 解析 ripgrep 路径（配置 > bundled > PATH）
    auto rg_path = process::ToolRegistry::instance().resolve_ripgrep();
    if (!rg_path) {
        return ResultV2<ToolResult>::err(
            Error::Code::ResourceNotFound,
            "Grep: ripgrep (rg) not found. Please install rg or configure tool_path.ripgrep.");
    }

    // 4. 构建 rg 命令行参数
    //    rg --no-config --no-heading --line-number --color never
    //       [--ignore-case] [--fixed-strings] [--glob <pat>] <pattern> <path>
    //    --no-config     忽略用户 ~/.config/ripgrep/config（保证行为一致）
    //    --no-heading    不按文件分组标题（输出更紧凑）
    //    --line-number   每行前输出行号
    //    --color never   禁用 ANSI 颜色（输出纯文本）
    process::ExecOptions opts;
    opts.cwd = ctx.cwd;

    opts.args.push_back("--no-config");
    opts.args.push_back("--no-heading");
    opts.args.push_back("--line-number");
    opts.args.push_back("--color");
    opts.args.push_back("never");

    if (case_insensitive) {
        opts.args.push_back("--ignore-case");
    }
    if (!use_regex) {
        opts.args.push_back("--fixed-strings");
    }
    if (!glob_filter.empty()) {
        opts.args.push_back("--glob");
        opts.args.push_back(glob_filter);
    }

    opts.args.push_back(pattern);
    opts.args.push_back(search_path);

    // 超时：默认 30 秒（防止超大仓库死循环）
    opts.timeout = std::chrono::milliseconds(30000);

    // 取消信号：绑定到 ctx
    if (ctx.cancel_flag != nullptr) {
        const std::atomic<bool>* flag = ctx.cancel_flag;
        opts.is_cancelled = [flag]() { return flag->load(std::memory_order_acquire); };
    }

    // 5. 执行 rg
    auto exec_result = process::exec(*rg_path, opts);
    if (exec_result.is_err()) {
        const auto& err = exec_result.error();
        return ResultV2<ToolResult>::err(
            err.code, std::format("Grep: failed to execute ripgrep: {}", err.message));
    }

    const auto& out = exec_result.value();

    // 6. 处理退出码
    //    rg 退出码：0=有匹配，1=无匹配，2=错误
    if (out.exit_code == 2) {
        // rg 报错（如正则语法错误、路径无权限等）
        std::string err_msg = out.stderr_text;
        if (err_msg.empty()) err_msg = "ripgrep reported an error (exit code 2)";
        return ResultV2<ToolResult>::err(Error::Code::ToolExecutionFailed,
                                         std::format("Grep: {}", err_msg));
    }

    if (out.cancelled) {
        return ResultV2<ToolResult>::err(Error::Code::Cancelled, "Grep: search was cancelled");
    }

    if (out.timed_out) {
        return ResultV2<ToolResult>::err(Error::Code::ToolExecutionFailed,
                                         "Grep: search timed out (30s)");
    }

    // 7. 格式化输出
    //    exit_code == 1 表示无匹配，返回友好提示
    if (out.exit_code == 1 || out.stdout_text.empty()) {
        return ResultV2<ToolResult>::ok(ToolResult::ok("No matches found for pattern: " + pattern));
    }

    // exit_code == 0：有匹配，截断并返回
    std::string formatted = truncate_matches(out.stdout_text, max_matches);

    // 上报进度
    ctx.report_progress(std::format("Grep found matches for: {}", pattern));

    return ResultV2<ToolResult>::ok(ToolResult::ok(std::move(formatted)));
}

}  // namespace agent::tool
