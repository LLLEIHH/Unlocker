# Unlocker 复刻执行方案（WinUI 3 版）

> **版本**：v1.1-winui3 ／ 2026-10-24（据 `Unlocker-Plan-Review-WinUI3.md` 修订）
>
> **取代关系**：本文取代 `Unlocker-Execution-Plan.md`（v0.8）中所有与 UI、构建、里程碑、测试执行环境相关的章节；  
> `core` / `ipc` / `shell` / `cli` 的业务设计仍以 v0.8 为权威，本文按需摘要并标注继承点。
>
> **一句话改动**：把前端从「Win32 + ListView 自绘」整体换成 **WinUI 3（XAML，unpackaged 自包含部署）**，  
> 后端（core / worker / 协议 / 黑名单）零改动，据此重排架构、构建链、里程碑与测试矩阵。

### 修订记录

**v1.1（2026-10-24，据评审 + 代码交叉核对）**

| #      | 类型       | 位置                  | 修订                                                                                                           |
| ------ | -------- | ------------------- | ------------------------------------------------------------------------------------------------------------ |
| H1     | 硬伤       | §0 / §8             | 人日明细实为 25.5，原文写 24 为错；本次采纳 S5 后重算为 **26**，三处同步                                                               |
| H2     | 硬伤       | §8 关键路径             | 补回 `M2`（FileOps 是 M3「解锁并删除」的前置）                                                                              |
| H3     | 硬伤       | §5.2.1              | `WM_DROPDATACOPY` **不存在**，改为 `WM_DROPFILES` / `WM_COPYGLOBALDATA`（OLE DnD 内部消息）                              |
| N1     | 与现状不符    | §6.4                | 「不得 include XAML 头」改为**运行时约束**；现状 `main.cpp` 单 TU include 无害，拆 TU 降为可选加固                                     |
| N2     | 与现状不符    | §4.4                | 启动序列草图改为与 `winui/main.cpp` 实际代码一致（含 mutex/argv 错误处理）                                                         |
| S1     | 建议       | §5.2.1              | 删除「无解」断言：**现状代码已有 `PatchDropForElevatedUi`**，有效性未实测，改由 W3 定论                                                 |
| S2     | 建议       | §7.6                | 补第 4 条铁律：unpackaged 三属性不得被属性页改掉                                                                              |
| S3     | 建议       | §5.2.2              | 删 `reinterpret_cast<HWND>(...)` 备选写法（现状已用 `GetWindowFromWindowId`）                                           |
| S4     | 建议       | §9.2 W9             | 补「首实例管道未就绪」竞态场景 + 客户端重试策略                                                                                    |
| S5     | 建议       | §8                  | M5.3 由 1 → 1.5 人日，边缘情况回归并入 M6/M7                                                                             |
| S6     | 建议       | §7.2                | 环境表标注本机已装齐，避免重复排查                                                                                            |
| **C1** | **自查发现** | §5.4 R2 / §5.2.1 示例 | **`get_weak()` 在本项目投影下不可用**（源码注释已记录：触发 `producer<>` 实例化失败），撤回"必取弱引用"的写法，统一 `this + alive_`                   |
| **C2** | **自查发现** | §5.1 / §5.2.1       | 现状 backdrop 是 `DesktopAcrylicBackdrop`（非 Mica）；现状提权分支已调 `ul::PatchDropForElevatedUi(Hwnd())`，原文两处描述与代码不符，已改正 |
| **C3** | **自查发现** | §5.2.3 / §10.2      | 单实例丢路径的确切机理是 `WaitNamedPipeW` 只等"实例忙"、不等"管道尚未创建"，原文只写"有竞态"未指出原因                                              |

---

## 0. 一页速览

| 项目       | v0.8（Win32）                                     | **v1.1（本方案，WinUI 3）**                                            |
| -------- | ----------------------------------------------- | ---------------------------------------------------------------- |
| 技术栈      | C++20 + Win32 + 少量 NTAPI                        | **不变**（core/ipc 一行不改）                                            |
| UI       | Win32 原生窗口 + 自绘 ListView（custom draw）           | **WinUI 3 / XAML**（WASDK 1.7，C++/WinRT）                          |
| UI 进程模型  | asInvoker，中完整性                                  | **不变**（提权会伤拖放，现状有 `PatchDropForElevatedUi` 但有效性待 W3 定论，见 §5.2.1） |
| 部署形态     | 单目录 flat 部署                                     | **unpackaged + `WindowsAppSDKSelfContained=true`**（不打包 MSIX）     |
| exe 身份   | 一个 exe 三态：UI / `--worker` / CLI                 | **不变**；但 UI 分支才碰 XAML                                            |
| 提权       | `ShellExecuteEx(runas)` 拉 worker + 命名管道 JSON v3 | **不变**                                                           |
| 黑名单      | 编译期规则表 + CRC fail-closed + FILE_ID 双表           | **不变**                                                           |
| Shell 扩展 | 原生 `IContextMenu` DLL                           | **不变**（WinUI 做不了，也不该做）                                           |
| 构建       | `build.cmd`（CMake）+ `build.cmd winui`（MSBuild）  | **不变的两条腿**，但 WinUI 腿加固为「可校验、可复现」                                 |
| 里程碑      | M0–M7，22.5 人日                                   | **M0–M7，M5 拆成 5 个子阶段，26 人日**                                     |
| 头号风险     | worker 被当提权跳板                                   | **风险换位**：worker 风险不变，**新增「WinUI 3 构建/资源链脆弱」为头号工程风险**             |

**为什么要写这一版**：当前 `winui/` 已经有一套可编译、UI 已现代化改版的前端，但卡在启动期 fail-fast（0x802B000A），  
且整个构建链依赖「NuGet 胶水 + 手写 targets + PRI 手工改名」这类脆弱环节。v0.8 是按 Win32 写的，  
继续拿它当施工图会把 UI 阶段的坑反复踩。本方案的目标是把 **「WinUI 3 到底怎么落地、怎么验收、坏在哪一步怎么定位」** 写死。

---

## 1. 决策记录（先定这几件事，后面才好写）

### 1.1 五条已定案

| #      | 决策                                                                                      | 理由                                                                                                                                                                                 | 被否方案                                                                                                         |
| ------ | --------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------ |
| **D1** | **WinUI 3 是唯一交付 UI**；`src/ui`（Win32）冻结为 legacy，构建开关 `UNLOCKER_BUILD_LEGACY_UI=OFF`（默认关） | 双 UI 并行维护成本高；但 Win32 版是当前唯一能正常运行的形态，删掉等于烧掉唯一退路                                                                                                                                     | ① 直接删 `src/ui`：WinUI 未全量回归前风险过高；② 双前端并列发布：人手不够                                                               |
| **D2** | **不打包（unpackaged）+ WASDK 自包含**                                                          | ① 经典 shell 扩展（`IContextMenu`）在 MSIX 下要改写成 `IExplorerCommand` + sparse package，工作量翻倍；② 自包含免去用户装 Windows App Runtime；③ worker 提权链路不会被包身份干扰                                           | MSIX 打包：Win11 右键菜单更"原生"，但 signtool 证书、包身份、提权子进程全要重做——留到 v1.2                                                 |
| **D3** | **单 exe 三身份不变**，`--worker` / CLI 分支**绝不初始化 XAML**                                       | worker 的三重身份校验要求「客户端映像路径 == 自身 exe」；拆 exe 会让校验与签名部署一起变复杂                                                                                                                           | 拆出 `UnlockerWorker.exe`：启动更快，但破坏现有校验模型                                                                       |
| **D4** | **Shell 扩展保持原生 Win32 DLL**                                                              | XAML 类型不能在 explorer.exe 内加载（进程是 explorer 的，UI 框架栈完全不同）；`IContextMenu` 只需要传路径                                                                                                       | 无（技术上不成立）                                                                                                    |
| **D5** | **UI 数据层用 code-behind 直接构造行（现有实现），不引入 `x:Bind` / ViewModel**                            | 现有实现已验证：`ListView.ItemsSource` 放 `Grid` 行，零 XAML 类型元数据依赖。而当前 `IXamlMetadataProvider` 是手写的、对未知类型返回 `nullptr`——一旦用 `x:Bind` 绑自定义类型，**必须**换成 cppwinrt 生成的 `XamlTypeInfo`，这是本项目最贵的一类返工 | 数据绑定 + `ObservableCollection<T>`：架构更"正规"，但要把 `UiItem/UiOwner` 拉成 IDL runtimeclass，并修好元数据链。**触发切换条件见 §5.3.1** |

### 1.2 如果推翻某条决策，改动面有多大

| 反悔项             | 影响面                                                                    |
| --------------- | ---------------------------------------------------------------------- |
| D1 恢复双 UI       | 里程碑 +2 人日（双份回归），无架构影响                                                  |
| D2 改 MSIX       | **+5～7 人日**：包身份、扩展重写、worker 提权（需 `fullTrustProcess`/`AppService`）、自动更新 |
| D3 拆 worker exe | +1.5 人日：`PipeServer` 映像路径校验、部署清单、签名                                    |
| D5 改数据绑定        | **+4～6 人日**：IDL 扩类型、元数据链重做、模板重写、逐项回归。收益是可测试性与虚拟化，**不建议在 v1.0 做**       |

