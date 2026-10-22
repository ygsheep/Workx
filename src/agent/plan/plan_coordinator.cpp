/**
 * @file plan_coordinator.cpp
 * @brief Plan Mode V2（#54）协调器实现
 * @version 1.0.0
 * @date 2026-09
 */

#include "agent/plan/plan_coordinator.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <format>
#include <regex>  // #54：M-1 从 explore 结果提取关键文件
#include <unordered_set>
#include "agent/config/app_config.h"

namespace agent::plan {

namespace {

/// @brief 按逗号拆分非空子域（去空与首尾空白）
std::vector<std::string> split_areas(const std::string& s) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= s.size()) {
        const size_t comma = s.find(',', start);
        std::string seg =
            s.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        // 去首尾空白
        auto n = [](unsigned char c) { return !std::isspace(c); };
        auto b = std::find_if(seg.begin(), seg.end(), n);
        auto e = std::find_if(seg.rbegin(), seg.rend(), n).base();
        const std::string trimmed = b >= e ? std::string{} : std::string(b, e);
        if (!trimmed.empty()) out.push_back(trimmed);
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return out;
}

/// @brief 从探索文本中提取"关键文件"引用（M-1）
/// @details 采集形如 <路径>\<扩展名> 的 token（含 `/` 或 `\` 分隔，避免误抓版本号/散文词）。
///          命中常见源码/配置扩展名，按原文去重保序。解析失败的文本走 set_critical_files 兜底。
std::vector<std::string> extract_file_refs(const std::string& text) {
    static const std::regex path_re(
        R"([A-Za-z0-9_./\\\-]+\.(?:cpp|cc|cxx|c|hpp|hh|hxx|h|py|rs|go|java|kt|kts|ts|tsx|js|jsx|vue|md|json|toml|yaml|yml|cmake|proto|sql|csv|sh))",
        std::regex_constants::icase);
    std::vector<std::string> files;
    std::unordered_set<std::string> seen;
    if (text.empty()) return files;
    try {
        for (auto it = std::sregex_iterator(text.begin(), text.end(), path_re);
             it != std::sregex_iterator(); ++it) {
            std::string tok = it->str(0);
            // 仅保留含路径分隔符的 token（提高信噪比）：形如 src/foo.cpp / src\foo.cpp
            if (tok.find('/') == std::string::npos && tok.find('\\') == std::string::npos) {
                continue;
            }
            if (seen.insert(tok).second) files.push_back(std::move(tok));
        }
    } catch (const std::regex_error&) {
        return {};  // 正则异常兜底：不阻断 explore 收尾
    }
    return files;
}

}  // namespace

PlanCoordinator::PlanCoordinator(IConfigManager& cfg) : m_cfg(cfg) {}

bool PlanCoordinator::auto_enabled() const { return m_cfg.get_or<bool>(keys::PLAN_AUTO, true); }

bool PlanCoordinator::interview_enabled() const {
    return m_cfg.get_or<bool>(keys::PLAN_INTERVIEW_ENABLED, true);
}

int PlanCoordinator::explore_agent_count() const {
    return m_cfg.get_or<int>(keys::PLAN_EXPLORE_AGENT_COUNT, 3);
}

std::string PlanCoordinator::explore_areas() const {
    return m_cfg.get_or<std::string>(keys::PLAN_EXPLORE_AREAS, "");
}

void PlanCoordinator::transition(PlanStage s) { m_stage = s; }

void PlanCoordinator::begin_plan(const std::string& reason) {
    // 复位上一轮规划（进入新一轮规划前清空历史探索与产物）
    m_reason = reason;
    m_context.clear();
    m_findings.clear();
    m_active_tasks.clear();
    m_explore_areas.clear();
    m_explore_launched = 0;
    m_artifact = PlanArtifact{};
    ++m_plan_cycle;  // #54：编号细化——规划轮次自增，explore task_id 借此跨轮全局唯一
    transition(PlanStage::Interview);  // 阶段 1（Interview）
    // auto 关闭 → 保持 interview 由宿主/用户驱动；开启且跳 Interview → 直通探索
    if (auto_enabled() && !interview_enabled()) {
        start_explore();
    }
}

void PlanCoordinator::set_interview_notes(const std::string& notes) {
    m_context = notes;
    if (m_stage == PlanStage::Interview) {
        start_explore();
    }
}

