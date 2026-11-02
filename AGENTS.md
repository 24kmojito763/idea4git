# AGENTS.md

## 项目目标

idea4git 是面向 Linux、Windows 和 macOS 的低资源占用 IDEA 精简版，重点是项目结构浏览、代码查看/编辑，以及完整的 Git 图形化工作流。当前方向为 C++、Qt 6 Widgets、Scintilla 和系统 Git CLI。

在上述产品范围内，以 IntelliJ IDEA 作为行为和交互参考：项目树、文件状态颜色、Git 操作、提交记录、差异比较和冲突解决应保持一致或提供等价流程。该目标不表示复刻 IDEA 的全部平台能力；按已确认范围，不实现代码补全、符号跳转、语言服务器（LSP）、编程语言插件、构建或调试器功能。技术选型在实现前仍需核验依赖许可、打包方式和跨平台可行性。

应用界面仅内置简体中文（默认）和英文两种语言；用户可在设置中切换，选择需持久化并在后续启动时生效。不实现外置语言包或语言插件系统。此限制针对应用界面，不限制仓库文件、代码注释或 Git 提交信息所使用的语言。

## 实现原则

- 通过系统 Git CLI 执行 Git 操作，不直接修改 `.git` 内部数据，也不自行重写 Git 语义。
- 对齐 IDEA 的项目目录树和 Git 文件状态标识：清洁/已提交、未跟踪、已修改、已新增、已删除、已暂存、冲突、忽略等状态须有明确映射；颜色语义遵循 IDEA 默认 VCS 文件状态颜色，并为每种状态建立验收用例。
- 完整覆盖目标范围内的 Git 图形化操作，包括暂存/撤销暂存（文件、代码块及行）、提交/amend、推送/拉取/fetch、分支/标签、合并/rebase、cherry-pick、revert/reset、stash、提交记录浏览和任意版本差异比较，以及冲突解决。
- 冲突解决需呈现可理解的三方内容（ours/base/theirs/result），并提供保存、暂存和标记已解决的明确操作。
- 通过 `QProcess` 等进程 API 传递独立参数，避免拼接 shell 命令；长时间操作不得阻塞 UI，并应处理取消、退出码及标准输出/错误。
- 保留用户已有 Git 配置、凭据助手、SSH、hooks 和仓库设置；涉及破坏性操作时明确展示目标和后果。
- Git 输出可能包含任意路径、空格、换行和非 ASCII 字符；解析与展示时不能假设路径简单或输出固定语言。
- 提交历史、差异和仓库状态应按需加载，避免默认扫描整个仓库或一次性载入全部历史。
- 跨平台代码优先使用 Qt 抽象；平台专用行为封装在明确边界内。

## CodeGraph

- 结构性代码问题优先使用 CodeGraph MCP：查符号用 `codegraph_search` / `codegraph_context`，追踪调用用 `codegraph_context` 后接一次 `codegraph_explore`。
- 不用 grep、find 或 Read 按符号名查找；只在查询字面文本或核验 CodeGraph 未覆盖的具体细节时使用文本工具。
- 不连续调用 `codegraph_node` 超过 3 次；多个符号优先合并到一次 `codegraph_explore`。
- 不信任置信度低于 0.7 的推测关系，需核验具体调用点。
- 文件变更后等待索引 watcher 同步；发现索引落后于 Git 或工作区状态时，先同步索引再依据其结果回答。

## 当前状态

v1.0.0 是首个跨平台发行目标，基于 C++17 / Qt 6 Widgets / QScintilla，支持代码编辑、文件级 Git、任意两提交比较、三方文本冲突解决及 merge/rebase/cherry-pick/revert/reset/stash。修改高级 Git 或冲突流程时先读 `docs/advanced-git.md`（安全确认、状态恢复和验收覆盖）；整体边界见 `docs/preview.md`。按行暂存、拓扑图、交互式 rebase 尚未实现。GitHub Actions 的三平台发布流程见 `docs/releases.md`；Windows/macOS 的首次 Release 构建与测试成功后才视为 CI 已验证，且 CI 不替代所有系统版本和 CPU 架构的手工 QA。

- 构建：`cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON`，然后 `cmake --build build -j 4`。
- 测试：`ctest --test-dir build --output-on-failure`。测试配置已设置 offscreen，使用独立临时仓库与配置，不得改用用户仓库运行写入测试。
- 运行：`./build/idea4git /absolute/project/path`。演示仓库生成方式见 `README.md`，不得在原项目中自动 `git init`。
- 发布：在 GitHub 推送 `v*` 版本标签会触发 Linux、Windows、macOS 构建与 Release 上传；发布前许可要求及平台限制见 `docs/releases.md`。
- 格式化：`clang-format-18 -i src/*.cpp src/*.h tests/*.cpp`；检查使用相同文件参数加 `--dry-run --Werror`。
- 本机验证版本：Qt 6.4.2、QScintilla 2.14.1、GCC 13.3.0、CMake 3.28.3、Git 2.43.0、clang-format 18.1.3。
- QScintilla 开源版为 GPLv3；本项目按 GPL-3.0-only 发布。分发时保留许可、对应源码和告知材料；不可按 Qt 的 LGPL 许可推断整体许可。
- 高级 Git 通过 `GitOperations` 的预览计划/执行边界实现；界面在 `gitdialogs.*` 和 `mainwindow_git.cpp`。显式文件命令才启用字面 pathspec，勿全局设置 `GIT_LITERAL_PATHSPECS`，否则会破坏 stash 的内部清理。
