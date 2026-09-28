# NxProxy (Qt Widgets) 桌面端现代化重构设计规范书 (v2.0)

> **设计基准文件**：`ui-shot-2.png`（尺寸 1083 × 990 px，客户区约 1067 × 951 px）  
> **量测分析方式**：基于 .NET `System.Drawing.Bitmap` 精确像素与颜色逐点采样分析（绝对拒绝主观揣测）  
> **设计系统准则**：严格依托 `ThemeManager` 与 `ThemeTokens`，完全遵从 Qt 6 Widgets / QSS 盒模型与 Windows 11 Fluent 视觉语言，禁止硬编码颜色，禁止引入第三方依赖。

---

## 1. 看了图之后的诊断：哪里显得复古 / 廉价（定量证据清单）

基于对最新截图 `ui-shot-2.png` 的全面采样与量测，提炼出 9 项导致当前界面呈现“复古”、“廉价”与“未完成感”的核心视觉与交互硬伤：

### 诊断 01：标题栏信息超载达 935px，与 Windows 11 系统控制按钮物理重叠
- **像素量测证据**：
  - 系统标题栏范围 `Y: [0, 30]`（高度 31px），底色为中性深灰 `#2F363F`。
  - 标题文字从 `X=16` 一路横跨至 `X=950`，文本像素总宽度达 **935px**（占整窗宽度 1083px 的 **86.3%**）。
  - 文字内容包含 `[Admin] [Select] [Tun+System Proxy] NxProxy 1.0.x...` 等 7 段方括号标签串联。
  - 在 Windows 11 标准窗口下，右上角三枚原生窗口控制按钮（最小化/最大化/关闭）固定占据 `X: [931, 1083]`（宽 152px），导致标题文字末尾的 20px 与关闭按钮区域发生重叠碰撞。
- **视觉显旧根因**：把操作系统顶级标题栏当成了控制台日志输出区，违反 Fluent Design 与 Desktop 界面规范。标准桌面软件标题栏仅展示应用名与极简态，调试标记必须收敛于窗体内。

### 诊断 02：顶部控制栏高度断崖（73px vs 48px），底部悬空 25px 且右侧空置 549px
- **像素量测证据**：
  - 顶部控制条总高度 **91px**（`Y: [31, 121]`），底色 `#1E1E1E`。
  - 左侧 5 枚主功能按钮（`toolButton_program` ~ `toolButton_about`）采用上图下文布局，尺寸为 59~61px 宽 × **73px 高**（`Y: [43, 115]`）。
  - 相邻的核心启停按钮 `toolButton_startstop`（`X: [364, 409]`）为小方钮，外框仅 **48px 高**（`Y: [43, 90]`）；其右侧的模式复选框总高仅 **45px**（`Y: [47, 91]`）。
  - 在 `Y: [91, 115]` 区域，启停按钮与复选框下方出现宽达 **128px、高 25px 的突兀黑洞悬空断崖**，基线彻底断裂。
  - 控制条从 `X=493` 至 `X=1041`（宽度达 **549px**，占栏宽 **58.2%**）纯为空白画布，没有任何数据看板或平衡元素。
- **视觉显旧根因**：上世纪 90 年代 MFC 工具条的散落排布，按钮高低错落、重心失衡，缺乏现代工具栏统一的 40px/48px 基线对齐和卡片化整合。

### 诊断 03：策略组列表单行纯文本硬拼，右侧留白 705px（67.2% 面积浪费）
- **像素量测证据**：
  - 策略组视窗 `selectorGroupList` 宽达 **1049px**（`X: [10, 1058]`），高度 180px（`Y: [142, 321]`）。
  - Item 1（选中项）高度 39px（`Y: [145, 183]`），底色为深蓝灰 `#3C5364`，最左侧 `X: [15, 17]` 带有 3px 宽的亮天蓝 `#76B9ED` 强调竖条。
  - 但其内部文本仍为纯文本硬拼接：`策略组名   →   选中国家/节点名   (N member(s))`，文字像素自 `X=20` 起至 `X=350` 终止，净宽仅 330px。
  - 从 `X=351` 至 `X=1056`（宽达 **705px**，占单项宽度的 **67.2%**）全为单调沉闷的灰色平铺。
- **视觉显旧根因**：横向铺满千像素的巨型视口仅展示一行带空格的日志字符串，没有任何结构化卡片分区或徽章胶囊，给人以未设计、未排版的极度毛糙感。

### 诊断 04：策略组视口高度未取整，第 5 项露头 14px 被下边缘硬性腰斩
- **像素量测证据**：
  - 列表视口净高为 **180px**（`Y: [142, 321]`）。
  - 前 4 项每个项高 39px，项间距 3px，累计消耗高：$39 \times 4 + 3 \times 3 = 165\text{px}$。
  - 剩余可用视口高度仅为 $180 - 165 - 3 = \mathbf{12\text{px}}$。
  - 导致第 5 项 Item 5（`Y: [307, 320]`）仅露出顶部 **14px**，字符顶部 4px 锯齿（`Y: [315, 318]`）直接被视口底边裁切，字符被腰斩。
- **视觉显旧根因**：视口高度未与卡片步进高度整数吸附，且未设置 `ScrollPerItem`，呈现出像排版溢出故障的业余缺陷。

