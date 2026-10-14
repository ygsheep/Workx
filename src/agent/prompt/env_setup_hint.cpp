/**
 * @file env_setup_hint.cpp
 * @brief 环境自建提示实现（Issue #176 关联）
 * @version 0.1.0
 * @date 2026-10
 */

#include "agent/prompt/env_setup_hint.h"

#include <string>
#include <vector>

namespace agent::prompt {

std::string format_env_setup_hint(const std::vector<ToolchainInfo>& common_tools) {
    std::vector<std::string> missing;
    for (const auto& tool : common_tools) {
        if (!tool.found) missing.push_back(tool.name);
    }
    if (missing.empty()) return {};

    std::string listed;
    const std::size_t shown =
        missing.size() < kEnvSetupMaxListed ? missing.size() : kEnvSetupMaxListed;
    for (std::size_t i = 0; i < shown; ++i) {
        if (i != 0) listed += ", ";
        listed += '`' + missing[i] + '`';
    }
    if (missing.size() > shown) {
        listed += ", …（另有 " + std::to_string(missing.size() - shown) + " 个）";
    }

    return "## Environment setup\n"
           "This environment does not ship everything. Missing among common tools: " +
           listed +
           ".\n"
           "If the task needs any of them, install them **before** the step that depends on "
           "them, and run the install as a **background task** (`Bash`/`PowerShell` with "
           "`run_in_background=true`) so it overlaps with other work instead of blocking the "
           "run. Batch it into one non-interactive command, e.g. "
           "`DEBIAN_FRONTEND=noninteractive apt-get install -y <pkgs>` (run `apt-get update` "
           "first only if it fails). Install only what the task actually requires — do not "
           "bulk-install everything — and keep an eye on the remaining time so environment "
           "setup does not consume the whole budget.\n";
}

}  // namespace agent::prompt