### 1.3 本方案不改变的硬结论（防止有人"顺手优化"）

- `manifest` 里 **`asInvoker` 是功能依赖**，不是风格。UI 一提权，拖放立刻死（WinUI 3 连补丁都没有，见 §5.2.1）。
- exe **必须**叫 `Unlocker.exe`（worker 三重校验）。
- 危险动作**只**发生在 worker 进程，UI 不做任何文件写操作。
- 黑名单 fail-closed：CRC 不匹配 → 全表 Deny。

---

## 2. 与 v0.8 的章节继承关系

| v0.8 章节                        | 本文处置            | 说明                                                                                     |
| ------------------------------ | --------------- | -------------------------------------------------------------------------------------- |
| §0 一页速览                        | **重写**          | §0                                                                                     |
| §1 范围 / 1.1 / 1.2 / 1.3        | **继承**          | 仅把「UI 原生对话框」等表述改为 WinUI 语汇                                                             |
| §2 功能矩阵 F1–F20                 | **继承 + 增列**     | §3.2 增加「UI 侧实现」列，标注 WinUI 下的具体做法                                                       |
| §3 架构与目录                       | **重写**          | §4.3（`winui/` 升为主前端，`src/ui` 降级）                                                       |
| §4.1 权限与进程模型                   | **继承 + 补注**     | §4.1 补 XAML 线程模型约束                                                                     |
| §4.2 HandleScanner             | **原样继承**        | v0.8 §4.2（221 行），本文不重复                                                                 |
| §4.3 PathResolver              | **原样继承**        | v0.8 §4.3                                                                              |
| §4.4 LockBreaker               | **原样继承**        | v0.8 §4.4（六级阶梯，L3 未实现）                                                                 |
| §4.5 FileOps                   | **原样继承**        | v0.8 §4.5（含递归删除 × 黑名单交互表）                                                              |
| §4.6 PolicyGuard               | **原样继承**        | v0.8 §4.6（49 条规则 + FILE_ID 表 + fail-closed）                                            |
| **§4.7 GUI（Win32 + ListView）** | **作废**          | 由本文 §5 全面取代。原文中「`DragAcceptFiles` / `WM_DROPFILES` / custom draw / `WM_PAINT` 空状态」全部失效 |
| §4.8 Shell 扩展                  | **继承 + 补 §6.3** | 说明为什么不能 WinUI 化                                                                        |
| §4.9 CLI                       | **继承 + 补 §6.4** | 说明为什么不能碰 XAML                                                                          |
| §4.10 IPC 协议                   | **继承 + 补 §5.4** | 协议不变，新增 UI 侧线程规范                                                                       |
| §5 关键数据结构                      | **部分修改**        | `ProcessInfo.icon`（`HICON`）在 WinUI 下同样不可序列化，且不可跨线程；改为 `IconKey`（§5.3.3）                |
| §6 里程碑                         | **重写**          | §8（M5 拆 5 段，人日重算）                                                                      |
| §7 测试矩阵                        | **继承 + 新增**     | §9（T 系列保留，新增 W1–W15）                                                                   |
| §8 风险清单                        | **继承 + 新增**     | §10（原 18 条 + 新增 8 条）                                                                   |
| §9 与 IObit 对照                  | **继承 + 补行**     | §11                                                                                    |
| §10 附录（常量 / 黑名单表）              | **原样继承**        | 见 v0.8 §10，本文不重复                                                                       |
| §11 下一步                        | **重写**          | §13                                                                                    |

---

## 3. 范围

### 3.1 要做 / 不做 / 红线

完全继承 v0.8 §1（不做内核驱动、不做静默提权、不做常驻服务、不提供"破解"文案）。  
仅两处表述调整：

- 「软件内点击按钮选择文件」→ 仍是 `IFileOpenDialog`（**不用** WinRT `FileOpenPicker`，理由见 §5.2.2）
- 「资源管理器右键菜单入口」→ 仍是原生 shell 扩展，与 WinUI 前端通过**命名管道**（通道 A，中→中完整性）通信，不走 UAC

### 3.2 功能矩阵（标注 UI 侧实现变化）

| #       | 功能              | 优先级 | UI 侧实现（WinUI 3）                                                   | 变化                                               |
| ------- | --------------- | --- | ----------------------------------------------------------------- | ------------------------------------------------ |
| F1      | 扫描占用进程          | P0  | 无 UI 依赖（worker 内）                                                 | —                                                |
| F2      | 关闭远程句柄          | P0  | 按钮 + 结果提示                                                         | —                                                |
| F3–F6   | 解锁后删/改名/移动/复制   | P0  | 按钮组 + `ContentDialog` 输入 + 进度条                                    | 提示框改用 `ContentDialog`（需 `XamlRoot`）              |
| F7      | 强制模式（杀进程）       | P0  | 进程行 `MenuFlyout` → 「结束进程」                                         | 原 `TrackPopupMenu` 退役                            |
| F8      | 重启兜底            | P0  | 结果提示 + 退出码 6 说明                                                   | —                                                |
| F9      | 右键菜单            | P1  | **原生扩展**，通过通道 A 送路径                                               | 与前端解耦                                            |
| F10     | 命令行             | P1  | 无（`CliMain` 直连 core）                                              | —                                                |
| F11     | 多文件批量           | P1  | `ListView` 多选 + 全选                                                | —                                                |
| F12     | POSIX 语义删除      | P1  | 同 F3                                                              | —                                                |
| F13     | 卸映像             | P2  | 未实现（同 v0.8）                                                       | —                                                |
| F14     | 挂起后关句柄          | P2  | 未实现                                                               | —                                                |
| F15     | 内置黑名单           | P0  | 行左侧红色描边 + 规则 ID tooltip                                           | 视觉落点从 custom draw 改为 XAML 边框                     |
| **F16** | **拖入文件**        | P0  | **XAML `AllowDrop` + `DragOver`/`Drop` + `GetStorageItemsAsync`** | **★ 机制完全不同**（§5.2.1）                             |
| **F17** | **软件内选择文件**     | P0  | **沿用 `IFileOpenDialog`**（COM，STA）                                 | 机制不变，但需 `Show(HWND)` 拿 `AppWindow` 的 HWND        |
| F18     | 提权 worker + IPC | P0  | `PipeClient` 异步回调 → `DispatcherQueue`                             | 回传机制从 `WM_APP+n` 改为 `DispatcherQueue.TryEnqueue` |
| **F19** | **占用者详情展示**     | P0  | 两级 `ListView` + 状态胶囊 + 进程头像                                       | **重写渲染层**，含 HICON→`BitmapImage`（§5.3.3）          |
| F20     | 两级扫描            | P0  | `InfoBar` 提示深扫                                                    | 提示控件从自绘横幅改为 `InfoBar`                            |

---

## 4. 架构

### 4.1 进程与权限模型（继承 v0.8 §4.1，仅补 XAML 约束）

```
Unlocker.exe  (asInvoker / 中完整性 / 主线程 STA)
├─ XAML: Application::Start → App → MainWindow
│    ├─ 拖入（XAML DnD，OLE 数据对象）
│    ├─ IFileOpenDialog（COM，STA，owner = AppWindow 的 HWND）
│    └─ 展示层：两级 ListView、图标缓存、主题跟随
├─ L1 扫描：本进程内跑 HandleScanner（免 UAC）
└─ 需要深扫 / 写操作
        │ ShellExecuteEx(runas，后台线程)
        ▼
   Unlocker.exe --worker（高完整性，**无任何 XAML 初始化**）
   ├─ 启特权 → L2 深扫 → PolicyGuard 重校验 → 执行 → 弹 Confirm
        │
        └── 命名管道（JSON v3）──> UI 渲染
```

**新增的两条 WinUI 约束**：

1. **UI 线程是 STA 且必须是唯一的 XAML 线程**。`init_apartment(single_threaded)` 必须在 `Application::Start` 之前，  
   所有 XAML 对象（含 `BitmapImage`、`ContentDialog`、`MenuFlyout`）只能在该线程创建。
2. **worker 分支禁止触碰任何 XAML/WASDK 类型**。`wWinMain` 里先判 `--worker` / CLI，再决定是否 `Application::Start`——  
   顺序不能反。代价是 worker 进程仍会加载 WASDK 的 DLL（静态导入），启动多约 40～80ms，可接受；换来的是单 exe 与三重校验不变。

### 4.2 分层职责（不变的部分）

