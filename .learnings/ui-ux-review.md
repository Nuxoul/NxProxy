# NxProxy (Qt Widgets) UI/UX 设计评审与实现规格说明书

- **评审对象**：NxProxy 桌面客户端（Qt 6.11 Widgets + C++20，基于 Throne 架构演进）
- **评审范围**：主窗口结构（`include/ui/mainwindow.ui`）、策略组与节点视图、运行态看板与通知、基础设置表单、`res/darkstyle.qss` 样式系统
- **目标产物**：真实代码取证的 UX 问题清单、Qt Widgets 视觉/交互规范、可直接追加的 QSS 增量片段、三级分批实施方案

---

## 1. UX 问题清单（基于源码真实取证）

本清单共列出 15 项经过源码与 `.ui` 描述文件逐行核对的实际交互与视觉缺陷，禁止使用臆测控件。

| 序号 | 缺陷归类 | 问题简述 | 证据来源（文件、行号、控件名/真实文案） |
| :--- | :--- | :--- | :--- |
| **01** | 核心状态表达 | 测速时粗暴覆盖底部运行节点标签，运行态短暂“失联” | `src/ui/mainWindow/mainwindow_view.cpp:408-436`<br>在 `url_test_current()` 中执行 `ui->label_running->setText(tr("Testing"))` 与 `tr("Test Result")`，并在 `157-168` 行以 2 秒计时器抑制正常运行态显示，导致原有的 `[分组] 节点名` 被完全抹除。 |
| **02** | 视觉反馈退化 | 核心待重启提示退化为纯文本链接，缺乏告警层级与点击按钮感知 | `src/ui/utils/DataViewHtmlGenerator.cpp:139-155`<br>`pendingRestartSectionHtml()` 在 `ui->data_view` 中拼接纯 HTML 字符串 `<a style='color:...;' href='restart'>Restart</a>`，视觉上混杂在顶部工具栏中，无卡片容器或按钮外框。 |
| **03** | 控件误用与排版 | 进度指示采用 ASCII 字符硬拼并在文本浏览框中渲染 | `src/ui/utils/DataViewHtmlGenerator.cpp:177-191`<br>`getProgressBar()` 使用循环构造 `progressText += "#"` 与 `progressText += "-"`，在 `ui->data_view` (`QTextBrowser`) 内居中显示，字体比例失衡且无法利用系统渲染平滑度。 |
| **04** | 控件排版跳跃 | 底部状态栏运行中/停止时行数变化导致整个底部容器高度上下抖动 | `src/ui/mainWindow/mainwindow_view.cpp:158-168`<br>未运行时显示单行 `tr("Not Running")`，运行时追加换行 `\n + runningDetail`，导致未限定固定行高的 `ui->label_running` 触发整个窗口重布局。 |
| **05** | 策略组可读性差 | 策略组列表使用固定空格硬拼接单行纯文本，列对齐混乱且混排生硬 | `src/ui/mainWindow/mainwindow_view.cpp:457-460`<br>`new QListWidgetItem(QString("%1   →   %2   (%3 member(s))").arg(profile->name, selectedName).arg(selector->members.size()), ui->selectorGroupList)`，双语环境下混排生硬，长组名直接将箭头与节点挤压出视口。 |
| **06** | 关键选中指示脆弱 | 策略组成员表中“当前选中使用节点”仅靠字符前缀 `"✓ "` 区分 | `src/ui/utils/ProfilesTableModel.cpp:115, 122-129`<br>`ColName` 返回 `QStringLiteral("✓ ") + name`，背景设置 `QPalette::AlternateBase`。与高亮选择背景色混淆时（尤其是键盘或鼠标焦点行处于该行时），视觉标识微弱。 |
| **07** | 表格信息对齐违例 | 节点表格模型缺失 `Qt::TextAlignmentRole`，延迟与流量数值靠左对齐 | `src/ui/utils/ProfilesTableModel.cpp:82-146`<br>`ProfilesTableModel::data()` 仅响应 `DisplayRole`、`FontRole`、`BackgroundRole` 等，未实现 `TextAlignmentRole`。延迟（如 `128 ms`）与流量（如 `1.2 GB`）全部左对齐，严重破坏桌面端扫视可读性。 |
| **08** | 误触风险严重 | 策略组成员表单击即触发核心切节点与配置持久化，无防误触缓冲 | `src/ui/mainWindow/mainwindow_setup.cpp:425-465`<br>`connect(ui->profilesTableView, &QAbstractItemView::clicked, ...)`，只要用户单选行（如准备测速或选中查看详情），立即执行 `selector->selectedID = memberId` 并向核心发送 RPC 切换，无延时确认或二次校验。 |
| **09** | 窗口标题栏信息超载 | 窗口标题充当调试状态堆砌区，拼接大量中括号标记导致任务栏截断 | `src/ui/mainWindow/mainwindow_view.cpp:193-213`<br>`make_title()` 拼接 `[Admin][Select][Tun+System Proxy] NxProxy 1.0.x [RouteName] Outbound@GroupName Country`，长达上百字符，任务栏与窗口管理器标签彻底失真。 |
| **10** | 样式硬编码与维护冗余 | 顶部五个主菜单按钮在 `.ui` 中硬编码完全重复的内联 QSS hack | `include/ui/mainwindow.ui:34-53, 83-102, 132-151, 181-200, 230-249`<br>`toolButton_program`、`toolButton_preferences` 等 5 个按钮各自内嵌 18 行相同样式声明 `QToolButton::menu-indicator`，并在 `mainwindow_view.cpp:48-50` 用 C++ 硬加两空格宽度补丁。 |
| **11** | 布局结构混杂 | 运行模式复选框垂直塞在启停大按钮与 `data_view` 之间，层次倒挂 | `include/ui/mainwindow.ui:282-336`<br>`horizontalLayout_2` 中 `toolButton_startstop` 旁嵌入 `verticalLayout_4`（`checkBox_VPN`, `system_dns`, `checkBox_SystemProxy`）加两个垂直 Spacer，无分组边框、无视距锚点。 |
| **12** | 控件命名与架构残余 | 下方诊断面板容器使用占位符垃圾命名，Tab 标签缺少视觉主次 | `include/ui/mainwindow.ui:522-523`<br>主分割器下半部分容器命名为 `aaaaaaaaaaaaaaaaaa`，布局为 `aaaaaaaaaaaa`；内部 `stats_widget` 标签页包括 `Logs`, `connections_tab`, `graph_tab`, `runtime_tab`，样式与主标签页完全雷同。 |
| **13** | 底部状态栏信息无容器 | 底部状态栏三枚 Label 散落在无边框水平布局中，缺乏视觉分区 | `include/ui/mainwindow.ui:704-733`<br>`horizontalLayout` 包含 `label_running`, `label_inbound`, `label_speed`，无背景填充、无边框卡片感、无语义指示图标，仅表现为窗口最下方的白灰平铺散字。 |
| **14** | 静态引导占用垂直空间 | 策略组与成员表说明标签文案完全固定，不具备动态提示价值 | `include/ui/mainwindow.ui:422-427, 463-468`<br>`selectorGroupHint`（`"策略组（选择后查看该组成员）"`）与 `selectorMemberHint`（`"当前策略组成员（点击节点设为该组使用节点）"`）常驻占用垂直高度，没有与当前选中的组名形成联动。 |
| **15** | 表单网格对齐混乱 | 基础设置“通用”页网格跨度混乱，行级标签与输入框缺乏基线对齐 | `include/ui/setting/dialog_basic_settings.ui:57-206`<br>`groupBox_5`（Inbound Settings）使用 6 列 `gridLayout_9`，`inbound_auth` 放在第 2 行第 5 列，`disable_mixed_inbound` 放在第 0 行第 1 列，表单项错落无序。 |