std::vector<std::pair<std::string, std::string>> PlanCoordinator::build_explore_prompts(
    const std::string& reason) const {
    std::vector<std::pair<std::string, std::string>> out;
    const int count = explore_agent_count();
    const std::vector<std::string> areas = split_areas(explore_areas());
    // 优先使用显式子域列表；不足数量补齐通用探索；超量截断到 count
    for (const std::string& a : areas) {
        if (out.size() >= static_cast<std::size_t>(count)) break;
        out.emplace_back(
            a,
            std::format("You are an exploration agent for a plan-mode survey. "
                        "Focus on the subdomain: {}.\nContext (interview): {}\nPlan reason: {}\n"
                        "Explore the codebase read-only, identify the relevant files and summarize "
                        "what would need to change. Return a concise finding.",
                        a, m_context, reason));
    }
    for (int i = static_cast<int>(out.size()); i < count; ++i) {
        const std::string area = std::format("subdomain-{}", i + 1);
        out.emplace_back(
            area,
            std::format("You are an exploration agent #{} in a plan-mode survey. "
                        "Explore the codebase read-only from a broad perspective and identify "
                        "the critical files and blind spots relevant to the requested work.\n"
                        "Context (interview): {}\nPlan reason: {}\n"
                        "Return a concise finding.",
                        i + 1, m_context, reason));
    }
    return out;
}

void PlanCoordinator::start_explore() {
    if (m_stage != PlanStage::Interview && m_stage != PlanStage::Exploring) {
        return;  // 仅允许从 interview 或自身幂等进入探索
    }
    transition(PlanStage::Exploring);  // 阶段 2
    m_active_tasks.clear();
    m_findings.clear();
    m_artifact = PlanArtifact{};

    const auto specs = build_explore_prompts(m_reason);
    m_explore_launched = specs.size();
    m_explore_areas.clear();
    for (const auto& [area, prompt] : specs) {
        m_explore_areas.push_back(area);
        // 编号细化：#54 task_id = pa-<cycle>-<n>，跨轮全局唯一（n 为轮内序号，供回投映射子域）
        const std::string task_id = std::format("pa-{}-{}", m_plan_cycle, m_explore_areas.size());
        // 即便 runner 为空也登记进 active，便于无 runner（机械聚测）场景统一推进；
        // 无 runner 时不产生真实任务，active 会在 host 主动收尾或机械完成时清空。
        m_active_tasks.push_back(task_id);
        if (m_explore_runner) {
            m_explore_runner(task_id, area, prompt);
        }
    }

    // 无 runner（纯测试/机械模式：宿主不启动任何真实子 Agent）：
    // 模拟"所有任务即完成"，直接进入 plan 综合。
    if (!m_explore_runner) {
        complete_explore_if_all_done();
    }
}

void PlanCoordinator::on_explore_task_done(const std::string& task_id, const std::string& result,
                                           bool was_error) {
    // #54 编号细化：task_id = pa-<cycle>-<n>，解析末段序号 n 映射回本轮启动的子域
    std::string area;
    if (task_id.rfind("pa-", 0) == 0) {
        const auto last_dash = task_id.find_last_of('-');
        if (last_dash != std::string::npos && last_dash + 1 < task_id.size()) {
            const auto idx = static_cast<std::size_t>(
                std::strtoul(task_id.c_str() + last_dash + 1, nullptr, 10));
            if (idx >= 1 && idx <= m_explore_areas.size()) {
                area = m_explore_areas[idx - 1];
            }
        }
    }

    if (!was_error) {
        m_findings.push_back(ExploreFinding{
            .task_id = task_id,
            .area = area,
            .summary = result,
            .critical_files = extract_file_refs(result)  // #54 M-1：从探索结论提取关键文件
        });
    }
    m_active_tasks.erase(std::remove(m_active_tasks.begin(), m_active_tasks.end(), task_id),
                         m_active_tasks.end());
    complete_explore_if_all_done();
}

void PlanCoordinator::complete_explore_if_all_done() {
    if (!m_active_tasks.empty()) {
        return;
    }
    transition(PlanStage::Planning);  // 阶段 3
    if (m_plan_runner) {
        m_artifact = m_plan_runner(m_findings, m_context);
    } else {
        m_artifact = synthesize_mechanically();
    }
    transition(PlanStage::AwaitingApproval);  // 阶段 4
}

void PlanCoordinator::submit_plan(PlanArtifact artifact) {
    m_artifact = std::move(artifact);
    transition(PlanStage::AwaitingApproval);
}

void PlanCoordinator::set_critical_files(std::vector<std::string> files) {
    // 事件提供了结构化文件且产物缺省 → 并入产物（去重保留），供执行阶段消费
    for (auto& f : files) {
        if (std::find(m_artifact.critical_files.begin(), m_artifact.critical_files.end(), f) ==
            m_artifact.critical_files.end()) {
            m_artifact.critical_files.push_back(std::move(f));
        }
    }
}

void PlanCoordinator::set_approved(bool approved) {
    if (approved) {
        transition(PlanStage::Done);  // 阶段 5
    } else if (m_stage == PlanStage::AwaitingApproval) {
        // 驳回 → 回到 interview 修订（清空本次探索，准备重新澄清）
        transition(PlanStage::Interview);
    }
}

void PlanCoordinator::finish() { transition(PlanStage::Done); }

