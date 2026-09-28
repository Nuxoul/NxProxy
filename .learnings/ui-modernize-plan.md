# NxProxy (Qt 6.11 Widgets) 界面现代化再设计方案与落地规格书

- **文档性质**：主界面现代化（Modernization）系统方案与可执行工程落地规格
- **目标工程**：NxProxy 桌面客户端（Qt 6.11 Widgets + C++20，基于 Throne 架构演进）
- **核心基线**：深色模式客户区 `1067 × 951 px`，Windows 11 系统调色板集成
- **交付目标**：现代视觉语言系统、可直接编译的 QSS 动态函数代码块、高保真 UX 交互规格、Tier 1~3 分批工程实施路径

---

## 1. 现代视觉语言规范 (Modern Visual Language System)

本规范严格基于 Qt Widgets 盒模型与 QSS 渲染特性构建，采用「低对比微描边 + 轻微表面层级差 (Elevation by Tint)」的现代 Fluent Design 风格，彻底替换旧版粗重边框与反差失衡色彩。

### 1.1 间距尺度与网格体系 (Spacing Rhythm)
统一基于 **4px / 8px 网格基数** 分配：

| 标称 Token | 像素值 (px) | 适用层级与具体控件分配 |
| :--- | :---: | :--- |
| **Space-0** | `0px` | 模块无缝对接区、表头与内容分割线、嵌套滚动区域内框消除 |
| **Space-1** (Micro) | `4px` | 单元格纵向留白、复选框勾选框与文字间距、延迟徽标垂直内边距、列表项间隙 |
| **Space-2** (Tight) | `6px` | 延迟徽标横向内边距、状态 Chip 垂直内边距、Tab 标签页横向间隙、小卡片内边距 |
| **Space-3** (Base) | `8px` | 顶部主工具按钮间距、策略组列表项水平内边距、表头文字水平留白、标准按钮内边距 |
| **Space-4** (Medium)| `12px` | 底部状态 Chip 之间横向间距 (原 11px 规整化)、主视窗卡片与边缘间隔、表单行距 |
| **Space-5** (Large) | `16px` | 左右工作区分栏间距、设置面板边缘主外边距、独立功能块外框 padding |
| **Space-6** (Section)| `24px` | 顶部控制中枢与下方主工作区之间的纵向视距隔离 |

---

### 1.2 圆角层级体系 (Corner Radius Hierarchy)

| 级别 | 半径值 | 适用控件与场景 | 设计动机 |
| :--- | :---: | :--- | :--- |
| **R-None** | `0px` | 视口外边缘、主窗口外框（由 OS 接管）、无边框列表视口内部 | 避免视口边缘像素抗锯齿污损 |
| **R-Subtle** | `2px` | 滚动条滑块 (`QScrollBar::handle`)、细进度条槽 (`QProgressBar`) | 微交互元素细腻感知 |
| **R-Control**| `4px` | 单行输入框 (`QLineEdit`)、下拉菜单 (`QComboBox`)、常规按钮、表头活动单元格 | 紧凑型表单控件标准轮廓 |
| **R-Card** | `6px` | 策略组列表外框与单项 (`selectorGroupList::item`)、底部状态 Chip (`label_*`)、Tab 标签页 (`QTabBar::tab`)、顶部看板卡片 (`data_view`) | 核心视觉组件，形成柔和卡片化语意 |
| **R-Dialog** | `8px` | 浮动上下文菜单 (`QMenu`)、模态提示卡片、核心待重启告警横幅 | 弹出与重叠层级突出 |

---

### 1.3 层次与字号排版规范 (Typography Hierarchy)

- **字体族**：
  - 界面正文 / 标题：`"Segoe UI", "Microsoft YaHei UI", -apple-system, sans-serif`
  - 数据 / 速率 / IP / 延迟徽标：`"Consolas", "Segoe UI Mono", "Courier New", monospace`

| 层次角色 | 像素值 (px) | 磅值 (pt) | 字重 (Weight) | 语义色 Token | 适用场景 |
| :--- | :---: | :---: | :---: | :--- | :--- |
| **Group Title** (组名) | `12px` | 9.0pt | 500 (Medium) | `t.muted` | 策略组列表项第 1 行、模块栏目微标题 |
| **Active Node** (当前节点) | `13px` | 9.5pt | 600 (SemiBold) | `t.onSurface` | 策略组列表项第 2 行主节点名、卡片标题 |
| **Table Header** (表头) | `12px` | 9.0pt | 600 (SemiBold) | `t.muted` | `profilesTableView` 水平列头 |
| **Table Body** (表格正文) | `12px` | 9.0pt | 400 (Regular) | `t.onSurface` | 节点类型、地址、名称正文 |
| **Data Mono** (等宽数值) | `11.5px`| 8.5pt | 500 (Medium) | `t.onSurface` | 实时速率 (`label_speed`)、流量数值、端口 |
| **Latency Badge** (延迟徽标) | `11px` | 8.0pt | 700 (Bold) | 动态状态色 | 策略组第二行角标、表格测试结果数值 |
| **Micro Caption** (辅助提示)| `10.5px`| 8.0pt | 400 (Regular) | `t.muted` | `selectorGroupHint`、表单注释、状态小标 |

---

### 1.4 表面与描边策略 (Surfaces & Strokes)

彻底弃用灰黑重边框（旧版 `#666666` 强描边导致视觉割裂），改用现代主题算法动态混色。所有计算基于 `ThemeTokens` 与 `blendToward(from, to, keep)`（`keep` 为第 1 个参数权重）：

1. **基础视窗与容器表面 (Surface Layering)**:
   - 窗口主底色：`t.surface`（深色调：`#2D2D2D`，浅色调：`#FFFFFF`）
   - 卡片 / 交替行表面 (Elevation 1)：`blendToward(t.onSurface, t.surface, 0.05)`（深色下约 `#353535`）
   - 输入 / 下拉深陷表面 (Sunken)：`blendToward(t.surface, QColor(0,0,0), 0.70)`（深色下约 `#1E1E1E`）
