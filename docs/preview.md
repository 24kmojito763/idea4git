# 0.2 本机预览版

这是可以操作真实文件与仓库的 Qt 桌面初版，不是完整 Git 客户端。当前仅在 Linux 实机编译、运行并验证。GitHub Actions 已配置 Windows 与 macOS 的构建、测试和打包；需等首次 CI 成功后才能视为这些平台通过验证。

## 已实现范围

| 区域 | 当前能力 |
| --- | --- |
| 原生界面 | 深色工作区；可调项目树/编辑区/提交面板/底部面板；最近项目、窗口与主分栏持久化 |
| 项目树 | QFileSystemModel 懒加载；按文件名筛选；隐藏 `.git`、`.codegraph`；状态着色向已加载目录聚合 |
| 编辑器 | QScintilla；多标签；行号/折叠/基础语法高亮；查找；UTF-8（含 BOM）与 LF/CRLF；原子保存与外部更改覆盖确认 |
| 状态 | Git porcelain v1 `-z`；索引与工作区分组；同一文件可同时出现在两组；冲突、忽略与未跟踪可区分 |
| 文件级 Git | 暂存；撤销暂存（包括尚无首次提交的仓库）；提交；明确确认后的 amend |
| 差异 | 工作区与暂存区分别预览；未跟踪文本预览；提交详情/补丁；增删行着色 |
| 版本比较 | 分支、标签、哈希、HEAD~N 两两比较；重命名/增删文件列表；左右全文、行着色及统一补丁 |
| 冲突解决 | ours/base/theirs/result 四区；选择一侧全文或编辑结果；保存、暂存并标记已解决；支持缺失阶段/删除结果 |
| 高级 Git | merge、非交互式 rebase、单提交 cherry-pick/revert、soft/mixed/hard reset、stash 管理；中断后的继续/中止/跳过 |
| 历史 | 当前 HEAD 的历史列表，每页 100 条，按需加载更早记录；无拓扑连线 |
| 分支 | 查看本地分支；普通切换；新建并切换；不使用 force |
| 远程 | 默认远程的 fetch、`pull --ff-only`、普通 push；操作前确认；不更改远程地址或上游配置 |
| 语言 | 内置简体中文（默认）和英文；设置后下次启动生效；无外置语言包或插件加载 |

默认打开 `src/main.cpp`，若不存在则打开 `README.md`；不存在这些文件时，可双击左侧树中的文本文件。点击更改文件查看差异，双击打开编辑；选中文件后通过提交面板的 `+`/`−` 暂存或撤销暂存。提交只包含 Git 索引中的内容，编辑器缓冲区与尚未暂存的修改不会自动加入提交。

## 安全与资源边界

- 系统 Git 通过 QProcess 独立参数调用，不拼接 shell，不直接写 `.git`。原有 hooks、凭据助手、SSH 和 Git 配置继续生效；差异预览禁用外部 diff/textconv 程序。
- 远程操作继承仓库的默认 remote、push 范围与 refspec，不额外添加 `--force`；已有配置本身可能指定多分支或强制 refspec，执行前需自行核验。首版尚无图形化远程目标选择器。
- 路径使用 NUL 分隔，支持 UTF-8 中文、空格、换行、重命名及以 `:(...)` 开头的字面文件名；UI 中将换行显示为符号，实际命令保留原路径。当前不支持非 UTF-8 的原始路径字节。
- 字面路径环境仅作用于带显式文件选择的命令，不传给 stash 等复合命令；全局启用会干扰 Git 自己的内部 pathspec。高级工作流与风险确认见 [高级 Git 说明](advanced-git.md)。
- 非 UTF-8 编码或二进制文件拒绝编辑；文本编辑上限 5 MiB，未跟踪差异预览上限 2 MiB，单个 Git 命令输出上限 16 MiB。达到上限时停止或拒绝操作，不静默覆盖文件。
- Git 串行异步运行，可取消；普通读取超时 30 秒，写操作/远程操作超时 120 秒。取消会丢弃尚未执行的任务；已生效的 Git 操作不会自动回滚。应刷新并检查真实状态。
- 终端交互式密码输入不可用（`GIT_TERMINAL_PROMPT=0`），需预先配置凭据助手或 SSH agent；复杂认证、hooks 启动的交互程序和子进程树取消仍需后续打磨。
- 同步的短文件读写在主线程进行并受文件大小上限保护；仓库状态由 F5、保存操作和窗口重新激活刷新，没有后台定时全仓库扫描。
- 演示仓库位于独立生成目录，其样例提交不是项目开发历史；生成脚本拒绝覆盖已有路径。应用不会初始化用户目录。

