# Unlocker 项目架构与开发手册

> 更新：2026-10-23 ｜ 用途：切换开发环境后的完整上下文恢复文档  
> 配套文档：`WinUI-TaskStatus.md`（当前任务状态与环境清单）、`Unlocker-Execution-Plan.md`（v0.8 执行方案，1619 行）、`Unlocker-Plan-Review.md` / `Unlocker-Plan-Review-R2.md`（两轮评审）

---

## 一、项目定位

**Unlocker** —— Windows 文件占用解除工具（Unlocker/FileUnlocker 的现代替代品）：  
找出锁定指定文件/目录的所有进程与句柄，按需解锁、解锁并删除/改名/移动/复制，必要时支持重启兜底删除。

- 语言：**C++20** / Win32 + WinUI 3（双前端）
- 版本：v0.8.0（CMake project 版本号）
- 许可目标：用户态，全流程 UAC 明确提示，无静默提权

---

## 二、总体架构（三层）

```
┌─────────────────────────────────────────────────────────┐
│  入口层（一个 exe 三种身份，按命令行分流，见 src/main.cpp）      │
│   默认 → Win32 GUI   --worker → 提权 worker   CLI 参数 → CLI  │
├──────────────┬──────────────────────┬───────────────────┤
│  src/ui      │  src/ipc             │  src/cli           │
│  Win32 GUI   │  PipeServer/PipeClient│  CliMain          │
│  (asInvoker) │  长度前缀 JSON v3     │  /d /r /m /c       │
├──────────────┴──────────────────────┴───────────────────┤
│  src/core —— 纯逻辑库（unlocker_core.lib，无 UI / 无 IPC 依赖）│
│   HandleScanner │ PathResolver │ PolicyGuard │ LockBreaker │
│   FileOps │ RebootScheduler │ Privilege                     │
├─────────────────────────────────────────────────────────┤
│  src/common —— Log / NtApi / ErrorMap / StringUtil / Handles │
└─────────────────────────────────────────────────────────┘
  另有：src/shell（右键菜单 shell 扩展 DLL）、winui/（WinUI 3 前端，复用 core+ipc）
```

**进程模型**：GUI 进程（asInvoker，不提权）通过命名管道连接一个以管理员身份  
重新启动的自身副本（`--worker`）。worker 做所有特权操作；GUI 只做展示与编排。

---

## 三、核心模块详解（src/core）

| 模块                | 职责与要点                                                                                                                                                                                                                       |
| ----------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `HandleScanner`   | NtQuerySystemInformation(SystemHandleInformation=64) 枚举句柄 + 常驻线程逐个 `NtQueryObject` 取名（带 100ms 超时防卡死）+ 8 线程池；启动时探测文件对象类型索引                                                                                                   |
| `PathResolver`    | 设备路径（`\Device\HarddiskVolumeX`）→ DOS 路径、junction/短名归一化；注意 Probe 与归一化的 OPEN_REPARSE_POINT 语义相反                                                                                                                               |
| `PolicyGuard`     | **黑名单与操作裁决**。X-Macro 规则表（`core/rules/BlacklistTable.h` + `Blacklist.def`，BL-001…）；CMake 构建期用 `tools/gen_rules_crc.py` 生成规则表 CRC（`build/generated/BlacklistCrc.h`），运行期自检，不匹配进 **fail-closed**（全部操作按 Deny）；另有 FILE_ID 双表防路径绕过 |
| `LockBreaker`     | 解锁阶梯：L0 关闭句柄（Duplicate+DUPLICATE_CLOSE_SOURCE）→ L1 温和消息（WM_CLOSE）→ L2 进程直关 → L4 服务停止/进程树 → L5 重启调度；**Level 3（卸载映像）未实现**                                                                                                     |
| `FileOps`         | 解锁后的文件操作（删除/改名/移动/复制）；先预检后执行；**递归删除 × 黑名单交互表**（父/子路径任一命中即拒绝）                                                                                                                                                                |
| `RebootScheduler` | PendingFileRenameOperations 重启兜底                                                                                                                                                                                            |
| `Privilege`       | SeDebugPrivilege 等令牌调整                                                                                                                                                                                                      |