| 层           | 目录           | 能否依赖 UI 框架 | 说明                                       |
| ----------- | ------------ | ---------- | ---------------------------------------- |
| common      | `src/common` | ❌          | 日志 / NTAPI / 错误映射 / RAII                 |
| core        | `src/core`   | ❌          | 纯逻辑，`unlocker_core.lib`，Win32 与 WinUI 共享 |
| ipc         | `src/ipc`    | ❌          | 只搬运，不做业务判断                               |
| **ui-xaml** | **`winui/`** | ✅          | **唯一前端**，产物 `Unlocker.exe`               |
| ui-legacy   | `src/ui`     | ✅（Win32）   | 冻结，默认不编译                                 |
| shell       | `src/shell`  | ❌（原生 COM）  | `UnlockerShell.dll`                      |
| cli         | `src/cli`    | ❌          | `CliMain`，同 exe 参数分支                     |

> 边界铁律：**`core` 从第一天起就必须是"可以被远程调用"的干净接口**——没有窗口句柄参数、没有全局 UI 状态。  
> 这条在 WinUI 下更重要：XAML 对象有线程亲和性，一旦漏进 core，worker 侧立刻崩。

### 4.3 目录结构（重写）

```
Unlocker/
├─ winui/                          ★ 主前端（WinUI 3，MSBuild 工程）
│  ├─ main.cpp                     // 三身份分流；--worker/CLI 不碰 XAML
│  ├─ App.xaml / App.xaml.cpp      // 启动、异常诊断、主题、backdrop
│  ├─ MainWindow.xaml(.h/.cpp)     // 主窗口（现有 43KB 实现，继续演进）
│  ├─ UnlockerApp.idl              // App 必须声明 IXamlMetadataProvider（WASDK 1.7 无大写 D）
│  ├─ XamlMetaDataProvider.idl/.cpp
│  ├─ XamlTypeInfoGlue.cpp         // 把生成的 XamlTypeInfo.g.cpp 编进工程
│  ├─ InitialPaths.h               // 首启参数传递
│  ├─ app.manifest                 // asInvoker（功能依赖，勿改）
│  └─ UnlockerApp.vcxproj
├─ src/
│  ├─ core/  ipc/  common/  cli/   // 原样保留，WinUI 与 Win32 共享
│  ├─ shell/                       // 原生右键扩展（不变）
│  └─ ui/                          ★ legacy（Win32），默认不编译
├─ tools/  MakeLock/  gen_rules_crc.py
├─ tests/   policy_tests.cpp  ipc_tests.cpp
└─ build.cmd  CMakeLists.txt
```

**与 v0.8 的差异**：`src/ui` 由「主 UI」降为「legacy 对照」；`winui/` 由「并行实验分支」升为唯一交付前端。  
`CMakeLists.txt` 增加 `UNLOCKER_BUILD_LEGACY_UI`（默认 OFF），`UNLOCKER_BUILD_WINUI` 默认改为 ON。

### 4.4 exe 内三身份时序（钉死，别改顺序）

```cpp
// ⚠️ 本段与 winui/main.cpp 现有实现逐行对齐（去掉 AppTrace 调试行）。改动请同步改两处。
int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    std::vector<std::wstring> args;
    if (argv) for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);

    // 1) 分流最先做：这两条路径不调用任何 XAML API
    if (HasFlag(args, L"--worker")) {
        const int rc = ul::WorkerMain(argc, argv);
        if (argv) LocalFree(argv);
        return rc;
    }
    if (ul::CliHasActionFlag(argc, argv)) {
        const int rc = ul::CliMain(argc, argv);
        if (argv) LocalFree(argv);
        return rc;
    }

    // 2) 单实例（Local\ 会话语义，不是 Global\）；已存在则经通道 A 转发路径后退出
    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\Unlocker.Instance");
    const bool alreadyRunning = mutex && GetLastError() == ERROR_ALREADY_EXISTS;
    if (alreadyRunning) {
        if (!args.empty()) ul::SendPathsToRunningInstance(args);
        if (mutex) CloseHandle(mutex);
        if (argv) LocalFree(argv);
        return 0;
    }

    // 3) 到这里才允许进 XAML
    ul::SetInitialPaths(args);
    if (argv) LocalFree(argv);

    winrt::init_apartment(winrt::apartment_type::single_threaded);   // 必须在 Start 之前
    winrt::Microsoft::UI::Xaml::Application::Start([](auto&&) {
        winrt::make<winrt::Unlocker::implementation::App>();
    });
    if (mutex) CloseHandle(mutex);
    return 0;
}
```


> 注意 mutex / `argv` 的释放：`alreadyRunning` 分支和正常退出路径各自 `CloseHandle` + `LocalFree`。
> 照旧版草图的"简化写法"去改真实代码，会直接漏释放（`argv` 来自 `CommandLineToArgvW`，必须 `LocalFree`）。

---

## 5. WinUI 3 前端设计（本方案的核心新增章节）

### 5.1 窗口骨架

| 项 | 做法 | 备注 |
| --- | --- | --- |
| 窗口 | `Microsoft.UI.Xaml.Window` + `AppWindow` | 尺寸/位置记忆写 `HKCU\Software\Unlocker\Window` |
| 标题栏 | `ExtendsContentIntoTitleBar(true)` + `SetTitleBar(拖动区)`；`AppWindow.TitleBar.PreferredHeightOption = Tall` | WASDK 1.6+ 支持；需给最小/最大/关闭按钮留 `RightInset` |
| 背景 | **现状 = `DesktopAcrylicBackdrop`**（`SetupWindow()` 里已设）。Win11 22H2+ 可换 `MicaBackdrop`（更"系统感"），Win10 保持 Acrylic；都要有纯色兜底 | Mica 属**可选增强**（P2），不要为了换 backdrop 冒启动风险；回退判定用 `AnalyticsInfo.VersionInfo` |
| 主题 | 跟随系统：监听根元素 `ActualThemeChanged`，切换自绘颜色令牌 | 现有实现已做；令牌表见 §5.6 |
| 高 DPI | XAML 自动；图标按 `XamlRoot.RasterizationScale` 选 16/32/48 尺寸 | 见 §5.3.3 |
| 键盘 | `KeyboardAccelerator`：F5 重扫、Del 移除项、Ctrl+A 全选、Enter 解锁 | 替代 Win32 的 `WM_KEYDOWN` |

### 5.2 输入入口（F16 / F17 / 单实例）

#### 5.2.1 拖放（F16）——机制完全替换

**这是 v0.8 与 v1.0 差异最大的一处。**

| 维度 | v0.8（Win32） | v1.0（WinUI 3） |
| --- | --- | --- |
| 注册 | `DragAcceptFiles(hwnd, TRUE)` | `AllowDrop="True"` 在根 `Grid` 上 |
| 事件 | `WM_DROPFILES`（只给结果，无过程） | `DragEnter` / `DragOver` / `DragLeave` / `Drop` |
| 取路径 | `DragQueryFileW` | `e.DataView().GetStorageItemsAsync()` → `IStorageItem.Path()` |
| 拖入高亮 | **做不到**，只能画静态虚线框 | **能做到**：`e.AcceptedOperation(...)` + `e.DragUIOverride().Caption(...)`，并给根容器加边框 |
| 提权后 | `ChangeWindowMessageFilterEx` 可打补丁 | **现状代码已调 `PatchDropForElevatedUi` 尝试放行，有效性未实测**（见下） |

```cpp
// 根 Grid：AllowDrop="True" DragEnter="OnDragEnter" DragOver="OnDragOver"
//          DragLeave="OnDragLeave" Drop="OnDrop"
void MainWindow::OnDragOver(IInspectable const&, DragEventArgs const& e) {
    if (!e.DataView().Contains(StandardDataFormats::StorageItems()))
        { e.AcceptedOperation(DataPackageOperation::None); return; }
    e.AcceptedOperation(DataPackageOperation::Copy);
    e.DragUIOverride().Caption(L"拖入以检查占用");
    e.DragUIOverride().IsGlyphVisible(true);
    SetDropHighlight(true);          // 视觉效果：根容器描边 + 中央文案淡出
}

fire_and_forget MainWindow::OnDrop(IInspectable const&, DragEventArgs const& e) {
    // ⚠️ 不要写 get_weak()/get_self()：本项目投影未做实现侧 producer 特化，
    //    get_self 会触发 producer<> 实例化失败（源码注释已记录）。统一 this + alive_。
    std::vector<std::wstring> paths;
    try {
        for (auto const& it : co_await e.DataView().GetStorageItemsAsync())
            paths.emplace_back(it.Path().c_str());
    } catch (winrt::hresult_error const&) {
        // 超过 MAX_PATH、虚拟项、压缩包内文件等：明确提示，不能静默
        if (alive_.load()) ShowMessage(L"无法读取拖入项", L"请改用「添加文件」按钮选择。");
        co_return;
    }
    if (alive_.load()) AddPaths(paths);   // AddPaths 是唯一入口；窗口已关则直接丢弃
}
```

**⚠️ 提权 → 拖放在 XAML 下大概率失效，但"能不能救"要由实测定论**

XAML 的拖放走 OLE DnD（`IDropTarget`），不是 `WM_DROPFILES`。UIPI 拦的是
`WM_DROPFILES`、`WM_COPYGLOBALDATA`（`0x0049`）、`WM_COPYDATA`，以及 OLE DnD 内部的跨进程窗口消息；
而 XAML 的 drop target 是 **AppWindow 内部的子窗口**，只对顶层 HWND 打 filter 未必覆盖到它。

