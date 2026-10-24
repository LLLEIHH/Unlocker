# Unlocker WinUI 3 现代化改版 — 任务状态清单

> 更新时间：2026-10-24 11:05（本机重新核验环境 + 打通启动阻塞）  
> 状态：**M5.1 已打通**，应用可正常启动并跑通「拖入/添加 → 扫描 → 操作」主流程

---

## 一、任务总览

| # | 任务 | 状态 |
| - | --- | --- |
| 1 | WinUI 3 UI 现代化改版（按确认的设计稿） | ✅ 已完成 |
| 2 | 构建管线修复（build.cmd / vcxproj） | ✅ 已完成 |
| 3 | 启动崩溃根因排查（0x802B000A fail-fast） | ✅ **根因已修正：不是 PRI，是元数据提供器** |
| 4 | 安装缺失的 VS 构建组件 | ✅ 本机 Community 17.14 自带，无需安装 |
| 5 | PriGen 资源生成正常 | ✅ 已完成（Unlocker.pri + 构建期自动复制 resources.pri） |
| 6 | 应用启动验证 | ✅ 窗口可见、OnLaunched/Activate 正常、单实例可用 |
| 7 | 按钮事件绑定 | ✅ **本轮补齐（此前所有按钮都无响应）** |
| 8 | 单实例冷启动竞态（R-27） | ✅ 已修（3s 重试预算） |
| 9 | 键盘加速键 / 窗口记忆 | ✅ 已补 |
| 9.5 | 窗口缩放自适应 + 最小尺寸 | ✅ 2026-10-24 11:15（见下节） |
| 10 | 收尾清理（调试代码、旧机器残留、文档） | 🟡 诊断开关已降级；旧机器残留待你确认后删 |

---

## 二、本轮（2026-10-24 10:45–11:05）做了什么

### 1. 环境重定位（旧文档写的是另一台机器的路径）

| 项 | 旧记载 | 本机实际 |
| --- | --- | --- |
| VS | BuildTools 或 Community | **只有 Community 17.14**（无 BuildTools），AppxPackage 任务集齐备 |
| CMake | VS 自带路径 | VS 自带路径不存在 → 走 PATH 的 cmake 4.4.4 |
| Python | 固定路径 | `%USERPROFILE%\.workbuddy\binaries\python\versions\3.13.12`（build.cmd 已按用户目录解析） |
| NuGet 四包 | winui/packages | 齐备，无需重新还原 |
| 产物 | winui/build-winui/x64/Release | 可复现 |

### 2. 启动崩溃的真正根因（推翻原 §7.4 结论）

- 实测：把 `Unlocker.pri` 复制成 `resources.pri` 后**仍然** `0x802B000A`；`makepri dump` 也证明 PRI 内容完整
  （`Files/Microsoft.UI.Xaml/Themes/themeresources.xbf`、`Files/App.xbf`、`Files/MainWindow.xbf` 都在）。
- 真因：`App` 手写的 `IXamlMetadataProvider` 对一切类型返回 `nullptr`。框架实例化 `XamlControlsResources`
  （App.xaml 合并字典）时取不到控件类型元数据 → catastrophic failure。
  印证：microsoft-ui-xaml#7606，以及生成的 `XamlTypeInfo.g.cpp:1091`
  （生成的 provider 自己会 `_otherProviders.push_back(XamlControlsXamlMetaDataProvider())`）。
- 解法（不碰手工投影）：新增 `winui/XamlTypeLookup.h`，在 `XamlTypeInfoGlue.cpp`（唯一包含
  `XamlTypeInfo.g.cpp` 的 TU）里把生成的 `XamlTypeInfoProvider` 暴露成三个自由函数，
  `App::GetXamlType/GetXmlnsDefinitions` 委托过去。

### 3. 补齐的工程缺口

- **按钮事件从未绑定**：`MainWindow.xaml` 没有 `Click=`，cpp 里也没有 `.Click(...)` 调用 →
  「添加文件/解锁/删除/改名/移动/复制/重新扫描」全部无响应。现在统一在 `SetupWindow()` 里绑定。
- **`resources.pri` 靠手工复制**：改成 vcxproj 的 `AfterTargets="Build"` 目标自动产出。
- **产物校验靠肉眼**：新增 `tools/check-winui-output.py`，`build.cmd winui` 构建后自动跑，失败即非 0 退出。
- **单实例冷启动竞态（R-27）**：`SendPathsToRunningInstance` 改为 3s 预算内轮询重试，
  转发失败时弹出可读提示（以前静默丢路径）。
- **键盘**：`Ctrl+O` / `Ctrl+Shift+O` / `F5` / `Ctrl+A` / `Del`（仅从列表移除）/ `Ctrl+Del` / `Enter`。
- **窗口位置记忆**：`HKCU\Software\Unlocker\WindowPlacement`，最大化时不覆盖。
- **AppTrace 降级**：默认只在 Debug 构建写 `winui-crash.log`；未处理异常记录仍然总会写。

