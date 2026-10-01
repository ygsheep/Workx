/**
 * @file python_command.h
 * @brief 测试用 Python 解释器命令名（跨平台）
 * @details 多个测试的"假服务器"都是 .py 脚本（fake_mcp_server.py / fake_http_mcp_server.py /
 *          fake_oauth_server.py），需要起子进程执行。解释器名在两大平台上不统一：
 *            · Windows：官方安装包只注册 `python`（`python3` 通常不存在，除非装了
 *              Microsoft Store 别名）
 *            · Linux / macOS：发行版只提供 `python3`；Ubuntu 22.04+ 未装
 *              python-is-python3 时 `python` 根本不存在
 *          此前各测试文件硬编码 "python"，在 Linux CI 上 execvp 直接失败，症状却是
 *          "读不到 PORT= 行 / 连接超时" 一类，极易被误判成产品缺陷（#104）。
 *
 * 使用示例：
 * @code
 *   #include "helpers/python_command.h"
 *   using namespace agent::test;
 *   proc->start(python_command(), {script_path}, {});
 *   cfg.command = python_command();
 * @endcode
 *
 * @note 需要覆盖解释器（如 CI 上用某虚拟环境）时设环境变量 WORKX_TEST_PYTHON。
 * @version 1.0.0
 * @date 2026-10
 */

#pragma once

#include <cstdlib>
#include <string>

namespace agent::test {

/// @brief 当前平台可用的 Python 解释器命令名
/// @return WORKX_TEST_PYTHON（若设置且非空）> Windows: "python" > 其他: "python3"
inline std::string python_command() {
    if (const char* override = std::getenv("WORKX_TEST_PYTHON"); override && *override) {
        return override;
    }
#ifdef _WIN32
    return "python";
#else
    return "python3";
#endif
}

}  // namespace agent::test