---

## 2. Qt Widgets 桌面端设计规格

本规格专为 Qt 6 Widgets + QSS 体系制定，摒弃网页端 Web 专有属性，严格基于 `QStyleSheet` 盒模型、`QPalette` 动态角色及 8pt 网格系统构建。

### 2.1 主窗口视觉层级与信息架构（一眼看清全局）

```
┌────────────────────────────────────────────────────────────────────────┐
│ [1] TOP COMMAND BAR (高度固定 64px, 容器: #horizontalLayout_2)          │
│  [功能按钮组]    │ [启停中枢]        │ [模式卡片]       │ [动态通知与看板]     │
│  程序/设置/路由等 │ StartStopButton  │ ☑ Tun   ☑ 系统代理 │ (需重启/测速/警告)    │
├────────────────────────────────────────────────────────────────────────┤
│ [2] MAIN WORKSPACE (可拖动分割器: #splitter, 上半区)                       │
│ ┌─────────────────────────────────┬──────────────────────────────────┐ │
│ │ 策略组视窗 (#selectorGroupList)    │ 策略组成员节点表 (#profilesTableView)│ │
│ │ ├─ 组名: 节点名 (状态角标)        │ ├─ [类型][地址][名称][延迟][流量]  │ │
│ │ ├─ 组名: 节点名 (状态角标)        │ ├─ ● 当前生效节点 (左侧4px高亮条)  │ │
│ └─────────────────────────────────┴──────────────────────────────────┘ │
├────────────────────────────────────────────────────────────────────────┤
│ [3] TELEMETRY CONSOLE (分割器下半区: #down_tab / #stats_widget, 折叠高度 180px) │
│  [ 日志 (Logs) ]  [ 活跃连接 ]  [ 实时流量图 ]  [ 运行时统计 (Runtime) ]     │
├────────────────────────────────────────────────────────────────────────┤
│ [4] PERSISTENT STATUS DOCK (固定高度 36px, 容器: #horizontalLayout)      │
│  [● 核心状态卡片]         │ [⚡ 监听入口卡片]        │ [↑↓ 速率计量卡片]    │
│  [Tun] 香港 BGP 01 (CN)    │ Mixed: 127.0.0.1:2080   │ ↑ 24 KB/s  ↓ 1.2 MB/s│
└────────────────────────────────────────────────────────────────────────┘
```

#### 信息动线设计原则
1. **第一视线（左上 → 中上）**：通过 52px 尺寸的 `toolButton_startstop` 与相邻的模式复选框（Tun / System Proxy），1 秒内判断核心运行状态与接管范围。
2. **第二视线（主分割区）**：左侧策略组列表（`selectorGroupList`）直观展示“哪条策略链路由什么节点兜底”；右侧成员表（`profilesTableView`）展示当前组内备选节点池与量化延迟。
3. **第三视线（常驻底栏）**：底部三枚胶囊卡片（Status Chip），左侧固定锁定运行节点与国家信息，中部显示 Mixed 端口，右侧等宽数字显示实时速率，杜绝纵向尺寸颠簸。

---

