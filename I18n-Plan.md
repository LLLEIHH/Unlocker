# 中英文双语支持 — 施工方案

> 版本 v1（2026-10-24 12:30）· 待确认
> 范围：界面文案 + 错误/状态文案（+ CLI，可选）

---

## 1. 语言选择规则（按你的要求）

| 情况 | 默认语言 |
| --- | --- |
| 系统首选 UI 语言是 `zh-*`（含 `zh-CN` / `zh-TW` / `zh-HK` / `zh-MO` / `zh-SG`） | **简体中文**（港澳台也统一简中，按你的要求） |
| 其它任何语言 | **English** |

判定用 `GetUserPreferredUILanguages()`（用户首选 UI 语言列表，比 `GetSystemDefaultUILanguage` 更贴「系统语言」的体感）；
取第一个 `zh` 前缀即中文。判定结果**只在首次运行时生效**，之后以用户手动切换为准。

用户手动选择持久化到 `HKCU\Software\Unlocker\Language`（`zh` / `en`）。
右上角 `ZH/EN` 按钮切换后立即生效（无需重启），并写入该键。

---

## 2. 技术方案：代码内字符串表（**不引 resw / PRI**）

理由：这工程的 XAML 管线是手写胶水（vcxproj 里自己补 MarkupCompile 目标），再往 PriGen 里塞
`PRIResource` 属于给最脆的地方加负载。而本项目 195 条文案全在代码里（`SetStatus`、`ShowMessage`、
`OccupancyText`、`ErrorMap`），走 resw 反而要额外做 `ResourceLoader` 查表 —— 收益为零。

所以：

```
src/common/Lang.h        // enum class Lang { Zh, En }; Lang GetLang(); void SetLang(Lang);
                         // Lang DetectSystemLang();
                         // const wchar_t* T(Str id);
                         // std::wstring Tf(Str id, ...);   // 带参数的格式化
src/common/Lang.cpp      // 两张同序表：kZh[] / kEn[]，静态断言长度相等（漏翻编译期就报）
```

- 表用**同序数组**而不是 map：查表 O(1)，且 `static_assert(sizeof(kZh)==sizeof(kEn))` 能防漏项
- 需要拼接的句子用 `Tf(Str::AddedN, n)`（内部 `swprintf`，格式串也进表），避免中文语序被硬编码
- 语言是进程级全局（`std::atomic<Lang>`），core / CLI / UI 共用一份

## 3. 需要改造的位置（实测条数）

| 文件 | 中文字面量 | 处理 |
| --- | --- | --- |
| `winui/MainWindow.xaml` | 29 | 18 个 `TextBlock` 加 `x:Name`，`ApplyLanguage()` 里统一赋值 |
| `winui/MainWindow.xaml.cpp` | 71 | 状态栏/对话框/徽章/空状态等，改 `T()`/`Tf()` |
| `src/common/ErrorMap.cpp` | 38 | `OccupancyText` / `OccupancyExplain` / `Win32Message` 走表 |
| `src/cli/CliMain.cpp` | 57 | 同上（**可延后**） |
| `src/common/Log.cpp`、`PolicyGuard` 提示 | ~20 | 日志保持中文（面向排障，不翻译） |

## 4. 右上角 ZH/EN 按钮

- 位置：`MainWindow.xaml` 根 Grid 第 0 行（标题栏区），`HorizontalAlignment="Right"`，
  放在 `TitleBarGrid` **之后**作为兄弟节点 —— 这样它在拖拽区之上，点击不会被 `SetTitleBar` 吞掉
- 右边距按 `AppWindow().TitleBar().RightInset()`（物理像素）除以 DPI 缩放换算成 DIP，
  避免 150% 缩放下压到最小化/关闭按钮
- 外观：`Button` + 单个 `TextBlock`（`ZH` / `EN` 表示"点一下切到哪种语言"），
  22 高、透明底、悬停才有底色，贴 Fluent 标题栏控件的样子
- 点击：`SetLang(反转)` → 写注册表 → `ApplyLanguage()` 全量刷新（含重建两个列表）

## 5. 分期与工作量

| 期 | 内容 | 预估 |
| --- | --- | --- |
| **一期（本次）** | `Lang.h/.cpp` + 语言判定 + 持久化 + ZH/EN 按钮 + UI 29 + MainWindow 71 + ErrorMap 38 | ~150 处改动 |
| 二期（可选） | `CliMain.cpp` 57 条 + `Log` 审计行 | ~60 处 |

## 6. 风险

| 风险 | 应对 |
| --- | --- |
| 漏翻导致中英混排 | 两张表同序 + `static_assert` 长度相等；`T()` 对未翻译项返回中文兜底 |
| 切换语言后列表里的旧文案不刷新 | `ApplyLanguage()` 末尾强制 `RebuildFileList()` / `RebuildProcessList()` |
| 中英文长度差异撑破布局 | 英文普遍更长，`TextBlock` 已有 `TextTrimming`；窄布局 <980 DIP 时按钮退化为纯图标，不受影响 |
| 标题栏按钮挡住拖拽 | 只占右上角一小块；其余区域仍是 `TitleBarGrid` 的拖拽区 |
| 一处全局状态被多线程读 | `std::atomic<Lang>`，写入只在 UI 线程 |

## 7. 验收

- [ ] 系统语言 zh-CN / zh-TW / zh-HK 启动 → 中文；en-US / ja-JP 启动 → 英文
- [ ] 点 ZH/EN → 立即全量切换，重启后保持
- [ ] 列表徽章、状态栏、错误对话框、空状态提示都跟着变
- [ ] 中英双语下窄布局（820×560 DIP）都不裁字
- [ ] `policy_tests` / `ipc_tests` 仍全绿（core 改了 ErrorMap，需要回归）
