# WinUI 3 执行方案评审记录

> **评审对象**：`Unlocker-Execution-Plan-WinUI3.md`（v1.0-winui3，2026-10-24）
> **评审日期**：2026-10-24
> **评审方式**：文档逐节核对 + 代码/工程现状交叉验证
> **核对材料**：`winui/main.cpp`、`winui/app.manifest`、`winui/UnlockerApp.vcxproj`、`winui/` 目录结构、`WinUI-TaskStatus.md`
>
> **总体结论**：方案质量高，决策记录（D1–D5）、v0.8 继承关系表、线程规范（§5.4 六条）、构建链风险（R-19/§7.4/§7.7）均准确到位，绝大部分论断与代码现状一致。发现 **3 处硬伤、2 处规则与现状不符、6 项建议**。建议至少修正硬伤 1–3 后再作为执行基准。

---

## 一、硬伤（应修正）

### H1. §8 人日合计错误

§8 里程碑表逐行相加：

```
1.5 + 3.5 + 2 + 4 + 2.5 + 2.5 + 1.5 + 1 + 1 + 0.5 + 2.5 + 3 = 25.5 人日
```

合计行与 §0 速览均写 **24 人日**，实际明细总和为 **25.5 人日**。

**改法**（二选一）：
- 直接改数字为 25.5；
- 或重新核减某阶段工作量，使明细与 24 对齐。

### H2. §8 关键路径图漏了 M2

关键路径图画的是 `M1 → M3 → M4`，M2（FileOps）凭空消失，与上表"M2 在 M3 之前"矛盾。FileOps 是 M3 解锁后删除的前置依赖，属关键路径环节，不应缺席。

**改法**：补为 `M1 → M2 → M3 → M4 → …`（按上表实际依赖关系）。

### H3. §5.2.1 消息名不存在

原文写「UIPI 会拦掉……`WM_DROPDATACOPY`/`WM_COPYGLOBALDATA` 消息」——**Windows 不存在 `WM_DROPDATACOPY` 这个消息**。

实际机制：UIPI 拦截的是 `WM_DROPFILES` 与 `WM_COPYGLOBALDATA`，以及 OLE drop target 的内部窗口消息。

**影响**：结论（提权后拖放失效）本身正确，但该依据位于排障决策树级别，依据错误会误导后续排查方向。

**改法**：把消息列表改为 `WM_DROPFILES` / `WM_COPYGLOBALDATA`（OLE DnD 内部消息）。

---

## 二、规则与现状不符

### N1. §6.4「worker/CLI 分支不得 include XAML 头」与现状冲突

§6.4 规则写「worker / CLI 分支**不得 include XAML 头**（可放独立 TU 隔离）」，但现有 `winui/main.cpp` 无条件 `#include "App.xaml.h"`，三个分支在同一编译单元。

运行时无害（不调用 XAML API 就不触发加载），但文档把**未达成的状态**写成了规则。

**改法**（二选一）：
- 把 UI 启动代码拆到独立 TU，使 main.cpp 现状符合规则；
- 或把规则软化为「不得**调用** XAML API / 不得触发 WinUI 初始化」（运行时约束而非编译期约束）。

### N2. §4.4 启动序列草图与实际代码有出入

实际 `main.cpp` 对 mutex 创建失败、`LocalFree(argv)` 释放均有处理，草图未体现。草图标注了「钉死，别改顺序」，若他人照草图"修正"实际代码，反而会引入泄漏/崩溃。

**改法**：草图直接抄现状代码（含错误处理），保证图文一致。

---

## 三、建议（可选）

### S1. 「拖放无解」写得过死（§5.2.1）

XAML 窗口背后仍是真实 HWND（经 `Win32Interop` 可取得），理论上可对 AppWindow 及其子窗口尝试 `ChangeWindowMessageFilterEx` 放行相关消息——这是 WPF 提权应用的老 workaround。

**改法**：措辞从「无解」改为「无公开可靠路径，不投入排查工时；W3 反证用例固化当前行为」。W3 用例已在计划中，行为是否可接受让测试说话，正文不必把"无解"写成事实。

### S2. §7.6 防回归铁律建议补第 4 条

`WindowsPackageType=None` + `WindowsAppSDKSelfContained=true` 是 D2 决策在构建侧的落点（已核验 `winui/UnlockerApp.vcxproj` 第 36–38 行：`AppxPackage=false`、`WindowsPackageType=None`、`WindowsAppSDKSelfContained=true`），但未进 §7.6 铁律。

