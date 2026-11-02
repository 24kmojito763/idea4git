# idea4git

面向 Linux、Windows 和 macOS 的低资源占用 IDEA 精简版，主要用于浏览项目结构、查看和修改代码，以及完成 IDEA 风格的 Git 图形化操作。

> 已有可本机编译运行的 **0.2 原生桌面预览版**：C++17、Qt 6 Widgets、QScintilla 和系统 Git（高级流程要求 Git 2.38+，本机为 2.43）。本版新增任意两提交比较、三方冲突解决及 merge/rebase/cherry-pick/revert/reset/stash。操作入口与边界见 [高级 Git 使用说明](docs/advanced-git.md)。

## 本机快速运行

Ubuntu 24.04 开发依赖：

```bash
sudo apt install cmake ninja-build g++ qt6-base-dev libqscintilla2-qt6-dev git
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 4
./build/idea4git /你的项目目录
```

首版具备项目树与 VCS 着色、多标签代码编辑/查找/安全保存、工作区/暂存区差异、文件级暂存/撤销暂存、提交/amend、分支新建/切换、fetch/仅快进 pull/普通 push，以及每页 100 条提交的历史列表。点击更改文件预览差异，双击打开；双击提交查看详情。设置中可在简体中文与英文之间切换，下次启动生效。

新增能力均从顶部 **Git** 菜单进入；历史列表可多选两条提交进行比较，右键单条提交可 Cherry-pick、Revert 或 Reset。合并/变基等中断后，窗口顶部显示继续、中止、跳过与解决冲突入口。具体操作及危险确认见 [八项功能说明](docs/advanced-git.md)。

尚未实现按行/块暂存、提交拓扑图和交互式 rebase；不是完整 IDEA 替代品。当前目录不是 Git 仓库时仅启用浏览/编辑，不会自动初始化或修改 Git 配置。

### 独立演示仓库

以下命令在 `build` 下生成一个真实且可操作的演示仓库，包含样例提交、已暂存/已修改/未跟踪文件。历史仅用于演示，不是本项目真实开发历史。目标目录已存在时脚本会拒绝覆盖，重复运行请选择新目录。

```bash
cmake -DDEMO_DIR="$PWD/build/idea4git-demo" -P scripts/create-demo.cmake
./build/idea4git "$PWD/build/idea4git-demo"
```

体验 0.2 高级功能可另建一个预置 merge 冲突、版本标签和 stash 的隔离仓库：

```bash
cmake -DDEMO_DIR="$PWD/build/idea4git-advanced-demo" -P scripts/create-advanced-demo.cmake
./build/idea4git "$PWD/build/idea4git-advanced-demo" --file src/palette.cpp
```

该仓库故意停在冲突处，先通过 Git 菜单解决冲突并继续，或确认中止后再体验其他操作。两种演示脚本均拒绝覆盖已有目录。