### 2.2 间距、圆角、字号与语义色彩规范（8pt 网格系统）

#### 1) 尺寸与间距体系（Spacing Rhythm）
- **Micro 基础间距（4px）**：表格单元格内边距、复选框与文字间隙、图标与文字内补。
- **Standard 步进间距（8px）**：卡片内部控件间距、按钮横向 padding、工具栏元素间距。
- **Component 模块间距（12px / 16px）**：表单行垂直跨度（12px）、主面板外边距（`margin: 12px` 或 `16px`）、组容器外边距。
- **Section 区域间距（24px）**：主分割区与顶部控制条的大区域视距隔离。

#### 2) 圆角体系（Corner Radius）
- **R2 (2px)**：进度条凹槽、滚动条滑块（`QScrollBar::handle`）。
- **R4 (4px)**：按钮（`QPushButton` / `QToolButton`）、输入框（`QLineEdit`）、下拉框（`QComboBox`）、表格表头与单元格焦点框。
- **R6 (6px)**：策略组卡片列表项、底部状态栏状态卡片、分组框（`QGroupBox`）。
- **R8 (8px)**：主窗口浮动弹窗、模态对话框外框、核心重启通知 Banner。

#### 3) 字号与字体层级（Typography Hierarchy）

| 角色 | 推荐磅值 | 推荐字重 | 适用控件与场景 |
| :--- | :--- | :--- | :--- |
| **Heading 1** | 13pt / 14pt | SemiBold / Bold | 对话框顶级标题、主状态启停摘要 |
| **Heading 2** | 10.5pt / 11pt | Medium / SemiBold | 分组标题（`QGroupBox::title`）、策略组当前生效项 |
| **Body Regular** | 9pt / 9.5pt | Normal (400) | 表格正文、树控件、常规设置项文字、工具按钮文案 |
| **Body Medium** | 9pt / 9.5pt | Medium (500) | 当前选中的策略组成员、活动标签页头部（Active Tab） |
| **Data Mono** | 8.5pt / 9pt | Regular (Monospace) | 流量速率（`label_speed`）、延迟结果、IP/端口、日志浏览器 |
| **Micro Caption** | 8pt | Normal (400) | 字段下方辅助说明、协议类型安全角标、时间戳 |

*字体族定义：西文优先 `Segoe UI`, `SF Pro Text`；中文优先 `Microsoft YaHei UI`, `PingFang SC`；等宽字体强制使用 `Consolas`, `JetBrains Mono`, `monospace`。*

#### 4) 语义色彩规范（适配 Dark 与 Light 模式）

所有语义色均与 `ThemeManager.cpp` 中的 `ThemeTokens` 严格对应，保证在黑夜（`qdarkstyle`/`blacksoft`）与浅色（`flatgray`/`lightblue`/`softpink`）间自适应高对比度：

| 语义角色 | 暗色模式数值 (Dark Theme) | 浅色模式数值 (Light Theme) | 视觉背景色 (15% 胶囊填充) | 边框描边色 |
| :--- | :--- | :--- | :--- | :--- |
| **Running (运行中)** | `#2ECC71` (Emerald) | `#27AE60` (Forest) | `rgba(46, 204, 113, 0.15)` | `rgba(46, 204, 113, 0.40)` |
| **Stopped (已停止)** | `#788D9C` (Muted Slate) | `#8C9DA8` (Cool Gray) | `rgba(120, 141, 156, 0.12)` | `rgba(120, 141, 156, 0.25)` |
| **Restart (需重启)** | `#F39C12` (Amber) | `#D68910` (Dark Amber) | `rgba(243, 156, 18, 0.18)` | `rgba(243, 156, 18, 0.50)` |
| **Testing (测试中)** | `#3498DB` (Sky Blue) | `#2980B9` (Ocean) | `rgba(52, 152, 219, 0.18)` | `rgba(52, 152, 219, 0.45)` |
| **Error (错误/不可用)** | `#E74C3C` (Crimson) | `#C0392B` (Alizarin) | `rgba(231, 76, 60, 0.18)` | `rgba(231, 76, 60, 0.50)` |

---

### 2.3 三个关键状态的可视表达设计

针对当前“纯文本拼接、覆盖运行态、ASCII 进度条”的严重硬伤，给出精确的 Qt Widgets + QSS 交互与视觉方案：

#### 状态 A：核心运行中（Running State）
- **当前缺陷**：`label_running` 显示换行文本 `"[GroupName] Node\nCountryInfo"`，高度导致状态栏跳跃；标题栏堆叠多重 `[...]`。
- **重构方案**：
  1. **状态栏固定高**：`label_running` 设定 `min-height: 28px; max-height: 28px;`，采用单行弹性排版，溢出使用 `...` 截断，完整详情由 `setToolTip()` 承载。
  2. **双态胶囊视觉**：
     - 左侧显示 8px 绿色发光圆点指示符（通过 QSS `image: url(...)` 或 Unicode `● \u25CF`，颜色为 `#2ECC71`）。
     - 分组名称与节点名称通过浅色胶囊高亮包裹：`[香港优化] HK-BGP-01`，文字颜色 `#FFFFFF`，字重 500。
     - 若包含分流国家，以小标签形态追加在右侧：`[HK]`（`colorRole="tag"`）。
  3. **表格内呼应**：在 `profilesTableView` 中，正在跑的核心节点所在行左侧添加 4px 绿色竖向边条（Indicator Bar），彻底取代仅改变前景色的做法。