2. **现代微描边公式 (Subtle Border Formula)**:
   - 低对比度边框：
     ```cpp
     QColor border = separate(blendToward(t.onSurface, t.surface, 0.16), t.surface, 1.28);
     ```
     *相比旧版 0.32 混合的重边框，0.16 保持了清晰的边缘轮廓，又完全融入背景。*
3. **列表与控件交互表面 (Interactive Surfaces)**:
   - 悬停填充 (Hover):
     ```cpp
     QColor hoverFill = separate(blendToward(t.accent, t.surface, 0.14), t.surface, 1.08);
     ```
   - 选中填充 (Selected):
     ```cpp
     QColor selectedFill = separate(blendToward(t.accent, t.surface, 0.28), t.surface, 1.15);
     ```
   - 状态 Chip 填充 (Chip Fill - 彻底消除白底灾难):
     ```cpp
     QColor chipFill = separate(blendToward(t.onSurface, t.surface, 0.08), t.surface, 1.05);
     ```
4. **左侧激活指示轨道 (Accent Indicator Rail)**:
   - 选中时左侧使用 `3px` 宽度的 `t.accent`（亮蓝 `#5F91B7` / `#76B9ED`），文字保持高对比度 `t.onSurface`，不再用刺眼的全幅高饱和度色块糊死文字。

---

### 1.5 控件密度与高度目标值 (Target Density & Heights)

| 控件对象 | 当前实测高度 | 目标现代化高度 | 几何修正策略 |
| :--- | :---: | :---: | :--- |
| **策略组单项** (`selectorGroupList::item`) | 39px (单行) | **48px** (双行) | 容纳两行排版（上组名 18px + 下节点/延迟 22px + 内补 8px），项间距 `margin-bottom: 4px` |
| **策略组视口** (`selectorGroupList`) | 180px (截断) | **216px** (整倍数) | 4 项完全展示：$48\times 4 + 4\times 3 + 8 = 216\text{px}$，消除第 5 项 14px 削顶残影 |
| **成员表格行高** (`profilesTableView::item`)| 24px (拥挤) | **30px** (舒适) | 净文本 13px + 上下各 8px 留白呼吸感，桌面端扫视极佳 |
| **表格水平表头** (`QHeaderView::section`) | 23px (扁平) | **30px** (标准) | 上下 5px 内边距，字重 600，支持现代卡片化分割 |
| **底部状态 Chip** (`label_running/inbound/speed`)| 38px (非对称) | **32px** (紧凑) | 限制 `min-height: 32px; max-height: 32px;`，单行绝对居中，杜绝高度晃动 |
| **顶部工具主按钮** (`toolButton_*`) | 73px (上图下文)| **48px** (左右图文) | 消除与启停大按钮 (48px) 之间 25px 的底部悬空断崖 |

---

### 1.6 状态色语义映射表 (Semantic State Matrix)

| 运行/测试状态 | 语义 Token | 深色模式 Hex | 浅色模式 Hex | 胶囊背景填充色 (Tint Fill) | 描边或指示条 (Border/Rail) |
| :--- | :--- | :---: | :---: | :--- | :--- |
| **运行中 (Running)** | `t.success` | `#2ECC71` | `#27AE60` | `blendToward(t.success, t.surface, 0.16)` | `1px solid blendToward(t.success, t.surface, 0.45)` |
| **已停止 (Stopped)** | `t.muted` | `#9AA0A6` | `#70757A` | `blendToward(t.muted, t.surface, 0.10)` | `1px solid blendToward(t.muted, t.surface, 0.25)` |
| **需重启 (Restart)** | `t.warning` | `#F39C12` | `#D68910` | `blendToward(t.warning, t.surface, 0.18)` | `1px solid blendToward(t.warning, t.surface, 0.50)` |
| **测试中 (Testing)** | `t.info` | `#3498DB` | `#2980B9` | `blendToward(t.info, t.surface, 0.18)` | `1px solid blendToward(t.info, t.surface, 0.45)` |
| **错误/超时 (Error)** | `t.danger` | `#E74C3C` | `#C0392B` | `blendToward(t.danger, t.surface, 0.18)` | `1px solid blendToward(t.danger, t.surface, 0.50)` |
| **未测试 (Untested)** | `t.muted` | `#7F8C8D` | `#95A5A6` | `blendToward(t.onSurface, t.surface, 0.05)` | `1px solid blendToward(t.onSurface, t.surface, 0.15)` |

---

## 2. 可直接落地的 QSS 代码实现 (Production QSS Specification)

所有样式均遵照 `src/ui/setting/ThemeManager.cpp` 的既有架构，通过 `QStringLiteral(...).arg(hex(...))` 动态装配，完全适配所有静态/系统主题。

### 2.1 现代核心控制中枢样式表 (`modernCommandCenterStyleSheet`)

覆盖目标控件：
- `toolButton_program`, `toolButton_preferences`, `toolButton_testing`, `toolButton_routing`, `toolButton_tools`（顶部 5 枚主功能按钮）
- `toolButton_startstop`（核心启停中枢大按钮）
- `checkBox_VPN`, `checkBox_SystemProxy`, `system_dns`（运行模式复选集群）
- `data_view`（顶部动态通知看板 / 需重启横幅 / 测速进度展示框）