---

## 三、本轮验证结果

| 验收项 | 结果 |
| --- | --- |
| 双击启动出窗口、无 fail-fast | ✅（窗口标题 `Unlocker`，`OnLaunched`/`Activate` 完成） |
| 带 2 个路径参数冷启动 | ✅ 日志 `UI 加入 2 项` |
| 已有窗口时带参再启动（W9①） | ✅ 不新开窗口，日志 `UI 加入 1 项` |
| 首实例启动 150ms 内二次启动（W9②） | ✅ 两条 `UI 加入 1 项`，路径未丢 |
| 删除 `resources.pri` 后构建校验拦截（W14） | ✅ 校验脚本退出码 1 |
| 正常关闭写回窗口位置 | ✅ 注册表 16 字节 |
| `policy_tests` / `ipc_tests` | ✅ 26/26、10/10 全绿 |

**未自动验证（需人工点）**：拖放实际手感（W2 的可视结果）、UAC 提权链路（T32–T34）、
键盘加速键在真实焦点下的行为、提权后拖放是否失效（W3）。

---

## 三之二、窗口缩放自适应 + 最小尺寸（2026-10-24 11:15）

**问题**：窗口拖小以后内容互相挤压、底部操作区被裁掉，且能拖到几乎不可用的尺寸。

**根因**（两条，都跟 DPI 有关）：
1. XAML 布局按 **DIP** 算，而 `AppWindow` 的尺寸是**物理像素**。原代码写死
   `Resize(1040, 720)`，150% 缩放下实际只有 693×480 DIP —— 生来就比设计尺寸小三分之一。
2. 底部六个操作按钮是固定文案 + 固定列宽，宽度不够时直接溢出被裁，没有任何退化策略。

**改法**：
- `OverlappedPresenter.PreferredMinimum{Width,Height}` = 820×560 DIP（按 `GetDpiForWindow()/96`
  折算成像素，150% 下是 1230×840）。实测程序化 `SetWindowPos(400,300)` 会被挡住。
- 初始尺寸 / 恢复尺寸统一按 DIP 折算；恢复时夹进当前显示器工作区，换显示器不会跑到屏幕外。
- `<980 DIP` 时六个操作按钮退化为纯图标（`ApplyNarrowLayout()`，在 `RootGrid.SizeChanged` 里切换），
  每个按钮都有 ToolTip 说明。
- 两张卡片加 `MinHeight`（176 / 132 DIP），防止被压成一条缝。
- 启动时往 `unlocker.log` 记一行 `窗口 WxH @x,y（DPI 缩放 s）`，排障时不用靠猜。

**踩坑记录**：
- `AdaptiveTrigger` 在这个手写 XAML 胶水工程里**不触发**（VisualState 一直停在基态），
  改用代码驱动 —— 别改回声明式写法。
- 手工写 GDI 截图工具时要注意：进程必须 `SetProcessDpiAwarenessContext(-4)`，
  否则 `GetWindowRect`/`BitBlt` 的坐标被虚拟化（1230×840 读成 820×560）；
  `GetDIBits` 要求位图未被任何 DC 选中，且每行按 4 字节对齐。

**验证**：窄布局（820×560 DIP）与宽布局（1067 DIP）截图各一张，无裁切、无重叠；
`resize_check.py 400x300` 被夹回 1230×840。

## 四、待办1. **人工回归**：拖放 → 扫描 → 五个操作 → 管理员深扫（需 UAC，无法自动化）。
2. **收尾清理**（等你确认删除清单）：`winui/build-winui.bak-lei/`、`build.bak-lei-cache/`。
3. **设计收口（可选）**：MainWindow 行内配色目前是代码按 `ActualTheme` 设置的字面量，
   可搬进 `ThemeDictionaries`（§5.6 令牌表）；不影响功能。

---

## 五、构建工具链（本机实测可用）

| 工具 | 版本/路径 |
| --- | --- |
| Visual Studio 2022 Community | 17.14，`C:\Program Files\Microsoft Visual Studio\2022\Community` |
| MSVC v143 | 14.44 |
| AppxPackage 任务集 | 已随 Community 安装（PriGen 可用） |
| Windows SDK | 10.0.26100（`makepri.exe` 在 `bin\10.0.26100.0\x64`） |
| CMake | PATH 上的 4.4.4（VS 自带路径不存在，build.cmd 自动 fallback） |
| Python | `%USERPROFILE%\.workbuddy\binaries\python\versions\3.13.12\python.exe` |
| NuGet 包 | `winui/packages/`：WASDK 1.7.251107005、CppWinRT 3.0.260818.1、SDK.BuildTools 10.0.22621.756、WebView2 1.0.2903.40 |

构建前必须清空 `HTTP_PROXY`/`http_proxy`（MSB6001 坑，build.cmd 已内置）。