---

## 四、安全模型（PolicyGuard / worker）

- 规则表 49 条（BL-001 起）+ FILE_ID 21 个，CRC 自检 fail-closed
- worker 侧：SACL `S:(ML;;NW;;;ME)` 管道 + 客户端 SID 白名单 + **三重身份校验**  
  （客户端映像路径必须等于自身 exe，所以 exe 名必须保持 `Unlocker.exe`）
- 每个请求 worker 端**重新过一遍 PolicyGuard**（不信任 GUI 侧判断），危险操作弹 Confirm 框
- op 白名单（未知 op 拒绝并断连）、空闲 5 分钟自动退出
- 协议：长度前缀 JSON，hello 握手 `ver=3`（版本不匹配拒绝）

---

## 五、三个入口 + shell 扩展

| 入口                                         | 说明                                                                                                                                             |
| ------------------------------------------ | ---------------------------------------------------------------------------------------------------------------------------------------------- |
| Win32 GUI（`src/ui/MainWindow.cpp`）         | asInvoker；拖放 + IFileOpenDialog 添加；两级 ListView（文件/进程，custom draw 状态配色）；提权自查横幅；单实例 `Local\Unlocker.Instance` 互斥体；`SendPathsToRunningInstance` 转发 |
| CLI（`src/cli/CliMain.cpp`）                 | `/d /r /m /c /--reboot`（删除/改名/移动/复制/重启兜底）；退出码 6 = 需重启                                                                                          |
| worker（`src/ipc/PipeServer.cpp`）           | 提权副本，跑 PipeServer 等 GUI 连接                                                                                                                     |
| shell 扩展（`src/shell/ContextMenu.cpp/.def`） | `UnlockerShell.dll` 右键菜单，产物 MakeLock 生成测试锁文件（`tools/MakeLock`）                                                                                 |

---

## 六、WinUI 3 前端（winui/）——当前工作重点

### 6.1 架构

- 复用 `src/core` + `src/ipc`（链接 unlocker_core.lib，PipeClient 提权 worker，与 Win32 GUI 完全同构）
- 同一 `Unlocker.exe` 三身份分流：`winui/main.cpp`（`--worker` / CLI / WinUI）
- UI：`App.xaml(.cpp)` + `MainWindow.xaml(.cpp)`，深浅色跟随系统（`ActualThemeChanged`），  
  DesktopAcrylicBackdrop 亚克力背景 + 自绘标题栏
- 现代化 UI 元素：状态胶囊徽章（四色语义）、文件类型彩色块、进程首字母头像、  
  FontIcon 图标按钮（Segoe Fluent Icons）、黑名单行红色左描边 + 规则 ID、底部进度条 + 统计栏

### 6.2 ⚠️ 手工胶水构建管线（本机无 VS UWP 工作负载时也能编译 XAML）

`UnlockerApp.vcxproj` 是**手工维护的 MSBuild 工程**（CMake 用 `build.cmd winui` 驱动），含：

- XAML 编译：WindowsAppSDK NuGet 包自带编译器 + 胶水 targets（XamlGluePass1/2）
- cppwinrt 投影：CppWinRT NuGet；IDL = `UnlockerApp.idl`（App 含 `IXamlMetadataProvider` 接口声明，**必须**，见下）+ `XamlMetaDataProvider.idl`
- `UnlockerApp_h.h` / `XamlMetaDataProvider_h.h`：手工维护的历史投影头
- `XamlTypeInfoGlue.cpp`：把生成的 XamlTypeInfo.g.cpp 编进工程
- 已回退的历史 workaround：`AppxGeneratePriEnabled` 等三属性、`GetMrtPackagingOutputs` 空实现  
  （UniversalBuildTools 装好后 PriGen 正常，这些已删）
- `build.cmd` 的 WindowsXaml 检查已降级为 WARN

### 6.3 关键约束

- **App 必须实现 `IXamlMetadataProvider`**（WASDK 1.7 接口名，无大写 D）——通过 IDL 声明进  
  App 投影基类；`App.xaml.cpp` 的 GetXamlType 系列当前对未知类型返回 nullptr