```cpp
static QString modernCommandCenterStyleSheet(const ThemeTokens &t) {
    const auto hex = [](const QColor &c) { return c.name(QColor::HexRgb); };
    const QColor border = separate(blendToward(t.onSurface, t.surface, 0.16), t.surface, 1.28);
    const QColor hoverBg = separate(blendToward(t.onSurface, t.surface, 0.08), t.surface, 1.08);
    const QColor pressBg = separate(blendToward(t.accent, t.surface, 0.20), t.surface, 1.15);
    const QColor sunkenBg = blendToward(t.surface, QColor(0, 0, 0), 0.75);

    return QStringLiteral(
        "/* ================= 顶部 5 枚主功能工具按钮 ================= */\n"
        "QToolButton#toolButton_program,\n"
        "QToolButton#toolButton_preferences,\n"
        "QToolButton#toolButton_testing,\n"
        "QToolButton#toolButton_routing,\n"
        "QToolButton#toolButton_tools {\n"
        "    background-color: transparent;\n"
        "    color: %1;\n"
        "    border: 1px solid transparent;\n"
        "    border-radius: 6px;\n"
        "    padding: 6px 10px;\n"
        "    min-height: 36px;\n"
        "    font-size: 9pt;\n"
        "    font-weight: 500;\n"
        "}\n"
        "QToolButton#toolButton_program:hover,\n"
        "QToolButton#toolButton_preferences:hover,\n"
        "QToolButton#toolButton_testing:hover,\n"
        "QToolButton#toolButton_routing:hover,\n"
        "QToolButton#toolButton_tools:hover {\n"
        "    background-color: %2;\n"
        "    border: 1px solid %3;\n"
        "    color: %4;\n"
        "}\n"
        "QToolButton#toolButton_program:pressed,\n"
        "QToolButton#toolButton_preferences:pressed,\n"
        "QToolButton#toolButton_testing:pressed,\n"
        "QToolButton#toolButton_routing:pressed,\n"
        "QToolButton#toolButton_tools:pressed {\n"
        "    background-color: %5;\n"
        "    border-color: %6;\n"
        "}\n"
        "QToolButton#toolButton_program::menu-indicator,\n"
        "QToolButton#toolButton_preferences::menu-indicator,\n"
        "QToolButton#toolButton_testing::menu-indicator,\n"
        "QToolButton#toolButton_routing::menu-indicator,\n"
        "QToolButton#toolButton_tools::menu-indicator {\n"
        "    subcontrol-origin: padding;\n"
        "    subcontrol-position: bottom right;\n"
        "    bottom: 2px;\n"
        "    right: 3px;\n"
        "}\n"
        "\n"
        "/* ================= 模式复选框集群 ================= */\n"
        "QCheckBox#checkBox_VPN,\n"
        "QCheckBox#checkBox_SystemProxy,\n"
        "QCheckBox#system_dns {\n"
        "    color: %1;\n"
        "    spacing: 6px;\n"
        "    font-size: 9pt;\n"
        "    font-weight: 500;\n"
        "    padding: 2px 4px;\n"
        "}\n"
        "QCheckBox#checkBox_VPN:hover,\n"
        "QCheckBox#checkBox_SystemProxy:hover,\n"
        "QCheckBox#system_dns:hover {\n"
        "    color: %4;\n"
        "}\n"
        "QCheckBox#checkBox_VPN::indicator,\n"
        "QCheckBox#checkBox_SystemProxy::indicator,\n"
        "QCheckBox#system_dns::indicator {\n"
        "    width: 16px;\n"
        "    height: 16px;\n"
        "    border: 1px solid %3;\n"
        "    border-radius: 4px;\n"
        "    background: %7;\n"
        "}\n"
        "QCheckBox#checkBox_VPN::indicator:hover,\n"
        "QCheckBox#checkBox_SystemProxy::indicator:hover,\n"
        "QCheckBox#system_dns::indicator:hover {\n"
        "    border-color: %6;\n"
        "}\n"
        "QCheckBox#checkBox_VPN::indicator:checked,\n"
        "QCheckBox#checkBox_SystemProxy::indicator:checked,\n"
        "QCheckBox#system_dns::indicator:checked {\n"
        "    background-color: %6;\n"
        "    border-color: %6;\n"
        "    image: url(:/icons/check_white.svg);\n"
        "}\n"
        "\n"
        "/* ================= 顶部动态通知与数据看板 ================= */\n"
        "QTextBrowser#data_view {\n"
        "    background-color: %7;\n"
        "    border: 1px solid %3;\n"
        "    border-radius: 6px;\n"
        "    padding: 6px 12px;\n"
        "    color: %1;\n"
        "    font-size: 9pt;\n"
        "    selection-background-color: %6;\n"
        "    selection-color: %4;\n"
        "}\n"
        "QTextBrowser#data_view:hover {\n"
        "    border-color: %8;\n"
        "}\n"
    ).arg(
        hex(t.onSurface),                       // %1 文本默认主色
        hex(hoverBg),                           // %2 工具按钮悬停背景
        hex(border),                            // %3 基础低对比边框
        hex(QColor(0xFF, 0xFF, 0xFF)),          // %4 高亮纯白
        hex(pressBg),                           // %5 按钮按下背景
        hex(t.accent),                          // %6 强调色 Accent
        hex(sunkenBg),                          // %7 深陷背景色
        hex(separate(border, t.surface, 1.5))   // %8 悬停加深边框
    );
}
```

---

### 2.2 现代化策略组与成员视图样式表 (`modernStrategyPanelStyleSheet`)

覆盖目标控件：
- `selectorGroupList`（策略组列表，支持 2 行内容、左侧高亮轨条、悬停与选中态、圆角滚动条）
- `selectorGroupHint`（策略组说明标签）
- `selectorMemberToolbar`（成员区域工具栏容器）
- `selectorMemberHint`（成员区域动态指示说明）
- `profilesTableView`（核心配置表格，含表头 30px 高度、单元格 30px 行高、无缝网格、交替行底色、平滑滚动条）