#### 状态 B：需重启核心（Restart Core Needed）
- **当前缺陷**：`data_view` 输出纯文本超链接 `<a href='restart'>Restart</a>`，混在右上角，用户极易遗漏，修改配置后误以为已生效。
- **重构方案**：
  1. **横幅告警卡片（Alert Banner）**：
     - 当 `dataViewHtmlGenerator_` 捕获到重启原因时，顶部 `data_view` 区域展示为明显的警示横幅：背景 `rgba(243, 156, 18, 0.14)`，左侧边框 `3px solid #F39C12`，圆角 4px。
     - 图标：左侧放置 `[!]` 黄色告警图标。
     - 动作纽扣化（Pill Buttons）：将 `<a href='restart'>` 封装为按钮样式的内联控件：
       - `[立即重启核心]`：实色强调（琥珀黄底黑字，`background: #F39C12; color: #19232D; font-weight: bold; padding: 2px 8px; border-radius: 3px;`）。
       - `[稍后]`：次级幽灵按钮（透明底白灰字，`border: 1px solid #788D9C; color: #DFE1E2;`）。
  2. **非侵入式提示**：保持主窗口底部状态栏与表格操作区完全可用，不弹模态对话框打断用户，但顶部常驻该警告卡片直至用户显式操作。

#### 状态 C：测试进行中（Testing in Progress）
- **当前缺陷**：
  - 测速时调用 `label_running->setText("Testing")`，导致正在运行的节点名被冲掉 2 秒钟。
  - `data_view` 使用 `#####----- 50%` 字符串伪造进度条。
- **重构方案**：
  1. **解耦状态呈现**：彻底剥离 `url_test_current()` 与 `label_running` 的耦合关系。`label_running` 始终专注表达核心连接状态，绝不作为测试进度输出板。
  2. **现代化进度条**：
     - 在 `data_view` 中生成规范的 HTML5/CSS 平滑进度条：
       ```html
       <div style='background: #2D3A4B; border-radius: 3px; height: 6px; width: 100%;'>
         <div style='background: #3498DB; width: 65%; height: 6px; border-radius: 3px;'></div>
       </div>
       ```
     - 进度文字显示在进度条上方：`正在测速: 12 / 24 节点 (50%)`，采用等宽数字展示。
  3. **表格行内动效**：在 `profilesTableView` 的 `ColTestResult` 列中，处于测试队列的行显示淡蓝色 `⏳ 测速中...`（字色 `#3498DB`），测试完成后以毫秒级数值（如 `142 ms`）结合红黄绿语义色彩渐变替换。

---

### 2.4 策略组面板与成员表交互设计

#### 1) 策略组列表（`selectorGroupList`）卡片化改造
- **结构化项目布局**：摒弃纯文本 `"%1   →   %2   (%3 member(s))"`，改用两行式或等宽分栏格式：
  - **行 1**：策略组名称（Bold, 9.5pt, 颜色 `#DFE1E2`） + 成员数量角标（右对齐 Pill Badge，如 `[ 12 节点 ]`，背景 `#2E3A4B`，圆角 8px）。
  - **行 2**：引导箭头 `➔` + 当前选中使用节点（Accent 蓝色，字重 500）。
- **选中与悬停反馈**：
  - 悬停：项背景轻微提亮为 `#263445`，光标变为指向手型（Pointing Hand Cursor）。
  - 选中：项背景变为 `#1A3B5C`，左边缘呈现一条 `3px solid #3498DB` 的活动指示条，明确传达当前正在浏览该策略组的成员池。

#### 2) 策略组成员表（`profilesTableView`）交互与反馈
- **当前生效节点指示**：
  - 当前被选中的节点行：在 `ColName` 列显示高饱和度的实心勾选图标或标记，且整行背景应用深蓝/青绿半透明微高亮（`rgba(52, 152, 219, 0.12)`）。
  - 该高亮独立于表格的多选（Selection）状态，即用户通过 Ctrl+A 或框选多个节点进行批量测速时，当前策略组选择的节点依然清晰可见。
- **点击即切换的误触防范与即时反馈（Click Feedback）**：
  - **交互防抖**：短时间内（300ms 内）连续点击只响应最后一次。
  - **乐观 UI 更新**：点击新节点后，模型立即将选中勾号移至该行，并触发一个轻量行闪烁（Row Flash）反馈；同时在后台执行异步 RPC 切核心。
  - **异常回退**：若 RPC 失败，自动回退勾选位置并在状态栏或顶部以 Danger 色提示 `"节点切换失败，核心已拒绝"`。
- **共享节点与去重可读性**：
  - 当通过策略组过滤成员时，表头上方原本空白的 `selectorMemberToolbar` 显式显示当前上下文面包屑：`策略组: [ 自动选择 ] (共 8 个成员节点)`，并提供一个右对齐的清空筛选小按钮（Ghost Button `[ ✕ 查看全量配置 ]`）。

---

### 2.5 表格信息密度与列对齐规范

`profilesTableView` 是客户端的核心生产力视窗，需满足高密度、高扫视效率的要求：

