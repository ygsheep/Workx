# 核心基础模块：config / events / task / utils / process
# 定义 workx_core 静态库（最底层，只依赖 nlohmann_json / liblogger）

add_library(workx_core STATIC)

# 导出名不带 workx_ 前缀（安装树中为 workx::core，避免 workx::workx_core 重复命名）
set_target_properties(workx_core PROPERTIES EXPORT_NAME core)

# 公共 include 路径（src/ 作为根，所有模块以 src/ 为基准 #include）
target_include_directories(workx_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
    $<INSTALL_INTERFACE:include>
)

target_compile_definitions(workx_core PUBLIC
    WORKX_VERSION="${PROJECT_VERSION}"
    WORKX_BUILD_INFO="${WORKX_BUILD_INFO}"
)
if(WORKX_FILE_VERSION)
    target_compile_definitions(workx_core PUBLIC
        WORKX_FILE_VERSION="${WORKX_FILE_VERSION}"
    )
endif()

# 第三方依赖（PUBLIC：公共头暴露 nlohmann_json / liblogger 类型）
# nlohmann_json 必选（根 CMakeLists 已 find_package REQUIRED），无条件链接
target_link_libraries(workx_core PUBLIC
    nlohmann_json::nlohmann_json
    logger
)

# Windows：utils/file_permissions.cpp 使用 GetNamedSecurityInfoW / SetEntriesInAclW /
# SetNamedSecurityInfoW（Advapi32），这些符号不会由其它库透传，需显式链接。
if(WIN32)
    target_link_libraries(workx_core PRIVATE advapi32)
endif()

target_sources(workx_core PRIVATE
    config/config_manager.cpp
    process/sandbox/sandbox_adapter.cpp
    process/sandbox/sandbox_detector.cpp
    process/subprocess.cpp
    process/tool_registry.cpp
    task/task_manager.cpp
    todo/todo_item.cpp
    tool_kind.cpp
    utils/error.cpp
    utils/file_index.cpp
    utils/file_permissions.cpp
    utils/line_diff.cpp
    utils/uuid.cpp
    utils/path_encoder.cpp
)