```cpp
static QString modernStrategyPanelStyleSheet(const ThemeTokens &t) {
    const auto hex = [](const QColor &c) { return c.name(QColor::HexRgb); };
    const QColor border = separate(blendToward(t.onSurface, t.surface, 0.16), t.surface, 1.28);
    const QColor hoverFill = separate(blendToward(t.accent, t.surface, 0.14), t.surface, 1.08);
    const QColor selectedFill = separate(blendToward(t.accent, t.surface, 0.28), t.surface, 1.15);
    const QColor altRow = blendToward(t.onSurface, t.surface, 0.04);
    const QColor sunkenBg = blendToward(t.surface, QColor(0, 0, 0), 0.75);
    const QColor scrollThumb = separate(blendToward(t.onSurface, t.surface, 0.25), t.surface, 1.30);
    const QColor scrollHover = separate(blendToward(t.onSurface, t.surface, 0.40), t.surface, 1.50);

    return QStringLiteral(
        "/* ================= 策略组提示标签 ================= */\n"
        "QLabel#selectorGroupHint,\n"
        "QLabel#selectorMemberHint {\n"
        "    color: %1;\n"
        "    font-size: 8.5pt;\n"
        "    font-weight: 600;\n"
        "    padding: 3px 2px;\n"
        "    letter-spacing: 0.2px;\n"
        "}\n"
        "QWidget#selectorMemberToolbar {\n"
        "    background-color: transparent;\n"
        "    border: none;\n"
        "}\n"
        "\n"
        "/* ================= 策略组列表视窗 ================= */\n"
        "QListWidget#selectorGroupList {\n"
        "    background-color: %2;\n"
        "    border: 1px solid %3;\n"
        "    border-radius: 6px;\n"
        "    padding: 4px;\n"
        "    outline: none;\n"
        "}\n"
        "QListWidget#selectorGroupList::item {\n"
        "    border-radius: 5px;\n"
        "    padding: 6px 10px;\n"
        "    margin-bottom: 4px;\n"
        "    border-left: 3px solid transparent;\n"
        "    color: %4;\n"
        "    min-height: 44px;\n"
        "}\n"
        "QListWidget#selectorGroupList::item:hover:!selected {\n"
        "    background-color: %5;\n"
        "    color: %6;\n"
        "}\n"
        "QListWidget#selectorGroupList::item:selected {\n"
        "    background-color: %7;\n"
        "    border-left: 3px solid %8;\n"
        "    color: %6;\n"
        "}\n"
        "\n"
        "/* ================= 成员表格 ================= */\n"
        "ProfilesTableView#profilesTableView {\n"
        "    background-color: %2;\n"
        "    alternate-background-color: %9;\n"
        "    border: 1px solid %3;\n"
        "    border-radius: 6px;\n"
        "    gridline-color: transparent;\n"
        "    selection-background-color: %7;\n"
        "    selection-color: %6;\n"
        "    font-size: 9pt;\n"
        "    outline: none;\n"
        "}\n"
        "ProfilesTableView#profilesTableView::item {\n"
        "    min-height: 30px;\n"
        "    max-height: 30px;\n"
        "    padding-left: 8px;\n"
        "    padding-right: 8px;\n"
        "    border: none;\n"
        "    border-bottom: 1px solid %10;\n"
        "}\n"
        "ProfilesTableView#profilesTableView::item:hover:!selected {\n"
        "    background-color: %5;\n"
        "}\n"
        "ProfilesTableView#profilesTableView::item:selected {\n"
        "    background-color: %7;\n"
        "    color: %6;\n"
        "}\n"
        "ProfilesTableView#profilesTableView QHeaderView::section {\n"
        "    background-color: %9;\n"
        "    color: %1;\n"
        "    font-size: 8.5pt;\n"
        "    font-weight: 600;\n"
        "    min-height: 30px;\n"
        "    max-height: 30px;\n"
        "    padding: 0px 8px;\n"
        "    border: none;\n"
        "    border-bottom: 1px solid %3;\n"
        "    border-right: 1px solid %10;\n"
        "}\n"
        "ProfilesTableView#profilesTableView QHeaderView::section:hover {\n"
        "    background-color: %5;\n"
        "    color: %6;\n"
        "}\n"
        "\n"
        "/* ================= 极简自适应平滑滚动条 ================= */\n"
        "QScrollBar:vertical {\n"
        "    background: transparent;\n"
        "    width: 8px;\n"
        "    margin: 2px 0px 2px 0px;\n"
        "}\n"
        "QScrollBar::handle:vertical {\n"
        "    background: %11;\n"
        "    min-height: 24px;\n"
        "    border-radius: 4px;\n"
        "}\n"
        "QScrollBar::handle:vertical:hover {\n"
        "    background: %12;\n"
        "}\n"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {\n"
        "    height: 0px;\n"
        "    border: none;\n"
        "}\n"
        "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {\n"
        "    background: transparent;\n"
        "}\n"
    ).arg(
        hex(t.muted),                           // %1
        hex(t.surface),                         // %2 列表/表格主底色
        hex(border),                            // %3 外围主微边框
        hex(t.onSurface),                       // %4
        hex(hoverFill),                         // %5
        hex(QColor(0xFF, 0xFF, 0xFF)),          // %6
        hex(selectedFill),                      // %7
        hex(t.accent),                          // %8
        hex(altRow),                            // %9 交替行与表头底色
        hex(blendToward(border, t.surface, 0.45)), // %10 细行内分割线
        hex(scrollThumb),                       // %11 滚动条手柄
        hex(scrollHover)                        // %12 滚动条悬停
    );
}
```

---

### 2.3 现代化底部常驻状态条样式表 (`modernStatusStripStyleSheet`)

覆盖目标控件：
- `label_running`（核心运行状态与当前节点 Chip）
- `label_inbound`（监听与入站端口 Chip）
- `label_speed`（实时上传/下载速率 Chip）

```cpp
static QString modernStatusStripStyleSheet(const ThemeTokens &t) {
    const auto hex = [](const QColor &c) { return c.name(QColor::HexRgb); };
    const QColor border = separate(blendToward(t.onSurface, t.surface, 0.16), t.surface, 1.28);
    // 采用与表面相融的深灰底色，彻底终结白底白字 (1.13:1) 的对比度故障：
    const QColor chipFill = separate(blendToward(t.onSurface, t.surface, 0.08), t.surface, 1.05);

    return QStringLiteral(
        "/* ================= 底部状态胶囊卡片 (Status Chips) ================= */\n"
        "QLabel#label_running,\n"
        "QLabel#label_inbound,\n"
        "QLabel#label_speed {\n"
        "    background-color: %1;\n"
        "    border: 1px solid %2;\n"
        "    border-radius: 6px;\n"
        "    color: %3;\n"
        "    padding: 0px 10px;\n"
        "    min-height: 32px;\n"
        "    max-height: 32px;\n"
        "    font-size: 9pt;\n"
        "    font-weight: 500;\n"
        "    margin-top: 2px;\n"
        "    margin-bottom: 2px;\n"
        "}\n"
        "QLabel#label_running:hover,\n"
        "QLabel#label_inbound:hover,\n"
        "QLabel#label_speed:hover {\n"
        "    border-color: %4;\n"
        "    background-color: %5;\n"
        "}\n"
        "QLabel#label_speed {\n"
        "    font-family: 'Consolas', 'Segoe UI Mono', monospace;\n"
        "    font-weight: 600;\n"
        "    letter-spacing: 0.3px;\n"
        "}\n"
    ).arg(
        hex(chipFill),                          // %1
        hex(border),                            // %2
        hex(t.onSurface),                       // %3
        hex(separate(border, t.surface, 1.4)),  // %4
        hex(blendToward(t.accent, chipFill, 0.10)) // %5 悬停微提亮
    );
}
```