## 状态与颜色验收

采用 IDEA Darcula 风格的 VCS 语义，固定深色调色板如下；尚未完成与具体 IDEA 版本/主题的逐项视觉校准，不宣称像素级一致。已暂存是独立维度，保留文件原本状态颜色，并通过分组表达。

| 状态 | 颜色 | 测试 |
| --- | --- | --- |
| 清洁/已提交 | `#bcbec4` | `statusStates(clean)` |
| 未跟踪 | `#d1675a` | `statusStates(untracked)` |
| 已修改 | `#6897bb` | `modified`、`staged-modified`、`partially-staged` |
| 已新增 | `#629755` | `statusStates(added)` |
| 已删除 | `#6c6c6c` | `deleted`、`staged-deleted` |
| 重命名 | `#32a2a8` | `statusStates(renamed)`、真实仓库重命名测试 |
| 冲突 | `#f15858` | `conflict`、`both-added`、真实合并冲突测试 |
| 忽略 | `#848504` | `statusStates(ignored)`、真实忽略目录测试 |

`git-workflows` 验证 NUL 解析、特殊路径、首次提交前撤销暂存、索引/工作区并存、真实提交与历史解析、重命名/删除/忽略、冲突状态以及异步取消/超时。

`editor-and-ui` 验证 BOM/CRLF 往返、非法与截断 UTF-8 拒绝、外部文件更改保护、重复标签、中文标准消息、语言偏好持久化、特殊文件名的 UI 打开/暂存，以及从仓库子目录通过真实按钮暂存/撤销暂存/提交/查看差异。测试不会访问远程网络。

远程服务认证、amend 的协作影响、真实项目大仓库性能和跨平台打包还没有端到端验收。当前截图只用于检查布局、字体与状态展示，不代表完整 IDEA 一致性验收。

## 尚未实现

按行/块暂存；提交拓扑图；历史搜索与 blame；远程/标签管理；交互式 rebase、rebase --onto 自定义序列、cherry-pick/revert 多提交范围批处理。三方解决器当前按文件使用全文选择/手工编辑，不提供逐冲突块自动合并；二进制、符号链接、子模块及超过大小限制的冲突保持只读，需外部工具处理。

Git LFS、submodule、签名提交、远程凭据弹窗和 hooks 的复杂交互没有专项验收。worktree 已测试独立 Git 状态目录定位及合并中止，但未覆盖完整功能/平台矩阵。不会实现补全、符号跳转、LSP、语言插件、项目构建或调试器。

## 依赖与发布限制

Linux 本机使用发行版 Qt 6.4.2 与 QScintilla 2.14.1 动态库。Qt Widgets 的发行版许可说明位于 `/usr/share/doc/libqt6widgets6t64/copyright`，QScintilla 的 GPLv3 说明位于 `/usr/share/doc/libqscintilla2-qt6-15/copyright`。Scintilla 底层的宽松许可不等于 QScintilla 封装层也可闭源使用。

项目分发许可为 GPL-3.0-only，且没有 AppImage。Windows/macOS Release ZIP 会由 CI 尝试部署 Qt 运行库；Linux Release 提供依赖系统 Qt 的 Debian/Ubuntu `.deb`。分发时仍需保留许可、对应源码和告知材料；还需在目标系统验证 Qt 插件、字体、Git 可执行程序发现和部署布局。macOS 包暂未签名或 notarize。

Windows/macOS CI 从固定版本的 QScintilla 源码构建静态库，并显式传入 `QSCINTILLA_INCLUDE_DIR`、`QSCINTILLA_LIBRARY` 和 Qt 的 `CMAKE_PREFIX_PATH`。该流程需通过首次 GitHub Actions 实跑验收；它不等同于已完成目标设备上的人工验收。
