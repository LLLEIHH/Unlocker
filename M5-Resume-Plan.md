# M5.1 打通 + 后续收口 — 本轮施工方案

> 版本：v1（2026-10-24 10:50）／ **执行完成 2026-10-24 11:05（P0–P2 全绿，见文末「执行结果」）**  
> 依据：`Unlocker-Execution-Plan-WinUI3.md` v1.1 §7.7 / §8 / §13，以及本轮本机实测

---

## 0. 环境重定位结论（旧机器路径已失效，全部重新核验）

| 项               | 旧文档记载                                        | **本机实际（2026-10-24 核验）**                                                                                    | 结论                                  |
| --------------- | -------------------------------------------- | ---------------------------------------------------------------------------------------------------------- | ----------------------------------- |
| VS              | BuildTools / Community 二选一                   | **只有 `C:\Program Files\Microsoft Visual Studio\2022\Community`**（无 BuildTools）                             | build.cmd / CMakeLists 的三级探测已覆盖，无需改 |
| AppxPackage 任务集 | 需装 UniversalBuildTools                       | `...\Community\MSBuild\Microsoft\VisualStudio\v17.0\AppxPackage\` 已在                                       | PriGen 可正常工作（已验证能产出 `Unlocker.pri`） |
| Windows SDK     | 10.0.26100                                   | `C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\makepri.exe` 存在                               | 可用                                  |
| CMake           | VS 自带                                        | VS 自带路径不存在 → fallback 到 PATH 的 `cmake 4.4.4`（WinGet）                                                       | 可用                                  |
| Python          | 固定盘符路径                                       | `C:\Users\11415\.workbuddy\binaries\python\versions\3.13.12\python.exe`（build.cmd 已按 `%USERPROFILE%` 解析）   | 可用                                  |
| NuGet 四包        | `winui/packages/`                            | 4 个包齐备（WASDK 1.7.251107005 / CppWinRT 3.0.260818.1 / SDK.BuildTools 10.0.22621.756 / WebView2 1.0.2903.40） | 无需重新还原                              |
| 产物              | `winui/build-winui/x64/Release/Unlocker.exe` | 存在（738 KB，10-24 01:26 构建）                                                                                  | 可复现                                 |
| 旧机器残留           | —                                            | `winui/build-winui.bak-lei/`、`build.bak-lei-cache/`                                                        | 收尾时列清单后删除（需你确认）                     |

**当前唯一阻塞（实测复现）**：双击 `Unlocker.exe` → `Application::Start` 之前全部正常，App 构造完成、  
`OnLaunched` 从未进入 → 未处理异常 `0x802B000A`，`[restricted]` 行无可读描述。

```
[10:45:38.432] wWinMain: Application::Start 前
[10:45:38.459] App ctor: 开始
[10:45:38.461] App ctor: InitializeComponent 完成
[2026-10-24 10:45:38] UNHANDLED hr=0x802B000A
```

---

## 1. 根因修正：不是 PRI，是 XAML 元数据提供器没有回落到框架 provider

原方案 §7.4 把 fail-fast 归因为「`resources.pri` 名字/结构不对」。本轮实测**否证**了这条：

- 把 `Unlocker.pri` 复制成 `resources.pri` 后启动，**仍然 `0x802B000A`**；
- `makepri dump` 显示 PRI 内容完整且映射正确：
  ```
  ResourceMap name="Unlocker"
    Files/Microsoft.UI.Xaml/Themes/themeresources.xbf
    Files/Microsoft.UI.Xaml/DensityStyles/Compact.xbf
    Files/Microsoft.UI.Xaml/Assets/NoiseAsset_256X256_PNG.png
    Files/App.xbf  Files/MainWindow.xbf
  ```

真正的机理（与 microsoft-ui-xaml#7606 一致）：**`XamlControlsResources` 在被实例化时要求  
宿主 `Application` 的 `IXamlMetadataProvider` 能解析框架控件类型**。本项目的 `App::GetXamlType`  
三个方法一律返回 `nullptr`（`App.xaml.cpp` 第 117–127 行，且 `App.xaml.h` 注释明确写了  
「对未知类型一律返回 nullptr」）——框架拿不到 `XamlControlsResources` 等类型的元数据，  
直接 catastrophic failure（`0x802B000A`，错误消息为空，正是这个原因的典型表现）。

而 XAML 编译器**已经生成了正确的 provider**，就在 `XamlTypeInfo.g.cpp` 里，并且它自带回落：

```cpp
// winui/build-winui/obj/x64/Release/XamlTypeInfo.g.cpp:1091
_otherProviders.push_back(::winrt::Microsoft::UI::Xaml::XamlTypeInfo::XamlControlsXamlMetaDataProvider());
```

问题是：`XamlTypeInfoProvider` 被生成在 `.g.cpp` 内部，`App` 从来没引用它，  
`XamlMetaDataProvider.cpp` 又不在 `UnlockerApp.vcxproj` 的编译列表里 → **这段正确代码是死代码**。

**修法（最小改动，不碰脆弱的手工投影）**：在已有的 `XamlTypeInfoGlue.cpp` 里把生成的  
provider 暴露成三个自由函数，`App` 的三个方法改为委托给它。生成类的公共接口  
（`XamlTypeInfo.xaml.g.h`）是：

```cpp
struct XamlTypeInfoProvider : std::enable_shared_from_this<XamlTypeInfoProvider> {
    IXamlType  GetXamlTypeByName(hstring const&);
    IXamlType  GetXamlTypeByType(Windows::UI::Xaml::Interop::TypeName const&);
    IXamlMember GetMemberByLongName(hstring const&);
};
```

---

## 2. 执行步骤

### P0 — M5.1 打通（本轮重点，预计 0.5 天）

| #   | 动作                                                                                                                                        | 文件                                                                |
| --- | ----------------------------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------- |
| 0.1 | 暴露生成的元数据提供器（单例 + 三个转发函数）                                                                                                                  | `winui/XamlTypeInfoGlue.cpp`、新增 `winui/XamlTypeLookup.h`          |
| 0.2 | `App::GetXamlType` / `GetXamlType(TypeName)` 改为委托；`GetXmlnsDefinitions` 返回框架 provider 的                                                   | `winui/App.xaml.cpp`、`winui/App.xaml.h`（去掉「一律返回 nullptr」的注释）      |
| 0.3 | 重新构建 `build.cmd winui`                                                                                                                    | —                                                                 |
| 0.4 | 启动验证：窗口可见、`OnLaunched` / `Activate` 有日志、无 fail-fast                                                                                       | 运行时                                                               |
| 0.5 | **固化 `resources.pri` 产出**：`AfterBuild` 目标把 `$(OutDir)Unlocker.pri` 复制成 `resources.pri`（如果 0.4 证明它其实必需就保留；若证明不需要，也在构建里保留这一步，因为它是 MRT 的硬约定） | `winui/UnlockerApp.vcxproj`                                       |
| 0.6 | 新增构建后校验脚本并接进 `build.cmd winui`（缺文件即非 0 退出）                                                                                                | `tools/check-winui-output.py`、`build.cmd`                         |
| 0.7 | 诊断代码降级：`AppTrace` 改为编译期开关（默认关），`winui-crash.log` 与 `UnhandledException` 保留                                                                | `winui/App.xaml.cpp`、`winui/MainWindow.xaml.cpp`、`winui/main.cpp` |

**验收**：W1（≤2s 出窗口、无 fail-fast）、W14（删掉 `resources.pri` 时校验脚本报错而不是静默）、  
`check-winui-output.py` 在缺任一文件时让构建失败。

> 若 0.4 仍失败：按 §7.7 决策树继续，但**不再往 PRI 方向投**；下一步是拿 `XamlControlsResources`  
> 的实例化异常原文（改为在 `OnLaunched` 里手动 `Resources().MergedDictionaries().Append(XamlControlsResources())`  
> 并包 try/catch 打印 hr+message），再定位。**时间盒：本步骤不超过 3 次尝试**，超了按你的规矩停下来让你选备选方案（R-20 的 A/B/C）。

### P1 — M5.3 缺口：单实例冷启动竞态（约 0.5 小时）

现状 `SendPathsToRunningInstance`（`src/ui/UiIpc.cpp:45`）是「`WaitNamedPipeW` 一次 → `CreateFileW` 一次，  
失败即返回 false」，**正好命中 R-27**：首实例还在 XAML 初始化时管道尚未创建，  
`WaitNamedPipeW` 立刻返回 `ERROR_FILE_NOT_FOUND`，路径直接丢，用户看到「窗口起来了但列表是空的」。

修法：总预算 3s 的重试循环（`ERROR_FILE_NOT_FOUND` → `Sleep(50)` 重试），超时返回 false，  
调用方给一行可读提示。对应验收 W9②。

### P2 — M5.5 打磨（约 2 小时）

| 项                                            | 现状                           | 动作                                                        |
| -------------------------------------------- | ---------------------------- | --------------------------------------------------------- |
| 键盘加速键（F5 重扫 / Del 移除 / Ctrl+A 全选 / Enter 解锁） | 未实现                          | XAML `KeyboardAccelerator` + 处理器                          |
| 窗口尺寸/位置记忆（`HKCU\Software\Unlocker\Window`）   | 固定 `Resize(1040×720)`        | 启动读取、关闭写回                                                 |
| 主题令牌                                         | XAML 里全是字面量颜色（当年为绕开元数据问题降级的） | **元数据修好后可选**：把颜色搬进 `ThemeDictionaries`（§5.6 令牌表）；不做也不影响功能 |

### P3 — 收尾（需你单独确认删除清单）

- 删除旧机器残留：`winui/build-winui.bak-lei/`（DbgView.exe、dbg_capture.txt、dbgmon.py、prisrc/、obj/、x64/）、`build.bak-lei-cache/`
- 更新 `WinUI-TaskStatus.md`（把根因从 PRI 改成元数据 provider）、`README.md`（UniversalBuildTools / asInvoker / 提权后拖放）
- 跑 `policy_tests.exe` + `ipc_tests.exe` 确认 core/ipc 零回归

---

## 3. 明确不做（本轮）

- 不动 `src/core` / `src/ipc`（协议、黑名单、IPC 全绿是刹车，UI 是发动机）
- 不引入 `x:Bind` / ViewModel（D5 不变，切换条件见 §5.3.1）
- 不改 `asInvoker`、不打包 MSIX、不拆 worker exe（D2/D3 不变）
- W3（提权后拖放是否失效）需要你手动「以管理员身份运行」后实测，我这边只能记录现状

---

## 4. 验收清单（本轮结束时逐条报告）

- [ ] `Unlocker.exe` 双击出窗口，日志有 `OnLaunched` / `Activate`，无 `0x802B000A`
- [ ] `check-winui-output.py` 通过；故意删 `resources.pri` 时构建失败
- [ ] 拖入 3 个文件 → 列表出行 → 状态胶囊正确（W2）
- [ ] 已有窗口时带参再启动 → 路径入列，不新开窗口（W9①）
- [ ] 冷启动 200ms 内带参二次启动 → 路径不丢（W9②，P1 后）
- [ ] `policy_tests.exe` 26 项 / `ipc_tests.exe` 10 项全绿

---

## 5. 执行结果（2026-10-24 11:05）

**根因判定被实测翻案**：把 `Unlocker.pri` 复制成 `resources.pri` 后仍然 `0x802B000A`，
`makepri dump` 显示 PRI 内容完整 → 原 §1 的判断成立，**不是 PRI**，是 `IXamlMetadataProvider`
对一切类型返回 `nullptr`。改法按 P0-0.1/0.2 落地（`winui/XamlTypeLookup.h` + `XamlTypeInfoGlue.cpp` 暴露生成 provider），
**一次构建即通过**。

| 项 | 结果 |
| --- | --- |
| P0-0.1 / 0.2 元数据委托 | ✅ `winui/XamlTypeLookup.h`、`XamlTypeInfoGlue.cpp`、`App.xaml.cpp/h` |
| P0-0.4 启动验证 | ✅ 窗口可见（标题 `Unlocker`），`OnLaunched`/`Activate` 完成，无 fail-fast |
| P0-0.5 `resources.pri` 固化 | ✅ vcxproj `UnlockerResourcesPri`（AfterBuild 自动复制） |
| P0-0.6 产物校验脚本 | ✅ `tools/check-winui-output.py`，接进 `build.cmd` 第 5 步；删 `resources.pri` 时退出码 1 |
| P0-0.7 AppTrace 降级 | ✅ 仅 Debug 写；未处理异常记录保留 |
| P1 单实例重试 | ✅ `SendPathsToRunningInstance` 3s 轮询；150ms 内二次启动路径不丢；失败弹提示 |
| P2 键盘加速键 | ✅ Ctrl+O / Ctrl+Shift+O / F5 / Ctrl+A / Del / Ctrl+Del / Enter（**新增**） |
| P2 窗口位置记忆 | ✅ `HKCU\Software\Unlocker\WindowPlacement`（最大化不覆盖） |
| 额外发现并修复 | ✅ **所有按钮从未绑定 Click**（界面能显示但按钮全无响应），已在 `SetupWindow()` 统一绑定 |
| 回归 | ✅ `policy_tests` 26/26、`ipc_tests` 10/10；Win32 主程序同源重编通过 |

剩余：P3 清理（旧机器残留目录）等确认；人工回归 UAC / 拖放手感。
