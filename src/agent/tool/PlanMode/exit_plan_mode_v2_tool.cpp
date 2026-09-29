/**
 * @file exit_plan_mode_v2_tool.cpp
 * @brief ExitPlanModeV2Tool 实现（#28）
 * @details 流程：
 *          1. 复用 ask_user_confirm 把方案呈现给用户（Yes/No）
 *          2. 批准 → 权限模式切回 Default（若当前为 Plan；Bypass 不降级）
 *          3. 发布 ExitPlanModeEvent（含批准结果）
 *          4. 无确认通道（event_bus 为空）→ fail-closed 视为未批准
 * @version 1.0.0
 * @date 2026-08
 */

#include "agent/tool/PlanMode/exit_plan_mode_v2_tool.h"

#include <cctype>
#include <filesystem>
#include <format>
#include <fstream>
#include <vector>  // #54：critical_files 结构化列表

#include "agent/tool/path_matcher.h"
#include "agent/tool/permission_ask.h"
#include "core/events/agent_events.h"
#include "core/events/i_event_bus.h"

namespace agent::tool {

const std::string& ExitPlanModeV2Tool::name() const {
    static const std::string n{"ExitPlanModeV2"};
    return n;
}

const std::string& ExitPlanModeV2Tool::description() const {
    static const std::string d{
        "Exit plan mode and present the plan to the user for approval. "
        "Requires the 'plan' field with the detailed plan (files to change, "
        "reasons, risks). After approval, write/edit/execute tools are allowed again."};
    return d;
}

const std::string& ExitPlanModeV2Tool::prompt() const {
    static const std::string p{
        "After researching in plan mode, call ExitPlanModeV2 with a complete plan: "
        "which files to change, why, and risks. The user must approve before you "
        "start editing. If rejected, adjust the plan and try again."};
    return p;
}

nlohmann::json ExitPlanModeV2Tool::input_schema() const {
    const nlohmann::json critical_files_schema = {
        {"type", "array"},
        {"items", {{"type", "string"}}},
        {"description",
         "#54: Optional list of critical files from exploration that the execution "
         "phase should touch. If omitted, the session coordinator fills it from the "
         "aggregated PlanArtifact."}};
    return {{"type", "object"},
            {"properties",
             nlohmann::json::object(
                 {{"plan",
                   {{"type", "string"},
                    {"description", "The detailed plan: files to change, approach, risks."}}},
                  {"critical_files", critical_files_schema}})},
            {"required", nlohmann::json::array({"plan"})},
            {"additionalProperties", false}};
}

ResultV2<ToolResult> ExitPlanModeV2Tool::call(const nlohmann::json& input,
                                              const ToolContext& ctx) const {
    if (!input.contains("plan") || !input["plan"].is_string()) {
        return ResultV2<ToolResult>::err(Error::Code::MissingArgument,
                                         "ExitPlanModeV2: 'plan' is required");
    }
    const std::string plan = input["plan"].get<std::string>();
    if (plan.empty()) {
        return ResultV2<ToolResult>::err(Error::Code::InvalidInput,
                                         "ExitPlanModeV2: 'plan' must not be empty");
    }

    // #54：读取可选结构化 critical_files（LLM 可从 explore 产物聚合的 markdown 中整理；
    //       缺省由宿主在审批时从 PlanArtifact 补齐）
    std::vector<std::string> critical_files;
    if (input.contains("critical_files") && input["critical_files"].is_array()) {
        for (const auto& cf : input["critical_files"]) {
            if (cf.is_string()) critical_files.push_back(cf.get<std::string>());
        }
    }

    // 1. 方案写入 markdown 文件（~/.workx/plan/plan_<session>.md），供 TUI 侧边栏预览，
    //    避免在提问里直接堆全文。写入失败则回退为全文内联展示。
    std::string plan_path;
    try {
        namespace fs = std::filesystem;
        fs::path dir = fs::path(expand_home("~/.workx/plan"));
        fs::create_directories(dir);
        // 会话 id 归一化后用于唯一文件名（避免覆盖 + 保证安全字符）
        std::string sid = ctx.session_id;
        for (char& c : sid) {
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') c = '_';
        }
        fs::path file = dir / (sid.empty() ? std::string("plan.md") : ("plan_" + sid + ".md"));
        {
            std::ofstream ofs(file, std::ios::trunc | std::ios::binary);
            if (ofs) ofs << plan;
        }
        if (fs::is_regular_file(file)) plan_path = fs::absolute(file).string();
    } catch (const std::exception&) {
        plan_path.clear();  // 家目录不可写等场景：走全文内联回退
    }

    // 2. 先发布方案预览事件，通知 TUI 在侧边栏打开方案文件（先于提问弹出）
    if (ctx.event_bus_ptr && !plan_path.empty()) {
        ctx.event_bus_ptr->publish_async(
            PlanPreviewEvent{.session_id = ctx.session_id, .plan_path = plan_path});
    }

    // 3. 呈现方案请求用户批准（无确认通道 → fail-closed 未批准）
    const std::string question =
        plan_path.empty()
            ? std::format("Plan:\n\n```\n{}\n```\n\nApprove this plan and exit plan mode?", plan)
            : std::format("方案已写入 {}（侧边栏已预览）。批准该方案并退出规划模式？", plan_path);
    const bool approved = ask_user_confirm(ctx, question);

    // 4. 批准 → 恢复进入计划前的原模式（评审 #1：非硬编码回 Default）
    //    Bypass 未进入 Plan，无需切换；拒绝则保持 Plan
    if (approved && is_plan_mode(ctx.permission_mode)) {
        if (ctx.on_exit_plan_mode) {
            ctx.on_exit_plan_mode();
        } else {
            ctx.set_permission_mode(PermissionMode::Default);
        }
    }

    // 5. 发布退出计划模式事件（publish_async 传值，typeid 匹配订阅）
    if (ctx.event_bus_ptr) {
        ctx.event_bus_ptr->publish_async(ExitPlanModeEvent{
            .session_id = ctx.session_id,
            .plan = plan,
            .approved = approved,
            .critical_files = critical_files  // #54：结构化关键文件（宿审批时从产物补齐缺省）
        });
    }

    if (approved) {
        return ResultV2<ToolResult>::ok(
            ToolResult::ok(std::string{"Plan approved. Exiting plan mode; write/edit and command "
                                       "execution are allowed again."}));
    }
    return ResultV2<ToolResult>::ok(
        ToolResult::ok(std::string{"Plan not approved. Staying in plan mode. Revise the plan and "
                                   "call ExitPlanModeV2 again."}));
}

}  // namespace agent::tool