---

### 2.4 现代化标签页与控制台样式表 (`modernTabAndTelemetryStyleSheet`)

覆盖目标控件：
- `tabWidget`（主工作区工作模式标签页）
- `stats_widget`（下半区控制台 Tab：Logs / Connections / Graph / Runtime）

```cpp
static QString modernTabAndTelemetryStyleSheet(const QPalette &pal, const ThemeTokens &t) {
    const auto hex = [](const QColor &c) { return c.name(QColor::HexRgb); };
    const QColor border = separate(blendToward(t.onSurface, t.surface, 0.16), t.surface, 1.28);
    const QColor hoverTab = separate(blendToward(t.accent, t.surface, 0.12), t.surface, 1.10);
    const QColor activeTab = separate(blendToward(t.accent, t.surface, 0.25), t.surface, 1.15);

    return QStringLiteral(
        "/* ================= 主标签页与控制台 Tab ================= */\n"
        "QTabWidget#tabWidget::pane,\n"
        "QTabWidget#stats_widget::pane {\n"
        "    border: 1px solid %1;\n"
        "    border-radius: 6px;\n"
        "    background-color: %2;\n"
        "    top: -1px;\n"
        "}\n"
        "QTabWidget#tabWidget QTabBar,\n"
        "QTabWidget#stats_widget QTabBar {\n"
        "    background: transparent;\n"
        "    qproperty-drawBase: 0;\n"
        "}\n"
        "QTabWidget#tabWidget QTabBar::tab,\n"
        "QTabWidget#stats_widget QTabBar::tab {\n"
        "    background: transparent;\n"
        "    color: %3;\n"
        "    border: 1px solid transparent;\n"
        "    border-top-left-radius: 5px;\n"
        "    border-top-right-radius: 5px;\n"
        "    padding: 6px 14px;\n"
        "    margin-right: 2px;\n"
        "    font-size: 9pt;\n"
        "    font-weight: 500;\n"
        "}\n"
        "QTabWidget#tabWidget QTabBar::tab:hover:!selected,\n"
        "QTabWidget#stats_widget QTabBar::tab:hover:!selected {\n"
        "    background-color: %4;\n"
        "    color: %5;\n"
        "}\n"
        "QTabWidget#tabWidget QTabBar::tab:selected,\n"
        "QTabWidget#stats_widget QTabBar::tab:selected {\n"
        "    background-color: %6;\n"
        "    border: 1px solid %1;\n"
        "    border-bottom: 2px solid %7;\n"
        "    color: %5;\n"
        "    font-weight: 600;\n"
        "}\n"
        "QTabWidget#tabWidget QTabBar::tab:disabled,\n"
        "QTabWidget#stats_widget QTabBar::tab:disabled {\n"
        "    color: %3;\n"
        "    opacity: 0.5;\n"
        "}\n"
    ).arg(
        hex(border), hex(t.surface), hex(t.muted),
        hex(hoverTab), hex(QColor(0xFF, 0xFF, 0xFF)),
        hex(activeTab), hex(t.accent)
    );
}
```

---

### 2.5 现代化输入框与下拉选择框样式表 (`modernInputAndDropdownStyleSheet`)

覆盖目标控件：
- 全局所有单行文本输入框 (`QLineEdit`)
- 全局所有下拉菜单选择框 (`QComboBox`)
- 全局数值微调框 (`QSpinBox`)

```cpp
static QString modernInputAndDropdownStyleSheet(const ThemeTokens &t) {
    const auto hex = [](const QColor &c) { return c.name(QColor::HexRgb); };
    const QColor border = separate(blendToward(t.onSurface, t.surface, 0.16), t.surface, 1.28);
    const QColor sunkenBg = blendToward(t.surface, QColor(0, 0, 0), 0.75);
    const QColor focusBorder = t.accent;

    return QStringLiteral(
        "/* ================= 全局输入框 (QLineEdit) ================= */\n"
        "QLineEdit {\n"
        "    background-color: %1;\n"
        "    border: 1px solid %2;\n"
        "    border-radius: 4px;\n"
        "    color: %3;\n"
        "    padding: 4px 8px;\n"
        "    min-height: 24px;\n"
        "    selection-background-color: %4;\n"
        "    selection-color: #FFFFFF;\n"
        "}\n"
        "QLineEdit:hover {\n"
        "    border-color: %5;\n"
        "}\n"
        "QLineEdit:focus {\n"
        "    border: 1.5px solid %4;\n"
        "    background-color: %1;\n"
        "}\n"
        "QLineEdit:disabled {\n"
        "    background-color: %6;\n"
        "    color: %7;\n"
        "    border-color: transparent;\n"
        "}\n"
        "\n"
        "/* ================= 全局下拉选择框 (QComboBox) ================= */\n"
        "QComboBox {\n"
        "    background-color: %1;\n"
        "    border: 1px solid %2;\n"
        "    border-radius: 4px;\n"
        "    color: %3;\n"
        "    padding: 3px 8px 3px 8px;\n"
        "    min-height: 24px;\n"
        "}\n"
        "QComboBox:hover {\n"
        "    border-color: %5;\n"
        "}\n"
        "QComboBox:focus {\n"
        "    border: 1.5px solid %4;\n"
        "}\n"
        "QComboBox::drop-down {\n"
        "    subcontrol-origin: padding;\n"
        "    subcontrol-position: top right;\n"
        "    width: 20px;\n"
        "    border-left: 1px solid transparent;\n"
        "}\n"
        "QComboBox::down-arrow {\n"
        "    image: url(:/icons/arrow_down_muted.svg);\n"
        "    width: 10px;\n"
        "    height: 10px;\n"
        "}\n"
        "QComboBox QAbstractItemView {\n"
        "    background-color: %8;\n"
        "    border: 1px solid %2;\n"
        "    border-radius: 6px;\n"
        "    selection-background-color: %4;\n"
        "    selection-color: #FFFFFF;\n"
        "    padding: 4px;\n"
        "    outline: none;\n"
        "}\n"
    ).arg(
        hex(sunkenBg),                          // %1
        hex(border),                            // %2
        hex(t.onSurface),                       // %3
        hex(focusBorder),                       // %4
        hex(separate(border, t.surface, 1.4)),  // %5
        hex(blendToward(t.onSurface, t.surface, 0.05)), // %6
        hex(t.muted),                           // %7
        hex(t.surface)                          // %8
    );
}
```