PlanArtifact PlanCoordinator::synthesize_mechanically() const {
    PlanArtifact a;
    a.interview_notes = m_context;
    a.findings = m_findings;
    // 聚合 critical_files：跨子域去重
    std::vector<std::string> files;
    for (const auto& f : m_findings) {
        for (const auto& file : f.critical_files) {
            if (std::find(files.begin(), files.end(), file) == files.end()) {
                files.push_back(file);
            }
        }
    }
    a.critical_files = std::move(files);
    // 摘要：逐子域拼接
    std::string summary;
    for (const auto& f : m_findings) {
        if (!summary.empty()) summary += "\n";
        summary += "[" + (f.area.empty() ? std::string("探索") : f.area) + "] " + f.summary;
    }
    if (summary.empty()) summary = "（未采集到探索结论）";
    a.summary = summary;
    a.markdown = render_markdown(a);
    return a;
}

std::string PlanCoordinator::render_markdown(const PlanArtifact& a) const {
    std::string md = "# Plan Mode V2 规划方案\n\n";
    md += "## 目标 / 约束（Interview）\n\n";
    md += a.interview_notes.empty() ? "（未收集）\n" : (a.interview_notes + "\n");
    md += "\n## 方案概述\n\n" + a.summary + "\n";
    md += "\n## Critical Files for Implementation\n\n";
    if (a.critical_files.empty()) {
        md += "（无）\n";
    } else {
        for (const auto& f : a.critical_files) {
            md += "- `" + f + "`\n";
        }
    }
    if (!a.steps.is_null()) {
        md += "\n## 实施步骤\n\n";
        if (a.steps.is_array()) {
            for (const auto& s : a.steps) {
                if (s.is_object()) {
                    md += "- " + s.value("action", std::string{}) + "：" +
                          s.value("detail", std::string{}) + "\n";
                } else if (s.is_string()) {
                    md += "- " + s.get<std::string>() + "\n";
                }
            }
        }
    }
    if (!a.risks.is_null() && a.risks.is_array() && !a.risks.empty()) {
        md += "\n## 风险\n\n";
        for (const auto& r : a.risks) {
            if (r.is_string()) md += "- " + r.get<std::string>() + "\n";
        }
    }
    return md;
}

nlohmann::json PlanCoordinator::serialize() const {
    nlohmann::json findings = nlohmann::json::array();
    for (const auto& f : m_findings) {
        findings.push_back({{"task_id", f.task_id},
                            {"area", f.area},
                            {"summary", f.summary},
                            {"critical_files", f.critical_files}});
    }
    nlohmann::json files = m_artifact.critical_files;
    return {{"stage", std::string(to_string(m_stage))},
            {"reason", m_reason},
            {"context", m_context},
            {"findings", findings},
            {"explore_launched", m_explore_launched},
            {"plan_cycle", m_plan_cycle},  // #54：跨轮编号唯一性在恢复后延续
            {"artifact",
             {{"summary", m_artifact.summary},
              {"critical_files", files},
              {"findings", findings},
              {"steps", m_artifact.steps},
              {"risks", m_artifact.risks},
              {"interview_notes", m_artifact.interview_notes},
              {"markdown", m_artifact.markdown}}}};
}

void PlanCoordinator::deserialize(const nlohmann::json& j) {
    if (!j.is_object()) return;
    m_stage = stage_from_string(j.value("stage", std::string{}));
    m_reason = j.value("reason", std::string{});
    m_context = j.value("context", std::string{});
    m_explore_launched = j.value("explore_launched", std::size_t{0});
    m_plan_cycle = j.value("plan_cycle", std::size_t{0});  // #54：延续编号唯一性
    m_findings.clear();
    if (const auto it = j.find("findings"); it != j.end() && it->is_array()) {
        for (const auto& f : *it) {
            ExploreFinding r;
            r.task_id = f.value("task_id", std::string{});
            r.area = f.value("area", std::string{});
            r.summary = f.value("summary", std::string{});
            if (f.contains("critical_files") && f["critical_files"].is_array()) {
                for (const auto& cf : f["critical_files"]) {
                    if (cf.is_string()) r.critical_files.push_back(cf.get<std::string>());
                }
            }
            m_findings.push_back(std::move(r));
        }
    }
    if (const auto it = j.find("artifact"); it != j.end() && it->is_object()) {
        PlanArtifact a;
        a.summary = it->value("summary", std::string{});
        a.interview_notes = it->value("interview_notes", std::string{});
        a.markdown = it->value("markdown", std::string{});
        a.steps = it->value("steps", nlohmann::json{});
        a.risks = it->value("risks", nlohmann::json{});
        if (it->contains("critical_files") && (*it)["critical_files"].is_array()) {
            for (const auto& cf : (*it)["critical_files"]) {
                if (cf.is_string()) a.critical_files.push_back(cf.get<std::string>());
            }
        }
        a.findings = m_findings;
        m_artifact = std::move(a);
    }
}

}  // namespace agent::plan