**现状**：`MainWindow::SetupWindow()` 在检测到提权时已调用 `ul::PatchDropForElevatedUi(Hwnd())`
（`src/ui/ElevationCheck.cpp`，对 `WM_DROPFILES` / `WM_COPYDATA` / `0x0049` 三条 `MSGFLT_ALLOW`）。
**这套补丁在 XAML 窗口上是否奏效，目前没有任何实测数据**——所以本方案不写"无解"这种断言，
也不把它当成可依赖的能力：

| 结论状态 | 处理 |
| --- | --- |
| W3 实测**有效** | 保留补丁 + 保留 InfoBar 提示（提示改为"拖放可能不稳"）；不因此放弃 `asInvoker` |
| W3 实测**无效** | 删除补丁调用（不留死代码），InfoBar 改为"请以普通权限重新启动"，正文写明该限制 |

无论哪种结果，**`asInvoker` 都不变**——它保的是"不依赖补丁也能正常拖放"，这才是功能基线。

本版确定的动作：

1. 启动自查 `TokenElevation`，若已提权 → 顶部 **`InfoBar`（Warning 级）**："当前以管理员权限运行，拖放可能不可用。建议以普通权限重新启动。"（现状已有这段文案与 `Banner()` 控件，只需按 W3 结论微调措辞与 Severity）；
2. 不做自动降权重启。「提权重启自身」列为 **P2 可选**：`WTSQueryUserToken` + `CreateProcessAsUser` 以 explorer 的令牌重启，需处理会话隔离，风险与收益不匹配。

**必须处理的输入边缘情况**（继承 v0.8 §4.7.2 的表，但有变化）：

| 拖入内容 | v1.0 行为 | 与 v0.8 差异 |
| --- | --- | --- |
| 单文件 / 多选混合 / 文件夹 | 全部加入；文件夹不预展开 | 不变 |
| 快捷方式 `.lnk` | 拿到的是 `.lnk` 路径，走 `IShellLink` 解析目标，**目标仍过黑名单** | 不变（解析逻辑在 UI 侧工具函数） |
| UNC 路径 | 拒绝并说明跨机器无法枚举对端句柄 | 不变 |
| 压缩包内虚拟文件 / 回收站项 | `GetStorageItemsAsync` 可能抛异常或返回无 `Path` 的项 → 拒绝 + 说明 | 判定位置从 `SIGDN_FILESYSPATH` 失败改为 `Path()` 为空/异常 |
| 路径 > MAX_PATH | **可能取不到**（WinRT `StorageItem` 的已知限制）→ 提示改用选择器 | ★ 新增限制，由 W15 覆盖 |
| 重复项 | 按 `CanonicalPath` 去重 | 不变 |
| 空拖入 | 忽略 | 不变 |
| > 500 项（BL-018） | 提示分批，不静默截断 | 不变 |

#### 5.2.2 文件选择（F17）——沿用 `IFileOpenDialog`

**不用 WinRT `FileOpenPicker`**，理由三条：

1. unpackaged 应用必须 `IInitializeWithWindow` 注入 HWND，多一层 interop，收益为零；
2. `FileOpenPicker` 无法直接表达 `FOS_PICKFOLDERS` 的"文件与文件夹两套按钮"语义（要另用 `FolderPicker`）；
3. 现有 `src/ui/FilePicker.cpp` 已验证可用，直接复用——**能跑通的代码不要为了"新框架"重写**。

唯一改动：`owner` 传 `HWND`（从 `AppWindow.Id()` 经 `Win32Interop` 取），其余逻辑不变。

```cpp
HWND hwnd = winrt::Microsoft::UI::GetWindowFromWindowId(this->AppWindow().Id());
auto paths = ul::PickFiles(hwnd);   // 原 FilePicker.cpp，逻辑零改动（现状即如此：ul::PickFiles(Hwnd())）
```

> 只用 `Win32Interop::GetWindowFromWindowId` 这一条正道。**不要**写
> `reinterpret_cast<HWND>(AppWindow().Id().Value)`——它依赖"WindowId 数值恰好是 HWND"这个未承诺的实现细节，
> 现状代码（`MainWindow::Hwnd()`）用的就是 `GetWindowFromWindowId`，别为了少一行把正确写法换掉。

要点：UI 线程是 STA（`init_apartment(single_threaded)`），`IFileOpenDialog::Show` 的模态循环与 XAML 消息泵兼容，不会被吞消息。
记住上次目录仍写 `HKCU\Software\Unlocker\LastDir`。

#### 5.2.3 单实例与外部唤醒（通道 A）

现有实现已经是 WinUI 友好的：`ul::StartUiPipeListenerFn(callback)` 在独立线程监听命名管道，
**不使用** `WM_COPYDATA`（WinUI 窗口没有自己的 `WndProc`，硬套要 subclass，没必要）。

规范三条：

1. 回调在**监听线程**触发，必须 `queue_.TryEnqueue(...)` 切回 UI 线程再 `AddPaths()`（现状已如此，含 `alive_` 二次判空）；
2. `queue_` 在 `MainWindow` 构造时 `DispatcherQueue::GetForCurrentThread()` 取一次（早于 `SetupWindow()`），
   回调里判空，窗口关闭后（`~MainWindow` 置 `alive_=false`）直接丢弃；
3. **必须补冷启动竞态的重试**（★ v1.1 新增，现状缺口）：

   监听器是在 `SetupWindow()` 里才 `StartUiPipeListenerFn(...)` 的，而首实例从 `CreateMutexW` 到这一步要经过
   XAML 初始化（数百毫秒）。此时第二实例的 `SendPathsToRunningInstance` 会调 `WaitNamedPipeW(name, 1500)`——
   **它只等"管道实例忙"，不等"管道尚未创建"**：管道不存在时立即返回 `ERROR_FILE_NOT_FOUND`，路径就此丢失，
   用户看到的是"点了右键菜单/带参启动，窗口起来了但列表是空的"。

   修法（客户端侧，`SendPathsToRunningInstance` 内）：总预算 3s 的重试循环
   （`WaitNamedPipeW` → `CreateFileW` 失败且 `GetLastError()==ERROR_FILE_NOT_FOUND` → `Sleep(50)` 重来），
   超时后返回 false，由调用方给一行可读提示而不是静默退出。**这是个 5 行的改动，但它是"右键菜单能不能用"的分水岭。**

三个来源（拖入 / 选择器 / 命令行·右键菜单）**必须汇到同一个 `AddPaths()`**——去重、裁决、刷新只做一次。
这条继承 v0.8 §4.7.1，是防止「拖入能拦、命令行拦不住」的唯一办法。

### 5.3 展示层（F19）

#### 5.3.1 数据层：为什么 v1.0 不做数据绑定（D5 的详细论证）

现状（已实现且可用）：`MainWindow.xaml.cpp` 里 `items_`（业务模型）+ `rows_`（`Grid` 行，与 `items_` 一一对应），
`ListView.ItemsSource` 直接放行元素，`RebuildFileList()` 重建。**零 `x:Bind`、零 `DataTemplate`、零自定义类型元数据。**

为什么保持：

- 当前 `IXamlMetadataProvider` 是手写实现，对未知类型返回 `nullptr`。它能工作，**只因为** XAML 里出现的全是框架类型。
  一旦引入 `x:Bind` 到自定义 `ViewModel`，XAML 框架会向 provider 要类型元数据，拿不到就是**加载期崩溃**（和现在这个
  0x802B000A 同类，且更难定位）。正确解法是换成 cppwinrt 生成的 `XamlTypeInfo`，那是另一条独立的返工线。
- 项目规模不需要：BL-018 上限 500 项，`ListView` 默认虚拟化足够。

**切换到数据绑定的触发条件**（满足任一条再动手，并单独立项）：

1. 需要按列排序/过滤/分组（`CollectionViewSource`）；
2. 单次列表稳定超过 2000 行；
3. 需要 UI 自动化测试直接断言数据（而不是扫描可视树）。

切换时的必做项：`UiItem`/`UiOwner` 进 IDL 声明为 `runtimeclass` → cppwinrt 生成元数据 → 用生成的
`XamlMetaDataProvider` 替换手写版 → `XamlTypeInfoGlue.cpp` 确认编入 `XamlTypeInfo.g.cpp`。

#### 5.3.2 增量渲染

扫描可能 1.5s，界面不能白屏。规范：

- 每命中一项就追加一行，**不要等全量扫完再重建**；
- 追加走 `queue_.TryEnqueue` 批量提交：攒 50ms 或攒 20 行提交一次，减少布局抖动；
- 行元素**复用**而不是重建（`RebuildFileList` 只在"删除项/全量刷新"时用）；
- 扫描期间的进度条 + `SummaryText`（"已发现 12 个占用进程"）常驻。

#### 5.3.3 图标：`HICON` → `BitmapImage`（★ 新增设计点）