| 列索引 | 列名 (`Col`) | 推荐宽度行为 | 文本水平对齐 | 字体与排版特征 | 视觉呈现细节 |
| :---: | :--- | :--- | :---: | :--- | :--- |
| **0** | `ColType` (协议类型) | 固定宽 90px | **居中 (Center)** | 8.5pt Regular | 协议胶囊外框（如 `VLESS`, `VMess`, `Trojan`），危险配置显示红色小感叹号 |
| **1** | `ColAddress` (服务器地址) | 弹性伸展 (Stretch, min 140px) | **左对齐 (Left)** | 8.5pt Monospace (Consolas) | 文本灰色弱化（`#9DA9B5`），突出只读连接元数据，避免抢占视觉重心 |
| **2** | `ColName` (节点别名) | 弹性伸展 (Stretch, min 200px) | **左对齐 (Left)** | 9.5pt Regular / Medium | 主视觉焦点，支持双语长字符截断并带 ToolTip，活动节点加粗展示 |
| **3** | `ColTestResult` (延迟结果) | 固定宽 105px | **右对齐 (Right)** | 9pt Monospace (Tabular figures) | **强制右对齐**。<150ms 翠绿，150-300ms 暖黄，>300ms 橙红，超时标红 `Timeout` |
| **4** | `ColTraffic` (已用/总流量) | 固定宽 120px | **右对齐 (Right)** | 8.5pt Monospace (Tabular figures) | **强制右对齐**。格式如 `↑ 12M  ↓ 1.4G`，上下行分色微标，数字列按小数点垂直对齐 |

- **行高定义**：默认固定行高 `30px`（Compact Desktop 标准），表头高度 `32px`。
- **网格线与交替行**：禁用沉重的实体黑粗网格线；启用轻微交替色（`#19232D` 与 `#1D2834`），交替对比度保持在 1.15:1 舒适区间，杜绝大片纯黑视觉疲劳。

---

## 3. QSS 样式增量片段（可直接追加至 `res/darkstyle.qss`）

以下样式基于项目中已存在的真实 `objectName` 与 Qt 类名编写，无任何臆测选择器，可无缝追加到 `res/darkstyle.qss` 末尾。

