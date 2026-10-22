# Workx 版本单一事实源 (Single Source of Truth)
# =====================================================
# 所有与版本相关的文件必须与本文件一致；升版一律使用脚本：
#   python scripts/bump_version.py <major|minor|patch>
# configure 期会校验 vcpkg.json / tests/consumer/vcpkg.json / flake.nix /
# nix/workx.nix 是否与 PROJECT_VERSION 同步，不一致直接 FATAL_ERROR。

# 版本提升规则 (SemVer, 0.x 阶段):
#   MAJOR = 0 固定；正式 1.0 前不升（0.y 阶段 MINOR 即代表不兼容变更）
#   MINOR = 不兼容变更（公共 API 破坏、行为不兼容）
#   PATCH = 兼容修复（bugfix、无 API 变化的内部优化）
set(WORKX_VERSION_MAJOR 0)
set(WORKX_VERSION_MINOR 10)
set(WORKX_VERSION_PATCH 2)

# 校验文件包含指定字面量（string(FIND) 为字面量查找，避免正则转义问题）
function(workx_check_version_file file_name needle)
    if(EXISTS "${WORKX_ROOT}/${file_name}")
        file(READ "${WORKX_ROOT}/${file_name}" _content)
        string(FIND "${_content}" "${needle}" _idx)
        if(_idx EQUAL -1)
            message(FATAL_ERROR
                "version mismatch: ${file_name} 缺少 '${needle}'\n"
                "请用 python scripts/bump_version.py <major|minor|patch> 统一升版，不要手改版本号")
        endif()
    endif()
endfunction()

# 派生构建版本号（不影响 PROJECT_VERSION，仅供二进制溯源）
# WORKX_BUILD_INFO = <PROJECT_VERSION>+<git describe>，
#   如 0.2.0+896e5be（无 v 前缀 tag）、0.2.0+0.2.0-3-gabc1234（有 tag）、...-dirty（工作区有改动）
# WORKX_FILE_VERSION = <文件级 @version 聚合>（相对最近 v-tag 的 src/core、src/agent 文件版本变化）
#   格式 "m<小改/新增数>M<大改数>"，如 m3M1；无 v-tag 或 Python 不可用时为空
# 需在 project() 之后调用（依赖 PROJECT_VERSION）
function(workx_derive_build_info)
    set(WORKX_BUILD_INFO "${PROJECT_VERSION}")
    find_package(Git QUIET)
    if(GIT_EXECUTABLE)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" describe --tags --always --dirty --match "v[0-9]*"
            WORKING_DIRECTORY "${WORKX_ROOT}"
            RESULT_VARIABLE _git_res
            OUTPUT_VARIABLE _git_desc
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        if(_git_res EQUAL 0 AND _git_desc)
            string(REGEX REPLACE "^v" "" _git_desc "${_git_desc}")
            set(WORKX_BUILD_INFO "${PROJECT_VERSION}+${_git_desc}")
        endif()
    endif()
    set(WORKX_BUILD_INFO "${WORKX_BUILD_INFO}" PARENT_SCOPE)

    # 文件级 @version 聚合统计（相对最近 v-tag，仅供溯源；统计失败则留空）
    set(WORKX_FILE_VERSION "")
    if(GIT_EXECUTABLE)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" describe --tags --abbrev=0 --match "v[0-9]*"
            WORKING_DIRECTORY "${WORKX_ROOT}"
            RESULT_VARIABLE _tag_res
            OUTPUT_VARIABLE _last_tag
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        find_package(Python3 QUIET COMPONENTS Interpreter)
        if(_tag_res EQUAL 0 AND _last_tag AND Python3_EXECUTABLE)
            execute_process(
                COMMAND "${Python3_EXECUTABLE}"
                        "${WORKX_ROOT}/scripts/version_files.py"
                        --stat --since "${_last_tag}"
                WORKING_DIRECTORY "${WORKX_ROOT}"
                RESULT_VARIABLE _fv_res
                OUTPUT_VARIABLE _fv_json
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET
            )
            if(_fv_res EQUAL 0 AND _fv_json)
                string(REGEX MATCH "\"minor\": *([0-9]+)" _fv_min "${_fv_json}")
                string(REGEX MATCH "\"major\": *([0-9]+)" _fv_maj "${_fv_json}")
                string(REGEX REPLACE ".*\"minor\": *([0-9]+).*" "\\1" _fv_min "${_fv_min}")
                string(REGEX REPLACE ".*\"major\": *([0-9]+).*" "\\1" _fv_maj "${_fv_maj}")
                if(_fv_min OR _fv_maj)
                    set(WORKX_FILE_VERSION "m${_fv_min}M${_fv_maj}")
                endif()
            endif()
        endif()
    endif()
    set(WORKX_FILE_VERSION "${WORKX_FILE_VERSION}" PARENT_SCOPE)
endfunction()