### 诊断 05：节点成员表列宽严重倒挂，核心「名称」仅 174px，而「流量」虚占 289px
- **像素量测证据**：
  - 表格水平列宽实测：类型列 129px（`X: [41, 170]`）、名称列 **174px**（`X: [171, 344]`）、测试结果列 151px（`X: [345, 495]`）、流量列 **289px**（`X: [496, 784]`）。
  - 承载核心业务价值的节点别名（如 `[HK] 香港BGP专线01`）仅分得 174px，Row 2 文字已顶到 `X=341`（距单元格右边线仅 3px），稍长名称即被截断。
  - 反观仅展示简单数字（如 `↑ 0B ↓ 0B`）的流量列，却霸占了 **289px**（几乎是名称列的 1.7 倍）。
- **视觉显旧根因**：典型未配置 `QHeaderView::setSectionResizeMode` 的默认表格布局，信息重心严重错位。

### 诊断 06：节点成员表右侧存在 273px 巨大纯黑死区，横向视口塌陷
- **像素量测证据**：
  - 表格容器右界位于 `X=1058`，总宽 1049px。
  - 第 4 列（流量列）右侧边界终止于 **X=784**。
  - 从 `X=785` 至 `X=1058` 存在宽达 **273px**（占表格视口总宽 **26.0%**）的空白纯色块（颜色为 `#2D2D2D`），既无表头，也无线框，完全塌陷。
- **视觉显旧根因**：表格没有启用弹性拉伸列（Stretch），使得千像素屏幕上约 1/4 的表格视口变成死黑废地。

### 诊断 07：表格行高 24px 与表头 23px 极度局促，上下文字呼吸感归零
- **像素量测证据**：
  - 表头 `QHeaderView::section` 高度实测仅 **23px**（`Y: [342, 364]`），底色 `#3C3C3C`。
  - 数据行每行实测净高 23px + 1px 底分割线 `#262626` = **总行高 24px**（如 Row 1: `Y: 365..388`，Row 2: `Y: 389..412`）。
  - 中英文字模高度在 9~11px 之间，在 24px 行高下，文字上下每侧内边距仅有 **3~4px**。
- **视觉显旧根因**：24px 是 Windows 95 / XP 时代的紧凑表格标准，现代高分屏下字符紧逼边框，产生强烈的窒息与压抑感。现代标准行高至少应为 32~36px。

### 诊断 08：下方控制台 Tab 2 激活态下大面积 262px 死黑，无任何空态引导
- **像素量测证据**：
  - 下方控制台容器 `stats_widget` 总高 **285px**（`Y: [644, 928]`）。
  - Tab 栏（`Y: [616, 643]`）中当前选中的是 Tab 2「连接」（`connections_tab`，背景色 `#3C3C3C`）。
  - 视图内仅顶部表头占据 23px（`Y: [644, 666]`），下方从 `Y=667` 到 `Y=928` 长达 **262px（占比 91.9%）纯为空白深灰 `#2D2D2D`**。
  - 没有任何文本、无占位图标、无“当前未运行，暂无活动连接”的说明。
- **视觉显旧根因**：未运行状态下直接暴露空白无数据控件，用户无法判断是程序崩溃、内核未响应还是正常无连接。

### 诊断 09：底部状态栏 Chip 3 双行折叠，导致三枚胶囊视线剧烈跳跃
- **像素量测证据**：
  - 底部状态栏总高 54px（`Y: [929, 982]`），底色 `#1E1E1E`。三枚 Chip 高度均为 38px（`Y: [935, 972]`），底色已修复为深灰 `#353535`，描边为 1px `#666666`。
  - Chip 1（`label_running`）与 Chip 2（`label_inbound`）：文字单行，垂直居中轴锁定在 **Y=954**，上下留白 14px，平稳匀称。
  - Chip 3（`label_speed`）：文字被折断为双行（Line 1 “上传速率” `Y: [941, 951]`；Line 2 “下载速率” `Y: [955, 966]`），纵向文字跨度达 26px，且因未运行无任何速率数值。
- **视觉显旧根因**：横向并排的 3 枚卡片中，前两枚是大字单基线，末尾突变为紧贴上下边缘的微型双行字，扫视时视线剧烈抖动，打破了整体秩序感。

---

## 2. 目标视觉语言规范（全窗口 Design Tokens）

严格基于 4px / 8px 网格步进、Windows 11 Fluent 视觉层次与色彩对比度原则构建统一规范。

### 2.1 基础度量体系与几何栅格

| 规范维度 | 规格取值 | 适用范围与依据 |
| :--- | :--- | :--- |
| **基础栅格** | **4px / 8px** | 全局度量基准，所有 padding、margin、gap 必须为 4 的整数倍 |
| **小圆角 (R4)** | **4px** | 按钮、输入框、下拉框、表头单元格、延迟状态微徽章（`selectorLatencyBadge`） |
| **中圆角 (R6)** | **6px** | 策略组列表外框、策略组卡片项、底部状态栏 Chip、分组容器 `QGroupBox` |
| **大圆角 (R8)** | **8px** | 弹窗、模态对话框、核心告警横幅容器（Alert Banner） |
| **主控制条高度** | **56px** (原 91px) | 统一所有工具按钮与启停中枢的垂直基线，消除 25px 底部断崖 |
| **策略组卡片项高** | **46px** (原 39px) | 完美容纳两行文字（组名 11px + 节点与徽章 14px + 内边距 16px + 间距 5px） |
| **表格行高** | **32px** (原 24px) | 表头 32px，数据行 32px。文字上下留白增至 10px，提升扫视舒适度 |
| **底栏 Chip 高度** | **36px** (原 38px) | 统一单行排版，内部图标与等宽数据垂直严格对齐 |