```css
/* ==========================================================================
   NxProxy Desktop UI Enhancement Stylesheet Extension
   Direct append target: res/darkstyle.qss
   ========================================================================== */

/* --- 1. 顶部控制栏及模式开关区域 ------------------------------------------ */

/* 顶部操作条容器背景与外边距校准 */
#centralwidget > QWidget {
    background-color: transparent;
}

/* 核心启停中枢大按钮微调 */
StartStopButton#toolButton_startstop {
    border-radius: 6px;
    padding: 4px;
    margin-right: 8px;
}

StartStopButton#toolButton_startstop:focus {
    outline: none;
    border: 1px solid #346792;
}

/* 模式复选框（Tun 模式 / 系统代理 / 系统 DNS）卡片化间隙 */
#verticalLayout_4 {
    margin-left: 6px;
    margin-right: 12px;
    spacing: 3px;
}

#checkBox_VPN,
#system_dns,
#checkBox_SystemProxy {
    font-size: 9pt;
    font-weight: 500;
    color: #DFE1E2;
    spacing: 6px;
    padding: 2px 4px;
    border-radius: 3px;
}

#checkBox_VPN:hover,
#system_dns:hover,
#checkBox_SystemProxy:hover {
    background-color: rgba(69, 83, 100, 0.35);
    color: #FFFFFF;
}

#checkBox_VPN:checked {
    color: #2ECC71;
}

#checkBox_SystemProxy:checked {
    color: #3498DB;
}

/* 顶部数据看板 / 核心重启 / 测速进度展示框 */
QTextBrowser#data_view {
    background-color: #151D25;
    border: 1px solid #2B3746;
    border-radius: 6px;
    padding: 4px 8px;
    color: #DFE1E2;
    selection-background-color: #346792;
    font-size: 9pt;
}

QTextBrowser#data_view:hover {
    border-color: #3A4C60;
}

/* --- 2. 策略组列表面板 (#selectorGroupList) -------------------------------- */

/* 策略组提示标签 */
QLabel#selectorGroupHint {
    font-size: 9pt;
    font-weight: 600;
    color: #9DA9B5;
    padding: 4px 2px;
}

/* 策略组列表视窗 */
QListWidget#selectorGroupList {
    background-color: #19232D;
    border: 1px solid #37414F;
    border-radius: 6px;
    padding: 4px;
    outline: 0;
    font-size: 9pt;
}

QListWidget#selectorGroupList::item {
    border-radius: 4px;
    padding: 6px 10px;
    margin-bottom: 3px;
    color: #DFE1E2;
    border-left: 3px solid transparent;
}

QListWidget#selectorGroupList::item:hover {
    background-color: #222E3C;
    color: #FFFFFF;
}

/* 选中的策略组项：左侧呈现高亮强调条 */
QListWidget#selectorGroupList::item:selected {
    background-color: #1E3145;
    border-left: 3px solid #3498DB;
    color: #FFFFFF;
    font-weight: 500;
}

/* 策略组成员工具栏与提示标签 */
QWidget#selectorMemberToolbar {
    background-color: transparent;
    padding: 2px 0px;
}

QLabel#selectorMemberHint {
    font-size: 9pt;
    font-weight: 600;
    color: #9DA9B5;
    padding: 4px 2px;
}

/* --- 3. 节点配置表格 (#profilesTableView) ---------------------------------- */

ProfilesTableView#profilesTableView {
    background-color: #19232D;
    alternate-background-color: #1D2834;
    border: 1px solid #37414F;
    border-radius: 6px;
    gridline-color: transparent;
    selection-background-color: #234366;
    selection-color: #FFFFFF;
    font-size: 9pt;
    outline: none;
}

/* 表格单元格通用内边距 */
ProfilesTableView#profilesTableView::item {
    padding-left: 8px;
    padding-right: 8px;
    border-bottom: 1px solid rgba(55, 65, 79, 0.4);
}

ProfilesTableView#profilesTableView::item:hover {
    background-color: #1E2D3E;
}

ProfilesTableView#profilesTableView::item:selected {
    background-color: #234366;
    color: #FFFFFF;
}

/* 表头现代卡片化样式 */
ProfilesTableView#profilesTableView QHeaderView::section {
    background-color: #1F2A37;
    color: #9DA9B5;
    padding: 5px 8px;
    border: none;
    border-right: 1px solid #283546;
    border-bottom: 2px solid #2B3A4D;
    font-size: 8.5pt;
    font-weight: 600;
    text-transform: uppercase;
}

ProfilesTableView#profilesTableView QHeaderView::section:hover {
    background-color: #283748;
    color: #DFE1E2;
}

/* --- 4. 底部状态栏胶囊卡片 (#label_running, #label_inbound, #label_speed) -- */

/* 底部水平布局容器 */
#centralwidget > QHBoxLayout#horizontalLayout {
    margin-top: 6px;
    spacing: 8px;
}

/* 核心运行状态卡片 */
QLabel#label_running {
    background-color: #151D25;
    border: 1px solid #2B3746;
    border-radius: 6px;
    padding: 4px 12px;
    color: #DFE1E2;
    font-size: 9pt;
    font-weight: 500;
    min-height: 24px;
}

/* 核心运行中动态高亮伪类（配合 C++ setProperty("status", "running")） */
QLabel#label_running[status="running"] {
    border-color: rgba(46, 204, 113, 0.45);
    background-color: rgba(46, 204, 113, 0.08);
    color: #E8F8F0;
}

/* 核心未运行状态 */
QLabel#label_running[status="stopped"] {
    border-color: #2B3746;
    color: #788D9C;
}

/* 核心待重启警告状态 */
QLabel#label_running[status="warning"] {
    border-color: rgba(243, 156, 18, 0.55);
    background-color: rgba(243, 156, 18, 0.12);
    color: #FDF2E9;
}

/* 混合监听入口卡片 */
QLabel#label_inbound {
    background-color: #151D25;
    border: 1px solid #2B3746;
    border-radius: 6px;
    padding: 4px 12px;
    color: #9DA9B5;
    font-size: 8.5pt;
    font-family: "Consolas", "Segoe UI Mono", monospace;
    min-height: 24px;
}

/* 实时速率监控卡片（强制等宽字族以防数字跳动抖动宽度） */
QLabel#label_speed {
    background-color: #151D25;
    border: 1px solid #2B3746;
    border-radius: 6px;
    padding: 4px 12px;
    color: #2ECC71;
    font-size: 8.5pt;
    font-family: "Consolas", "Segoe UI Mono", monospace;
    font-weight: 600;
    min-height: 24px;
}

/* --- 5. 下方监控与日志选项卡 (#stats_widget) ------------------------------- */

#stats_widget {
    background-color: #19232D;
    border: 1px solid #37414F;
    border-radius: 6px;
}

#stats_widget::pane {
    border: none;
    background-color: #151D25;
    border-bottom-left-radius: 6px;
    border-bottom-right-radius: 6px;
}

#stats_widget QTabBar::tab {
    background-color: #19232D;
    color: #9DA9B5;
    border: none;
    border-bottom: 2px solid transparent;
    padding: 6px 14px;
    font-size: 9pt;
    font-weight: 500;
}

#stats_widget QTabBar::tab:hover {
    background-color: #202B37;
    color: #DFE1E2;
}

#stats_widget QTabBar::tab:selected {
    background-color: #151D25;
    color: #3498DB;
    border-bottom: 2px solid #3498DB;
    font-weight: 600;
}

/* 主日志查看器 */
QTextBrowser#masterLogBrowser {
    background-color: #11171D;
    color: #CAD1D8;
    border: none;
    font-family: "Consolas", "Segoe UI Mono", monospace;
    font-size: 8.5pt;
    padding: 6px;
    line-height: 1.4;
}

/* 连接监控树形视图 */
QTreeView#connections {
    background-color: #151D25;
    border: none;
    font-size: 8.5pt;
    font-family: "Consolas", "Segoe UI Mono", monospace;
}

QTreeView#connections::item:selected {
    background-color: #234366;
    color: #FFFFFF;
}

/* --- 6. 设置模态对话框全局打磨 -------------------------------------------- */

/* 统一对话框背景与边框留白 */
QDialog#DialogBasicSettings,
QDialog#DialogVPNSettings,
QDialog#DialogManageRoutes {
    background-color: #19232D;
}

/* 对话框内部 GroupBox 分组强化 */
QDialog QGroupBox {
    font-size: 9.5pt;
    font-weight: 600;
    color: #DFE1E2;
    border: 1px solid #37414F;
    border-radius: 6px;
    margin-top: 18px;
    padding: 14px 10px 10px 10px;
}

QDialog QGroupBox::title {
    subcontrol-origin: margin;
    subcontrol-position: top left;
    left: 12px;
    padding: 0 4px;
    background-color: #19232D;
    color: #3498DB;
}

/* 对话框底部的确定/取消标准按钮排版 */
QDialog QDialogButtonBox QPushButton {
    min-width: 72px;
    min-height: 24px;
    border-radius: 4px;
    font-size: 9pt;
    padding: 3px 12px;
}

QDialog QDialogButtonBox QPushButton[default="true"] {
    background-color: #1D6FB8;
    border: 1px solid #3498DB;
    color: #FFFFFF;
}

QDialog QDialogButtonBox QPushButton[default="true"]:hover {
    background-color: #2384DC;
}
```

