# ============================================================
# workx 主程序（FTXUI 双栏 TUI）
# 复用 workx_agent + workx_core（EventBus / ChatSession / ReActLoop）。
# ============================================================

# ---- FTXUI（vendored 源码，含 Windows 鼠标事件补丁）----
include(FetchContent)
FetchContent_Declare(
    ftxui
    SOURCE_DIR ${CMAKE_SOURCE_DIR}/third_party/ftxui
)
FetchContent_MakeAvailable(ftxui)

# 根 CMakeLists 把 /we4700-4703（MSVC 告警转错误）作为第一方代码策略，但会
# 继承到 vendored 第三方 ftxui，使其 C4702「不可达代码」等告警变成编译错误
# （screen/terminal.cpp、component/event.cpp 等）。为 ftxui 三个库目标统一
# 强制包含抑制头，在 TU 最顶端禁用这些告警，且不改动 ftxui 源码。
if(MSVC)
    foreach(_ftxui_target screen dom component)
        if(TARGET ${_ftxui_target})
            target_compile_options(${_ftxui_target} PRIVATE
                "/FI${CMAKE_SOURCE_DIR}/third_party/ftxui/suppress_msvc_warnings.h")
        endif()
    endforeach()
endif()

# ---- stb_image（vendored 单头图像解码库）----
# 实现单元 stb_image.cpp 独立编译为静态库，避免把第三方实现直接编进 workx
# 目标（会继承宿主 /we 告警策略）。MSVC 下 /FI 注入抑制头豁免 /we（同 ftxui/
# tree-sitter grammar 的处理），并顺带压掉第三方常见噪音告警。
add_library(stb_image STATIC
    ${CMAKE_SOURCE_DIR}/third_party/stb/stb_image.cpp
)
target_include_directories(stb_image PUBLIC ${CMAKE_SOURCE_DIR}/third_party/stb)
if(MSVC)
    target_compile_options(stb_image PRIVATE
        "/FI${CMAKE_SOURCE_DIR}/third_party/stb/suppress_msvc_warnings.h"
        /wd4244 /wd4100 /wd4267 /wd4456 /wd4996
    )
else()
    target_compile_options(stb_image PRIVATE
        -Wno-unused-function
        -Wno-unused-variable
    )
endif()

add_executable(workx
    main.cpp
    app.cpp
    crash_reporter.cpp
    command/builtins.cpp
    vm/message_node.cpp
    vm/view_model.cpp
    bridge/action_queue.cpp
    bridge/event_bridge.cpp
    render/markdown_to_elements.cpp
    render/code_card.cpp
    render/image_view.cpp
    render/syntax_highlight.cpp
    widgets/sidebar.cpp
    widgets/sidebar_tabs.cpp
    widgets/file_viewer.cpp
    widgets/change_viewer.cpp
    widgets/project_tree.cpp
    widgets/composer.cpp
    widgets/input_history.cpp
    widgets/status_line.cpp
    widgets/suggest_panel.cpp
    widgets/search_palette.cpp
    widgets/provider_manager.cpp
    wizard.cpp
)

# ============================================================
# workx 目标的应用图标与运行时资源打包
# 注意：add_custom_command(TARGET workx POST_BUILD ...) 必须在 workx
#       目标所在的目录调用（CMake 限制）。源/目标路径变量由根
#       CMakeLists.txt / src/CMakeLists.txt 计算并通过普通变量传递。
# ============================================================

# --- Windows：通过 RC 资源文件嵌入 ICO ---
if(WIN32 AND EXISTS "${WORKX_ICON_ICO}")
    set(WORKX_RC_FILE "${CMAKE_CURRENT_BINARY_DIR}/workx_icon.rc")
    # 使用数字 ID（1），Windows 资源管理器自动识别 exe 中的最低 ID ICON 资源
    file(WRITE "${WORKX_RC_FILE}"
        "1 ICON \"${WORKX_ICON_ICO}\"\n"
    )
    target_sources(workx PRIVATE "${WORKX_RC_FILE}")
    message(STATUS "Icon: Windows ICO embedded via RC file")
endif()

# --- macOS：设置 Bundle 图标 ---
if(APPLE)
    if(EXISTS "${WORKX_ICON_ICNS}")
        set_target_properties(workx PROPERTIES
            MACOSX_BUNDLE TRUE
            MACOSX_BUNDLE_ICON_FILE "icon.icns"
        )
        set_source_files_properties("${WORKX_ICON_ICNS}" PROPERTIES
            MACOSX_PACKAGE_LOCATION "Resources"
        )
        target_sources(workx PRIVATE "${WORKX_ICON_ICNS}")
        message(STATUS "Icon: macOS ICNS added to bundle Resources")
    else()
        message(WARNING "macOS build without icon: ${WORKX_ICON_ICNS} missing")
    endif()
endif()

# --- Linux：拷贝 PNG 到输出目录 ---
if(UNIX AND NOT APPLE AND EXISTS "${WORKX_ICON_PNG}")
    add_custom_command(TARGET workx POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${WORKX_ICON_PNG}"
            "$<TARGET_FILE_DIR:workx>/icon.png"
        COMMENT "Copying application icon: icon.png"
    )
    message(STATUS "Icon: Linux PNG copied to output dir")
endif()