### 2.2 字号阶梯与字重体系（Typography Hierarchy）

| 角色命名 | 磅值 / 像素 | 字重 (Weight) | 字族 (Font-Family) | 适用场景与控件 |
| :--- | :--- | :--- | :--- | :--- |
| **Title / Heading** | 10.5pt (14px) | 600 (SemiBold) | `Segoe UI Variable`, `Microsoft YaHei UI` | 弹窗标题、顶部启停状态主要文案 |
| **Section Header** | 9.5pt (13px) | 600 (SemiBold) | `Segoe UI Variable`, `Microsoft YaHei UI` | 区域标题（策略路由组、节点候选池） |
| **Body Regular** | 9pt (12px) | 400 (Regular) | `Segoe UI Variable`, `Microsoft YaHei UI` | 表格正文、常规按钮文案、提示标签 |
| **Body Medium** | 9pt (12px) | 500 (Medium) | `Segoe UI Variable`, `Microsoft YaHei UI` | 策略组当前节点名（`selectorNodeName`） |
| **Caption Small** | 8pt (11px) | 400 (Regular) | `Segoe UI Variable`, `Microsoft YaHei UI` | 策略组名（`selectorGroupName`）、成员数 |
| **Data Monospace** | 8.5pt (11px) | 600 (SemiBold) | `Consolas`, `Segoe UI`, monospace | 延迟数值（`128 ms`）、流量数字、端口 IP |

### 2.3 语义颜色全集与 Token 派生表达式

所有颜色均依托 `ThemeTokens`，严禁硬编码。以下给出在当前 Windows 11 深色模式下的实测色值与派生计算式：

| 语义角色 | 实测基准 Hex | Token 派生表达式 (C++ 规范) | 视觉对比度 | 适用控件与状态 |
| :--- | :--- | :--- | :---: | :--- |
| **Window Surface** | `#1E1E1E` | `t.surface` (底层窗体画布) | 基准 | 主窗口背景、顶部工具栏底色 |
| **Card Surface** | `#262626` | `blendToward(t.onSurface, t.surface, 0.03)` | 1.12:1 | 策略组卡片、常驻底栏背景 |
| **Panel Surface** | `#2D2D2D` | `blendToward(t.onSurface, t.surface, 0.06)` | 1.25:1 | 成员表格奇数行、控制台内容底色 |
| **Row Alternate** | `#333333` | `blendToward(t.onSurface, t.surface, 0.10)` | 1.38:1 | 成员表格偶数交替行底色 |
| **Border Normal** | `#3D444D` | `paneBorder(t)` $\implies$ `separate(blendToward(t.onSurface, t.surface, 0.32), t.surface, 1.9)` | 2.1:1 | 列表容器外边框、表格外框、Chip 外框 |
| **Border Subtle** | `#2B3036` | `blendToward(t.onSurface, t.surface, 0.15)` | 1.4:1 | 表格行间分割线、卡片微弱分割线 |
| **Hover State** | `#283542` | `separate(blendToward(t.accent, t.surface, 0.16), t.surface, 1.08)` | 1.22:1 | 列表项 `:hover`、按钮 `:hover`、行 `:hover` |
| **Selected State** | `#263B52` | `separate(blendToward(t.accent, t.surface, 0.32), t.surface, 1.15)` | 1.45:1 | 策略组卡片选中项、表格活动节点行 |
| **Accent Rail** | `#5F91B7` | `t.accent` | 4.2:1 | 选中项左侧 3px 竖向指示条 |
| **Text Primary** | `#F2F2F2` | `t.onSurface` | 13.5:1 | 核心正文、主要节点别名、选中项文字 |
| **Text Secondary** | `#9DA9B5` | `t.muted` $\implies$ `separate(blendToward(t.onSurface, t.surface, 0.62), t.surface, 4.0)` | 5.2:1 | 组名辅助说明、表头文字、提示标签文案 |
| **Status Good** | `#2ECC71` | `separate(t.success, t.surface, 4.5)` | 6.8:1 | 延迟 ≤100ms 文字色、运行中绿灯 |
| **Good Tint Fill**| `#1B3624` | `blendToward(t.success, t.surface, 0.18)` | 1.3:1 | 优质延迟徽章背景色（18% 柔光填充） |
| **Status Warn** | `#E09B22` | `separate(t.warning, t.surface, 4.5)` | 5.8:1 | 延迟 101~300ms 文字色 |
| **Warn Tint Fill**| `#382C17` | `blendToward(t.warning, t.surface, 0.18)` | 1.3:1 | 警告延迟徽章背景色（18% 柔光填充） |
| **Status Bad** | `#E74C3C` | `separate(t.danger, t.surface, 4.5)` | 5.2:1 | 延迟 >300ms / 超时文字色 |
| **Bad Tint Fill** | `#381E1E` | `blendToward(t.danger, t.surface, 0.18)` | 1.3:1 | 超时与不可用徽章背景色 |
| **Status Info** | `#3498DB` | `separate(t.info, t.surface, 4.0)` | 5.1:1 | Connect OK 连通文字色、测速中文字色 |
| **Info Tint Fill**| `#1B2D3B` | `blendToward(t.info, t.surface, 0.18)` | 1.3:1 | 连通 OK 徽章背景色 |