该配置被 VS 属性页"顺手点掉"的症状同样隐蔽（直接变成 MSIX 打包或框架依赖部署）。

**改法**：补为第 4 条：vcxproj 必须保持 `AppxPackage=false` + `WindowsPackageType=None` + `WindowsAppSDKSelfContained=true`。

### S3. §5.2.2 备选 HWND 写法依赖实现细节

`reinterpret_cast<HWND>(AppWindow().Id().Value)` 依赖未承诺的实现细节（Id 数值恰好是 HWND）。正道 `GetWindowFromWindowId` 已写，备选写法徒增误导。

**改法**：删除备选写法，只留 `Win32Interop::GetWindowFromWindowId`。

### S4. W9 单实例用例缺竞态场景

「首实例刚启动、命名管道监听尚未就绪时，二实例立即发送路径」的竞态窗口未覆盖。

**改法**：W9 补充该场景及预期行为（如：管道不可达时的降级策略——忽略/重试/提示）。

### S5. M5.3 工作量偏乐观

M5.3 仅 1 人日，需覆盖拖放全部边缘情况（.lnk / UNC / 长路径 / 500 项 / 去重）+ 文件选择器 + 单实例转发。R-23 风险条目自己的描述都比这复杂。

**改法**：M5.3 调至 1.5–2 人日，或在阶段说明中明确"边缘情况留 M6 回归期收尾"。

### S6. §7.2 环境表加本机状态标注

UniversalBuildTools 一行建议标注：**本机（VS 2022 Community 17.14）组件已装齐、经 2026-10-24 核验等效，此表仅供新机器 / CI 重建环境时使用**。避免后来者重复走一遍 23 分钟的组件安装排查。

---

## 四、核验通过项（无问题）

以下内容经代码/工程文件交叉验证，与文档一致：

| 文档章节 | 核验点 | 结果 |
| --- | --- | --- |
| §4.4 | `main.cpp` 三身份分流顺序（worker / CLI 先于 XAML；`init_apartment` 在 `Application::Start` 之前） | ✅ 一致 |
| §4.4 | 单实例 mutex 用 `Local\`（会话语义），非 `Global\` | ✅ 一致 |
| §4.3 | `winui/` 目录结构与文档目录图一致（App/MainWindow/pch/idl/vcxproj/manifest 齐备） | ✅ 一致 |
| §4.4 / §7.6 | `app.manifest`：`asInvoker` + `permonitorv2` + `longPathAware` 均在 | ✅ 一致 |
| D2 / §7.6 | vcxproj unpackaged 自包含三属性已设 | ✅ 一致 |
| §7.3 / §7.4 | 手写胶水 targets、resources.pri 等式，与 `WinUI-TaskStatus.md` 踩坑记录吻合 | ✅ 一致 |
| §7.4 | `WindowsAppRuntimeAutoInitializer`（MddBootstrap 自初始化）来源描述 | ✅ 一致 |

---

## 五、修订清单汇总

| # | 级别 | 位置 | 动作 |
| --- | --- | --- | --- |
| H1 | 硬伤 | §8 / §0 | 人日 24 → 25.5（或核减明细） |
| H2 | 硬伤 | §8 关键路径图 | 补 M2（FileOps） |
| H3 | 硬伤 | §5.2.1 | `WM_DROPDATACOPY` → `WM_DROPFILES` / `WM_COPYGLOBALDATA` |
| N1 | 不符 | §6.4 | 规则改为运行时约束，或拆 TU 使现状达标 |
| N2 | 不符 | §4.4 | 草图抄现状代码 |
| S1 | 建议 | §5.2.1 | 「无解」改为「不投入，W3 固化行为」 |
| S2 | 建议 | §7.6 | 补第 4 条铁律（unpackaged 三属性） |
| S3 | 建议 | §5.2.2 | 删 `reinterpret_cast` 备选写法 |
| S4 | 建议 | W9 | 补管道竞态场景 |
| S5 | 建议 | §8 M5.3 | 工作量调至 1.5–2 人日或明确边缘情况后置 |
| S6 | 建议 | §7.2 | 环境表加本机已装齐标注 |

> 待用户确认后，按上表修订 `Unlocker-Execution-Plan-WinUI3.md` 原文。