v0.8 的 `ProcessInfo.icon` 是 `HICON`，"仅 UI 进程内有效，不参与序列化"。在 WinUI 下这条**还要再收紧**：

- `HICON` **不能**直接给 XAML 用（没有 `ImageSource` 转换）；
- `BitmapImage` 有**线程亲和性**，只能在 UI 线程创建；
- 因此 `ProcessInfo` 里改为 `IconKey`（进程 exe 的规范化路径 + 图标尺寸档），图标由 UI 侧按需解析并缓存。

```
后台/调用点：ExtractIconEx / SHGetFileInfo → HICON
             → WIC (IWICImagingFactory::CreateBitmapFromHICON)
             → PNG 编码进 InMemoryRandomAccessStream   ← 这段可在后台线程
UI 线程：    BitmapImage().SetSourceAsync(stream)        ← 必须 UI 线程
缓存：      unordered_map<wstring /*exePath + "|" + size*/, BitmapImage>
```

要点：

- 只对**已显示的进程**补全图标（通常 1–5 个），不要预扫 200 个进程；
- 同一 exe 多行只取一次（测试项 W7）；
- 取不到（受保护进程）→ 统一的占位头像（进程名首字母 + 哈希配色），不要留空；
- `HICON` 用完必须 `DestroyIcon`（v0.8 §8 已列此风险，WinUI 下依然成立）。

#### 5.3.4 状态与视觉语义（继承 v0.8 §4.7.7 的表，改为 XAML 落地）

| 状态 | 色 | 控件形态 |
| --- | --- | --- |
| `Locked` 已锁定 | 红 | `Border` 圆角胶囊 + `DangerFill` 令牌 |
| `ImageMapped` 映像占用 | 橙 | 同上，`CautionFill` |
| `AccessDenied` 权限不足 | 黄 | 同上 |
| `Opened` 已打开 | 灰 | `SubtleFill` |
| `Free` 可操作 | 绿 | `SuccessFill` |
| `LockedByKernel` 驱动锁定 | 紫/灰 | 同上 |
| `NotFound` | 灰 | 移除该行 |
| `Error` | 深灰 | 附加原始错误码 tooltip |

其余交互（双击进程行 → `SHOpenFolderAndSelectItems`；右键 → 复制 PID / 打开目录 / 结束进程；
空状态"未找到持有进程，可能由安全软件或文件系统驱动锁定"，**不得**写"无人占用"）全部继承 v0.8 §4.7.7。

> "重新扫描"只做 `Probe` 轻量校验，不做后台定时轮询——无谓 CPU 且是杀软敏感行为（继承原结论）。

### 5.4 异步与线程规范（★ 新增，WinUI 最容易出事的地方）

| 规则 | 内容 |
| --- | --- |
| R1 | **UI 线程唯一入口**：所有跨线程结果（管道回调、扫描线程、批处理线程）必须 `queue_.TryEnqueue` |
| R2 | **`fire_and_forget` 用 `this + alive_`**：本项目投影未做实现侧 producer 特化，`get_weak()` / `get_self()` 会触发 `producer<>` 实例化失败（源码注释已记录）。每个 `co_await` 之后必须先判 `if (!alive_.load()) return;` |
| R3 | **不阻塞 UI 线程**：禁止 `WaitForSingleObject` / 同步管道读 / 同步文件操作；`PipeClient` 的收发全在后台线程 |
| R4 | **XAML 对象不跨线程**：`BitmapImage` / `ContentDialog` / `MenuFlyout` / `Clipboard` 只在 UI 线程触碰 |
| R5 | **`ContentDialog` 必须设 `XamlRoot`**，否则抛异常（`this->Content().XamlRoot()`） |
| R6 | **取消语义**：`alive_` 原子标志 + 向 worker 发 `cancel`；窗口关闭后回调直接丢弃，不弹框 |

### 5.5 交互状态机

| 状态 | 按钮 | 提示 |
| --- | --- | --- |
| 空列表 | 仅「添加文件 / 添加文件夹」可用 | 中央空状态大图标 + 文案 |
| 扫描中（L1） | 操作按钮禁用，显示"正在检查占用…" | 进度条不确定态 |
| 可操作 | 解锁 / 删除 / 改名 / 移动 / 复制按裁决启用 | 状态栏显示当前权限档（普通 / 管理员） |
| 需确认（Confirm） | 可勾选，点击后由 **worker** 弹框 | UI 侧**不**代问（避免双提示，v0.8 R2 已修） |
| 提权中 | 按钮显示"正在请求管理员权限…" | — |
| UAC 被取消 | 一行提示 + 重试按钮 | **不弹错误框**，不留残留进程 |
| Deny | 行红色描边 + 复选框禁用 | tooltip 显示规则 ID + 原因 |

### 5.6 视觉令牌（浅/深两套，避免硬编码颜色）

| 令牌 | 浅色 | 深色 | 用途 |
| --- | --- | --- | --- |
| `LayerFill` | `#FFFFFF` | `#202020` | 卡片底 |
| `SubtleFill` | `#F3F3F3` | `#2D2D2D` | 行悬停 / 胶囊灰 |
| `StrokeDefault` | `#E5E5E5` | `#3A3A3A` | 分割线 |
| `DangerFill` | `#C42B1C` | `#FF99A4` | 锁定 / 阻止 |
| `CautionFill` | `#9D5D00` | `#FCE100` | 映像占用 / 需确认 |
| `SuccessFill` | `#0F7B0F` | `#6CCB5F` | 可操作 |
| `AccentFill` | `#005FB8` | `#60CDFF` | 主按钮 |

用 `ThemeDictionaries` 定义 `Light` / `Dark` 两套 `ResourceDictionary`，控件只引用 `{ThemeResource ...}`。
优先复用系统资源（`CardBackgroundFillColorDefaultBrush`、`CaptionTextBlockStyle` 等），自定义令牌只补系统没有的语义色。


### 5.7 已知限制与降级路径

| 限制 | 降级 |
| --- | --- |
| 提权运行 → 拖放可能不可用（补丁有效性待 W3 定论） | `InfoBar` 告知 + 引导普通权限重启；补丁有效则由 W3 结论决定保留与否 |
| 拖入 > MAX_PATH 路径可能失败 | 提示改用选择器；选择器路径走 `\\?\` 支持 |
| Mica 仅 Win11 | 回退 `DesktopAcrylicBackdrop` → 纯色 |
| 自包含体积大（约 120–180MB） | Release 用 ZIP 分发；文档写明；v1.2 再评估 framework-dependent |
| 无 UI 自动化 | 核心逻辑单测 + 手工清单（§9.3） |

---

## 6. core / ipc / shell / cli（继承摘要）

### 6.1 core（v0.8 §4.2–4.6，不改）

| 模块 | 一句话 |
| --- | --- |
| `HandleScanner` | `NtQuerySystemInformation(64)` + 常驻线程 `NtQueryObject` 取名（100ms 超时）+ 8 线程池 |
| `PathResolver` | 设备路径 ↔ DOS 路径、junction/短名归一化（Probe 与归一化的 `OPEN_REPARSE_POINT` 语义相反） |
| `PolicyGuard` | X-Macro 规则表 49 条 + FILE_ID 双表 + 构建期 CRC + fail-closed |
| `LockBreaker` | L0 关句柄 → L1 `WM_CLOSE` → L2 进程直关 → L4 服务/进程树 → L5 重启兜底；**L3 未实现** |
| `FileOps` | 删/改名/移动/复制；先预检后执行；递归删除 × 黑名单交互表 |
| `RebootScheduler` | `PendingFileRenameOperations` |
| `Privilege` | `SeDebug/Backup/Restore`（注意 `Delete` 重试必须提 **SeRestore**） |

### 6.2 ipc（v0.8 §4.10，协议不变）

- 长度前缀 JSON，`hello` 握手 `ver=3`，版本不匹配即断连；
- 管道 DACL：自身 SID **+ UI 侧用户 SID**（跨用户提权场景）+ SACL `S:(ML;;NW;;;ME)`；
- worker 端**三重身份校验**（客户端映像路径 == 自身 exe）+ op 白名单 + 每请求重过 `PolicyGuard`；
- 空闲 5 分钟自退。

**WinUI 侧唯一补充**：`PipeClient` 的事件回调在后台线程触发，UI 只通过 `queue_.TryEnqueue` 消费（§5.4 R1/R3）。
不要试图在回调里直接改 `rows_` / `items_`——那是未定义行为，且现象往往是"偶发空白列表"这种极难复现的 bug。

### 6.3 为什么 shell 扩展必须原生（D4）

- 扩展 DLL 被加载进 **explorer.exe**，宿主不是我们的进程，XAML/WASDK 无法在其内初始化；
- 扩展的职责只有"拿到用户选中的路径 → 送给已运行实例（通道 A）或带参启动"；
- 中→中完整性走命名管道，不触发 UAC（这是方案 B 的连带收益，v0.8 §4.7.5 已论证）。

### 6.4 为什么 CLI 与 worker 不能碰 XAML（D3）

- worker 要求依赖少、可在无人交互的提权上下文稳定运行；
- XAML 需要 STA + `Application::Start` + 消息泵 + PRI 资源，任何一项缺失都崩；
- 因此 `wWinMain` 的**前两个分支必须在任何 `winrt::` API 调用与 `Application::Start` 之前完成**。

> **规则是运行时约束，不是编译期约束**（v1.1 修正）。现状 `winui/main.cpp` 无条件 `#include "App.xaml.h"`，
> 三个分支在同一 TU——这是**可以接受的**：不调用 XAML API 就不会触发 XAML 初始化，DLL 静态导入只带来约 40–80ms 启动开销。
> 原文把它写成"不得 include XAML 头"属于把未达成的状态当规则，已改。
>
> 把 UI 启动代码抽到独立 `winui/UiEntry.cpp` 是可选的编译期加固（P2，收益是防误用，代价是动一次构建），
> **不要为了它耽误 M5.1**。