---

## 3. UX 交互规格 (Interaction & UX Specification)

### 3.1 策略组面板操作与即时反馈闭环

1. **选择组（Click Group → Inspect Members）**:
   - **操作行为**：在 `selectorGroupList` 单击某个策略组卡片。
   - **交互响应**：
     - 当前项立即获得焦点，左侧亮起 `3px` Accent 导轨指示条，背景淡入 `selectedFill`（无刺眼全蓝反色）。
     - 右侧 `profilesTableView` 通过 `show_selector_members(id)` 立即无刷新过滤重载该组的备选节点池。
     - 表格顶部 `selectorMemberHint` 联动变更为：`当前策略组成员: [组名]（点击切换节点）`。
2. **切换节点（Select Member → Instant Switch）**:
   - **操作行为**：在 `profilesTableView` 中单选某节点行。
   - **即时确认与防误触规则**：
     - 若点击已是当前使用中的节点（行首标有 `✓`）：界面保持原状，不触发无谓的 RPC 写入。
     - 若点击备选新节点：
       - **UI 状态原子更新**：该行行首立即展示 `✓` 标记并呈现 4px 亮蓝指示，原活动节点指示取消。
       - **左侧列表同步刷新**：`selectorGroupList` 对应组项的第 2 行立即更新为新节点名，并重刷该节点的延迟徽标。
       - **持久化与 RPC 执行**：调用 `Configs::dataManager->profilesRepo->Save(...)`。若核心在运行态，后台异步触发 `SelectOutbound`。
   - **反馈文案与展示位置**：
     - **热切换成功**：在顶部控制看板 `data_view` 顶部淡入绿色成功胶囊：`✓ 策略组 [组名] 已切换至 [节点名]`（3 秒后平滑淡出）。
     - **需重启生效**：当节点未在运行配置中（如新增节点）时，`data_view` 立即变为橙色警告横幅：`⚠️ 策略组 [组名] 已设定为 [节点名]，需重启核心生效`，右侧并排展示实体按钮 `[立即重启]`。
     - **核心拒绝/异常**：`data_view` 变为红色警告框：`❌ 核心切换失败: [错误详情]`，状态回滚。
3. **双击与右键菜单行为**:
   - **策略组列表 (`selectorGroupList`)**:
     - *右键菜单*：`[测速此策略组成员]` / `[复制组内节点链接]` / `[策略组规则配置...]`。
     - *双击*：焦点平滑跳转至右侧成员表的首行，并置光标于第 1 个成员节点。
   - **成员表格 (`profilesTableView`)**:
     - *右键菜单*：`[设为此组生效节点]`（加粗）/ `[单独测速此节点]` / `[复制分享链接]` / `[编辑配置...]` / `[从组中移除]`。
     - *双击*：触发编辑该节点详情对话框 (`dialog_edit_profile`)。
4. **键盘导航无障碍规范 (Keyboard Accessibility)**:
   - 在策略组列表中：按下 `↑` / `↓` 逐项平移，右侧成员表随键位同步动态刷新；按下 `Enter` 或 `Tab` 将键盘焦点切换入成员表。
   - 在成员表格中：按下 `↑` / `↓` 移动光标；按下 `Space`（空格键）或 `Enter` 将当前高亮行设为该组生效节点；按下 `Esc` 清除选中。
5. **空态与未测试展示**:
   - **空态 (Empty State)**：当组内无可用节点（`members.empty()`）时，`profilesTableView` 视口居中呈现水墨中性插画 + 提示文案：`此策略组暂无成员节点`，下方提供主操作链接 `[向此组添加节点]`。
   - **未测试态 (Untested State)**：若节点尚未执行延迟测试（`latency == 0`），徽标展示为灰调中性文本 `-- ms`（底色 `blendToward(t.onSurface, t.surface, 0.05)`，文字 `t.muted`），严禁渲染为具有误导性的 `0 ms` 或留白。

---

### 3.2 顶部 571px 与表格右侧 273px 空白死区的深度消化方案

#### A. 顶部 571px 死区 (`X = 487..1057`) 消化方案
**根因分析**：原界面中 `toolButton_startstop` 与模式复选框在 X=486 截断，右侧 `data_view` 处于折叠或纯文本空白，导致 54.4% 的横向画布纯黑浪费。

**落地重构设计**：
将原本单一纯文本的 `data_view` (`QTextBrowser`) 升级为**多态复合控制中枢 (Modern Command Center)**，自 X=487 弹性拉满至 X=1057：
1. **日常运行看板（态 1 - Default Hub）**：
   - 左侧：当前核心状态指示灯 + 运行模式胶囊（`Tun 模式` / `System Proxy: 127.0.0.1:2080`）。
   - 中部：网络连接指标计量（当前活跃连接数 `24 Conns`，实时内存占用 `18.4 MB`）。
   - 右侧：路由分流摘要（`绕过大陆 IP` / `DNS: 混合分流`）。
2. **核心待重启告警横幅（态 2 - Restart Hub）**：
   - 触发条件：修改出站、切换了未在运行配置中的策略组成员或变更了 Tun 设定。
   - 展现形态：卡片整体以 `rgba(243, 156, 18, 0.18)` 琥珀色微光填充，左侧呈现告警图标 `⚠️` 与文字说明 `检测到核心配置已变更，需重启应用生效`；右侧放置实体高亮按钮 `[立即重启]`（32px 高度，点击即触发无缝平滑重载）。