---

## 3. 可直接粘贴的 Qt QSS 代码（带 Token 映射声明）

以下规则严格遵照 Qt 6 QSS 支持的属性子集（杜绝 box-shadow、flexbox、CSS transform 等非法属性），使用现有确凿 `objectName` 编写。

### 3.1 追加至 `ThemeManager::strategyPanelStyleSheet(const ThemeTokens &t)`

此函数直接负责策略组面板、成员表提示及底部状态栏：

```cpp
// 在 ThemeManager.cpp 的 strategyPanelStyleSheet() 中声明派生色
const auto hex = [](const QColor &c) { return c.name(QColor::HexRgb); };
const QColor border       = paneBorder(t);                                                    // %1
const QColor subtleBorder = blendToward(t.onSurface, t.surface, 0.15);                        // %2
const QColor hover        = separate(blendToward(t.accent, t.surface, 0.16), t.surface, 1.08);// %3
const QColor selected     = separate(blendToward(t.accent, t.surface, 0.32), t.surface, 1.15);// %4
const QColor chipFill     = blendToward(t.onSurface, t.surface, 0.08);                        // %5
const QColor cardBg       = blendToward(t.onSurface, t.surface, 0.03);                        // %6

// 延迟徽章 4 级状态色 (文字与半透明填充)
const QColor badgeGoodText = separate(t.success, t.surface, 4.5);                             // %7
const QColor badgeGoodBg   = blendToward(t.success, t.surface, 0.18);                         // %8
const QColor badgeWarnText = separate(t.warning, t.surface, 4.5);                             // %9
const QColor badgeWarnBg   = blendToward(t.warning, t.surface, 0.18);                         // %10
const QColor badgeBadText  = separate(t.danger, t.surface, 4.5);                              // %11
const QColor badgeBadBg    = blendToward(t.danger, t.surface, 0.18);                          // %12
const QColor badgeInfoText = separate(t.info, t.surface, 4.0);                                // %13
const QColor badgeInfoBg   = blendToward(t.info, t.surface, 0.18);                            // %14

return QStringLiteral(
    /* =========================================================================
       1. 策略组列表视窗与卡片项 (修复视口截断、两行排版、Accent 导轨)
       ========================================================================= */
    "#selectorGroupList {\n"
    "    background-color: transparent;\n"
    "    border: 1px solid %1;\n"
    "    border-radius: 6px;\n"
    "    padding: 4px;\n"
    "    outline: none;\n"
    "}\n"
    "#selectorGroupList::item {\n"
    "    background-color: %6;\n"
    "    border: 1px solid %2;\n"
    "    border-radius: 5px;\n"
    "    margin-bottom: 4px;\n"
    "    padding: 0px;\n"
    "    min-height: 44px;\n"
    "}\n"
    "#selectorGroupList::item:hover:!selected {\n"
    "    background-color: %3;\n"
    "    border-color: %1;\n"
    "}\n"
    "#selectorGroupList::item:selected {\n"
    "    background-color: %4;\n"
    "    border: 1px solid %1;\n"
    "    border-left: 3px solid %15;\n" /* %15: t.accent */
    "}\n"

    /* 策略组两行项内部 Label 层次规范 */
    "#selectorGroupName {\n"
    "    color: %16;\n"                  /* %16: t.muted */
    "    font-size: 11px;\n"
    "    font-weight: 500;\n"
    "}\n"
    "#selectorGroupCount {\n"
    "    color: %16;\n"
    "    font-size: 10px;\n"
    "    background-color: %2;\n"
    "    border-radius: 3px;\n"
    "    padding: 1px 4px;\n"
    "}\n"
    "#selectorNodeName {\n"
    "    color: %17;\n"                  /* %17: t.onSurface */
    "    font-size: 12px;\n"
    "    font-weight: 600;\n"
    "}\n"

    /* 延迟徽章胶囊 (Pill Badge) */
    "#selectorLatencyBadge {\n"
    "    font-family: 'Consolas', 'Segoe UI', monospace;\n"
    "    font-size: 11px;\n"
    "    font-weight: 600;\n"
    "    border-radius: 4px;\n"
    "    padding: 2px 7px;\n"
    "}\n"
    "#selectorLatencyBadge[latencyClass=\"good\"]  { color: %7;  background-color: %8;  border: 1px solid %7;  }\n"
    "#selectorLatencyBadge[latencyClass=\"warn\"]  { color: %9;  background-color: %10; border: 1px solid %9;  }\n"
    "#selectorLatencyBadge[latencyClass=\"bad\"]   { color: %11; background-color: %12; border: 1px solid %11; }\n"
    "#selectorLatencyBadge[latencyClass=\"info\"]  { color: %13; background-color: %14; border: 1px solid %13; }\n"
    "#selectorLatencyBadge[latencyClass=\"muted\"] { color: %16; background-color: transparent; border: 1px solid %2; }\n"

    /* 区域提示标签 */
    "#selectorGroupHint, #selectorMemberHint {\n"
    "    color: %16;\n"
    "    font-size: 11px;\n"
    "    padding: 2px 4px;\n"
    "}\n"

    /* =========================================================================
       2. 常驻底部状态栏 (消灭双行基线抖动，3枚单行独立 Chip 秩序化)
       ========================================================================= */
    "#label_running, #label_inbound, #label_speed {\n"
    "    background-color: %5;\n"
    "    border: 1px solid %1;\n"
    "    border-radius: 6px;\n"
    "    color: %17;\n"
    "    font-size: 12px;\n"
    "    padding: 4px 12px;\n"
    "    min-height: 34px;\n"
    "    max-height: 34px;\n"
    "    margin-right: 8px;\n"
    "}\n"
    "#label_speed {\n"
    "    font-family: 'Consolas', 'Segoe UI', monospace;\n"
    "}\n"
)
.arg(hex(border))          // 1
.arg(hex(subtleBorder))    // 2
.arg(hex(hover))           // 3
.arg(hex(selected))        // 4
.arg(hex(chipFill))        // 5
.arg(hex(cardBg))          // 6
.arg(hex(badgeGoodText))   // 7
.arg(hex(badgeGoodBg))     // 8
.arg(hex(badgeWarnText))   // 9
.arg(hex(badgeWarnBg))     // 10
.arg(hex(badgeBadText))    // 11
.arg(hex(badgeBadBg))      // 12
.arg(hex(badgeInfoText))   // 13
.arg(hex(badgeInfoBg))     // 14
.arg(hex(t.accent))        // 15
.arg(hex(t.muted))         // 16
.arg(hex(t.onSurface));    // 17
```

