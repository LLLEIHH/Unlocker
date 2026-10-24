# Unlocker WinUI 环境清单核验与修正方案

> 生成时间：2026-10-24 01:15
> 依据：`WinUI-TaskStatus.md` 第五节《所需软件与环境工具清单》
> 性质：**方案文档，待确认后执行**（无任何安装动作将发生）

---

## 一、核验结论（一句话版）

**清单所列全部必需组件在本机均已就位，无需安装任何软件。**
唯一问题：文档与 build.cmd 中写死的 VS「BuildTools」路径与本机不符——本机实际安装的是
**VS 2022 Community 17.14**（同一版本号 17.14.37806.7），且所需组件在 Community 中齐全。

---

## 二、逐项核验明细

### 5.1 操作系统与运行时

| 项目 | 文档要求 | 本机现状 | 结论 |
| --- | --- | --- | --- |
| Windows 11 (10.0.26100) | Win10 1809+ | 已满足（SDK 26100 已正常安装） | ✅ 无需动作 |
| Windows App Runtime | 无需单独安装（自包含部署） | — | ✅ 无需动作 |

### 5.2 构建工具链（必需项）

| 工具 | 文档要求 | 本机实际 | 结论 |
| --- | --- | --- | --- |
| VS 2022 17.14 | BuildTools @ `C:\Program Files (x86)\...\BuildTools` | **Community** @ `C:\Program Files\Microsoft Visual Studio\2022\Community`（17.14.42 / 37806.7，vswhere 确认） | ⚠️ 产品存在、路径与文档不符 |
| MSVC v143 14.44 | VC.Tools.x86.x64 | `Community\VC\Tools\MSVC\14.44.35207`，`cl.exe` 在位 | ✅ |
| UniversalBuildTools 工作负载 | 提供 AppxPackage 任务集等 | Community 的 `MSBuild\Microsoft\VisualStudio\v17.0\AppxPackage\` 下 `Microsoft.Build.AppxPackage.dll`、`Microsoft.Build.Packaging.Pri.Tasks.dll` 等 11 个 DLL **全部在位** | ✅ 等效能力已具备 |
| WindowsXaml 规则 | XAML targets | `Community\MSBuild\Microsoft\WindowsXaml\v17.0\` targets 在位 | ✅ |
| Windows SDK 10.0.26100 | makepri + UWP winmd | `Windows Kits\10\bin\10.0.26100.0\x64\makepri.exe` 在位；Lib 10.0.26100.0 在位 | ✅ |
| CMake | VS 自带 | PATH 上有 WinGet 版 CMake 4.4.4（build.cmd 自带 fallback，会用它） | ✅ |
| Python 3.13 | 辅助脚本 | `C:\Users\11415\.workbuddy\binaries\python\versions\3.13.12`（build.cmd 用 `%USERPROFILE%` 定位，天然兼容） | ✅（文档中 `C:\Users\LEI\...` 为过时笔误） |

### 5.3 NuGet 包（winui/packages/）

| 包 | 要求版本 | 本机 | 结论 |
| --- | --- | --- | --- |
| Microsoft.WindowsAppSDK | 1.7.251107005 | ✅ 在位 | ✅ |
| Microsoft.Windows.CppWinRT | 3.0.260818.1 | ✅ 在位 | ✅ |
| Microsoft.Windows.SDK.BuildTools | 10.0.22621.756 | ✅ 在位（含包内 makepri.exe，vcxproj 实际用这份） | ✅ |
| Microsoft.Web.WebView2 | 1.0.2903.40 | ✅ 在位 | ✅ |
| nuget.exe | tools\nuget\ | ✅ 在位 | ✅ |

### 5.4 / 5.5（环境注意事项、诊断工具）

- 5.4 全部为**操作注意事项**（清代理变量、CRLF、提权方式、catalog.json 检查），非安装项。
  其中清代理逻辑 build.cmd 已内置；VS 安装类注意事项本轮**不适用**（无需安装）。
- 5.5 诊断工具为**可选排障件**：应用内 AppTrace 已内置；makepri 已就位；
  DebugView / 事件查看器属于系统自带或按需下载，本轮不安装。

---

## 三、发现的两个不一致（本方案的修正对象）

### 问题 ①：build.cmd 写死的 VSDIR 指向不存在的 BuildTools

- 第 13 行 `set "VSDIR=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"`，
  本机该目录不存在。
- 现状影响：`CMAKE` 变量失效 → 第 16 行自动 fallback 到 PATH 的 cmake（能跑）；
  XAMLDIR 检查只 WARN 不阻塞。**即当前构建实际可以跑通，但与文档描述不符，且坑在暗处。**
- 拟修正：VSDIR 按「BuildTools → Community → vswhere 探测」顺序定位，
  找不到任何 VS 时报错退出（fail loud，不再静默 fallback 伪装正常）。

### 问题 ②：WinUI-TaskStatus.md 5.2 节路径勘误

- 「VS 2022 BuildTools @ (x86)\...\BuildTools」→ 实为 Community @ Program Files。
- 「Python @ C:\Users\LEI\...」→ 实为 `C:\Users\11415\...`。
- 拟修正：更新该表两处路径，并加一行备注「本机以 Community 17.14 提供全部等效组件，
  无需安装 BuildTools」。

---

## 四、执行步骤（待确认）

| 步骤 | 动作 | 风险 |
| --- | --- | --- |
| 1 | 修改 `build.cmd`：VSDIR 三级探测（BuildTools → Community → vswhere），均未命中则显式报错 | 低（脚本级，纯文本修改） |
| 2 | 勘误 `WinUI-TaskStatus.md` 5.2 节两处路径 + 补充备注 | 低 |
| 3 | 验证构建：`build.cmd winui` 跑通，确认产物 `winui\build-winui\x64\Release\Unlocker.exe` | 无（只构建不运行） |
| 4 | 同场验证 Win32 主程序：`build.cmd`（默认 target）跑通，确认 `build\Release\` 产物 | 无 |

- **不做的事**：不安装任何 VS 组件 / SDK / 运行时（清单已齐）；
  不下载 DebugView（可选项，需要时再说）；不动 winui/packages。
- 失败处理：按你的规则，任何一步连续失败超过 3 次即停下交还决策。

---

## 五、需要你确认的点

1. ✅ / ❌ 按「步骤 1–4」执行修正与验证？
2. VSDIR 定位策略用「三级探测」还是直接写死 Community 路径（更简单，但换机器又要改）？
   我推荐三级探测。