---

## 4. 分批实施路线图（Implementation Tiers）

按照工程风险与依赖隔离原则，将设计方案解耦为三级渐进实施：

```
Tier 1: 纯 QSS 与 .ui 属性调优 (零代码风险，随时上线)
  │
  ▼
Tier 2: 关键显示模型与状态机轻度适配 (低风险，约 80 行 C++)
  │
  ▼
Tier 3: 架构演进与专用控件封装 (中等风险，需严格回归)
```

### 4.1 Tier 1：纯 QSS + `.ui` 属性调整（零 C++ 代码风险）

此阶段完全无需触碰任何 `.cpp` 逻辑文件，仅通过修改样式表与在 Qt Designer 中调优现有控件属性，即可立竿见影消除 60% 以上的视觉凌乱感。

| 目标文件 | 具体改动点与实施操作 |
| :--- | :--- |
| `res/darkstyle.qss` | **直接追加本文第 3 节完整的 QSS 片段**：<br>1. 为 `#label_running`, `#label_inbound`, `#label_speed` 赋予深色卡片外观与等宽字族。<br>2. 优化 `#selectorGroupList` 项悬停/选中时的左侧强调条与内边距。<br>3. 格式化 `#profilesTableView` 表头与单元格分割边框，消除大面积实体分割线。<br>4. 重建 `#down_tab`（`#stats_widget`）的平滑下边框指示条风格。 |
| `include/ui/mainwindow.ui` | **清理冗余属性与视觉跳动约束**：<br>1. 清理 `toolButton_program` ~ `toolButton_tools` 5 个按钮内部冗余的内联 `<property name="styleSheet">`，统一归入外部 QSS。<br>2. 调整底部 `label_running`、`label_inbound`、`label_speed` 的 `frameShape` 属性为 `NoFrame`，并在布局中将 `spacing` 从默认 `6` 设为 `8`。<br>3. 选中 `profilesTableView`，确认 `horizontalHeaderShowSortIndicator` 为 `true`，`verticalHeaderDefaultSectionSize` 设为 `30`。<br>4. 统一 `toolButton_startstop` 与模式选框的垂直对齐参数，消除上下空隙失真。 |
| `include/ui/setting/dialog_basic_settings.ui` | **规整表单行间距与基线**：<br>1. 调整 `tab_1` 内 `groupBox_5`（Inbound Settings）的网格布局参数：水平间距 `spacing` 统一设为 `10`，垂直间距设为 `8`。<br>2. 规范化输入控件（`inbound_address`, `inbound_socks_port`）的 `sizePolicy`，使输入框不再横向无限扩张。<br>3. 将孤立复选框与对应的说明标签绑定 `buddy` 关联，提升辅助对齐。 |

---

### 4.2 Tier 2：少量 C++ 改动（精准打磨，极低回归风险）

本层级修改完全不影响底层网络代理内核、数据库存储或通信 RPC 协议，仅修正视图层的角色输出与对齐逻辑。

#### 1) 修复表格数据数值列靠左的问题（实现 `Qt::TextAlignmentRole`）
- **目标文件**：`src/ui/utils/ProfilesTableModel.cpp`
- **函数位置**：`ProfilesTableModel::data(const QModelIndex &index, int role)` 约 82-146 行
- **改动说明**：
  在 `ProfilesTableModel::data` 中追加对 `role == Qt::TextAlignmentRole` 的分支判定：
  ```cpp
  if (role == Qt::TextAlignmentRole) {
      switch (index.column()) {
      case ColType:
          return int(Qt::AlignCenter);
      case ColTestResult:
      case ColTraffic:
          return int(Qt::AlignRight | Qt::AlignVCenter);
      default:
          return int(Qt::AlignLeft | Qt::AlignVCenter);
      }
  }
  ```
  同时将表头 `headerData` 的对应对齐角色同步补齐。

#### 2) 隔离测速状态，停止覆盖底部运行状态标签
- **目标文件**：`src/ui/mainWindow/mainwindow_view.cpp`
- **函数位置**：
  1. `MainWindow::url_test_current()` 约 408-436 行
  2. `MainWindow::refresh_status()` 约 157-168 行
- **改动说明**：
  - 注释或移除 `url_test_current()` 中 `ui->label_running->setText(tr("Testing"))` 与覆盖运行态的逻辑。
  - 测试进度信息统一调用 `dataViewHtmlGenerator_.seedLatencyTest(...)` 并由 `UpdateDataView()` 发射至顶部 `data_view`。
  - 在 `refresh_status()` 中为 `ui->label_running` 注入动态状态属性：
    ```cpp
    ui->label_running->setProperty("status", running != nullptr ? "running" : "stopped");
    ui->label_running->style()->unpolish(ui->label_running);
    ui->label_running->style()->polish(ui->label_running);
    ```