### 3.2 追加至 `ThemeManager::overlayStyleSheet(const ThemeTokens &t)`

此函数负责全应用通用的表格、列头、标签页与按钮现代化细节：

```cpp
// 在 ThemeManager.cpp 的 overlayStyleSheet() 中追加：
const auto hex = [](const QColor &c) { return c.name(QColor::HexRgb); };
const QColor border       = paneBorder(t);
const QColor subtleBorder = blendToward(t.onSurface, t.surface, 0.15);
const QColor headerBg     = blendToward(t.onSurface, t.surface, 0.12);
const QColor tableRowAlt  = blendToward(t.onSurface, t.surface, 0.04);
const QColor tableHover   = separate(blendToward(t.accent, t.surface, 0.14), t.surface, 1.08);
const QColor tableSelect  = separate(blendToward(t.accent, t.surface, 0.28), t.surface, 1.15);

sheet += QStringLiteral(
    /* =========================================================================
       3. 核心节点表格 ProfilesTableView (现代 32px 行高、高雅交替底色、平滑选中态)
       ========================================================================= */
    "#profilesTableView {\n"
    "    background-color: %1;\n"        /* t.surface */
    "    alternate-background-color: %2;\n" /* tableRowAlt */
    "    gridline-color: transparent;\n"
    "    border: 1px solid %3;\n"        /* border */
    "    border-radius: 6px;\n"
    "    outline: none;\n"
    "}\n"
    "#profilesTableView::item {\n"
    "    min-height: 32px;\n"
    "    max-height: 32px;\n"
    "    padding: 0px 8px;\n"
    "    border-bottom: 1px solid %4;\n" /* subtleBorder */
    "    color: %5;\n"                   /* t.onSurface */
    "}\n"
    "#profilesTableView::item:hover {\n"
    "    background-color: %6;\n"        /* tableHover */
    "}\n"
    "#profilesTableView::item:selected {\n"
    "    background-color: %7;\n"        /* tableSelect */
    "    color: #FFFFFF;\n"
    "}\n"
    "#profilesTableView QHeaderView::section {\n"
    "    background-color: %8;\n"        /* headerBg */
    "    color: %9;\n"                   /* t.muted */
    "    font-size: 11px;\n"
    "    font-weight: 600;\n"
    "    border: none;\n"
    "    border-bottom: 1px solid %3;\n"
    "    border-right: 1px solid %4;\n"
    "    padding: 6px 8px;\n"
    "    min-height: 28px;\n"
    "}\n"
    "#profilesTableView QHeaderView::section:vertical {\n"
    "    width: 28px;\n"
    "    qproperty-alignment: 'AlignCenter';\n"
    "}\n"

    /* =========================================================================
       4. 顶部主控制条与按钮修缮
       ========================================================================= */
    "#toolButton_startstop {\n"
    "    border-radius: 6px;\n"
    "    font-weight: 600;\n"
    "    padding: 6px 12px;\n"
    "}\n"
)
.arg(hex(t.surface))       // 1
.arg(hex(tableRowAlt))     // 2
.arg(hex(border))          // 3
.arg(hex(subtleBorder))    // 4
.arg(hex(t.onSurface))     // 5
.arg(hex(tableHover))      // 6
.arg(hex(tableSelect))     // 7
.arg(hex(headerBg))        // 8
.arg(hex(t.muted));        // 9
```

