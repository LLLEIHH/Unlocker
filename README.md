# Unlocker

**Windows 文件占用解锁工具** —— 删不掉的文件夹、改不了名的文件、移动时提示"正被使用"？先看清楚是谁占着，再决定怎么解开。

纯用户态实现，**不加载内核驱动**；权限按「动作」分配，而不是整个窗口一直以管理员身份运行。

[![CI](https://github.com/LLLEIHH/Unlocker/actions/workflows/build.yml/badge.svg)](https://github.com/LLLEIHH/Unlocker/actions/workflows/build.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![Platform](https://img.shields.io/badge/platform-Windows%2010%20%7C%2011%20x64-lightgrey)

![Unlocker 界面](docs/screenshot-zh.png)

<details>
<summary>英文界面</summary>

![English UI](docs/screenshot-en.png)

</details>

## 下载

到 [Releases](../../releases/latest) 下载 **`Unlocker-Portable.exe`**（约 28 MB），双击即用：

- **单文件**，不需要安装，不需要 VC++ 运行库
- 首次运行自动解压到 `%LOCALAPPDATA%\Unlocker\App\<哈希>\`，之后直接复用
- 想彻底卸载：删掉这个 exe 和上面那个目录即可，不写注册表（只存窗口位置和语言）

## 能做什么

| 能力 | 说明 |
| --- | --- |
| 找占用者 | 枚举系统句柄，直接告诉你是**哪个进程**、握着**几个句柄**、能不能安全释放 |
| 8 种占用状态 | 可操作 / 已打开 / 已锁定 / 映像占用 / 权限不足 / 驱动锁定 / 不存在 / 无法访问 —— 状态不同，处理方式不同 |
| 分级解锁 | L0 直接删 → L1 关句柄 → L2 结束进程 → L4 重启后删除，能低级别解决就绝不动用高级别 |
| 安全操作 | 解锁后删除 / 改名 / 移动 / 复制，支持批量与递归目录 |
| 黑名单兜底 | 系统关键路径、页面文件、注册表配置单元等**内置规则表**直接拒绝，`--force` 也绕不过 |
| 中英双语 | 系统是中文（含港澳台）默认简中，否则英文；右上角 ZH/EN 随时切换 |
| 深扫 | 普通权限看不到服务与受保护进程的占用，需要时再单独提权扫一遍 |

## 安全模型（先说清楚，别踩坑）

- **不加载驱动**：全部通过公开 Win32/NTAPI 完成，所以做不到"解锁一切"，比如被文件系统过滤驱动锁住的情况只能如实告诉你
- **最小权限**：GUI 以普通权限启动（拖放和右键菜单才正常），只有真正写盘或深扫时才弹 UAC 拉起一个 worker
- **黑名单不可关闭**：规则表带 CRC 自检，校验不过就直接进入 fail-closed（全表拒绝），宁可不干活也不误删
- **单文件版的残余风险**：程序解压到用户可写的 `%LOCALAPPDATA%`，启动前会全量校验文件哈希，但同用户进程理论上仍能在校验后、提权前替换文件（TOCTOU）。这是"便携"换来的代价，正规安装到 `Program Files` 则没有这个问题

## 两种构建形态

| 形态 | 产物 | 说明 |
| --- | --- | --- |
| **WinUI 3**（当前交付 UI） | `Unlocker.exe` + 189 个文件 | 亚克力背景、原生标题栏、跟随系统深浅色；**WinUI 3 的 C++ 没有单文件模式**，所以另外打了单文件版 |
| **单文件版** | `Unlocker-Portable.exe`（28 MB） | 自解压引导器，把上面那堆压成 27.5 MB 塞进资源里，首次运行释放 |
| Win32（legacy） | `Unlocker.exe`（466 KB） | 最早的 Win32 界面，功能齐全但停止演进，保留给需要极小体积的场景 |

## 从源码构建

需要 **VS 2022**（Community 即可，含 C++ 工作负载）、**CMake ≥ 3.20**、**Windows SDK 10.0.26100**、Python 3。

WinUI 版第一次构建会自动下载 `nuget.exe` 并还原 4 个包（约 430 MB），需要联网；
离线环境请自行把 `nuget.exe` 放到 `tools/nuget/` 目录下（跳过下载的分支见 `build.cmd`）。

```bat
build.cmd            :: Win32 版 + 单元测试，产物在 build\Release\
build.cmd winui      :: WinUI 3 版，产物在 winui\build-winui\x64\Release\
```

单文件版由 CMake 自动打包（`tools/make_bundle.py`），构建完在 `build\Release\Unlocker-Portable.exe`。

跑测试：

```bat
build\Release\policy_tests.exe     :: 黑名单裁决、分级、三次确认
build\Release\ipc_tests.exe        :: 管道协议、身份校验、超长帧
```

## 目录结构

```
src/core/      句柄扫描、占用判定、分级拆锁、文件操作、黑名单裁决（无 UI 无 IPC 依赖）
src/ipc/       命名管道 + 提权 worker（三重身份校验）
src/bundle/    单文件版引导器（自解压 + 完整性校验）
winui/         WinUI 3 前端（唯一在演进 UI）
src/ui/        Win32 版前端（legacy，已冻结）
tools/         规则表 CRC 生成、图标生成、载荷打包、产物校验、开发期截图脚本
docs/          截图与文档
```

## 贡献

欢迎提 Issue 和 PR，先看 [CONTRIBUTING.md](CONTRIBUTING.md)。安全问题请走 [SECURITY.md](SECURITY.md)，不要开公开 issue。

## 许可

[MIT](LICENSE)。这是个会删文件的工具，请先在虚拟机或测试目录里试用。

---

# 实现细节

> 以下是按 `Unlocker-Execution-Plan.md`（v0.8，含两轮评审结论）落地的施工记录，面向要改代码的人。
> 设计取舍、踩过的坑、为什么这么写都记在这里。

## 一、已经能跑的部分

| 能力 | 状态 | 实现位置 |
|---|---|---|
| 句柄枚举 / 占用者定位（F1） | 完成 | `src/core/HandleScanner.cpp` |
| 8 种占用状态判定 + 进程身份补全（F19） | 完成 | 同上（版本资源含 `080404B0` 语言块回退） |
| 路径归一化 / CanonicalPath / junction 解引用（4.3 / 4.6.4） | 完成 | `src/core/PathResolver.cpp` |
| 内置黑名单三级裁决 + FILE_ID 双表 + CRC fail-closed（F15） | 完成 | `src/core/PolicyGuard.cpp` + `rules/Blacklist.def` |
| 分级拆锁 L0/L1/L2/L4/L5（F2/F7/F12） | 完成 | `src/core/LockBreaker.cpp` |
| 删除/改名/移动/复制，递归删除与黑名单交互（F3-F6/F11） | 完成 | `src/core/FileOps.cpp` |
| 重启兜底（F8） | 完成 | `src/core/RebootScheduler.cpp` |
| 提权 worker + 命名管道 + 三重身份校验 + 重校验 + 空闲退出（F18/F20） | 完成 | `src/ipc/*` |
| GUI：拖入 / 选择器 / 两级列表 / 深浅扫 / 提权自查（F16/F17/F20） | 完成 | `src/ui/*` |
| 右键菜单扩展（F9） | 完成（x64） | `src/shell/ContextMenu.cpp` |
| CLI（F10，退出码 0-8） | 完成 | `src/cli/CliMain.cpp` |
| 造锁工具 | 完成 | `tools/MakeLock` |
| Level 3（卸 DLL 映像） | 未实现（P2，方案里也是可选） | — |

## 二、构建

需要 MSVC 2022（本机为 **VS 2022 Community 17.14**，`C:\Program Files\Microsoft Visual Studio\2022\Community`，
MSVC 14.44 + SDK 10.0.26100；`build.cmd` 会按 BuildTools → Community → vswhere 三级探测）与 CMake ≥ 3.20。

```bat
build.cmd
```

或手动：

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DPython3_EXECUTABLE=<python.exe>
cmake --build build --config Release
```

产物在 `build\Release\`：`Unlocker.exe`、`UnlockerShell.dll`、`MakeLock.exe`、`policy_tests.exe`、`ipc_tests.exe`。

**本机踩到的坑（必须知道）**：环境里同时存在 `HTTP_PROXY` 与 `http_proxy`（大小写重复键）时，
MSBuild 的 ClCompile 任务会抛 `MSB6001 ... 关键字 HTTPS_PROXY / https_proxy`，表现为
“No CMAKE_CXX_COMPILER could be found”。构建前清掉一组代理变量即可（`build.cmd` 已处理）。

规则表 CRC 由 `tools/gen_rules_crc.py` 在构建期从 `Blacklist.def` 生成到
`build\generated\BlacklistCrc.h`，运行期重算并比对；不匹配即 **fail-closed（全表 Deny）**。

## 三、使用

### GUI

直接双击 `Unlocker.exe`（`asInvoker`，**不弹 UAC**）。拖文件/文件夹进窗口，或点「添加文件 / 添加文件夹」。
拖入即跑 **L1 免提权扫描**；只有「深扫」和任何写操作才提权。

> manifest 里的 `asInvoker` 是功能依赖：改成 `requireAdministrator` 会让拖放在 Win10/11 上
> 静默失效（收不到 `WM_DROPFILES`），不报错、不崩溃。

### CLI

```
Unlocker.exe --list <路径...>            只读，免提权，遇到打不开的进程标注「需要管理员深扫」
Unlocker.exe --list --json <路径...>     机器可读
Unlocker.exe /u  <路径...>               仅解锁（关句柄）
Unlocker.exe /d  <路径...>               解锁并删除
Unlocker.exe /r 新名 <路径>              解锁并改名
Unlocker.exe /m 目标目录 <路径...>       解锁并移动
Unlocker.exe /c 目标目录 <路径...>       解锁并复制
              --force                    允许杀进程（**不等于**无视黑名单）
              --reboot                   失败时排队到重启
              --quiet                    禁止任何 UAC（需要提权时退出码 8）
```

退出码：`0` 成功 / `1` 参数错误 / `2` 未找到占用者 / `3` 权限不足 / `4` 黑名单拦截 /
`5` 解锁失败 / `6` 需要重启 / `7` 需要确认但 quiet / `8` 需要提权但 quiet。

### 右键菜单

```
Unlocker.exe --install      （管理员）
Unlocker.exe --uninstall
```

Win11 新菜单里只会在「显示更多选项」中出现；不放 MSIX 包这是必然行为，不是 bug。

## 四、安全设计落点（与方案逐条对应）

- **UI 中完整性 + 提权 worker**：`main.cpp` 三态分流；`--worker` 无 `--pipe` 直接退出（退出码 2）。
- **管道**：`D:(A;;GA;;;UI_SID)(A;;GA;;;自身SID)S:(ML;;NW;;;ME)`。
  SACL 是功能必需（否则中完整性 UI 连不上高完整性 worker，报 `ACCESS_DENIED` 极易查错方向）；
  DACL 同时放行 UI 侧 SID（跨用户提权场景）。
- **客户端三重校验**：PID == `--parent-pid`、映像路径 == 本 exe、客户端创建时间早于 worker。
- **worker 端重校验**：路径重新归一化 + 重新裁决 + 规则表 CRC 自检，UI 传来的路径一律不可信。
- **Confirm 只由 worker 弹框**：UI 只标注「需确认」，不收集决定；`--quiet` 下返回 `code=7`。
- **op 白名单**：`hello/ping/scan/unlock/delete/rename/move/copy/reboot/cancel/shutdown`，
  没有任何 `exec/shell/run` 入口；未知 op 直接拒绝并断连。
- **单帧 ≤ 16MB**，超长长度头直接断连。
- **递归删除**：先遍历预检、再统一执行；目录 Deny 整棵跳过、子项 Deny 跳过并汇总。
- **自我排除**：扫描过滤自身与父进程，避免「Unlocker 被 Unlocker 占用」。

## 五、测试

```
build\Release\policy_tests.exe    26 项全绿（T17-T24 + PREFIX 边界 + COUNT + 三段式提示）
build\Release\ipc_tests.exe       10 项全绿（SACL 建管道 / hello / ping / scan / 重校验 / 畸形帧 / 版本错位）
```

手工验证过：

```
MakeLock.exe <file> 45                    # 造锁
Unlocker.exe --list <file>                # -> 已锁定 + MakeLock.exe (PID)
MakeLock.exe <file> --sharedelete 25      # 可删占用
Unlocker.exe --list <file>                # -> 已打开（不是「锁定」）
Unlocker.exe --list C:\Windows\System32\ntoskrnl.exe   # -> [内置黑名单] BL-003
Unlocker.exe --worker                     # -> 退出码 2，无任何行为
```

**仍需在真机上人工点一遍**（自动化无法批准 UAC）：T32（取消 UAC）、T33（会话复用只弹一次）、
T34（空闲 5 分钟自退）、T48（手动提权后拖放仍有横幅提示）、T52（标准用户输入管理员凭据跨用户提权）。

## 六、WinUI 3 版本（已能启动并跑通主流程）

`winui/` 下是 WinUI 3（C++/WinRT）版本，设计按三条已确认的决策落地：
**自绘标题栏（`ExtendsContentIntoTitleBar`）+ 配色跟随系统（ThemeResource / `ActualThemeChanged`）+ 卡片式布局**。

```
winui/
├─ UnlockerApp.vcxproj      # 引用 WindowsAppSDK 1.7 + CppWinRT；XAML 胶水手写
├─ App.xaml / .h / .cpp     # 合并 XamlControlsResources；IXamlMetadataProvider 委托给生成 provider
├─ XamlTypeLookup.h         # ← 新增：把生成的 XamlTypeInfoProvider 暴露给 App（M5.1 的解法）
├─ XamlTypeInfoGlue.cpp     # 唯一包含 XamlTypeInfo.g.cpp 的 TU
├─ MainWindow.xaml          # 亚克力 + 两张圆角卡片 + 自绘标题栏 + 键盘加速键
├─ MainWindow.xaml.h/.cpp   # 拖放、扫描、深扫、五个操作、事件绑定、窗口位置记忆
├─ main.cpp                 # 三态分流（--worker / CLI / WinUI）+ 单实例转发
└─ packages/                # nuget 还原产物（WindowsAppSDK / CppWinRT / WebView2 / SDK.BuildTools）
```

数据层与安全层**零改动**：直接编译 `src/core` + `src/ipc` + `src/ui`，worker、黑名单、协议、退出码全部复用。

### 构建

```bat
build.cmd winui
```

脚本会：还原 NuGet 包 → CMake 配置 → 调 MSBuild 构建 → 跑 `tools/check-winui-output.py` 校验产物
（缺 `Unlocker.exe` / PRI / XBF / 框架 dll，或 PRI 里没有 `themeresources.xbf` 等资源名，直接失败）。
产物：`winui\build-winui\x64\Release\Unlocker.exe`。

### 曾经卡住的点（已解决，别绕回去）

1. **需要 UWP/WinUI 构建支持**：现装的是 **VS 2022 Community 17.14**，其 MSBuild 自带
   `AppxPackage` 任务集（`Microsoft.Build.AppxPackage.dll`、`Packaging.Pri.Tasks.dll`），
   PriGen 能正常产出 `Unlocker.pri` → 构建后自动复制成 `resources.pri`（MRT unpackaged 约定名）。
2. **启动 fail-fast 0x802B000A 的真正原因不是 PRI**，而是 `App` 的 `IXamlMetadataProvider` 三个方法
   一律返回 `nullptr`：框架实例化 `XamlControlsResources` 时取不到控件类型元数据 → 直接崩。
   解法：把 XAML 编译器生成的 `XamlTypeInfoProvider`（自带回落框架 provider 的逻辑）通过
   `winui/XamlTypeLookup.h` 暴露出来，`App::GetXamlType` 委托给它。
   **不要再**通过「删 `XamlControlsResources`、把颜色写成字面量」来绕过。
3. WinUI 版本必须是 **unpackaged**（`WindowsPackageType=None` + 自包含）。
   MSIX 打包的应用里 `ShellExecuteEx(runas)` 基本不可用，而整套安全模型依赖「UI 拉起提权 worker」。
   另外 exe 必须仍然叫 `Unlocker.exe`——worker 三重校验要求客户端映像路径等于自身 exe。

### 键盘

`Ctrl+O` 添加文件 / `Ctrl+Shift+O` 添加文件夹 / `F5` 重新扫描 / `Ctrl+A` 全选 /
`Del` 从列表移除（不动磁盘）/ `Ctrl+Del` 解锁并删除 / `Enter` 解锁。
窗口位置与大小记在 `HKCU\Software\Unlocker\WindowPlacement`（只在正常关闭且非最大化时写入）。

### 窗口尺寸（DPI 与自适应）

- **最小尺寸 820×560 DIP**，用 `OverlappedPresenter.PreferredMinimum{Width,Height}` 设死，
  程序化 `SetWindowPos` 也压不下去。注意这个 API 收的是**物理像素**，代码里按
  `GetDpiForWindow()/96` 折算（150% 缩放下其实是 1230×840 像素）。
- **宽度 < 980 DIP** 时底部六个操作按钮自动退化成纯图标（文案隐藏、ToolTip 保留描述），
  避免横向挤爆把「重新扫描」裁出可视区。
  切换是代码在 `RootGrid.SizeChanged` 里做的 —— 实测 `AdaptiveTrigger` 在这个手写 XAML
  胶水工程里不触发，别改回声明式写法。
- 卡片有 `MinHeight`（文件卡 176 / 进程卡 132 DIP），配合上面的最小高度保证内容不会被压扁。
- 恢复上次位置时会夹进当前显示器工作区，换显示器/改缩放后不会跑到屏幕外。

开发期核对界面用 `tools/screencap.py`（GDI 截图，不依赖 Pillow/.NET）和
`tools/resize_check.py`（程序化缩放，验证最小尺寸）。两个都要在 DPI 感知下跑，脚本里已处理。


## 七、已知限制

1. **进程列表右键「结束进程」菜单未做**：协议 op 表是白名单，没有 `kill` 项；终止进程目前只能通过
   「强制模式 + 解锁并删除」这条路径走 `Level 4`，并且会先过黑名单与二次确认。
   要补这个菜单，得先给协议加一个 `kill(pid)` 并在 worker 端做同样的裁决，不是纯 UI 改动。
2. **未产出 x86 版本**：`UnlockerShell.dll` 需要 32/64 位各一份（注册表分别写 `HKCR\CLSID` 与
   `HKCR\Wow6432Node\CLSID`）。CMake 已支持 `-A Win32`，但本次只构建了 x64。
3. **Level 3（卸映像）未实现**，映像占用会如实提示「只能结束进程或重启后删除」。
4. `HICON` 不参与 IPC（`ProcessInfo` 过不了管道），UI 侧要靠 `fullPath` 自己取图标；当前列表用文字状态，
   图标未接入。
5. 黑名单规则表 CRC 只能防「改了规则忘了改 CRC」这类手滑，**不是防破解**；真正的完整性只能靠代码签名。

## 八、版本控制

仓库用 git 管理（`main` 分支）。几个约定，改 `.gitignore` 之前先看一眼：

| 规则 | 原因 |
| --- | --- |
| 忽略 `/build/`、`/winui/build-winui*/` | 构建产物（约 2.4 GB），随时可再生 |
| 忽略 `/winui/packages/` | NuGet 还原产物（428 MB），`build.cmd winui` 会自动重新下载 |
| 忽略 `/.workbuddy/` | 开发期记忆与会话数据，不属于源码 |
| 忽略 `/tools/nuget/nuget.exe` | 8.5 MB 二进制，`build.cmd winui` 缺失时会自动下载 |

`.gitattributes` 里 **`*.cmd` / `*.bat` 强制 `eol=crlf`**：LF 的批处理在 cmd.exe 下会解析出错
（`chcp 65001` 时还会吃中文字节）。仓库内统一存 LF，工作区按规则还原成 CRLF。

## 九、应用图标

图标不是手绘的位图，而是**几何定义 → 一次生成全部尺寸**：

```
assets/icons/unlock-a4.svg     # 设计稿（矢量，可编辑）
assets/icons/unlocker.ico      # 成品：16/24/32/48/64/128/256 七档
tools/make_icon.py             # 生成器（纯 Python，无 Pillow/ImageMagick 依赖）
```

改图形只需要改 `tools/make_icon.py` 里的 `VARIANTS` 几何常量，然后：

```bat
python tools\make_icon.py --variant a4 --ico assets\icons\unlocker.ico --svg assets\icons\unlock-a4.svg
python tools\make_icon.py --sheet preview.png        :: 三方案对照图
```

设计空间固定 256×256：圆角方块 8..248（r=56）、锁体 72..184 × 114..206、
锁梁弧心与锁体主轴都在 x=128（早期版本锁梁整体偏右 14 单位，看起来是歪的，已修）。

两个 exe 都通过各自的资源脚本引用它：

| 前端 | 资源脚本 | 说明 |
| --- | --- | --- |
| WinUI 3 | `winui/UnlockerApp.rc` | 图标 + 版本信息；rc.exe 靠 `AdditionalIncludeDirectories` 找 ico |
| Win32 | `src/ui/Resource.rc` | 同上；ico 路径由 `CMakeLists.txt` 的 `include_directories` 提供 |

资源 ID 用数字 `101`（`#define IDI_APPICON 101`），不用字符串名 —— 字符串名虽然也能编译，
但 shell 取图标是按序号落的，用名字容易出现「资源在、任务栏却没图标」。

校验方式（不依赖任何额外工具）：`FindResourceW(exe, 101, RT_GROUP_ICON)` 非空，
或 `ExtractIconExW` 取第 0 个图标能画出来。

## 十、单文件版（Unlocker-Portable.exe）

WinUI 3 的 C++ 版本**没有**单文件模式：运行时 DLL、`resources.pri`、`*.xbf` 必须落盘
（实测 Release 目录 189 项 / 62 MB）。所以单文件版是个**自解压引导器**：

```
unlocker/Unlocker-Portable.exe          # 29 MB，单文件，拷到哪都能双击
   └─ 内嵌 RCDATA 载荷（27.5 MB）
        └─ 首次运行解压到 %LOCALAPPDATA%\Unlocker\App\<载荷哈希>\
             └─ 启动真正的 Unlocker.exe
```

| 项 | 说明 |
| --- | --- |
| 体积 | 62.1 MB → **27.5 MB**（XPRESS_HUFF，2.26x） |
| 依赖 | 只有 `Cabinet` / `ADVAPI32` / `KERNEL32` / `USER32` —— **静态 CRT，不需要 VC++ 运行库** |
| 首次运行 | 解压约 **0.5 秒**（62 MB 解压 + 落盘），之后写 `ready.tag` |
| 再次运行 | 0.7 秒（其中 ~0.5 秒是启动前的完整性校验，见下） |
| 完整性校验 | 每次启动重算全部 275 个文件的 FNV-1a 64，与载荷里的记录比对；对不上就重新解压（自愈） |
| 目录 ACL | 解压目录写**显式 DACL 并断开父目录继承**（只留 当前用户 / SYSTEM / Administrators），父目录被放宽也波及不到 |
| 升级 | 载荷哈希变了就落到新目录，旧版本自动清理，不会越用越大 |
| 并发 | 命名互斥量 `Local\Unlocker.Bundle.Extract` 串行化解压 |
| 命令行 | 参数原样转发给内层 exe，退出码原样返回（CLI / `--worker` 都靠这条） |

**为什么不用压缩库**：本机没有 zlib 可链，而 Windows 自带的 CompressAPI 恰好能用。
坑在于「自己拼压缩流格式」——MSZIP 的块头格式试了三种写法都和系统解压器对不上（都返回 605），
最后改成**生产和消费都用 CompressAPI**（`tools/make_bundle.py` 压缩、`BundleMain.cpp` 解压），
格式天然一致。XPRESS_HUFF 对 DLL 约 2.3x，解压 60 MB 只要 0.1 秒。

构建：

```bat
build.cmd            :: 走 CMake，会自动先跑 tools/make_bundle.py 生成载荷
```

产物 `build\Release\Unlocker-Portable.exe`。

`make_bundle.py` 打包的是 `winui\build-winui\x64\Release`，所以**每次构建都会重跑打包**
（`add_custom_target(... ALL)`）—— 用 `add_custom_command(OUTPUT)` 的话「输出已存在」会永久跳过，
改了 UI 还发旧界面。脚本内部比内容，没变化就不落盘，因此重复构建不会白白重编 29 MB 资源。

### 残余风险（说清楚，不粉饰）

单文件版把 exe 解到 `%LOCALAPPDATA%` —— **用户可写目录**，而程序会从这里拉起需要 UAC 提权的 worker。
启动前的哈希校验能挡住「文件被改/被截断」，但挡不住同用户进程在校验之后、提权之前替换文件（TOCTOU）。
真正的边界是「同一个用户本来就完全控制自己的 `%LOCALAPPDATA%`」，任何 ACL 都改不了这一点。
要彻底消除，只能不把提权目标放在用户可写目录（例如正规安装到 `Program Files` 用固定路径）。