### 测试、格式化与截图

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build -j 4
ctest --test-dir build --output-on-failure
clang-format-18 -i src/*.cpp src/*.h tests/*.cpp
clang-format-18 --dry-run --Werror src/*.cpp src/*.h tests/*.cpp
QT_QPA_PLATFORM=offscreen ./build/idea4git build/idea4git-demo --screenshot build/preview-zh.png
QT_QPA_PLATFORM=offscreen ./build/idea4git build/idea4git-demo --english --screenshot build/preview-en.png
IDEA4GIT_ADVANCED_QA="$PWD/build/advanced-qa" QT_QPA_PLATFORM=offscreen ./build/idea4git_advanced_ui_tests
QT_QPA_PLATFORM=offscreen ./build/idea4git_demo_preview build/idea4git-advanced-demo conflict build/conflict-demo.png
QT_QPA_PLATFORM=offscreen ./build/idea4git_demo_preview build/idea4git-advanced-demo compare build/compare-demo.png --english
```

`clang-format-18` 是可选开发工具。测试使用临时仓库和隔离的 Git/应用配置，不访问远程仓库或改写用户全局配置。截图模式不保存窗口/最近项目偏好。普通启动会记住最近项目、窗口大小和分栏位置。

本机已验证：Ubuntu 24.04 x86_64，GCC 13.3.0，CMake 3.28.3，Qt 6.4.2，QScintilla 2.14.1，Git 2.43.0，clang-format 18.1.3。GitHub Actions 已配置 Windows 和 macOS 构建、测试与打包；首次运行成功前，这两个平台仍视为未验证。

## GitHub Releases

跨平台安装包发布后可从 [idea4git Releases](https://github.com/24kmojito763/idea4git/releases) 下载。

推送 `v` 开头的版本标签（例如 `v0.2.0`）后，GitHub Actions 会分别构建并测试 Linux、Windows 和 macOS，然后自动创建 GitHub Release 并附上平台产物：Ubuntu/Debian 的 `.deb`、Windows x64 的 `.zip`，以及当前 macOS runner 架构的 `.zip`。macOS 与 Windows 包会携带 Qt 运行库；三个平台都要求用户另行安装 Git。Linux `.deb` 由系统包管理器解析 Qt 运行时依赖。

项目许可证已确定为 **GPL-3.0-only**，仓库包含完整 `LICENSE`。Release 工作流会附上构建所用的 QScintilla 源码。macOS 包目前没有 Developer ID 签名或 notarization，Windows 包也未签名；Gatekeeper 或 SmartScreen 仍可能显示安全提示。

详细构建与发布步骤见 [Release 指南](docs/releases.md)。

### 许可与发布

本项目按 **GPL-3.0-only** 发布；开源 QScintilla 使用 GPLv3，Qt Widgets 则提供 LGPLv3/GPL 等许可路径。发布二进制时需保留许可与版权告知，并提供对应源码材料。若计划闭源发布，需另行评估 QScintilla 商业许可或更换编辑器集成方式；不能仅凭 Qt 的 LGPL 许可认定整个应用可闭源分发。详见 [依赖与限制](docs/preview.md)。

## 目标

- 控制常驻内存与启动开销，避免嵌入完整浏览器运行时；以轻量化方式提供 IDEA 用户熟悉的项目与 Git 工作流。
- 复用本机 Git，保留用户已有的 Git 配置、凭据助手、SSH 与 hooks。
- 提供项目结构/文件目录浏览，以及代码查看、基础语法高亮和编辑；不包含代码补全、符号跳转、LSP、编程语言插件、构建或调试功能。
- 项目目录树按 IDEA 的 VCS 文件状态语义显示文件/目录颜色，区分清洁（已提交）、未跟踪、已修改、已新增、已删除、已暂存、冲突和忽略等状态。
- 应用界面只内置简体中文和英文，默认简体中文；可在设置中切换并记住选择，不提供外置语言包或语言插件。
- 在 Linux、Windows 和 macOS 共用主要代码，并分别构建、打包和验证。

## 技术栈

- **语言与界面：** C++、Qt 6 Widgets
- **代码编辑/查看：** QScintilla（当前本机版本 2.14.1，开源版为 GPLv3）
- **Git 操作：** 调用系统 Git CLI
- **构建系统：** CMake
- **代码结构索引：** CodeGraph

## IDEA 对齐范围

本项目是 IDEA 的精简版，不复刻 IDEA 的所有语言开发、构建和调试能力；但在项目树、代码查看/编辑和 Git 功能范围内，以 IDEA 的交互和行为作为对齐目标。每项功能都应建立 IDEA 对照和验收用例，特别是文件状态与颜色映射、暂存状态、历史比较和冲突解决。

### 项目与代码

- 展示项目目录树和文件；根据 Git 状态为文件/目录着色，遵循 IDEA 默认 VCS 状态颜色语义。
- 打开、浏览和编辑代码文件，提供基础语法高亮；不提供补全、跳转、LSP 或编程语言插件。

### Git 操作

- 仓库状态、文件暂存/撤销暂存、按行或代码块暂存、提交与 amend
- 分支、标签、远程仓库管理；fetch、pull、push
- 提交历史图、按日期/作者/消息筛选、提交详情、blame
- 工作区/暂存区/提交之间的差异，以及任意两条 Git 记录之间的差异
- merge、rebase（包括交互式流程）、cherry-pick、revert、reset、stash
- 三方冲突查看与解决，并能明确地将解决结果标记为已解决

Git LFS、submodule、worktree、签名提交等高级场景应纳入 IDEA 对照清单并确认具体交互与验收标准。调用 Git CLI 不会自动提供 IDEA 的图形界面或完整工作流，所有操作仍需由应用实现并验证。

## 开发状态

源码位于 `src/`，自动测试位于 `tests/`，使用 CMake/CTest 和 QtTest。现有四个套件覆盖基础 Git、编辑器、八项高级 Git 工作流及实际界面按钮操作。完整规划范围仍按上文推进；功能边界见 [版本说明](docs/preview.md) 和 [高级 Git 说明](docs/advanced-git.md)。