---

## 4. 交互与信息架构改动（以操作效率为导向）

### 4.1 策略组面板的核心呈现与点击联动逻辑
- **卡片展示内容规范**：
  - **行 1 (顶行)**：左侧为策略组名称（`selectorGroupName`，如 `节点选择`、`自动选择`，9.5pt 加粗），右侧为成员计数胶囊（`selectorGroupCount`，如 `14 节点`）；
  - **行 2 (底行)**：左侧为当前使用的出站节点名称（`selectorNodeName`，如 `香港 BGP 专线 01`，文字过长采用中段省略 `Qt::ElideMiddle`），右侧为**语义延迟徽章**（`selectorLatencyBadge`，展示 `68 ms` / `Connect OK` / `超时`）。
- **点击交互逻辑**：
  - **单击策略组卡片**：
    1. 当前项立即激活高亮（左边缘弹出 3px Accent 色条）；
    2. 下方/右侧 `profilesTableView` 立即过滤，仅显示该组所包含的候选成员节点；
    3. 表格视口自动滚屏吸附到当前该组正在使用的生效节点上；
    4. 成员表上方提示条动态更新为：`策略组: [节点选择] (共 14 个节点)`。
  - **单击成员表中的备选节点**：
    1. **即时乐观 UI 更新（Optimistic UI）**：该节点行立即显示高亮与勾选标记，上方策略组卡片底行的节点名与延迟徽章瞬间同步替换；
    2. **防抖异步 RPC（Debounced RPC）**：向内核发送 `ChangeSelectedNode(groupID, nodeID)`，施加 200ms 防抖，防止用户快速连点打崩内核管道；
    3. **异常静默回退**：若内核返回切换失败，自动回退勾选位置，并通过底部状态栏弹出 Danger 提示 `“切换节点失败：节点不可达”`。

### 4.2 测速中 / 未测试 / 失败 / 空态处理设计
1. **测试中状态（Testing in Progress）**：
   - 策略组卡片与表格对应行延迟位置显示淡蓝胶囊（`latencyClass="info"`），文案为 `⏳ 测速中...`；
   - 彻底解耦 `label_running`：**绝对禁止在测速期间将底部状态栏改为 “Testing”**，保持核心运行态长驻可见；
   - 顶部控制条右侧显示轻量进度微标：`测速进度: 6/14 (42%)`。
2. **未测试状态（Untested / Idle）**：
   - 延迟徽章显示细线描边灰色中划线 `—`（`latencyClass="muted"`），避免未测速节点满屏显示大面积红色“0ms”或错误假象。
3. **失败 / 超时状态（Timeout / Unavailable）**：
   - 延迟徽章显示柔和红底暗红字 `超时`（`latencyClass="bad"`），鼠标悬停（ToolTip）提示精确失败原因（如 `dial tcp: i/o timeout`）。
4. **策略组为空的空态（Empty State）**：
   - 当策略组无成员（或订阅失效）时，表格不展示死黑空白，居中展示柔和文本：`此策略组暂无可用的出站节点`，并提供 `[导入节点]` 快捷动作。

### 4.3 「当前节点 + 延迟 + 连通性」三者共存设计权衡
- **严正结论**：**严禁在界面上并列放置三个独立控件**（这会导致卡片与表格被无意义的信息垃圾填满）。
- **统一方案**：收敛为一个**“智能状态徽章（Smart Latency Badge）”**，通过文案与色彩矩阵无损传达三项信息：
  - *连通良好 + 有 HTTP 延迟* $\implies$ 绿/黄底黑字：`68 ms` / `185 ms`（数值代表延迟，绿色代表连通与可用）；
  - *连通良好 + 仅 TCP/ICMP 探测通过* $\implies$ 蓝底白字：`Connect OK`（连通性确认）；
  - *连通失败 / 阻断* $\implies$ 红底白字：`超时` / `不可用`（连通性断开）；
  - *正在探测* $\implies$ 蓝底闪烁：`测速中...`。
  三者融为一体，尺寸仅占 60px 宽度，信息完整且秩序井然。

### 4.4 键盘操作与防多余点击
- **键盘导航**：
  - 支持键盘方向键 `↑` / `↓` 顺畅遍历策略组或表格成员行；
  - 聚焦表格行时，按下 `Space`（空格键）直接激活切为当前生效节点；
  - 按下 `Ctrl + T` 立即对当前选中的单节点或全策略组发起测速。
- **杜绝多余点击**：
  - 彻底废除“先在表格中点击选中行 $\implies$ 再点击右键 $\implies$ 再点‘设为活动节点’”的三步冗余流程；单击即设为当前生效节点（带防抖）。

---

## 5. QSS 做不到、必须改代码的部分（C++ 实现精准指引）

以下功能属于 Qt 样式表引擎的能力盲区，必须在 C++ 代码中配置：