3. **测速动态看板（态 3 - Progress Hub）**：
   - 触发条件：用户点击了单项、选中项或全组测速。
   - 展现形态：看板无缝展开，左侧展示测速类型 `URL 延迟测速 (32/120)`；中部嵌入原生微圆角 `QProgressBar`；右侧呈现红色小按钮 `[终止测速]`。

#### B. 表格右侧 273px 死区 (`X = 785..1057`) 消化方案
**根因分析**：原表格 4 列分配为：类型 129px、名称 174px、测试结果 151px、流量 289px，总宽仅 743px。核心视线「节点名称」仅分得 174px 导致大面积截断，而流量虚占 289px，表格右侧出现 273px 未绘制死黑。

**落地重构列宽策略**：
在 C++ 端声明弹性拉伸模型：
```cpp
// 1. 禁用末列盲目拉伸，避免流量列过大
ui->profilesTableView->horizontalHeader()->setStretchLastSection(false);

// 2. 紧凑型元数据固定列
ui->profilesTableView->setColumnWidth(ProfilesTableModel::ColType, 84);        // 类型：固定 84px
ui->profilesTableView->setColumnWidth(ProfilesTableModel::ColTestResult, 110);  // 结果：固定 110px
ui->profilesTableView->setColumnWidth(ProfilesTableModel::ColTraffic, 120);     // 流量：固定 120px

// 3. 核心节点名称列全量弹性吸收剩余 500px+ 空间
ui->profilesTableView->horizontalHeader()->setSectionResizeMode(
    ProfilesTableModel::ColName, QHeaderView::Stretch
);
```
**视觉改善**：节点别名（如 `[HK] 香港BGP专线 - IPLC原生 - 01`）可获得达 **500px 以上** 的开阔视距，彻底消除文字截断；表格横向 100% 填满视窗，273px 死黑完全消失。

---

### 3.3 底部状态常驻条（三个 Status Chip）规格化

三枚 Chip 水平等分排布（或按 `35% : 35% : 30%` 比例分配），统一采用单行绝对排版，高度锁定在 **32px**（容器高度 44px），杜绝换行引发的纵向颠簸：

1. **Chip 1 (`label_running`) - 核心运行中枢与路由节点**:
   - **日常文案**：`● [运行中] 默认出站 → 香港 01 [HK]`（未运行时为：`○ 核心已停止`）。
   - **微交互与省略**：文本左侧为 8px 语义小圆点；长文本采用 `Qt::ElideRight` 自动截断；鼠标悬停触发富文本 ToolTip，展示完整出站协议、UUID 脱敏值、已连续运行时间。
2. **Chip 2 (`label_inbound`) - 入站网络与系统接管**:
   - **日常文案**：`⚡ Mixed: 127.0.0.1:2080 | Tun: 198.18.0.1`。
   - **微交互**：单击 Chip 即刻将混合代理端口复制至剪贴板，并短暂弹出 Tooltip `已复制: 127.0.0.1:2080`；右键可快速配置 PAC 或切换监听端口。
3. **Chip 3 (`label_speed`) - 实时速率与流量累计**:
   - **日常文案**：**单行等宽排版** `↑ 24.5 KB/s   ↓ 1.2 MB/s`（彻底废弃双行换行导致的垂直不对称）。
   - **排版控制**：强制绑定 `Consolas` 等宽字体，数字预留固定字符位（如速率单位对齐），无论网速剧烈波动，文字物理宽度绝不发生晃动抖动。

---

### 3.4 窗口标题栏过载收敛方案

- **旧版问题**：`make_title()` 直接将 `[Admin][Select][Tun+System Proxy] NxProxy 1.0.x [RouteName] Outbound@GroupName Country` 硬拼入原生标题，文字跨度宽达 **991px**，与 Windows 右上角原生三键（宽 138px）严重重叠冲突。
- **现代化收敛方案**：
  1. **保留在窗口标题栏的内容（极简桌面标准）**：
     ```cpp
     // 仅保留应用名称、版本号与主运行状态，总长严格控制在 30 字符以内 (~220px)
     setWindowTitle(QString("NxProxy %1%2%3")
         .arg(APP_VERSION)
         .arg(isAdmin ? " [Admin]" : "")
         .arg(isRunning ? QString(" - %1").arg(currentGroupName) : tr(" (已停止)")));
     ```
  2. **彻底下沉至界面内部的标记**：
     - `[Tun] / [System Proxy]`：已有顶部模式复选框与底部 Chip 2 直观展示，从标题栏完全剔除。
     - `[RouteName]`：已有顶部工具栏“路由”菜单与控制台标签展示，从标题栏完全剔除。
     - `Outbound@GroupName Country`：由策略组项第 2 行与底部 Chip 1 权威展示，从标题栏完全剔除。

---

### 3.5 测速进行中的动态表达 (Testing In-Progress UI)

彻底废除旧版中在 `ui->label_running` 粗暴覆写 `Testing...` 并用 2 秒计时器破坏正常运行状态的缺陷逻辑，重构为**非破坏性两级并行反馈**：

1. **表格行级微观指示 (Per-row Spinner/Badge)**:
   - 正在测试的节点行：在 `ColTestResult` 列原位呈现淡蓝高亮胶囊：`⏳ 测速中...`，前景色采用 `t.info` (`#3498DB`)。
   - 已完成的节点行：立即根据实测毫秒值上色（$\le 100\text{ms}$ 翡翠绿 / $\le 300\text{ms}$ 琥珀黄 / $>300\text{ms}$ 绯红），无需等全量测试结束即可逐行直观呈现。
2. **顶部看板宏观进度 (Global Progress in `data_view`)**:
   - 测速启动时，顶部 `data_view` 自动切为测速控制台：
     - **左侧状态**：`正在执行 URL 延迟测速 (32 / 120)`
     - **中间进度**：Qt 原生无缝进度条 `QProgressBar`，高度 8px，平滑渲染 $26.6\%$
     - **右侧入口**：提供实体红色次要按钮 `[取消测速]`（关联 `testRunner->stop()`）