# --- vendor/ripgrep：构建时拷贝到 workx 输出目录的 tools/ 子目录 ---
# 二进制由 scripts/fetch-ripgrep.py 下载到 vendor/ripgrep/<platform>/
# 运行时由 ToolRegistry 从 <exe_dir>/tools/rg 查找
# 缺失时自动回退 C++ 标准库实现，不报错
if(WORKX_HAS_VENDOR_RIPGREP)
    add_custom_command(TARGET workx POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:workx>/tools"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${WORKX_RG_SOURCE}"
            "$<TARGET_FILE_DIR:workx>/tools/${WORKX_RG_DEST_NAME}"
        COMMENT "Bundling ripgrep: ${WORKX_RG_DEST_NAME}"
    )
    target_compile_definitions(workx PRIVATE WORKX_HAS_BUNDLED_RIPGREP=1)
endif()

# --- vendor/jq：拷贝单文件 jq 到 <exe_dir>/tools/jq ---
# 由 scripts/fetch-jq.py 下载；运行时由 ToolRegistry::resolve_jq() 查找
if(WORKX_HAS_VENDOR_JQ)
    add_custom_command(TARGET workx POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:workx>/tools"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${WORKX_JQ_SOURCE}"
            "$<TARGET_FILE_DIR:workx>/tools/${WORKX_JQ_DEST_NAME}"
        COMMENT "Bundling jq: ${WORKX_JQ_DEST_NAME}"
    )
    target_compile_definitions(workx PRIVATE WORKX_HAS_BUNDLED_JQ=1)
endif()

# --- bundled skills：拷贝源码目录到 <exe_dir>/skills/bundled ---
# 运行时由 skill 加载器 find_bundled_skills_dir() 发现（与 <exe_dir>/tools 约定一致）
add_custom_command(TARGET workx POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/src/agent/skill/bundled"
        "$<TARGET_FILE_DIR:workx>/skills/bundled"
    COMMENT "Bundling built-in skills: ${CMAKE_SOURCE_DIR}/src/agent/skill/bundled"
)

# ⚠️ #108：撤销 vendored ftxui 的 SYSTEM 标记，否则系统级 ftxui 会遮蔽仓库内这份。
#
# third_party/ftxui/cmake/ftxui_set_options.cmake:53 在 FetchContent 场景下
# 用 `target_include_directories(${library} SYSTEM INTERFACE ...)` 暴露自己的
# include —— 于是 CMake 用 /external:I（MSVC）/ -isystem（GCC）传递它，而**这类目录
# 在搜索顺序上排在所有普通 -I 之后**。
# 后果：本机若装有系统/用户级 vcpkg 的 ftxui（D:/Programs/vcpkg/installed/x64-windows/
# include/ftxui，经 vcpkg 的 MSBuild 集成作为普通 /I 注入），旧版头文件就会遮蔽仓库内
# 这份 —— src/tui 随即报一堆 C2664（旧版 string_width(const std::string&) /
# text(std::string) 对不上 string_view）与 C2039（无 Cell / Screen::CellAt）。
# 上游 :40-45 的注释本身就警告了这个陷阱，只是它只给 standalone 场景做了规避。
#
# 修法：清掉 SYSTEM 标记并以普通 INTERFACE 重新暴露同一个目录，让它回到普通 -I，
# 从而压过任何包管理器注入的路径。只影响本仓库的构建（vendored 场景），不改变 ftxui 源码。
foreach(_ftxui_target screen dom component)
    if(TARGET ${_ftxui_target})
        set_property(TARGET ${_ftxui_target} PROPERTY INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "")
        # 必须用 $<BUILD_INTERFACE:> 包裹：ftxui 会 install(EXPORT) 自己的目标，
        # 裸的源码树绝对路径进 INTERFACE_INCLUDE_DIRECTORIES 会被 CMake 的导出校验拒绝
        #（“which is prefixed in the source directory” → Generate step failed）。
        target_include_directories(${_ftxui_target} INTERFACE
            $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/third_party/ftxui/include>)
    endif()
endforeach()

target_include_directories(workx PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(workx PRIVATE
    workx_agent
    workx_core
    workx_island
    stb_image
    ftxui::screen
    ftxui::dom
    ftxui::component
)

if(nlohmann_json_FOUND)
    target_link_libraries(workx PRIVATE nlohmann_json::nlohmann_json)
endif()

# ---- Tree-sitter 语法高亮（可选）----
# WORKX_TS_GRAMMAR_LIBS/DEFS 由根 CMakeLists.txt 收集（FetchContent 或 vcpkg），
# 这里统一链接到 workx 目标；WORKX_TS_<NAME> 宏用于语法高亮器按需注册 grammar。
if(WORKX_HAS_TREE_SITTER)
    target_link_libraries(workx PRIVATE ${WORKX_TS_RUNTIME_LIB} ${WORKX_TS_GRAMMAR_LIBS})
    target_compile_definitions(workx PRIVATE WORKX_HAS_TREE_SITTER=1 ${WORKX_TS_GRAMMAR_DEFS})
    message(STATUS "workx: tree-sitter syntax highlighting enabled (${WORKX_TS_GRAMMAR_LIBS})")
endif()

if(WIN32)
    add_compile_options(/utf-8)
endif()
