# Agent 层：api / command / compact / config / core / input / message / model / tool
# 定义 workx_agent 静态库（Agent Harness：ReAct 循环 + 工具运行时，宿主无关）

add_library(workx_agent STATIC)

# 导出名不带 workx_ 前缀（安装树中为 workx::agent，避免 workx::workx_agent 重复命名）
set_target_properties(workx_agent PROPERTIES EXPORT_NAME agent)

target_include_directories(workx_agent PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
    $<INSTALL_INTERFACE:include>
)

# ---- HTML → Markdown 第三方库（WebFetchTool 用，不造轮子）----
#
# P0 策略：优先使用已 vendor 的单头文件库 third_party/html2md
# （Google C++ 风格 / MIT / 纯 STL — 零额外依赖、三平台开箱即用）。
#
# 若用户想换成 leventkaragol/libcpp-html-to-md（基于 libxml2，质量更高），
# 只需打开 WORKX_FETCH_HTML2MD=ON 并保证 find_package(LibXml2) 能通过（
# vcpkg install libxml2 iconv），本文件已保留切换接口。
option(WORKX_FETCH_HTML2MD "Use FetchContent-based libxml2 html2md (requires libxml2 + iconv)" OFF)
set(WORKX_HAVE_VENDORED_HTML2MD FALSE)
set(_vendored_header "${WORKX_ROOT}/third_party/html2md/html2md.h")
if(NOT WORKX_FETCH_HTML2MD AND EXISTS "${_vendored_header}")
    message(STATUS "WebFetchTool html2md: use vendored single-header (${_vendored_header})")
    target_include_directories(workx_agent PRIVATE "${WORKX_ROOT}/third_party")
    target_compile_definitions(workx_agent PRIVATE WORKX_HAS_HTML2MD=1 WORKX_HTML2MD_VENDORED=1)
    set(WORKX_HAVE_VENDORED_HTML2MD TRUE)
elseif(WORKX_FETCH_HTML2MD)
    include(FetchContent)
    set(WORKX_HTML2MD_GIT_REPO "https://github.com/leventkaragol/libcpp-html-to-md.git" CACHE STRING "")
    set(WORKX_HTML2MD_GIT_TAG  "v1.0.0" CACHE STRING "")
    FetchContent_Declare(
        html2md
        GIT_REPOSITORY "${WORKX_HTML2MD_GIT_REPO}"
        GIT_TAG        "${WORKX_HTML2MD_GIT_TAG}"
        GIT_SHALLOW    TRUE
    )
    set(BUILD_TESTING_PREV ${BUILD_TESTING})
    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(html2md)
    set(BUILD_TESTING ${BUILD_TESTING_PREV} CACHE BOOL "" FORCE)
    find_package(LibXml2 QUIET)
    if(TARGET html2md::html2md AND LIBXML2_FOUND)
        message(STATUS "WebFetchTool html2md: via FetchContent (libxml2)")
        target_include_directories(workx_agent PRIVATE ${LIBXML2_INCLUDE_DIRS})
        target_link_libraries(workx_agent PRIVATE html2md::html2md ${LIBXML2_LIBRARIES})
        target_compile_definitions(workx_agent PRIVATE WORKX_HAS_HTML2MD=1)
    else()
        message(WARNING "WORKX_FETCH_HTML2MD=ON 但 libxml2 或 html2md 目标缺失；WebFetchTool HTML→MD 降级为 strip_tags 极简实现")
    endif()
else()
    message(WARNING "vendored html2md 头文件不存在（${_vendored_header}）；WebFetchTool HTML→MD 降级为 strip_tags 极简实现。构建完成后可放回头文件即可启用第三方路径。")
endif()

target_compile_definitions(workx_agent PUBLIC
    WORKX_VERSION="${PROJECT_VERSION}"
    WORKX_BUILD_INFO="${WORKX_BUILD_INFO}"
)
if(WORKX_FILE_VERSION)
    target_compile_definitions(workx_agent PUBLIC
        WORKX_FILE_VERSION="${WORKX_FILE_VERSION}"
    )
endif()

# 第三方依赖（PUBLIC：公共头暴露 nlohmann_json / CURL / logger）
# nlohmann_json 必选（根 CMakeLists 已 find_package REQUIRED），无条件链接
target_link_libraries(workx_agent PUBLIC
    nlohmann_json::nlohmann_json
    workx_core
    CURL::libcurl
    logger
)
# Windows：ssrf.cpp 使用 socket/inet_pton/getaddrinfo（winsock2），
# curl 的 CMake 配置不保证透传 ws2_32，需显式链接
if(WIN32)
    target_link_libraries(workx_agent PUBLIC ws2_32)
endif()
target_compile_definitions(workx_agent PUBLIC WORKX_HAS_CURL)