### 5.1 节点表列宽策略重置（根除 273px 死区与列宽倒挂）
- **涉及文件**：`src/ui/mainWindow/mainwindow_setup.cpp`
- **对应函数**：`MainWindow::init_profiles_table_view()` 与 `MainWindow::refresh_proxy_list_column_size()`
- **必须执行的 C++ 代码**：
  ```cpp
  // 彻底消除右侧 273px 死区，并将弹性宽度赋予最重要的「名称」列
  auto *hHeader = ui->profilesTableView->horizontalHeader();
  hHeader->setSectionResizeMode(ColType,       QHeaderView::Fixed);
  hHeader->resizeSection(ColType, 84);                           // 类型：固定紧凑 84px
  hHeader->setSectionResizeMode(ColName,       QHeaderView::Stretch); // 名称：自动弹性吃满全部剩余视口 (300px+)
  hHeader->setSectionResizeMode(ColTestResult, QHeaderView::Fixed);
  hHeader->resizeSection(ColTestResult, 110);                    // 延迟：固定 110px
  hHeader->setSectionResizeMode(ColTraffic,    QHeaderView::Fixed);
  hHeader->resizeSection(ColTraffic, 120);                       // 流量：收缩至 120px (原 289px 严重虚假留白)
  hHeader->setStretchLastSection(false);                         // 严禁将最后一列拉伸
  ```

### 5.2 表格整行滚动吸附与行高设置（根除首行削顶 9px 与 24px 局促感）
- **涉及文件**：`src/ui/mainWindow/mainwindow_setup.cpp`
- **对应函数**：`MainWindow::init_profiles_table_view()`
- **必须执行的 C++ 代码**：
  ```cpp
  // 1. 开启整项滚动吸附，彻底解决滚轮触发后的 9px 削顶残影
  ui->profilesTableView->setVerticalScrollMode(QAbstractItemView::ScrollPerItem);
  ui->selectorGroupList->setVerticalScrollMode(QAbstractItemView::ScrollPerItem);

  // 2. 统一设定桌面标准舒适行高 (32px) 与表头高度 (32px)
  ui->profilesTableView->verticalHeader()->setDefaultSectionSize(32);
  ui->profilesTableView->horizontalHeader()->setFixedHeight(32);
  ```

### 5.3 标题栏去调试化重构（根除 935px 标题超载与系统按钮碰撞）
- **涉及文件**：`src/ui/mainWindow/mainwindow_view.cpp`
- **对应函数**：`MainWindow::make_title()` (约第 197-217 行)
- **必须执行的 C++ 代码**：
  ```cpp
  // 标题栏回归原生桌面标准：仅展示应用名、版本与停运标记，其余状态交由界面 Chip 承载
  auto make_title = [=, this](bool isTray) {
      if (isTray) {
          // 托盘悬停 ToolTip 保持丰富详情
          QStringList tip;
          tip << QString("%1 %2").arg(software_name, NKR_VERSION);
          if (running != nullptr) tip << running->outbound->DisplayTypeAndName() + "@" + group_name;
          else tip << tr("Stopped");
          return tip.join("\n");
      }
      // 窗口顶栏：限制在 30 字符内，绝不碰触右上角关闭按钮
      return QString("%1 %2%3").arg(software_name, NKR_VERSION, running != nullptr ? "" : QString(" (%1)").arg(tr("Stopped")));
  };
  setWindowTitle(make_title(false));
  ```

### 5.4 底部速率 Chip 单行化与等宽数字对齐（根除双行折叠与基线跳跃）
- **涉及文件**：`src/ui/mainWindow/mainwindow_view.cpp`
- **对应函数**：`MainWindow::update_traffic()` 或速率刷新点
- **必须执行的 C++ 代码**：
  ```cpp
  // 废除换行输出，改用单行等宽排版，未运行时显示连字符或 0 B/s
  const QString upStr   = running ? uploadSpeed   : "0 B/s";
  const QString downStr = running ? downloadSpeed : "0 B/s";
  ui->label_speed->setText(QString("↑ %1   ↓ %2").arg(upStr, -8).arg(downStr, -8));
  ui->label_speed->setAlignment(Qt::AlignCenter);
  ```

### 5.5 解耦测速行为与运行态标签（根除测速冲掉节点名缺陷）
- **涉及文件**：`src/ui/mainWindow/mainwindow_view.cpp`
- **对应函数**：`MainWindow::url_test_current()` (约第 408-436 行)
- **改动方式**：
  - 彻底删除 `ui->label_running->setText(tr("Testing"))` 及其还原计时器；
  - 测速状态仅通过表格中的单元格模型与策略组徽章渲染，使 `label_running` 专注表达核心当前连接。

---

## 6. 分级实施清单（按投入产出比排期）

### Tier 1：高收益、极低风险（今天就能完成落地，立竿见影）