3. **完成退出**:
   - 测速完毕后，`data_view` 平滑淡出并恢复为日常控制看板；底部状态栏全程锁定当前活动节点与速率，零受扰、零闪烁。

---

## 4. 分批实施工程计划 (Tiered Implementation Plan)

为确保项目平稳演进、零业务破坏性风险，工程实施严格划分为三级梯队：

```
┌────────────────────────────────────────────────────────────────────────┐
│ Tier 1: 纯 QSS 与调色板注入 (100% 样式层, 零 C++ 逻辑风险, 立即生效)        │
├────────────────────────────────────────────────────────────────────────┤
│ Tier 2: 布局与列宽对齐调整 (少量 C++ 视图属性与计算, 消除死区与削顶)          │
├────────────────────────────────────────────────────────────────────────┤
│ Tier 3: Delegate 绘制与复合控件重构 (双行高保真项, 消除 ASCII 进度条)       │
└────────────────────────────────────────────────────────────────────────┘
```

### 4.1 Tier 1：纯 QSS 样式表与调色板（零风险，即刻落地）

- **涉及文件**：`src/ui/setting/ThemeManager.cpp`
- **改动函数**：
  - `strategyPanelStyleSheet(const ThemeTokens &t)`
  - `windows11TabStyleSheet(const QPalette &pal, const ThemeTokens &t)`
  - `overlayStyleSheet(const ThemeTokens &t)`
- **核心改动项**：
  1. 底部状态栏 Chip 配色重构：将原本的反常浅底色替换为 `chipFill = separate(blendToward(t.onSurface, t.surface, 0.08), t.surface, 1.05)`，文字对比度从 **1.13:1** 飙升至 **7.5:1** 以上，彻底终结反光白斑。
  2. 边框降噪：将主面板与视口边框系数从 `0.32` 调整为 `0.16`（`separate(blendToward(t.onSurface, t.surface, 0.16), t.surface, 1.28)`），界面瞬间呈现高级轻拟物悬浮感。
  3. 表头与行高样式约束：在 QSS 中注入 `ProfilesTableView#profilesTableView::item { min-height: 30px; }` 与 `QHeaderView::section { min-height: 30px; }`。
  4. 滚动条精细化：全量注入 8px 宽度的平滑无边框滚动条 QSS。
- **预期视觉收益**：白底白字灾难彻底根治，全软件粗重黑线消失，圆角系统统一为 4px/6px，视觉质感直接达到主流现代应用水准。

---

### 4.2 Tier 2：视图属性与列宽拉伸（极低风险，微量 C++）

- **涉及文件**：
  - `src/ui/mainWindow/mainwindow_view.cpp`
  - `src/ui/mainWindow/mainwindow_setup.cpp`
- **改动函数与位置**：
  1. `refresh_proxy_list_column_size()` (`mainwindow_view.cpp:286-338`)：
     - 将 `ColName` 声明为 `QHeaderView::Stretch`；
     - `ColType` 固定为 84px，`ColTestResult` 固定为 110px，`ColTraffic` 固定为 120px；
     - 禁用 `setStretchLastSection(true)`。
     *预期收益：彻底吃掉表格右侧 273px 巨大黑底死区，长节点别名获得 500px+ 完整开阔视距。*
  2. 表格滚动吸附机制 (`mainwindow_setup.cpp:410-420`)：
     - 加入 `ui->profilesTableView->setVerticalScrollMode(QAbstractItemView::ScrollPerItem);`
     - 加入 `ui->selectorGroupList->setVerticalScrollMode(QAbstractItemView::ScrollPerItem);`
     *预期收益：滚动后整行吸附边界，彻底根治 Row 0 削顶 9px 与策略组末项 14px 截断残影。*
  3. 标题栏瘦身 `make_title()` (`mainwindow_view.cpp:193-215`)：
     - 收敛为 `NxProxy %1%2` 极简字符结构。
     *预期收益：标题字符从 991px 缩减至 220px，与原生窗口控制三键留出 700px+ 安全缓冲。*
  4. 底部速率单行等宽化 (`mainwindow_view.cpp:170-185`)：
     - 将双行换行输出重写为单行：`ui->label_speed->setText(QString("↑ %1   ↓ %2").arg(up, down));`
     *预期收益：底部状态条高度彻底锁定，文字上下跳跃与界面抖动完全消除。*

---

### 4.3 Tier 3：自定义 Delegate 绘制与复合组件（进阶工程化）

- **涉及文件**：
  - 新增：`src/ui/utils/SelectorGroupDelegate.h / .cpp`
  - 改动：`src/ui/utils/DataViewHtmlGenerator.cpp`
  - 改动：`src/ui/mainWindow/mainwindow_view.cpp`
- **核心改动项**：
  1. **策略组列表原生双行 Delegate (`SelectorGroupDelegate`)**:
     - 基于 `QStyledItemDelegate` 重写 `paint()`：
       - 第 1 行绘制 `profile->name`（9pt, `t.muted`，小字）；
       - 第 2 行左侧绘制生效节点名称（9.5pt, `t.onSurface`, 字重 600）；
       - 第 2 行右侧根据 `profile->latency` 绘制圆角彩色胶囊徽标（绿/黄/红/灰），彻底丢弃旧版单行粗暴拼接与 HTML 占位。
  2. **顶部看板由 HTML 文本框重构为原生卡片 (`CommandCenterWidget`)**:
     - 替换 `ui->data_view` (`QTextBrowser`) 内硬拼 ASCII `####----` 的老旧方案；
     - 使用轻量原生 `QProgressBar` 与实体 `QPushButton`，提供真正的平滑进度与一键重启支持。
  3. **表格首列活动状态竖条指示 (Active Node Rail)**:
     - 扩展 `ProfilesTableVerticalHeader` 或模型，在当前生效节点最左侧绘制 3px 宽的 `t.accent` 高亮竖条与发光点，替代简陋的字符串前缀 `"✓ "`。
- **预期视觉收益**：主界面彻底完成现代化蜕变，交互响应与视觉美学达到原生 Fluent / macOS 顶级客户端水准。