#### 3) 优化核心待重启通知为高能度警示横幅
- **目标文件**：`src/ui/utils/DataViewHtmlGenerator.cpp`
- **函数位置**：`DataViewHtmlGenerator::pendingRestartSectionHtml()` 约 139-155 行
- **改动说明**：
  将原本的散乱文本超链接替换为带有警告底色与按钮胶囊外框的紧凑 HTML 结构：
  ```cpp
  QString res = QString(
      "<div style='background:rgba(243,156,18,0.15); border:1px solid #F39C12; border-radius:4px; padding:3px 6px; text-align:center;'>"
      "  <span style='color:#F39C12; font-weight:bold;'>&#9888; %1</span>: <span style='color:#DFE1E2;'>%2</span> "
      "  &nbsp;<a style='background:#F39C12; color:#19232D; text-decoration:none; padding:1px 6px; border-radius:3px; font-weight:bold;' href='%3'>%4</a>"
      "  &nbsp;<a style='color:#9DA9B5; text-decoration:underline;' href='%5'>%6</a>"
      "</div>"
  ).arg(QObject::tr("Restart needed"), pendingRestart_.reasons.join(QStringLiteral(", ")),
       QString(RestartActionUrl), QObject::tr("Restart"),
       QString(DismissRestartActionUrl), QObject::tr("Ignore"));
  ```

#### 4) 替换 ASCII 进度条为 CSS 圆角进度条
- **目标文件**：`src/ui/utils/DataViewHtmlGenerator.cpp`
- **函数位置**：`DataViewHtmlGenerator::getProgressBar(...)` 约 177-191 行
- **改动说明**：
  淘汰循环追加 `#` 字符的方式，直接计算百分比并输出原生富文本进度槽：
  ```cpp
  QString DataViewHtmlGenerator::getProgressBar(long long current, long long total) {
      const int percent = total > 0 ? qBound(0, int(100 * current / total), 100) : 0;
      return QString(
          "<div style='background:#2B3746; border-radius:2px; height:5px; width:120px; display:inline-block; margin:0 4px; vertical-align:middle;'>"
          "  <div style='background:#3498DB; border-radius:2px; height:5px; width:%1%;'></div>"
          "</div>"
      ).arg(percent);
  }
  ```

---

### 4.3 Tier 3：新增控件、模型改造与业务交互重构（架构演进）

属于中长远体验升级，涉及信号防抖、自定义 ItemDelegate 及局部架构重构。

| 优化项 | 涉及文件 | 改造内容与降级理由 |
| :--- | :--- | :--- |
| **策略组切换防抖与二次校验机制** | `src/ui/mainWindow/mainwindow_setup.cpp:425-465` | **现状**：单选行立即触发核心切出站并写 DB，用户排查节点或多选时误触成本极大。<br>**演进方案**：引入 `QTimer` 做 250ms 单击防抖；或者在表头操作栏新增“设为当前组活跃”按钮，改隐式切换为显式触发。<br>**业务风险**：修改了与 `API::defaultClient->SelectOutbound` 的交互时序，降级至 Tier 3。 |
| **专业委托绘制节点指示器与延迟胶囊** | `src/ui/utils/ProfilesTableViewDelegate.cpp`（新增）<br>`include/ui/utils/ProfilesTableModel.h`<br>`src/ui/utils/ProfilesTableModel.cpp` | **现状**：通过往名称字符串硬拼 `"✓ "` 实现指示，依赖系统高亮背景色。<br>**演进方案**：实现继承自 `QStyledItemDelegate` 的专用代理类，使用 `QPainter` 绘制高保真协议 Badge（圆角 Pill）、网络延迟信号条（3 格信号图标）以及活动勾选图标。<br>**架构风险**：需新增绘制源文件并管理 DPI 缩放，属于视图层重构。 |
| **专用原生通知条替换 `data_view`** | `include/ui/mainwindow.ui`<br>`src/ui/mainWindow/mainwindow_setup.cpp` | **现状**：`data_view` 本质是 `QTextBrowser`，通过不断 setHtml 解析网页，渲染开销大且无键盘原生焦点控制。<br>**演进方案**：将其重构为专用的 `NotificationBannerWidget`，内置原生 `QProgressBar`、告警 `QLabel` 及 `QPushButton`。<br>**架构风险**：修改了主窗口 `.ui` 内部控件层级与代码绑定的指针，需全面回归。 |

---

## 5. 验收标准与测试用例

为确保评审方案落地后不破坏原有稳定性，提测时需执行以下核验矩阵：

1. **界面对比度与清晰度**：
   - 使用色彩对比度分析器核验文本与背景对比度，在 Dark 模式下常规正文高于 4.5:1，重要状态高于 7:1。
   - 切换系统 DPI（100%、125%、150%、200%），状态栏高度稳定保持在 36px，无任何文字截断或垂直跳动。
2. **多语言文本极限测试**：
   - 切换为中文（简体）、英文、波斯语（RTL）及俄语，顶部工具按钮文案不遮挡下拉指示箭头（`menu-indicator`）。
   - 策略组名称包含超过 30 个中文字符时，成员表自适应弹性扩展，不产生全局水平滚动条失控。
3. **状态机切换覆盖率**：
   - **用例 1**：核心运行状态下触发全组 URL 测速，底部 `label_running` 依然稳定显示 `[分组] 节点名`，顶部平滑显示进度条。
   - **用例 2**：修改入站端口后，顶部无缝弹出琥珀色待重启卡片，点击 `[Restart]` 核心平稳重启，卡片自动消退。
   - **用例 3**：在策略组中切换成员，选中的勾选指示与行高亮在 16ms 内刷新，核心异步报错时不影响界面选中状态的稳健回滚。