---

## 7. 构建系统（本方案风险最高的部分，重点写）

### 7.1 构建拓扑

```
build.cmd                → 清代理变量 → 探测 VS → CMake 配置/构建 unlocker_core + tests + MakeLock
build.cmd winui          → 上面的 core 库 → MSBuild winui/UnlockerApp.vcxproj → 自包含产物
cmake --build build --config Release --target winui    （等价）
```

- **CMake 不管 XAML**：C++/WinRT + XAML 编译管线由 MSBuild + NuGet targets 提供，塞进 CMake 得不偿失。
- `unlocker_core.lib` 由 CMake 产出，`UnlockerApp.vcxproj` 以 `AdditionalDependencies` 链接它——
  保持"core 只有一份源码、两个前端共享"。

### 7.2 必需组件（照抄并核对 `WinUI-TaskStatus.md` §5）

| 组件 | 版本/路径 | 缺了会怎样 |
| --- | --- | --- |
| VS 2022 **Community** 17.14 | `C:\Program Files\Microsoft Visual Studio\2022\Community` | 无法编译 |
| MSVC v143（14.44） | — | 无法编译 |
| **`Microsoft.VisualStudio.Workload.UniversalBuildTools`** | — | **AppxPackage 任务集 + MrtCore PriGen 缺失 → PRI 生成失败 → 启动 fail-fast**。⚠️ 独立包 `Microsoft.VisualStudio.Windows.Build` 已从 17.14 目录移除，**只能装这个工作负载**。<br>**本机状态（2026-10-24 核验）：VS 2022 Community 17.14 已具备等效组件，无需再装** —— 本表仅供新机器 / CI 重建环境使用，别重复走那 23 分钟的安装排查 |
| Windows SDK 10.0.26100 | — | `makepri.exe`、XAML 编译器引用的 UWP winmd |
| NuGet 四包 | WindowsAppSDK 1.7.251107005 / CppWinRT 3.0.260818.1 / SDK.BuildTools 10.0.22621.756 / WebView2 1.0.2903.40 | 还原失败即无法编译；WebView2 的 winmd 是被 WASDK winmd 引用的，**不能省** |
| Python 3.13 | 规则表 CRC 生成 | 无 CRC → fail-closed 自检失效（应视为阻塞） |

**构建环境三条**（历史踩坑，已固化在 `build.cmd`）：清空 `HTTP_PROXY`/`http_proxy`（否则 MSB6001 伪装成
"找不到 CMAKE_CXX_COMPILER"）；批处理 CRLF + 纯 ASCII；VS Installer 静默安装要求启动进程本身已提权。

### 7.3 XAML 编译管线（现状 = 手工胶水，不要"规范化"它）

```
.xaml ──MarkupCompilePass1/2──> .g.h / .g.cpp / .xbf
.idl  ──cppwinrt.exe────────> winmd → 投影头 + module.g.cpp
                              └─> XamlTypeInfo.g.cpp（本项目靠 XamlTypeInfoGlue.cpp 编入）
```

现状要点（**这些都是"能编译"的必要条件，动之前先想清楚**）：

- `UnlockerApp.vcxproj` 是**手工维护**的工程，XAML 编译靠 WindowsAppSDK NuGet 包自带的编译器 + `XamlGluePass1/2` targets；
- `UnlockerApp_h.h` / `XamlMetaDataProvider_h.h` 是手工维护的投影头；
- `XamlTypeInfoGlue.cpp` 负责把生成的 `XamlTypeInfo.g.cpp` 编进工程；
- `App` **必须**在 IDL 里声明 `Microsoft.UI.Xaml.Markup.IXamlMetadataProvider`（WASDK 1.7 接口名**无大写 D**），否则
  XAML 框架在 `OnLaunched` 之前 QI 失败 → fail-fast；
- `build.cmd` 里对 `Microsoft.Windows.UI.Xaml.Cpp.Targets` 的检查保持 **WARN**（该组件不是本项目的实际依赖路径）。

### 7.4 PRI 与自包含部署（★ 当前唯一阻塞所在）

**必须成立的等式**：

```
输出目录/
├─ Unlocker.exe
├─ resources.pri                    ← MRT 运行时按这个名字找，名字不对 = 控件样式全丢
│   └─ 内含 Microsoft.UI.Xaml/Themes/themeresources.xbf（XamlControlsResources 的实体）
├─ App.xbf / MainWindow.xbf
├─ Microsoft.UI.Xaml.dll 等 WASDK 运行时 dll
└─ Microsoft.UI.Xaml/Assets/...      ← 载荷展开目录
```

落地方式（按优先级）：

1. **AfterBuild 复制 + 校验脚本**（最稳，先做这个）：`Unlocker.pri` → `resources.pri`，并在 `check-winui-output` 里断言清单齐全；
2. 再尝试用工程属性直接产出 `resources.pri`（`ProjectPriFileName` 一类），**以实测为准**，不要凭猜写进工程；
3. 语言分包（`resources.language-*.pri`）与主 pri 的配套关系需实测确认。

> 教训（已发生）：PriGen 恢复正常后，`Unlocker.pri` 内容抽查是对的（`themeresources.xbf` 已内嵌），
> 但应用仍 fail-fast——所以**"内容对"不等于"加载对"**，名字、目录、语言分包任一处不对都失败。校验脚本必须覆盖名字。

### 7.5 构建后校验（新增交付物 `tools/check-winui-output.py`）

断言以下文件存在且非空，失败即让 `build.cmd winui` 返回非 0：

```
Unlocker.exe
resources.pri
App.xbf  MainWindow.xbf
Microsoft.UI.Xaml.dll
Microsoft.WindowsAppRuntime.dll        (及同族运行时 dll)
Microsoft.UI.Xaml/Themes/ 或 Microsoft.UI.Xaml/Assets/ 载荷目录
```

并把 `resources.pri` 的 `makepri dump` 摘要（是否含 `themeresources.xbf`）打到日志里。
**这一步把"启动崩溃后再回来查"变成"构建时就报错"**，是本方案对构建链最重要的加固。

### 7.6 四条防回归铁律（写进 README 和 CI 检查）

1. `App` 在 IDL 里实现 `IXamlMetadataProvider`（无大写 D）；
2. 产物 exe 名 = `Unlocker.exe`；
3. `app.manifest` 的 `requestedExecutionLevel = asInvoker`；
4. **`vcxproj` 必须保持 `AppxPackage=false` + `WindowsPackageType=None` + `WindowsAppSDKSelfContained=true`**
   （已核验现状第 36–38 行即此配置）。这是 D2 在构建侧的落点：被 VS 属性页"顺手点掉"后会静默变成 MSIX 打包或框架依赖部署，
   症状是"换了台机器就起不来"，或者"凭空要求装 Windows App Runtime"。

任意一条被"顺手改掉"，症状分别是：启动 fail-fast / worker 拒绝所有连接 / 拖放静默失效 / 部署形态漂移。
四者都不会给出明显错误信息，排查成本极高。

### 7.7 启动失败排障决策树（把 §7.4 的经验固化成流程）

```
应用启动即退（0x802B000A / 无窗口 / 事件查看器 1000）
 │
 ├─ 1. 读 %LOCALAPPDATA%\Unlocker\winui-crash.log 的 [restricted] 行
 │      → 有原始描述：按其指向修（通常是资源/类型解析）
 │      → 无记录：说明崩在 Application::Start 更早，查 2
 ├─ 2. 校验输出目录清单（跑 check-winui-output）
 ├─ 3. makepri dump resources.pri，确认 themeresources.xbf 在内、名字对
 ├─ 4. 事件查看器 1000/1026 看崩溃模块（Microsoft.UI.Xaml.dll / WindowsAppRuntime）
 └─ 5. 以上都过 → 怀疑 XAML 元数据（是否引入了自定义类型的绑定）
```

---

## 8. 里程碑（8 阶段，26 人日）