| 序号 | 改动文件路径 | 具体修改内容 | 用到的 Token / 核心表达式 | 预期视觉收益 |
| :---: | :--- | :--- | :--- | :--- |
| **T1-1** | `src/ui/setting/ThemeManager.cpp` | 在 `strategyPanelStyleSheet()` 补充完整的策略组两行文字、延迟徽章配色规则及底栏固定高度 Chip | `badgeGoodText = separate(t.success, t.surface, 4.5)`<br>`chipFill = blendToward(t.onSurface, t.surface, 0.08)` | 策略组卡片呈现现代两行高雅排版，延迟色标清晰，底栏 Chip 不再抖动（视觉收益 **9.5**） |
| **T1-2** | `src/ui/setting/ThemeManager.cpp` | 在 `overlayStyleSheet()` 注入表格现代化规则，设置 32px 舒适行高、微妙交替底色与无网格边框 | `tableRowAlt = blendToward(t.onSurface, t.surface, 0.04)`<br>`tableSelect = blendToward(t.accent, t.surface, 0.28)` | 彻底消除表格紧凑局促感，文字上下留白充裕，扫视效率成倍提升（视觉收益 **8.5**） |
| **T1-3** | `src/ui/mainWindow/mainwindow_setup.cpp` | 重置表格列宽：名称列设为 `Stretch`，流量列缩至 120px，开启 `ScrollPerItem` | `hHeader->setSectionResizeMode(ColName, QHeaderView::Stretch)`<br>`verticalScrollMode = ScrollPerItem` | 彻底消灭表格右侧 273px 巨大死黑死区，消除首行削顶 9px 故障（视觉收益 **9.5**） |
| **T1-4** | `src/ui/mainWindow/mainwindow_view.cpp` | 精简 `make_title()`，去除 7 重方括号堆叠，恢复 Windows 11 规范标题 | `setWindowTitle(QString("%1 %2%3").arg(software_name, NKR_VERSION, ...))` | 彻底解决标题栏横跨 935px 与关闭按钮撞车重叠的灾难（视觉收益 **8.0**） |
| **T1-5** | `src/ui/mainWindow/mainwindow_view.cpp` | 修复 `label_speed` 为单行等宽排版（`↑ 0 B/s   ↓ 0 B/s`）并限制高度 | `ui->label_speed->setFixedHeight(34)`<br>`Consolas, monospace` | 底部 3 枚胶囊卡片高度与文字基线严格统一，消除扫视跳跃（视觉收益 **8.0**） |

---

### Tier 2：中度改造（消除结构性失衡，建立一流桌面端体验）

| 序号 | 改动文件路径 | 具体修改内容 | 预期视觉收益 |
| :---: | :--- | :--- | :--- |
| **T2-1** | `include/ui/mainwindow.ui` | 顶部控制条结构归一化：将主功能按钮由上图下文（73px）改为水平图文小卡片（40px），启停按钮统一为 40px 高度，消除 25px 底部断崖 | 控制栏高度从 91px 缩减至 56px，多腾出 35px 宝贵垂直空间给表格，基线严整（视觉收益 **8.5**） |
| **T2-2** | `src/ui/mainWindow/mainwindow_view.cpp` | 测速流程解耦：删除 `label_running->setText("Testing")`，在顶部右侧新增轻量进度胶囊，表格对应行显示淡蓝动画徽章 | 运行态在测速时不失联，进度可视化平滑可靠（视觉收益 **8.0**） |
| **T2-3** | `include/ui/mainwindow.ui` + `mainwindow_setup.cpp` | 调整主分割器权重与控制台空态：默认将上方工作区分割比例加大，下方控制台折叠至 140px；当未运行时在 `connections` 视图居中显示友好占位引导 | 消除下方 262px 纯黑死区，让无连接时的空态变得精致温馨（视觉收益 **7.5**） |

---

### Tier 3：架构级演进（比肩顶级原生客户端）

| 序号 | 改动文件路径 | 具体修改内容 | 预期视觉收益 |
| :---: | :--- | :--- | :--- |
| **T3-1** | `include/ui/mainwindow.ui` | 将策略组与节点表由纵向堆叠重构成**左右水平 Splitter 分栏架构**（左侧策略组列表 320px + 右侧节点详情表 720px） | 彻底契合千像素宽屏视口，策略组项无需浪费 700px 横向空间，节点表垂直高度拉伸至 600px+（视觉收益 **10.0**） |
| **T3-2** | 全局窗口架构 | 引入 Windows 11 DWM Mica / Acrylic 材质边框，自定义沉浸式无边框标题栏 | 彻底迈入现代 Windows 11 Fluent 顶级应用行列（视觉收益 **9.0**） |

---

## 7. 诊断事实核对结论（针对用户看不到图的明确反馈）

1. **截图中策略组列表是否有内容？**  
   - **确定有内容**。截图中策略组列表视口（`Y: 142..321`）内实际加载了 **5 个策略组项**。
   - Item 1（选中项）底色为 `#3C5364`，带有 3px 天蓝左边条；Item 2~4 底色为 `#2D2D2D`，高度均为 39px；Item 5 处于视口底边，被硬性切断仅露头 14px。
   - 所有项目前均采用单行硬编码字符串排版：`策略组名   →   选中国家/节点名   (N member(s))`，右侧存在长达 705px 的纯灰色空洞。两行式卡片与彩色延迟微徽章在截图中**尚未生效**。

2. **当前界面是否处于「未运行 / 空态」？**  
   - **确定处于「未运行（Stopped / Idle）」状态**。
   - **铁证 1**：底部状态栏 Chip 1（`label_running`，`X: 29..76, Y: 949..959`）文字像素采样确认清晰显示为 **“未运行”**（白字深灰底 `#353535`）。
   - **铁证 2**：底部状态栏 Chip 3（`label_speed`）仅显示两行静态字“上传速率”与“下载速率”，数值为空/零。
   - **铁证 3**：下方控制台 Tab 2「连接」被激活，整个 262px 视图内部没有任何连接数据行，呈现大面积纯黑灰色块。