- exe 必须叫 `Unlocker.exe`（worker 三重校验）
- **UniversalBuildTools 工作负载是 WinUI 3 运行的硬依赖**（PriGen 产出 resources.pri，  
  XamlControlsResources 的默认控件样式全靠它；缺它时任何含标准控件的 XAML 都无法解析，  
  启动即 0x802B000A fail-fast）

---

## 七、构建系统

### 7.1 构建命令

```cmd
:: Win32 主程序（GUI + worker + CLI + shell 扩展 + MakeLock）
build.cmd                          :: 或 cmake --build build --config Release
:: WinUI 3 前端
build.cmd winui                    :: 或 cmake --build build --config Release --target winui
```

- 产物：
  - Win32：`build/Release/Unlocker.exe`（三身份一体）、`UnlockerShell.dll`、`MakeLock.exe`
  - WinUI：`winui/build-winui/x64/Release/Unlocker.exe`
- 测试：`build/Release/policy_tests.exe`（26 项，T17-T24）、`ipc_tests.exe`（10 项，协议/重校验/畸形帧）

### 7.2 环境要求

**详见 `WinUI-TaskStatus.md` 第五节**（完整软硬件清单）。速记：

- VS BuildTools 17.14 + MSVC v143 + **UniversalBuildTools 工作负载**（WinUI 必需）
- Windows SDK 10.0.26100、CMake（VS 自带）、Python 3.13
- NuGet（`tools/nuget/nuget.exe`）还原四包：WindowsAppSDK 1.7 / CppWinRT 3.0 / SDK.BuildTools / WebView2
- 构建前清空代理变量（MSB6001 坑，build.cmd 已内置）

---

## 八、执行方案（v0.8）功能基线

`Unlocker-Execution-Plan.md`（1619 行）为唯一权威需求文档，两轮评审记录在  
`Unlocker-Plan-Review*.md`。里程碑 8 阶段 22.5 人日；已实现 F3-F6（删除/改名/移动/复制）、  
F8（重启兜底）、F12（POSIX 语义删除）、§4.4 六级解锁阶梯（L1、L5 代码已含）、§4.5  
FileOps 与递归删除×黑名单交互表、§9/§10.1。  
**已知未做**：Level 3 卸映像、进程右键「结束进程」（协议无 kill op）、x86 版 DLL、  
真机 UAC 交互用例（T32-T34/T48/T52）。

---

## 九、当前状态与待办（切机后先读这个）

**详见 `WinUI-TaskStatus.md`**（含环境工具清单）。速记（截至 2026-10-23 17:55）：

- ✅ WinUI 3 现代化改版、构建管线、启动崩溃根因链、UniversalBuildTools 安装、PriGen 恢复
- ❌ **唯一阻塞**：应用启动仍在 OnLaunched 前 fail-fast（XamlControlsResources 加载失败）。  
  下一动作：重建（`App.xaml.cpp` 已加 IRestrictedErrorInfo 诊断）→ 运行 → 读  
  `%LOCALAPPDATA%\Unlocker\winui-crash.log` 的 `[restricted]` 行 → 按消息修复  
  （候选：PRI 命名 Unlocker.pri→resources.pri 的正式化；.xaml→.xbf 解析回退确认）
- ⬜ 收尾：清理调试代码/临时文件、更新 README、Win32 回归

---

## 十、切换电脑 Checklist

1. **初始化版本控制**（⚠️ 项目当前无 git，历史教训：v0.9 回退只能靠逆向）：  
   `git init && git add -A && git commit -m "v0.8 baseline"`
2. 安装环境：按 `WinUI-TaskStatus.md` 第五节（BuildTools + UniversalBuildTools + SDK + NuGet 四包会自动还原）
3. 构建：`build.cmd`（Win32 验证）→ `build.cmd winui`（WinUI 验证）
4. 测试：跑 `policy_tests.exe` / `ipc_tests.exe` 应全绿
5. 若 WinUI 仍启动崩溃：先看 `%LOCALAPPDATA%\Unlocker\winui-crash.log`
6. 历史日志：`.workbuddy/memory/2026-10-22.md`（v0.9 回退、Win32 实现）、  
   `2026-10-23.md`（改版、构建修复、崩溃排查全过程）
