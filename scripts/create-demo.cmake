cmake_minimum_required(VERSION 3.22)
# Generates a disposable, real Git repository. Never reuses an existing directory.
if(NOT DEFINED DEMO_DIR OR NOT IS_ABSOLUTE "${DEMO_DIR}")
    message(FATAL_ERROR "Pass an absolute, nonexistent path as -DDEMO_DIR=...")
endif()
if(EXISTS "${DEMO_DIR}")
    message(FATAL_ERROR "Refusing to overwrite existing directory: ${DEMO_DIR}")
endif()
find_program(GIT_EXECUTABLE git REQUIRED)
get_filename_component(PROJECT_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
file(MAKE_DIRECTORY "${DEMO_DIR}/docs" "${DEMO_DIR}/.demo-hooks")
file(COPY "${PROJECT_ROOT}/src" "${PROJECT_ROOT}/tests" "${PROJECT_ROOT}/CMakeLists.txt"
    DESTINATION "${DEMO_DIR}")
file(WRITE "${DEMO_DIR}/.gitignore" "/build*/\n/.demo-hooks/\n")
file(WRITE "${DEMO_DIR}/README.md"
    "# idea4git 演示仓库\n\n这是由 scripts/create-demo.cmake 生成的独立临时仓库。\n"
    "提交记录仅用于演示，不代表 idea4git 项目的实际开发历史。\n"
    "可在此安全体验编辑、暂存、撤销暂存和提交；不会修改原项目或全局 Git 配置。\n")

function(demo_git)
    execute_process(COMMAND "${GIT_EXECUTABLE}"
        -c user.name=idea4git\ Demo -c user.email=demo@example.invalid
        -c commit.gpgSign=false -c core.hooksPath=${DEMO_DIR}/.demo-hooks
        ${ARGN} WORKING_DIRECTORY "${DEMO_DIR}"
        RESULT_VARIABLE result OUTPUT_QUIET ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Demo git command failed: ${error}")
    endif()
endfunction()

demo_git(init --initial-branch=main)
demo_git(add -- .)
demo_git(commit -m "搭建 Qt 6 原生桌面工作区")
file(WRITE "${DEMO_DIR}/docs/interaction.md" "# 交互约定\n\n项目树、编辑区、Git 提交面板。\n")
demo_git(add -- docs/interaction.md)
demo_git(commit -m "确定项目树与编辑器布局")
file(APPEND "${DEMO_DIR}/docs/interaction.md" "\n文件状态遵循 IDEA 风格的颜色语义。\n")
demo_git(add -- docs/interaction.md)
demo_git(commit -m "补充文件状态与颜色规范")
file(APPEND "${DEMO_DIR}/docs/interaction.md" "\n暂存区与工作区状态分别展示。\n")
demo_git(add -- docs/interaction.md)
demo_git(commit -m "区分暂存区和工作区更改")
file(APPEND "${DEMO_DIR}/docs/interaction.md" "\n提交前预览差异，默认不执行破坏性操作。\n")
demo_git(add -- docs/interaction.md)
demo_git(commit -m "完善差异预览与提交体验")
demo_git(branch feature/editor-polish)

file(WRITE "${DEMO_DIR}/src/workspaceconfig.h"
    "#pragma once\n\nnamespace idea4git {\nstruct WorkspaceConfig {\n"
    "    bool showIgnoredFiles = false;\n    int historyPageSize = 100;\n};\n}\n")
demo_git(add -- src/workspaceconfig.h)
file(READ "${DEMO_DIR}/src/main.cpp" main_source)
string(REPLACE "    window.show();" "    // 保持原生窗口体验，按需加载仓库数据。\n    window.show();" main_source "${main_source}")
file(WRITE "${DEMO_DIR}/src/main.cpp" "${main_source}")
file(APPEND "${DEMO_DIR}/src/codeeditor.cpp" "\n// TODO: 为编辑器增加可配置的字号与缩进。\n")
file(WRITE "${DEMO_DIR}/docs/下一步.md" "# 下一步\n\n- 按行暂存\n- 交互式 rebase\n- Windows 打包验证\n")
message(STATUS "Created isolated demo repository: ${DEMO_DIR}")