| 阶段 | 内容 | 交付物 | 验收 | 工作量 |
| --- | --- | --- | --- | --- |
| **M0** | 工程骨架：CMake/MSVC、`asInvoker` manifest、三身份分流、日志、RAII、错误映射、`Blacklist.def` 骨架 | 可编译空壳 | 双击**不弹** UAC；`--worker` 无参直接退出 | 1.5 |
| **M1** | `HandleScanner` + `PathResolver` + `CanonicalPath` + `Probe` 状态判定（F19） | `--list` 可用 | 记事本占用的 txt 正确列出 PID 与状态；15 万句柄机器 1.5s 内 | 3.5 |
| **M2** | `FileOps`（删/改名/移动/复制）+ Level 0/1 | 无占用文件操作全通 | 只读、跨卷移动、中文长路径全通过 | 2 |
| **M3** | `LockBreaker` L2/L4 + `PolicyGuard` 裁决 + FILE_ID 表 + fail-closed | 真能解锁且删不动系统文件 | 关句柄后 explorer 立即可删；T17–T24 全绿 | 4 |
| **M4** | 提权 worker + 命名管道（ACL/SACL）+ 协议 v3 + 三重校验 + 重校验 + 空闲退出 | worker 可被拉起并完成任务 | T32–T38、T51、T52 全绿 | 2.5 |
| **M5.1** | **WinUI 3 构建链可复现**：清掉启动 fail-fast、PriGen/`resources.pri` 固化、自包含部署、`check-winui-output` 脚本 | **窗口能正常显示** | W1/W13/W14 通过；构建即校验 | **2.5** |
| **M5.2** | **窗口骨架与渲染层**：标题栏、backdrop、主题令牌、两级列表、状态胶囊、图标缓存、空状态 | 静态界面（喂假数据） | W4/W6/W7/W10 通过 | **1.5** |
| **M5.3** | **输入入口**：XAML 拖放（含高亮）、`IFileOpenDialog`、单实例转发（含冷启动重试）、快捷方式解析、边缘情况提示 | 能加入列表并裁决 | W2/W3/W5/W9（含竞态）/W15、T25–T31 通过 | **1.5** |
| **M5.4** | **编排与异步**：扫描→渲染增量、`PipeClient` → `DispatcherQueue`、按钮状态机、UAC 取消、进度 | 全流程可用 | W5/W8、T39–T50 通过 | **1** |
| **M5.5** | **打磨**：键盘加速键、窗口尺寸记忆、错误文案、主题细节、高 DPI | 可交付界面 | T48–T50、W10/W11 通过 | **0.5** |
| **M6** | Shell 扩展 + CLI + 单实例（通道 A） | 右键可用 | 多选/文件夹/Win11 二级菜单；扩展→UI 链路通 | 2.5 |
| **M7** | Level 5 重启兜底 + 加固 + 全量测试 + 打包 | Release 包 | 测试矩阵全绿；蓝屏用例 0 | 3 |
| | | | **合计** | **26** |

> 人日口径（v1.1 核对，逐行相加 = 26）：
> `1.5 + 3.5 + 2 + 4 + 2.5 + 2.5 + 1.5 + 1.5 + 1 + 0.5 + 2.5 + 3 = 26`
> 与 v0.8 的 22.5 相比净增 3.5：M5 整体从 3.5（Win32 GUI）升到 **7**（WinUI 5 个子阶段），
> 其余阶段一律不变（core/ipc/shell 零改动，这是 D1/D3 的直接收益）。

**关键路径**：

```
M1 → M2 → M3 → M4 ─┬─> M5.1 → M5.2 → M5.3 → M5.4 → M5.5 → M7
                    └─> M6 ────────────────────────────────┘
```

- 关键路径实为 **M1 → M2 → M3 → M4 → M5.1**：`FileOps`（M2）是 M3 "解锁并删除"的验收前提，
  上一版把 M2 漏画在关键路径之外是错的，v1.1 已补。
- **M5.1 是当前唯一阻塞**，也是全项目当前唯一"不知道还要多久"的环节——所以它的验收标准写成"构建即校验 + 窗口可见"，
  而不是"改到不崩"。
- **M5.2/M5.3 可以并行**（渲染层与输入层互不依赖，只要有假数据就能做 5.2）。
- **M6 与 M5.x 完全并行**（shell 扩展/CLI 不依赖 XAML）。
- M5.1 一旦超过 **3 个工作日**仍未打通，触发 §10 的备选方案评估（见 R-19），不要无限投。

---

## 9. 测试矩阵

### 9.1 继承用例（v0.8 §7，执行方式调整）

| 组 | 用例 | 变化 |
| --- | --- | --- |
| 基础 T1–T16 | 保持 | T1/T2 等原「手动点界面」改为「WinUI 界面」；断言点不变 |
| 黑名单 T17–T24 | 保持，`policy_tests.exe` 自动断言 | **不变**（纯 core） |
| 输入 T25–T31 | 保持 | **T25 语义变了**：Win32 是靠 `asInvoker` 保住 `WM_DROPFILES`；WinUI 下先按 W3 记录"补丁是否奏效"，再断言行为，不预设"必失效" |
| IPC T32–T38 | 保持 | T33/T34 不变；T37 的"UI 收到退出码"改为"UI 收到回调并提示" |
| 占用详情 T39–T47 | 保持 | T46「列表内不出现自身进程」不变；T47 不变 |
| 两级扫描 T48–T50 | 保持 | T48 语义按 §5.2.1 改写（不再期待"仍能拖放"） |
| 管道 T51–T52 | 保持 | 不变 |

### 9.2 WinUI 专项（新增 W1–W15）

| # | 用例 | 构造方式 | 期望 |
| --- | --- | --- | --- |
| W1 | 冷启动到窗口可见 | 干净环境双击 | ≤ 2s 出窗口，无 fail-fast，`resources.pri` 缺失时**必须**报错而不是静默 |
| W2 | 拖放基本链路 | explorer 拖 3 个文件进窗口 | `DragOver` 出现描边与文案；落点后 3 项入列表并完成裁决 |
| W3 | 提权后拖放（**结论性用例**） | 右键「以管理员身份运行」，再尝试拖入 | 记录两种结果之一并回写文档：**有效** → 保留 `PatchDropForElevatedUi`；**无效** → 删补丁、InfoBar 改为"请以普通权限重启"。无论哪种，`asInvoker` 不变 |
| W4 | 主题联动 | 系统切换深/浅色 | 立即跟随；自绘令牌与系统资源同步；无残留硬编码色 |
| W5 | 增量渲染 | 180k 句柄机器扫描 | 扫描期间界面可交互、行逐步出现、无白屏、不卡顿 > 100ms |
| W6 | 500 行滚动 | 一次拖入 500 个文件 | 滚动帧率稳定（虚拟化生效），无内存线性暴涨 |
| W7 | 图标缓存 | 500 行中 200 个是同名 exe | 该 exe 只解析一次；无 `HICON` 泄漏（GDI 对象数稳定） |
| W8 | 跨线程回调 | worker 高频推送进度 | 无 `RPC_E_WRONG_THREAD`；UI 不崩；关窗时回调被安全丢弃 |
| W9 | 单实例转发（含竞态） | ① 已有窗口时带路径再启动一次；② **首实例冷启动瞬间**（窗口未出现前 200ms 内）就带路径启动第二次；③ 反复 10 次 | ① 不新开窗口，路径入列；② **必须在总预算 3s 内成功投递**（`Sleep(50)` 重试循环），不得丢路径；③ 10 次 0 失败 |
| W10 | 高 DPI | 200% 缩放下运行 | 布局不错位；图标按缩放档取对应尺寸；无模糊 |
| W11 | 窗口状态 | 移动/缩放/最小化后重启 | 尺寸位置记忆生效 |
| W12 | XAML 元数据（反向） | 临时给 `ListView` 加 `x:Bind` 到自定义类型 | **预期失败**——用于固化 D5 的边界，并作为未来切换的预警 |
| W13 | 干净机器部署 | 无 VS、无 Windows App Runtime 的机器 | 直接运行；若依赖缺失，报错信息可读 |
| W14 | 资源缺失（反证） | 删掉输出目录的 `resources.pri` | 复现 fail-fast；证明该文件是硬依赖、校验脚本不可省 |
| W15 | 长路径拖入 | 构造 > 260 字符的路径拖入 | 能取到则正常入列；取不到必须给出"请改用添加文件"的可读提示，不得静默 |


### 9.3 自动化边界（明确写下不做的事）

| 层 | 手段 | 说明 |
| --- | --- | --- |
| core | `policy_tests.exe`（26 项）、`ipc_tests.exe`（10 项） | 已有，**必须全绿**才发版 |
| 黑名单 | 同上（T17–T24 固化为断言） | 发布门禁 |
| 构建 | `check-winui-output.py` | 新增，失败即构建失败 |
| **UI** | **手工清单**（W 系列 + T 系列中的 UI 项） | v1.0 **不引入** WinAppDriver/FlaUI：维护成本与 WASDK 版本绑定过紧，收益低于手工回归 |
| UI 逻辑 | 把可判定的逻辑（去重、裁决、路径解析）**继续放在 core / 纯函数**，UI 只做渲染 | 这是"不上 UI 自动化"的前提——**凡是能搬进 core 的判断都必须搬** |