target_sources(workx_agent PRIVATE
    # config
    config/app_config.cpp

    # factory（宿主无关会话装配，B1：workx 与 codex 双端复用）
    factory.cpp

    # api
    api/sse_parser.cpp
    api/backend_factory.cpp
    api/client.cpp
    api/remote/remote_backend.cpp
    api/remote/sse_stream_reader.cpp
    api/remote/http_client.cpp
    api/remote/ssrf.cpp
    api/provider/openai_adapter.cpp
    api/provider/anthropic_adapter.cpp

    # command (source 实现；inclaude/ 仅头文件)
    command/source/registry.cpp
    command/source/executor.cpp

    # skill
    skill/source/frontmatter.cpp
    skill/source/skill_loader.cpp
    skill/source/conditional.cpp
    skill/source/hooks.cpp

    # hook: 通用 Hook 事件系统（#50）
    hook/hook_event.cpp
    hook/hook_match.cpp
    hook/hook_manager.cpp

    # compact
    compact/token_count.cpp
    compact/prefix_shape.cpp
    compact/cache_aware_compactor.cpp
    compact/tool_result_maintainer.cpp
    compact/sha256.cpp

    # core
    core/chat_session.cpp
    core/react_loop.cpp
    core/agent_type.cpp      # 0.6.x：AgentType 枚举 + 别名解析
    core/verdict.cpp         # 0.6.x：#31 目标验证器（verdict.h）
    core/goal_guarded_agent.cpp  # 0.6.x：#31 目标导向 Agent（goal_guarded_agent.h）
    core/agent_loop_adapters.cpp # 0.6.x：#33 IAgentLoop 适配器（ReActAdapter / GoalGuardedAdapter）
    core/query_engine.cpp    # 0.6.x：QueryEngine 编排器落地（query_engine.h）
    core/mode_agent_common.cpp   # 0.6.x：#32 多模式 Agent 公共纯函数
    core/script_agent.cpp        # 0.6.x：#32 ScriptAgent（确定性脚本）
    core/batch_agent.cpp         # 0.6.x：#32 BatchAgent（同构并行）
    core/watch_agent.cpp         # 0.6.x：#32 WatchAgent（文件/事件监控）
    core/role_agent.cpp          # 0.6.x：#33 角色 Agent（Planner/Executor/Coordinator/Researcher/Reviewer）

    # session
    session/session_store.cpp

    # model
    model/provider_preset.cpp
    model/provider_config.cpp
    model/config.cpp
    model/context_resolver.cpp
    model/model_catalog.cpp

    # tool: 通用工具
    tool/context.cpp
    tool/encoding.cpp
    tool/path_expand.cpp
    tool/path_matcher.cpp
    tool/path_validator.cpp
    tool/permission_ask.cpp
    tool/secret_scanner.cpp
    tool/line_endings.cpp
    tool/quote_normalizer.cpp
    tool/file_history.cpp
    tool/shell_guard.cpp

    # audit: 审计日志系统（#37）
    audit/audit_logger.cpp

    # tool: 各工具实现
    tool/FileReadTool/file_read_tool.cpp
    tool/FileEditTool/file_edit_tool.cpp
    tool/FileWriteTool/file_write_tool.cpp
    tool/FileWriteTool/diff.cpp
    tool/FileReadState/file_read_state.cpp
    tool/TodoStore/todo_store.cpp
    tool/TodoWriteTool/todo_write_tool.cpp
    tool/TaskTools/task_create_tool.cpp
    tool/TaskTools/task_get_tool.cpp
    tool/TaskTools/task_update_tool.cpp
    tool/TaskTools/task_list_tool.cpp
    tool/GlobTool/glob_tool.cpp
    tool/GrepTool/grep_tool.cpp
    tool/BashTool/bash_tool.cpp
    tool/PowerShellTool/powershell_tool.cpp
    tool/ShellTool/shell_detector.cpp
    tool/AgentTool/agent_tool.cpp
    tool/AskUser/AskUserTool.cpp
    tool/AskUser/ask_user_config.cpp
    tool/BriefTool/BriefTool.cpp
    tool/PlanMode/enter_plan_mode_tool.cpp
    tool/PlanMode/exit_plan_mode_v2_tool.cpp

    # plan: Plan Mode V2（#54）五阶段协调器
    plan/plan_coordinator.cpp
    tool/Task/task_output_tool.cpp
    tool/Task/task_stop_tool.cpp
    tool/MCPTool/mcp_tool.cpp
    tool/ListMcpResourcesTool/list_mcp_resources_tool.cpp
    tool/ReadMcpResourceTool/read_mcp_resource_tool.cpp
    tool/WebFetchTool/web_fetch_tool.cpp
    tool/WebSearchTool/web_search_tool.cpp
    tool/SkillTool/skill_tool.cpp

    # mcp: MCP client 基础设施（Issue #27）
    mcp/mcp_config.cpp
    mcp/mcp_stdio_process.cpp
    mcp/mcp_transport.cpp
    mcp/mcp_client.cpp
    mcp/mcp_client_manager.cpp
    mcp/mcp_oauth.cpp

    # input: 头文件（显式列入以便 IDE 导航）
    input/parser.h
    input/processor.h
    input/types.h

    # util: JSON Schema 轻量校验（#80）
    util/json_schema.cpp

    # headless: 非交互执行模式（#77）
    headless/headless.cpp
)