---

## 10. 风险清单

### 10.1 继承 v0.8 §8 的 18 条

全部保留，逐条复核后仅两条措辞调整：

- 「用户手动以管理员运行 UI → 拖放与右键菜单静默失效」→ 现状仍保留 `PatchDropForElevatedUi` 尝试放行，**但 XAML 下有效性未知**；缓解手段统一为"横幅告知 + 不依赖补丁"（§5.2.1），结论由 W3 定夺；
- 「图标 GDI 句柄泄漏」→ 追加"`BitmapImage` 有线程亲和性，跨线程创建同样崩"（§5.3.3）。

### 10.2 新增 WinUI 3 专项风险 10 条

| # | 风险 | 影响 | 缓解 |
| --- | --- | --- | --- |
| **R-19** | **构建链不可控**：PriGen / `resources.pri` / XAML targets 任一处不对就 fail-fast，且报错信息为空 | 阻塞全部 UI 工作，工时不可预估（**当前正处在此处**） | ① M5.1 单独成阶段，验收=构建即校验；② `check-winui-output.py` 前置拦截；③ 排障决策树（§7.7）；④ **硬性时间盒：3 个工作日未通 → 启用备选**（见下） |
| R-20 | 备选方案成本：若自包含 + 手工胶水始终不稳 | 方案推翻 | 备选 A：改用 VS 官方 WinUI 3 模板工程重建 `winui/`（丢弃胶水 targets，重搭 1 天，但回归已写好的 XAML）；备选 B：`WindowsPackageType=MSIX` + 打包部署（+5～7 天）；备选 C：退回 Win32 UI 继续交付（v0.8 仍可用） |
| R-21 | XAML 类型元数据陷阱：手写 `IXamlMetadataProvider` 返回 `nullptr` 只在"纯框架类型"下成立 | 未来一旦引入绑定/自定义控件，崩溃点难定位 | D5 明确不做绑定；W12 作为反向用例固化边界；切换时用生成的 `XamlTypeInfo` |
| R-22 | 自包含体积（120–180MB） | 分发体验差、杀软扫描慢 | ZIP 分发 + 文档说明；评估 framework-dependent 放 v1.2；不塞无用语言资源 |
| R-23 | 拖放长路径/特殊项取不到（`StorageItem` 限制） | 用户"拖了没反应" | try/catch + 明确文案 + 引导选择器；W15 覆盖 |
| R-24 | 大列表 XAML 性能（行元素为 `Grid`，container 复用差） | 500 行滚动卡顿 | 增量 + 批提交；行元素复用；超 2000 行再评估数据绑定 |
| R-25 | worker/CLI 与 XAML 同 exe，启动成本与 DLL 加载风险 | worker 启动慢、极端环境下 WASDK dll 加载失败连带 worker 起不来 | 分支在 `winrt::` 调用之前完成；监控 worker 冷启动 ≤ 800ms；必要时把 worker 拆 exe（D3 反悔成本 1.5 天） |
| R-26 | 无 UI 自动化导致回归靠人 | 迭代一旦加快，回归漏洞变多 | 判断逻辑下沉 core + 手工清单固化成 checklist；每个版本记录"回归执行人 + 结论" |
| **R-27** | **单实例冷启动丢路径**：`WaitNamedPipeW` 只等"实例忙"，管道尚未创建时立即失败 | 右键菜单/带参启动"窗口起来了但列表是空的"，且**不报错**，用户只会觉得软件坏了 | 客户端 3s 重试循环（§5.2.3 第 3 条）+ W9② 反复 10 次验证；这是 5 行改动、决定"右键菜单能不能用" |
| **R-28** | **投影约束会被忘掉**：本项目用**手工维护的投影头**（`UnlockerApp_h.h` / `XamlMetaDataProvider_h.h`），`get_weak`/`get_self` 会触发 `producer<>` 实例化失败 | 后来者照标准 C++/WinRT 写法写 `get_weak()`，直接编译失败或行为异常，然后花半天排查 | §5.4 R2 写死为规则；`MainWindow.xaml.cpp` 现有源码注释保留；评审新代码时把 `get_weak` 当红线 |

---

## 11. 与 IObit Unlocker 的能力对照（补 WinUI 相关行）

| 能力 | IObit | v0.8（Win32） | **v1.0（WinUI 3）** |
| --- | --- | --- | --- |
| 找占用进程 / 关句柄 / 强制模式 / 重启兜底 | ✅ | ✅ | ✅（不变） |
| 内核级锁定 | 可能靠驱动 | ❌ | ❌（不变，明确的能力缺口） |
| 内置黑名单 | 不可见 | ✅ 三级裁决 + 规则 ID | ✅（视觉更清晰：描边 + 胶囊） |
| 拖入文件 | ✅ | ✅ 无拖入高亮 | ✅ **有真实拖入高亮 + 光标文案** |
| 软件内选择文件 | ✅ | ✅ | ✅（`IFileOpenDialog` 不变） |
| 占用者详情 | 进程名 + PID | + 描述 + 句柄数 + 8 状态 | 同左，**渲染现代化**（图标缓存 / 头像 / 主题跟随） |
| 提权模型 | 主程序直接提权 | UI 中完整性 + worker + 两级扫描 | 同左（**且提权后拖放彻底不可用，逼着用户按正确方式启动**） |
| 界面现代化 | 传统 Win32 控件 | 自绘（受限于 GDI） | ✅ 原生 Fluent：Mica/亚克力、深浅色、圆角、动效 |

---

## 12. 交付检查单（发版前逐条打勾）

**产物**

- [ ] `Unlocker.exe`（三身份一体，x64）+ `UnlockerShell.dll` + `MakeLock.exe`
- [ ] 输出目录通过 `check-winui-output.py`（含 `resources.pri`）
- [ ] 版本号一致：exe 资源 / 关于页 / `CMakeLists.txt` / `app.manifest`

**功能**

- [ ] 拖入（不弹 UAC）→ 看到占用者 → 解锁 → 删除成功
- [ ] 深扫入口：被管理员进程占用时给出"另有 N 个进程无法检查"+ 提权按钮
- [ ] 黑名单：`System32\drivers\*.sys` 拖入即红行 + 拒绝
- [ ] 右键菜单 → 路径进 UI（不弹 UAC）
- [ ] CLI `/d /r /m /c`，退出码含 6 = 需重启

**质量**

- [ ] `policy_tests.exe` 26 项全绿；`ipc_tests.exe` 10 项全绿
- [ ] W1–W15、T1–T52 手工清单全部执行且记录结论
- [ ] 干净机器（无 VS / 无 Windows App Runtime）能启动
- [ ] 无 `printf` 级调试残留；`AppTrace` 已降级为可选开关
- [ ] README 写明：`asInvoker` 的原因、提权后拖放的行为（按 W3 结论写）、UniversalBuildTools 是必需组件

---

## 13. 下一步（开工顺序）

**当前状态**：`winui/` 的 UI 改版、构建管线、崩溃根因链、UniversalBuildTools 安装、PriGen 恢复都已完成；
**唯一阻塞是启动期 fail-fast（XamlControlsResources 加载失败）**。所以第一件事不是写 UI，是把 M5.1 打通。

1. **M5.1 攻坚（最高优先）**：重建（诊断代码已加 `IRestrictedErrorInfo`）→ 运行 → 读
   `%LOCALAPPDATA%\Unlocker\winui-crash.log` 的 `[restricted]` 行 → 按 §7.7 决策树逐层排除。
   同时把 `check-winui-output.py` 写出来（半天投入，直接决定后面还会不会被同一个坑绊住）。
   **时间盒 3 个工作日**，未通则按 R-20 评估备选。
2. **M5.1 收口**：`resources.pri` 的产出方式固化成构建步骤（不再手工改名），并把"缺文件即构建失败"接进 `build.cmd winui`。
3. **M5.2 渲染层**：清掉 `AppTrace` 后，用假数据把两级列表、状态胶囊、图标缓存、主题令牌做到"看起来就是成品"。
   ——这一步不依赖后端，可以和 M5.1 的排障并行，是**当前性价比最高的工作**。
4. **M5.3/M5.4 接真数据**：接 `PipeClient`，跑通 T39–T50；重点是 §5.4 的六条线程规则，先写对再谈体验。
5. **收尾**：删临时物（`winui/build-winui/DbgView.exe`、`dbgmon.py`、`dbg_out.txt`、`prisrc/`）、
   更新 `README.md` 与 `build.cmd` 注释（UniversalBuildTools 必需）、Win32 主程序回归确认（`unlocker_core` 未改，跑测试即可）。

> 最后一句提醒（继承 v0.8 的原则，换了 UI 依然成立）：**先有刹车，再装发动机**。
> 黑名单与 IPC 重校验是刹车，UI 是发动机。UI 做得再漂亮，也挡不住一次蓝屏带来的信任崩塌。
