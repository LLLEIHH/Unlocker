# Unlocker 复刻执行方案

> 目标：用 C++ / Win32 复刻 IObit Unlocker 的用户态能力（拖入/选择文件 → 显示占用进程与占用状态 → 解锁 → 删除/改名/移动/复制 + 右键菜单 + 内置黑名单）
>
> 范围：**纯用户态，不写内核驱动**，删不掉的走重启兜底
>
> 版本：**v0.8** / 2026-10-22（已回退 v0.9 的范围收窄，恢复解锁后的文件操作能力）
>
> v0.2：新增 F15 内置黑名单（三级裁决 + 路径/文件ID 双表匹配 + fail-closed 自检），§4.6 重写，补 T17-T24
>
> v0.3：新增 F16 拖入 / F17 内置文件选择，§4.7 重写，补 T25-T31
>
> v0.4：确定方案 B —— 主程序 asInvoker，提权 worker 独立进程。重写 §4.1 权限模型、新增 §4.10 IPC 协议、里程碑拆为 8 阶段（新增 M4），补 T32-T38
>
> v0.5：新增 F19 占用详情展示（占用状态判定 + 进程身份补全），§4.2 补三节、§4.7 补展示设计，补 T39-T46
>
> **v0.8：第二轮评审 —— [修] 管道 DACL 同时放行 UI 侧用户 SID（跨用户提权时 worker 跑在管理员账户，只放行自己会让标准用户 UI 连不上）；[修] BL-003-006/020 补齐 `<Windows>\` 前缀（裸 `System32\...` 对全路径做全等比较永远匹配不上，规则静默失效）；[修] Confirm 统一由 worker 弹框，消除重复确认；[修] 越权边界措辞（三重校验挡冒充、不挡 UI 被注入）；[修] DELETE 重试归 SeRestorePrivilege；[补] L1 遇 AccessDenied 的处理、`hello` 版本握手、T50 双提示；[正] `Global\` 互斥体不需要 SeCreateGlobalPrivilege**

---

## 0. 一页速览

| 项目      | 结论                                                                                             |
| ------- | ---------------------------------------------------------------------------------------------- |
| 技术栈     | C++20 / MSVC / Win32 API + 少量 NTAPI（只做查询，不做危险操作）                                               |
| UI      | Win32 原生窗口 + ListView（不引入 Qt，减少体积和签名麻烦）                                                        |
| 权限模型    | **双进程（方案 B）**：主程序 `asInvoker`，提权 worker 按需 `runas` 启动，命名管道传 JSON                               |
| 扫描分级    | **两级**：L1 在 UI 进程内免提权扫（覆盖同 IL 进程），只有深扫与写操作才提权。绝大多数操作不弹 UAC                                     |
| 驱动      | **不做。** 代价：内核级锁定（杀软 minifilter、PPL 进程）无法破解                                                     |
| 交付物     | `Unlocker.exe`（x64 + x86，同一 exe 兼具 UI / worker / CLI 三种身份）、`UnlockerShell.dll`（右键菜单，x64 + x86） |
| 黑名单     | **编译进二进制**的三级规则表（Deny/Confirm/Allow），硬禁无开关、不可关闭；**worker 端独立重校验**                              |
| 里程碑     | M0 → M7，共 8 个阶段，约 22.5 人日                                                                      |
| 最大风险    | **提权 worker 被当作跳板**（方案 B 的头号风险）/ 误杀关键进程导致蓝屏 / 强关句柄导致目标进程崩溃 / 杀软误报                              |
| 不可突破的边界 | 共享冲突是 CreateFile 阶段内核判定的，用户态只有两条路：关掉对方句柄，或终止对方进程                                               |

**先看清一件事**：所谓"解锁"没有 API 可以直接调用。Windows 里只有"谁持有句柄"和"怎么把那个句柄弄掉"两件事。所有同类工具的差别，只在于**发现得准不准**和**拆得狠不狠**。

---

## 1. 范围定义

### 1.1 要做

- 找出占用指定文件/目录的所有进程（PID、进程名、句柄值）
- **主程序以普通权限运行**，只在真正执行解锁动作时提权（保证拖放、对话框、右键菜单全部原生可用）
- **拖入文件/文件夹即可解锁**（支持多选、混合拖入）
- **软件内点击按钮选择文件或文件夹**（多选、记住上次目录）
- 在**不终止进程**的前提下释放普通文件句柄
- 解锁后立即执行：删除 / 重命名 / 移动 / 复制
- 强制模式：终止所有关联进程后再操作（带白名单保护）
- **内置黑名单：系统关键文件禁止解锁，硬禁项无法被任何参数或开关绕过**
- 删不掉时排队到重启执行
- 资源管理器右键菜单入口
- 完整命令行接口，便于脚本化

### 1.2 不做（以及为什么）

| 不做                          | 原因                                                  |
| --------------------------- | --------------------------------------------------- |
| 内核驱动读句柄名                    | 需要 EV 证书 + 微软签名，HVCI 开启时可能被拦；收益只在极少数场景              |
| 绕过杀软 minifilter             | 属于对抗安全软件，法律和稳定性双重风险                                 |
| 注入 WOW64 跨位数卸载 DLL          | 收益低（直接杀进程更省事），复杂度极高                                 |
| 破解 PPL 保护进程                 | 系统不给你权限，硬来就是 BSOD                                   |
| "恢复已删除文件"                   | 与解锁无关，另一个产品                                         |
| 常驻 Windows 服务 / 开机自启 broker | 服务形态会让"提权执行任意删除"变成永久后门，远超本工具所需；worker 只在用户主动操作时短暂存在 |
| 记忆提权状态（免 UAC）               | 客观上是 UAC 绕过，杀软必报，绝对不做                               |

### 1.3 合规红线

- 只用于**本机自有文件**的清理，不做远程/批量扫描他机
- 不提供"破解被保护文件"的宣传文案，避免被归类为恶意工具
- 提权 worker **只接受固定操作枚举**，不存在"帮我执行某条命令"这类接口
- worker 生命周期与单次操作绑定（空闲 5 分钟自退），不驻留、不开后门、不联网上报

---

## 2. 功能矩阵

| #   | 功能              | 优先级 | 依赖的系统机制                                                       | 验收标准                                                               |
| --- | --------------- | --- | ------------------------------------------------------------- | ------------------------------------------------------------------ |
| F1  | 扫描占用进程          | P0  | `NtQuerySystemInformation(64)` + `NtQueryObject`              | 对记事本打开的 txt，1.5s 内列出 PID                                           |
| F2  | 关闭远程句柄          | P0  | `OpenProcess` + `DuplicateHandle(DUPLICATE_CLOSE_SOURCE)`     | 关闭后 explorer 能直接删除                                                 |
| F3  | 解锁并删除           | P0  | Level 2 拆锁 + `FileDispositionInfoEx`                          | 100KB~2GB 文件均成功                                                    |
| F4  | 解锁并重命名          | P0  | `SetFileInformationByHandle(FileRenameInfo)`                  | 同目录改名成功，占用者不崩                                                      |
| F5  | 解锁并移动           | P0  | `MoveFileEx` / `FileRenameInfo`                               | 同卷成功；跨卷降级为复制+删                                                     |
| F6  | 解锁并复制           | P1  | `ReadFile` 循环 + 保留时间戳/属性                                      | 复制 MD5 一致                                                          |
| F7  | 强制模式（杀进程）       | P0  | `TerminateProcess`                                            | 白名单进程拒绝执行并提示                                                       |
| F8  | 重启后删除/改名        | P0  | `MoveFileEx(MOVEFILE_DELAY_UNTIL_REBOOT)`                     | 重启后文件消失，注册表项已清理                                                    |
| F9  | 右键菜单            | P1  | `IContextMenu` + `IShellExtInit`                              | 文件/文件夹/多选均出现菜单项                                                    |
| F10 | 命令行             | P1  | 同一 exe 的参数分支                                                  | 退出码可判定，日志可追溯                                                       |
| F11 | 多文件批量           | P1  | 列表模型                                                          | 20 个文件一次处理，失败项单独标记                                                 |
| F12 | POSIX 语义删除      | P1  | `FileDispositionInfoEx` (21)                                  | 只读文件、假性占用可秒删                                                       |
| F13 | 卸映像（DLL 注入）     | P2  | `CreateRemoteThread` + `FreeLibrary`                          | 仅同位数；失败降级到 F7                                                      |
| F14 | 挂起进程后关句柄        | P2  | `NtSuspendProcess`                                            | 降低目标进程崩溃概率                                                         |
| F15 | 内置黑名单           | P0  | 编译期规则表 + 路径归一化 + `FILE_ID_INFO`                               | 对 `ntoskrnl.exe`/drivers 目录/注册表 hive 一律 Deny；7 种绕过写法 + 1 个误伤反例全部覆盖 |
| F16 | 拖入文件解锁          | P0  | `DragAcceptFiles` + `WM_DROPFILES`（UI 中完整性，无需 UIPI 补丁）        | 单文件/多选/混合/文件夹/快捷方式全部正确入列表                                          |
| F17 | 软件内选择文件         | P0  | `IFileOpenDialog`（`FOS_ALLOWMULTISELECT` / `FOS_PICKFOLDERS`） | 两个按钮分别选文件和文件夹；记住上次目录；虚拟位置不崩                                        |
| F18 | 提权 worker + IPC | P0  | `ShellExecuteEx(runas)` + 命名管道（带 SACL）+ 长度前缀 JSON             | UI 全程普通权限；worker 端独立重校验；客户端身份强制校验                                  |
| F19 | 占用者详情展示         | P0  | 状态探测 + 版本资源 + `SHGetFileInfoW`                                | 显示进程名/PID/描述/句柄数/8 种占用状态；受保护进程给出"无法检查"提示                           |
| F20 | 两级扫描            | P0  | L1 在 UI 内跑 `HandleScanner`，L2 交给 worker                       | 普通文件拖入**不弹 UAC**；遇高 IL 进程时提示"需要管理员深扫"并可一键提权                        |

## 3. 架构与目录结构

```
Unlocker/
├─ src/
│  ├─ main.cpp                 // 入口分发：--worker -> WorkerMain；有路径参数 -> CLI；否则 GUI
│  ├─ core/                    // 纯逻辑，无 UI，UI 与 worker 两个进程共享
│  │  ├─ Privilege.cpp/.h      // 令牌特权调整（只对 worker 生效）
│  │  ├─ HandleScanner.cpp/.h  // 句柄枚举（F1）
│  │  ├─ PathResolver.cpp/.h   // 设备路径 <-> DOS 路径归一化
│  │  ├─ LockBreaker.cpp/.h    // 分级拆锁（F2/F7/F12/F13/F14）
│  │  ├─ FileOps.cpp/.h        // 删除/改名/移动/复制（F3-F6）
│  │  ├─ RebootScheduler.cpp/.h// 重启兜底（F8）
│  │  ├─ PolicyGuard.cpp/.h    // 黑名单裁决引擎（F15）
│  │  └─ rules/
│  │     ├─ Blacklist.def      // 规则单一数据源（X-Macro）
│  │     └─ BlacklistTable.h   // 展开成枚举 + 表，含 CRC32 自检
│  ├─ ipc/
│  │  ├─ Protocol.h            // 消息枚举 + JSON 结构（两端唯一真源）
│  │  ├─ PipeClient.cpp/.h     // UI 侧：拉起 worker、发请求、读进度
│  │  ├─ PipeServer.cpp/.h     // worker 侧：建管道、鉴权、分发
│  │  └─ WorkerMain.cpp        // --worker 入口，含重校验与空闲退出
│  ├─ ui/
│  │  ├─ MainWindow.cpp/.h     // 主窗口 + 列表 + 唯一入口 AddPaths()
│  │  ├─ DropHandler.cpp/.h    // 拖放接收（F16）
│  │  ├─ FilePicker.cpp/.h     // IFileOpenDialog 封装（F17）
│  │  ├─ ElevationCheck.cpp/.h // 启动时检测是否被手动提权（B7 兜底）
│  │  └─ Resource.rc
│  ├─ shell/
│  │  ├─ ContextMenu.cpp       // IContextMenu 实现
│  │  └─ ClassFactory.cpp
│  └─ common/
│     ├─ NtApi.h               // NTAPI 声明与结构体
│     ├─ Handles.h             // RAII 句柄封装
│     ├─ Log.cpp/.h            // 环形日志
│     └─ ErrorMap.cpp/.h       // NTSTATUS / Win32 -> 中文提示
├─ MakeLock/                   // 造锁工具，独立小工程
└─ Unlocker-Execution-Plan.md
```

**分层原则**：`core` 不依赖 UI，也不依赖 IPC——它只是"给我路径，我返回结果"。同一个 `core`，被 worker 进程调用就具备管理员权限，被 CLI 直接调用（已提权运行时）也一样跑。`ipc` 只负责搬运，不写业务判断。

> 这条边界很重要：**能复用的不是"代码"，是"进程"**。`core` 放在哪一端都行，但权限判定必须在 worker 端。

---

## 4. 核心模块设计

### 4.1 权限与进程模型（方案 B，第一步就得对）

```xml

<requestedExecutionLevel level="asInvoker" uiAccess="false"/>
```

```
Unlocker.exe（asInvoker，中完整性，始终）
├─ 拖入 / 选择器 / 右键菜单      -> 收集路径
├─ L1 扫描（本进程内，免提权）    -> 覆盖同 IL 进程，不弹 UAC
└─ 需要「深扫」或「写操作」时
        │ ShellExecuteEx(runas，在后台线程)
        ▼
   Unlocker.exe --worker（高完整性）
   ├─ 启特权
   ├─ L2 深扫（含受保护进程）
   └─ 重校验 -> 裁决 -> 执行（全部危险动作只发生在这里）
        │
        └── 命名管道（JSON）──> 结果回到 UI 渲染
```

**读不需要权限，写才需要。** 这是 v0.7 的核心调整：句柄表枚举本身不需要管理员，只有 `OpenProcess` 打开高完整性/PPL 进程时才需要 `SeDebugPrivilege`。所以扫描做两级，把 UAC 从"每次拖入都弹"降到"绝大多数情况不弹"。

**为什么这么绕**：如果主程序自己 `requireAdministrator`，它会变成高完整性进程，而资源管理器是中完整性——UIPI 会拦掉拖放（详见 4.7.5）。把 UI 留在中完整性，拖放、`IFileOpenDialog`、Shell 扩展全部原生可用，只有真正动文件的那一下才提权。

**代价**：写操作要弹一次 UAC。但因为扫描分两级（4.2 / F20），只是拖入看占用者的话**根本不会提权**——UAC 只在你真正要动文件时出现。次数控制见 4.10.6。

```cpp
// 只在 worker 进程里调用
void EnablePrivileges() {
    const wchar_t* list[] = { SE_DEBUG_NAME, SE_BACKUP_NAME, SE_RESTORE_NAME };
    // AdjustTokenPrivileges 循环；注意必须检查 GetLastError()==ERROR_NOT_ALL_ASSIGNED
}
```

**坑**：`AdjustTokenPrivileges` 即使在特权不存在时也返回 `TRUE`，必须检查 `GetLastError()`。

**两个特权的分工别记混**：


| 特权                   | 豁免的访问                                         | 典型用途               |
| -------------------- | --------------------------------------------- | ------------------ |
| `SeBackupPrivilege`  | **读**（`GENERIC_READ`、`FILE_LIST_DIRECTORY` 等） | 备份工具绕过 ACL 读文件     |
| `SeRestorePrivilege` | **写 / DELETE**（`GENERIC_WRITE`、`DELETE` 等）    | 还原工具绕过 ACL 改文件、删文件 |

`ACCESS_DENIED` 之后的重试必须提 **SeRestore**，因为我们要的是 `DELETE` 访问。写成 `SeBackup` 在 worker 里不会挂（两个都启了），但只要有人把这段挪到 UI 侧单独启用，就会"看起来提权了、还是打不开"。少了这一步，后面 `OpenProcess` 会静默失败一大批进程。

**身份判定**：同一个 exe 三种身份，`main.cpp` 第一件事就是分流。

```cpp
int wmain(int argc, wchar_t** argv) {
    if (HasFlag(argc, argv, L"--worker")) return WorkerMain(argc, argv);  // 内部协议，不对用户公开
    if (argc > 1)                        return CliMain(argc, argv);
    return GuiMain();
}
```

> `--worker` 不是给用户用的参数。worker 启动时必须校验命令行携带的管道名与 128 位随机令牌，缺失或格式错误直接退出——不要把 worker 做成一个"传入路径就干活"的裸命令行工具，那就是个 LOLBin。


### 4.2 句柄扫描器 HandleScanner（F1，整个项目的技术核心）

#### 步骤

1. `NtQuerySystemInformation(SystemExtendedHandleInformation /*64*/, buf, size, &need)`
   - 缓冲区初始 1 MB，返回 `STATUS_INFO_LENGTH_MISMATCH` 时按 `need + 64KB` 重试，最多 8 次
   - 结构体：`SYSTEM_HANDLE_INFORMATION_EX` + `SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX[]`
2. **按类型过滤**：只保留 `ObjectTypeIndex == g_fileTypeIndex`
3. 按 PID 分组，多线程处理
4. 每个进程：
   - `OpenProcess(PROCESS_DUP_HANDLE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)`，失败就跳过（记录原因）
   - `DuplicateHandle(hProc, hRemote, GetCurrentProcess(), &hDup, 0, FALSE, 0)`（注意这里**不要** DUPLICATE_CLOSE_SOURCE，只是来看名字）
   - `NtQueryObject(hDup, ObjectNameInformation, ...)` 取名
   - `CloseHandle(hDup)`
5. 名字归一化后与目标路径比较，命中则记录

#### 三个必须处理的坑

**坑 1：ObjectTypeIndex 每次开机都变**

不能硬编码。运行时探测：

```cpp
// 打开自身 exe，在句柄表里找 (pid == self && handle == hSelf)，取其 ObjectTypeIndex
HANDLE hSelf = CreateFileW(selfPath, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, ...);
// 扫描一次句柄表，匹配 pid+handle，得到索引
```

**坑 2：`NtQueryObject(ObjectNameInformation)` 对管道/同步对象会永久阻塞**

一旦卡住，整个工具界面就死了。必须放进独立线程 + 超时：

```cpp
bool QueryHandleName(HANDLE hDup, std::wstring& out) {
    struct Ctx { HANDLE h; std::wstring s; bool ok; } ctx{ hDup, {}, false };
    HANDLE th = CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
        auto* c = static_cast<Ctx*>(p);
        std::vector<BYTE> b(0x8000);
        ULONG ret = 0;
        if (NT_SUCCESS(NtQueryObject(c->h, ObjectNameInformation, b.data(), (ULONG)b.size(), &ret))) {
            auto* us = reinterpret_cast<PUNICODE_STRING>(b.data());
            if (us->Buffer && us->Length) { c->s.assign(us->Buffer, us->Length / 2); c->ok = true; }
        }
        return 0;
    }, &ctx, 0, nullptr);

    if (WaitForSingleObject(th, 100) == WAIT_TIMEOUT) {   // 100ms 上限
        TerminateThread(th, 0);                            // 这里泄漏栈是可接受的代价
        CloseHandle(th);
        return false;
    }
    CloseHandle(th);
    return ctx.ok;
}
```

> 注意 `TerminateThread` 的边界：线程若卡在**不可中断的内核等待**里，这个调用不会生效，线程会永久挂着（每个卡住的管道句柄吃掉一个线程 + 8KB 栈）。实测中绝大多数管道对象是可终止的，但要有兜底：连续失败 20 次就提示用户重启工具，别让线程池被慢慢抽干。

**坑 3：Session 0 与受保护进程**

- 服务进程需要 `SeDebugPrivilege` 才能 `OpenProcess`
- `System(4)` / `Registry` / PPL 进程（如 `csrss.exe`、部分杀软）`OpenProcess` 直接返回 `ERROR_ACCESS_DENIED`：**跳过的是"该进程的句柄名解析"，不是"这条占用线索"**。计数要留着，界面上按 4.2 末节如实提示"存在 N 个无法检查的进程"，不要重试、不要弹错

#### 占用状态判定（F19）：只说"被 xxx 占用"是不够的

用户真正关心的是"我到底能不能删"。所以扫描输出不该是一张 PID 列表，而是一个**状态**。

| 状态               | 判定方式                                                                                       | 用户看到的含义                       | 建议动作        |
| ---------------- | ------------------------------------------------------------------------------------------ | ----------------------------- | ----------- |
| `Free`           | `CreateFile(DELETE)` 成功，且枚举无持有者                                                            | 没人占用                          | 直接操作        |
| `Opened`         | 打开成功，但枚举到持有者                                                                               | 被打开了，但持有者允许删除                 | 直接删，多半能成    |
| `Locked`         | 打开返回 `ERROR_SHARING_VIOLATION`                                                             | 有人不允许删除                       | 拆锁后操作       |
| `ImageMapped`    | 目标路径出现在某个进程的已加载模块列表中（`CreateToolhelp32Snapshot(TH32CS_SNAPMODULE)` / `EnumProcessModules`） | 映像被映射，关句柄也删不掉                 | 只能杀进程 / 重启  |
| `AccessDenied`   | 启用 `SeRestorePrivilege` 重试后仍 `ERROR_ACCESS_DENIED`                                         | 权限或 ACL 问题，不是占用               | 提示改 ACL     |
| `LockedByKernel` | 删不掉，但枚举不到任何持有者                                                                             | 杀软过滤驱动 / 驱动锁 / delete-pending | 只能重启删除，如实说  |
| `NotFound`       | 打开返回 `ERROR_FILE_NOT_FOUND` / `PATH_NOT_FOUND`                                             | 文件已经不在了                       | 刷新列表，直接移除该项 |
| `Error`          | 其它任何打开失败                                                                                   | 路径非法 / 设备未就绪 / 未知错误           | 显示原始错误码，不猜  |

```cpp
OccupancyState Probe(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), DELETE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                           nullptr);
    if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); return OccupancyState::Opened; }

    switch (GetLastError()) {
        case ERROR_SHARING_VIOLATION: return OccupancyState::Locked;
        case ERROR_ACCESS_DENIED:     return OccupancyState::AccessDenied;  // 提 SeRestore 后重试一次
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:    return OccupancyState::NotFound;      // 绝不折算成"可操作"
        case ERROR_INVALID_NAME:
        case ERROR_BAD_PATHNAME:
        case ERROR_INVALID_PARAMETER: return OccupancyState::Error;
        default:                      return OccupancyState::Error;         // 未知失败一律从严
    }
}
```

**`default` 绝对不能返回 `Free`**：文件不存在、路径非法、盘符未就绪都会落到 `default`，界面就会显示"可操作"，用户点下去才发现根本找不到文件。未知失败必须归到 `Error`，让上层如实报错。

> **别用 `GrantedAccess` 判断映像映射**：文件句柄的访问掩码里只有 `FILE_*` 系列权限，不存在 `SECTION_MAP_EXECUTE`（那是 Section 对象句柄的权限）。想知道"这个 dll 是否被某个进程加载着"，唯一可靠的办法是去查那个进程的模块列表。

`Free` 与 `Opened` 的最终区分要配合枚举结果：打开成功 + 枚举为空 = `Free`；打开成功 + 有持有者 = `Opened`。`NotFound` / `Error` 是**终止态**，不进解锁流程，直接显示"文件不存在"/"无法访问"。

> **注意 `FILE_FLAG_OPEN_REPARSE_POINT` 在 Probe 里是必须的，在归一化里是致命的**：这里要删的是链接本身（不是它指向的目标），所以必须打开重解析点；而黑名单归一化要的恰恰相反——必须跟到真身。两处语义相反，别复制粘贴（详见 4.6.4）。

**判定顺序不能反**：先试打开，再枚举。反过来的话，会把"只是被打开、其实能删"的文件误报成"锁定"，用户白紧张一场。很多用户都以为"有进程打开就不能删"，实际上持有者带了 `FILE_SHARE_DELETE` 照样能删——这一栏的价值就是把这个区别讲清楚。

#### 进程身份补全：从 PID 到"人话"

只给 PID 等于没说。每个占用者补全成：

```cpp
struct ProcessInfo {
    DWORD         pid;
    std::wstring  name;          // notepad.exe
    std::wstring  fullPath;      // QueryFullProcessImageNameW
    std::wstring  description;   // 版本资源 FileDescription："记事本"
    std::wstring  company;       // CompanyName："Microsoft Corporation"
    bool          elevated;
    bool          protectedProc; // OpenProcess 失败
    HICON         icon = nullptr;
};
```

| 字段                    | API                                                                  | 坑                                           |
| --------------------- | -------------------------------------------------------------------- | ------------------------------------------- |
| fullPath              | `QueryFullProcessImageNameW`                                         | 失败退回 `GetModuleFileNameExW`                 |
| description / company | `GetFileVersionInfoSizeW` → `GetFileVersionInfoW` → `VerQueryValueW` | **语言块必须回退**：`040904B0` → `080404B0` → 遍历所有块 |
| icon                  | `SHGetFileInfoW(SHGFI_ICON \| SHGFI_SMALLICON)`                      | 用完 `DestroyIcon`，否则 GDI 句柄泄漏                |
| elevated              | `OpenProcessToken` → `TokenElevation`                                | 不影响能否解锁，但影响提示语气                             |

**语言回退是必须的**：中文系统上大量程序的版本资源只有 `080404B0` 块，直接查 `040904B0` 会拿到空字符串，界面上就会出现"描述："后面空空如也。

**性能**：遍历全部进程读版本资源，200 个约 400ms。但这里只需要补全**实际占用者**（通常 1-5 个），成本可忽略。**不要为了"以后可能用得上"而全量预加载**——这是最容易在后期变成"打开就卡两秒"的自作聪明。

#### 查不到的时候要如实说，但不能编

这里有一个容易想当然的地方，必须写清楚：**`NtQuerySystemInformation` 返回的是全系统的句柄条目，所以你确实"知道" `System(4)` 有 N 个文件句柄；但要把句柄解析成路径，必须 `OpenProcess(PROCESS_DUP_HANDLE)` + `DuplicateHandle`，对受保护进程这一步是失败的。**

结论：**你无法确定那个句柄指向的是不是你的目标文件。** 所以不能显示

```
System (PID 4)        [占用了此文件]      ← 这是编的，做不到
```

正确的做法是分成两种情形：

| 情形                                               | 你能确定的             | 界面写法                                     |
| ------------------------------------------------ | ----------------- | ---------------------------------------- |
| 能枚举到句柄，但 `QueryFullProcessImageNameW` 失败（拿不到进程名） | 该 PID 确实持有目标文件的句柄 | 占位行：`PID 812 [无法读取进程信息]` + 句柄数           |
| `OpenProcess` 就失败，整个进程的句柄都没解析                    | 只知道"有 N 个无法检查的进程" | 列表底部汇总一行：`另有 3 个受保护进程无法检查（可能是系统进程或安全软件）` |

这样写用户既不会误以为"没人占用"，也不会被"是 System 占的"这种假结论误导去杀进程。

`LockedByKernel` 同理，要给出可执行的解释："未找到持有进程，可能由安全软件或文件系统驱动锁定，建议使用重启后删除。" 这是**推测**，措辞里要带"可能"。

**自我排除**：扫描前不要长期持有目标句柄（`Probe` 的句柄用完即关），枚举结果里过滤掉**自己和父进程**：

```cpp
// 只排自己不够：L1 在 UI 里跑，而 UI 做归一化时会短暂打开目标文件
std::set<DWORD> selfPids = { GetCurrentProcessId() };
// worker 侧再排掉父进程（就是 UI）
DWORD parent = ParentPidOf(GetCurrentProcessId());
if (parent) selfPids.insert(parent);
```

否则会出现"Unlocker 被 Unlocker 占用"这种笑话，而且用户会当成 bug 报上来。两级扫描之后这条更要紧——UI 自己就在扫、自己就在开文件。

#### 性能预算

| 场景           | 句柄总数  | 文件句柄 | 目标耗时     |
| ------------ | ----- | ---- | -------- |
| 轻载笔记本        | ~60k  | ~8k  | < 600 ms |
| 开发机（IDE+浏览器） | ~150k | ~25k | < 1.5 s  |
| 服务器          | ~400k | ~60k | < 3 s    |

达成手段：① 类型过滤砍掉 85%；② 8 线程按进程并行；③ 命中即早退（找的是特定路径，不必扫完）。

#### 两级扫描（F20）：能不提权就不提权

句柄表枚举本身**不需要管理员**——`NtQuerySystemInformation` 谁都能调。真正需要 `SeDebugPrivilege` 的只有 `OpenProcess` 打开高完整性 / PPL 进程那一步。所以扫描分两级：

| 级别     | 在哪跑    | 权限   | 能看到什么              | 代价            |
| ------ | ------ | ---- | ------------------ | ------------- |
| **L1** | UI 进程内 | 中完整性 | 同 IL 及更低 IL 进程的句柄名 | 零 UAC，约 1s    |
| **L2** | worker | 高完整性 | 全部，含管理员程序、服务、PPL   | 一次 UAC（会话内复用） |

调用策略：

1. 拖入即跑 **L1**，不弹 UAC
2. L1 里 `OpenProcess` 失败的进程计入 `skippedPids`，**不当作"没人占用"**
3. L1 的 `Probe` 返回 `AccessDenied` 时**不要重试**——中完整性进程手里没有 `SeRestorePrivilege`，重试一百次也是同一个结果。直接给出"需要提权"的提示，和深扫入口合并成一个动作
4. 若 L1 结论是 `Locked` 但持有者列表为空（只可能是被高 IL 进程占着），界面提示"需要管理员深扫"并给按钮
5. 用户点深扫，或执行任何**写操作** → 提权起 worker，由 worker 做 L2

**L1 的边界必须写在界面上**，不能只写在文档里：

```
未找到持有进程
该文件可能被管理员权限的程序或系统服务占用。
[以管理员身份深扫]
```

否则用户看到"没人占用"却删不掉，只会认为软件坏了。

> **写操作为什么仍然一律走 worker？** 因为中完整性进程本来就有权删用户自己的文件，理论上"就地删"能省掉大部分 UAC。但那样会出现两条写路径（UI 内一条、worker 里一条），黑名单、审计、错误映射都要维护两遍，测试面翻倍。为了省一次 UAC 付出长期维护成本，不划算。**读可以分叉，写必须单一路径。**

### 4.3 路径归一化 PathResolver

这是最容易出 bug 的地方——`\Device\HarddiskVolume3\Users\...` 和 `C:\Users\...` 是同一个文件。

```cpp
std::wstring Normalize(const std::wstring& raw) {
    // 1) 去掉 \\?\ / \??\ 前缀
    // 2) 设备路径替换：枚举 A-Z 调 QueryDosDeviceW 建 map<\Device\HarddiskVolumeN, C:>
    // 3) 卷 GUID 路径：GetVolumeNameForVolumeMountPoint 反查
    // 4) GetLongPathNameW 展开 8.3 短名
    // 5) 去尾部反斜杠、统一大写（仅用于比较，回显用原样）
}
bool PathEqual(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(Normalize(a).c_str(), -1, Normalize(b).c_str(), -1, TRUE) == CSTR_EQUAL;
}
```

**额外坑**：符号链接/联接点。如果目标是 `C:\link\a.txt`，实际文件可能在 `D:\real\a.txt`。用 `GetFinalPathNameByHandleW` 对目标先求一次真身，且**两个路径都要参与比较**。

### 4.4 锁破除 LockBreaker（分级策略）

按"副作用从小到大"逐级尝试，任何一级成功就停：

```
Level 0  正常删除        直接删，多半是只读属性/ACL/目录非空，根本不是占用
Level 1  POSIX 语义删除   覆盖"假性占用"与只读属性
Level 2  关闭远程句柄     真正的解锁，主力手段
Level 3  卸载 DLL 映像    P2，可选
Level 4  终止进程        强制模式，带白名单
Level 5  重启后执行       兜底，必定成功
```

#### Level 1：POSIX 语义删除

```cpp
// FILE_DISPOSITION_INFO_EX（SDK 老版本没有就自己定义）
struct FILE_DISPOSITION_INFO_EX_ { DWORD Flags; };
// 标志位（以 winbase.h 为准）
// FILE_DISPOSITION_FLAG_DELETE                    0x00000001
// FILE_DISPOSITION_FLAG_POSIX_SEMANTICS           0x00000002
// FILE_DISPOSITION_FLAG_FORCE_IMAGE_SECTION_CHECK 0x00000004
// FILE_DISPOSITION_FLAG_ON_CLOSE                  0x00000008
// FILE_DISPOSITION_FLAG_IGNORE_READONLY_ATTRIBUTE 0x00000010  (较新版本)

bool PosixDelete(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(),
        DELETE | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;          // 真正的共享冲突在这里就挂了

    FILE_DISPOSITION_INFO_EX_ di{};
    di.Flags = 0x1 | 0x2 | 0x10;
    BOOL ok = SetFileInformationByHandle(h, FileDispositionInfoEx /*21*/, &di, sizeof(di));
    CloseHandle(h);
    return ok != FALSE;
}
```

> **务必理解的边界**：共享冲突是 `CreateFile` 阶段的判定，取决于对方句柄的 `dwShareMode` 有没有 `FILE_SHARE_DELETE`。POSIX 语义**不能**绕过它。它能解决的是：对方带了 `FILE_SHARE_DELETE`（索引器、部分杀软扫描器、被同进程打开过的文件）但普通删除仍然失败的情况，以及只读属性导致的 `ACCESS_DENIED`。写文档和文案时别把它吹成"万能删除"。

#### Level 2：关闭远程句柄（核心）

```cpp
bool CloseRemoteHandle(DWORD pid, HANDLE hRemote) {
    HANDLE hProc = OpenProcess(PROCESS_DUP_HANDLE, FALSE, pid);
    if (!hProc) return false;

    HANDLE dup = nullptr;
    // 关键：DUPLICATE_CLOSE_SOURCE —— 复制成功的同时源句柄被关闭
    // 注意 DUPLICATE_CLOSE_SOURCE 并不会"不创建目标句柄"：它只是顺带关掉源句柄，
    // 目标句柄照样在我们进程里生成，所以必须接住并 CloseHandle，否则自己泄漏一个。
    BOOL ok = DuplicateHandle(hProc, hRemote, GetCurrentProcess(), &dup,
                              0, FALSE, DUPLICATE_CLOSE_SOURCE);
    if (ok && dup != nullptr && dup != INVALID_HANDLE_VALUE) CloseHandle(dup);
    CloseHandle(hProc);
    return ok != FALSE;
}
```

**注意**：网上大量文章写的是"复制到自己进程再 `CloseHandle`，让原进程句柄失效"——**这是错的**。关掉副本只减少对象引用计数，源进程的句柄依然持有 `FILE_OBJECT`，文件照样锁着。必须有 `DUPLICATE_CLOSE_SOURCE`。

**必须循环**：同一进程可能对同一文件持有多个句柄（映射视图、备份句柄、子对象）。

```cpp
for (int round = 0; round < 3; ++round) {
    auto hits = scanner.FindByPath(target);     // 重新扫描
    if (hits.empty()) break;
    for (auto& h : hits) CloseRemoteHandle(h.proc.pid, h.handleValue);
    Sleep(80);                                   // 等 IRP 收尾，别用 0
}
```

#### Level 4：终止进程 + 白名单

```cpp
// 一档：绝对禁止，连确认入口都不给
const wchar_t* kHardDenyProc[] = {
    L"System", L"Registry", L"smss.exe", L"csrss.exe", L"wininit.exe",
    L"winlogon.exe", L"services.exe", L"lsass.exe", L"MsMpEng.exe"
};

// 二档：高危但常见，降为"需确认"
const wchar_t* kConfirmProc[] = { L"svchost.exe", L"explorer.exe" };
```

- **`svchost.exe` 不能进硬拒名单**。它承载 Windows Search、Windows Update、部分 Defender 组件，恰恰是"文件删不掉"最常见的元凶。一刀切禁止，工具在最典型的场景就废了。降为 Confirm：明确告诉用户"终止它会连带停掉同组服务"，让用户自己决定
- `explorer.exe` 同理，杀了会重启桌面，但用户经常需要它
- 命中一档：**拒绝执行并给出解释**，不是弹个"失败"
- 命中二档：列出该进程承载的服务/窗口标题，二次确认后才杀
- 未命中：`TerminateProcess` 前再检查是否是当前进程（自杀保护）
- 执行前必须让用户看到将终止的进程清单（强制模式也要给二次确认）

#### Level 5：重启兜底

```cpp
// 路径必须是完整 Win32 路径，且调用者具备 SeRestorePrivilege
MoveFileExW(target.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);            // 删除
MoveFileExW(target.c_str(), newPath.c_str(), MOVEFILE_DELAY_UNTIL_REBOOT);    // 改名/移动
```

底层写入：

```
HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\PendingFileRenameOperations  (REG_MULTI_SZ)
```

`smss.exe` 在用户态进程启动之前处理它，所以**必定成功**。这也是所有"重启后删除"提示的真相。

### 4.5 文件操作 FileOps

**递归删除时黑名单怎么生效**（必须定死，否则实现时各写各的）：

| 情形                      | 行为                                                 |
| ----------------------- | -------------------------------------------------- |
| 目录本身命中 Deny             | 整个操作拒绝，不做任何部分删除                                    |
| 目录本身 Allow，子项里有 Deny    | **跳过该子项**，其余照常删，结束时汇总报告"跳过 2 项（BL-001）"            |
| 目录本身 Allow，子项里有 Confirm | 一次性列出所有 Confirm 项，**由 worker 弹一次框**，确认后对整批生效，不逐项弹窗 |
| 删除过程中某项失败（非黑名单原因）       | 记录并继续，最后统一汇报；不中断整批                                 |

> **Confirm 的决定权只有一处：worker。** UI 侧（GUI、CLI、右键菜单）一律只负责"标注这一项需要确认"，不弹确认框、不收集 `confirmed` 标记。三个入口因此天然行为一致，也不会出现"UI 问一次、worker 又问一次"的重复打扰。

> 先遍历预检、再统一执行，不要边遍历边删。否则用户在确认框上看到"将删除 1200 个文件"时，前 300 个可能已经删掉了——那不是确认，是通知。

| 操作   | 首选 API                                              | 备注                                                 |
| ---- | --------------------------------------------------- | -------------------------------------------------- |
| 删除   | `SetFileInformationByHandle(FileDispositionInfoEx)` | 目录非空需递归                                            |
| 改名   | `SetFileInformationByHandle(FileRenameInfo)`        | 句柄需带 `DELETE` + `FILE_SHARE_DELETE`                |
| 移动同卷 | `FileRenameInfo`（相对句柄）                              | 比 `MoveFileEx` 更可靠                                 |
| 移动跨卷 | 复制 + 删源                                             | 被占用时只能走重启                                          |
| 复制   | `CopyFileExW`                                       | 失败再手写 `ReadFile` 循环（共享模式 `READ\|WRITE\|DELETE` 打开） |

复制时保留元数据：`GetFileTime` / `SetFileTime`、`GetFileAttributes`、`GetFileSecurity`→`SetFileSecurity`。


### 4.6 黑名单与安全护栏 PolicyGuard（F15）

设计前提：**不能指望用户在点"解锁并删除"之前想清楚那是不是 `ntoskrnl.exe`。** 刹车必须做在软件里，而且不能是个能被顺手关掉的复选框。

#### 4.6.1 三级裁决

| 裁决        | 含义       | UI 表现                             | 能否绕过                   |
| --------- | -------- | --------------------------------- | ---------------------- |
| `Deny`    | 硬禁，直接拒绝  | 红色提示条 + 规则 ID + 具体原因              | **不可**，无参数、无开关、无注册表项可关 |
| `Confirm` | 高危，需二次确认 | 黄色提示 + 状态列"需确认"（**UI 只标注，不收集决定**） | 可，但强制写审计日志             |
| `Allow`   | 放行       | 正常流程                              | —                      |

多规则命中时取最严格者：`Deny > Confirm > Allow`。**任一环节为 Deny，整条链路短路**——包括强制模式、CLI、右键菜单、重启兜底（重启兜底同样要过黑名单，否则就是个后门）。

#### 4.6.2 规则写在哪：编译进二进制

用户要求"软件内写入"，那就不放外部可编辑文件（一个 txt 规则表三分钟就能被改成全放行）。规则用 X-Macro 单一数据源，编译期展开成枚举 + 表：

```cpp
// src/core/rules/Blacklist.def
// 参数：规则ID, 匹配类型, 模式, 裁决, 严重度, 原因
BL_RULE(BL_001, MATCH_PREFIX, LR"(<Windows>\System32\drivers\)",        DENY,    CRIT, "驱动目录：关闭驱动文件句柄可能导致蓝屏")
BL_RULE(BL_002, MATCH_PREFIX, LR"(<Windows>\System32\config\)",         DENY,    CRIT, "注册表 hive 目录")
BL_RULE(BL_003, MATCH_EXACT,  LR"(<Windows>\System32\ntoskrnl.exe)",    DENY,    CRIT, "Windows 内核主文件")
BL_RULE(BL_016, MATCH_PREFIX, LR"(<Windows>\)",                          CONFIRM, HIGH, "系统目录下的文件，确认不是系统自身需要")
BL_RULE(BL_030, MATCH_PREFIX, LR"(<Windows>\Temp\)",                     ALLOW,   INFO, "临时目录显式放行，避免误伤")
```

```cpp
// src/core/rules/BlacklistTable.h
enum RuleId { RULE(BL_001), RULE(BL_002), ... };
static constexpr Rule kRules[] = { RULE(BL_001), RULE(BL_002), ... };
static constexpr DWORD kRulesCrc = 0x8F3A2C11;   // 编译期算好，启动时比对
```

要点：

- **占位符不写死**：`<Windows>` 启动时用 `GetWindowsDirectoryW` 展开，用户把系统装在 D 盘也正确
- **X-Macro 保证 ID 与数据永不脱节**：新增规则只改 `.def`，枚举、表、日志字符串自动同步
- **`constexpr` + 只读段**：规则表落在 `.rdata`，没有运行时改写入口

#### 4.6.3 双表匹配：路径 + 文件 ID

只按路径匹配有洞。硬链接能让 `C:\tmp\x.sys` 和 `C:\Windows\System32\drivers\x.sys` 指向**同一个文件**，路径完全不同。

所以启动时额外建一张**文件 ID 表**：把硬禁清单里的具体文件逐个打开，取 `GetFileInformationByHandleEx(FileIdInfo)` 的 `FILE_ID_INFO`（卷序列号 + 128 位 FileId），存进哈希集合。

```
verdict = Allow
canonical = CanonicalPath(handle)                 // 见 4.6.4，句柄真身
if 命中路径规则          -> verdict = 该规则裁决
if FILE_ID(canonical) ∈ 硬禁ID集 -> verdict = Deny
若 canonical 拿不到（文件被独占） -> verdict 至少收紧到 Confirm  // 从严：拿不到真身就按高危算
```

目录级规则（`drivers\`、`WinSxS\`）没法穷举 ID，只能靠前缀 + 归一化兜。

**八种匹配类型的语义**（规则表里用到哪些，这里就必须定义清楚，否则每个人实现出来的行为都不一样）：

| MATCH\_*      | 输入是什么          | 语义                                                                  | 典型规则                 |
| ------------- | -------------- | ------------------------------------------------------------------- | -------------------- |
| `EXACT`       | 归一化路径          | 完全相等（大小写不敏感）                                                        | BL-003 内核主文件         |
| `PREFIX`      | 归一化路径          | 以该串开头，**且必须在目录分隔符处切分**（`\Windows\System` 不能匹配 `\Windows\System32X`） | BL-001 驱动目录          |
| `GLOB`        | 归一化路径          | 仅支持 `*` 和 `?`，**只在最后一段**通配，不支持 `**`                                 | BL-005 `win32k*.sys` |
| `FILE_ID`     | `FILE_ID_INFO` | 卷序列号 + FileId 命中集合                                                  | BL-013 硬链接防护         |
| `VOLUME_ROOT` | 归一化路径          | 路径恰好是该卷根（`C:\` / `D:\`），不带尾部子项                                      | BL-007               |
| `VOLUME`      | 卷设备 + 分区类型     | 按 `IOCTL_DISK_GET_PARTITION_INFO_EX` 判定 EFI / 系统保留                  | BL-015               |
| `SELF`        | 进程自身           | 目标等于当前 exe 或其所在目录                                                   | BL-014               |
| `COUNT`       | 本次请求的对象数       | **不参与单路径裁决**，只在批量操作入口做一次前置检查                                        | BL-018               |

> `PREFIX` 的"分隔符切分"必须写进单测。少了它，`\Windows\System` 会把 `\Windows\System32X` 一起拦掉——这种 bug 在开发机上极难发现，到用户那儿就是"莫名其妙删不掉"。

`COUNT` 不参与单文件裁决，这个边界也要定死：否则会出现"只删 1 个文件却要求确认，因为列表里还有 500 个"这种荒谬提示。

#### 4.6.4 归一化：黑名单的命门

黑名单 90% 的漏洞都不在规则表，在归一化。必须走**句柄真身**，不能只对输入字符串做字符串处理：

```cpp
std::wstring CanonicalPath(HANDLE h) {
    // 1) GetFinalPathNameByHandleW(h, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS)
    //    -> 解开 junction / symlink / 8.3 短名，并给出 \\?\ 前缀形式
    // 2) 剥 \\?\ 与 \??\ 前缀
    // 3) GetLongPathNameW 二次展开
    // 4) 折叠 .. 与 .、清理尾随空格与点
    // 5) 统一大写（仅用于比较，回显时用原样）
}
```

对还没打开的目标路径，也要先拿到句柄再归一化，但**不要带 `FILE_FLAG_OPEN_REPARSE_POINT`**：

```cpp
// 黑名单归一化：跟随 reparse，拿到真身
HANDLE h = CreateFileW(raw.c_str(), FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       nullptr, OPEN_EXISTING,
                       FILE_FLAG_BACKUP_SEMANTICS,   // 注意：没有 OPEN_REPARSE_POINT
                       nullptr);
std::wstring target = CanonicalPath(h);   // junction 在这里被解开
```

**为什么这一处必须和 Probe 反着来**：带了 `FILE_FLAG_OPEN_REPARSE_POINT`，句柄指向的是**链接本身**，`GetFinalPathNameByHandleW` 只会原样返回链接路径（`C:\tmp\win\System32\ntoskrnl.exe`），永远不会解析到 `C:\Windows\...`。那 T21 就会落空——而 junction 恰好是最需要拦的绕过手法。Probe 要这个 flag（删的是链接），归一化不要（要的是真身），两处语义相反。

**两份路径都要参与匹配**：一份是用户传入的原始路径（他可能就是想删链接本身），一份是解开后的真身。任一命中 Deny 就拒绝，取最严格者。

打不开（被独占）就退化为 `GetFullPathNameW` + 逐段 `FSCTL_GET_REPARSE_POINT` 手工解析，并按上文规则**收紧为 Confirm**。

必须拦住的手法（每条都是真实攻击面，全部进测试矩阵）：

| 绕过写法                                           | 拦截机制                                                                  |
| ---------------------------------------------- | --------------------------------------------------------------------- |
| `C:\Windows\System32\..\System32\ntoskrnl.exe` | `GetFullPathNameW` 折叠 `..`                                            |
| `\\?\C:\Windows\...`、`\??\C:\...`              | 剥前缀                                                                   |
| `C:\WINDOWS\system32\NTOSKRNL.EXE`             | 大写后比较                                                                 |
| `C:\PROGRA~1\...`                              | `GetLongPathNameW` 展开                                                 |
| `ntoskrnl.exe.`、`ntoskrnl.exe `（尾随点/空格）        | 归一化清理                                                                 |
| `mklink /J C:\tmp\win C:\Windows`              | **不带** `OPEN_REPARSE_POINT` 打开后 `GetFinalPathNameByHandle` 解 junction |
| `mklink /H C:\tmp\k.exe ...\ntoskrnl.exe`（硬链接） | `FILE_ID_INFO` 比对                                                     |
| 用户自建 `D:\MyApp\System32\a.dll`                 | 前缀不匹配，**正确放行**（不能误伤）                                                  |

最后一行同样重要：黑名单过严会被用户骂"连我的项目文件夹都不让删"。

#### 4.6.5 进程侧黑名单

杀进程这条路同样要过规则表（见 4.4 Level 4）。判定不能只看进程名——名字随手就能改，要用 `QueryFullProcessImageNameW` 拿完整路径再过一次路径规则。`OpenProcess` 直接失败（PPL / 受保护进程）时视为 Deny，不要重试。当前进程自己永远 Deny。

#### 4.6.6 防篡改：fail-closed

- 启动时对 `kRules` 做 CRC32 自检，与 `kRulesCrc` 比对
- **校验失败 → 全表降级为 Deny**（fail-closed）。宁可工具不能用，也不能变成删系统文件的凶器
- **说清这个自检的局限**：它只能防"改了规则忘了改 CRC"这类手滑。有人同时 patch 规则和 CRC 常量，自检就形同虚设——真正的完整性只能靠**代码签名**保证（签名验不过，系统根本不会加载）。所以 CRC 自检的定位是"防手滑"，不是"防破解"，文案上别把它说成安全机制
- UI 不提供"关闭硬禁"的入口；`Confirm` 级可在设置里整体关闭，但需要管理员权限并写审计日志
- 预留企业规则包：外部签名规则**只能追加 Deny**，不能放宽内置规则（单向加严）

#### 4.6.7 拒绝时说什么

不要只说"操作被拒绝"。三段式，缺一不可：

```
BL-001 · 已阻止
C:\Windows\System32\drivers\dump_diskdump.sys

该文件位于系统驱动目录。强制关闭它的句柄可能导致系统蓝屏或驱动异常。
此规则为内置保护，无法关闭。
```

规则 ID 必须同时写进日志，用户反馈时能直接说"我卡在 BL-001"，排查成本从半小时降到 10 秒。

#### 4.6.8 审计日志

每次裁决都落一条：

```
2026-10-22 17:35:02 [DENY] rule=BL-001 target=C:\Windows\...\x.sys
    canonical=C:\WINDOWS\SYSTEM32\DRIVERS\X.SYS  op=UnlockAndDelete  caller=CLI
2026-10-22 17:35:10 [CONFIRM-YES] rule=BL-016 target=C:\Windows\System32\foo.dll pid=812
```

调用来源（GUI / CLI / Shell）要单独记，否则线上问题分不清是谁触发的。


### 4.7 GUI（Win32 + ListView）

```
┌──────────────────────────────────────────────────────────────┐
│ [添加文件] [添加文件夹]                        [ ] 强制模式    │
├──────────────────────────────────────────────────────────────┤
│                                                              │
│              把文件或文件夹拖到这里                            │
│              （空列表时的拖放区，虚线框）                       │
│                                                              │
├──────────────────────────────────────────────────────────────┤
│ 路径                        │ 大小  │ 占用者            │ 状态   │ ← 文件维度
│ C:\Temp\a.log               │ 12KB  │ notepad.exe (+2) │ ● 锁定 │
│ C:\Windows\System32\x.dll   │ 1.2MB │ —                │ ✕已阻止│
│ D://data//b.bin               │ 8MB   │ svchost.exe (+2)  │ ● 映射 │
├──────────────────────────────────────────────────────────────┤
│ 进程                        │ PID  │ 描述        │ 句柄 │ 状态  │ ← 进程维度
│   notepad.exe               │ 812  │ 记事本       │  ×3  │ 锁定  │
│   explorer.exe              │ 1532 │ Windows 资源…│  ×1  │ 打开  │
│   System                    │ 4    │ 受保护进程   │  1   │ 未知  │
├──────────────────────────────────────────────────────────────┤
│ [解锁] [解锁并删除▾] [解锁并改名] [解锁并移动]                 │
└──────────────────────────────────────────────────────────────┘
```

#### 4.7.1 三种输入入口

| 入口         | 触发                    | 优先级 | 说明                              |
| ---------- | --------------------- | --- | ------------------------------- |
| 拖入         | 资源管理器拖文件/文件夹到窗口       | P0  | 主入口，见 4.7.2                     |
| 软件内选择      | "添加文件"/"添加文件夹"按钮      | P0  | `IFileOpenDialog`，见 4.7.3       |
| 命令行 / 右键菜单 | `Unlocker.exe <path>` | P1  | 已有 F9/F10，复用同一条 `AddPaths()` 通路 |

**三个入口必须汇到同一个函数**：

```cpp
// 唯一的入口，所有来源最终都调它；去重、裁决、UI 刷新都只在这里做一次
void MainWindow::AddPaths(const std::vector<std::wstring>& raw);
```

这是防止"拖入的能拦、命令行的拦不住"这类不一致的唯一办法。

#### 4.7.2 拖入（F16）

**实现选择：`DragAcceptFiles` + `WM_DROPFILES`。**

方案 B 定案后，主窗口是中完整性进程，与资源管理器同级，**UIPI 完全不拦**，不需要任何 `ChangeWindowMessageFilterEx` 补丁。这是选 B 最直接的收益：拖放只剩"一句注册 + 一个消息处理"，且行为在所有 Windows 版本上一致。

```cpp
// 主窗口创建时，仅此一行
DragAcceptFiles(hwnd, TRUE);

// WM_DROPFILES 处理
case WM_DROPFILES: {
    HDROP h = (HDROP)wParam;
    UINT n = DragQueryFileW(h, 0xFFFFFFFF, nullptr, 0);
    if (n > kMaxDrop /*500，对应 BL-018*/) { /* 提示分批，不静默截断 */ }
    std::vector<std::wstring> paths;
    for (UINT i = 0; i < n; ++i) {
        UINT len = DragQueryFileW(h, i, nullptr, 0);
        std::wstring p(len + 1, L'\0');
        DragQueryFileW(h, i, p.data(), len + 1);
        p.resize(len);
        paths.push_back(p);
    }
    DragFinish(h);
    AddPaths(paths);       // 统一入口
    return 0;
}
```

> 如果哪天有人想把 UI 改回 `requireAdministrator`，这段代码会在 Win10/11 上直接静默失效（收不到 `WM_DROPFILES`），而且不报错、不崩溃——排查成本极高。**manifest 里那行 `asInvoker` 是功能依赖，不是风格选择**，建议在 `main.cpp` 顶部写注释钉死。

**用户手动"以管理员身份运行"怎么办**（这是真实会发生的事，不是边缘情况）：

manifest 不能阻止用户右键提权。一旦 UI 变成高完整性，拖放和右键菜单会像上面说的那样静默断掉。所以启动时必须自查：

```cpp
bool IsElevated() {
    HANDLE tok = nullptr;
    OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok);
    TOKEN_ELEVATION e{}; DWORD len = 0;
    GetTokenInformation(tok, TokenElevation, &e, sizeof(e), &len);
    CloseHandle(tok);
    return e.TokenIsElevated != 0;
}

// MainWindow 创建后立刻执行
if (IsElevated()) {
    // 1) 打补丁，让拖放还能用（此时是我们自己的窗口，风险可控）
    ChangeWindowMessageFilterEx(hwnd, WM_DROPFILES,               MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(hwnd, WM_COPYDATA,                MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(hwnd, 0x0049 /*WM_COPYGLOBALDATA*/, MSGFLT_ALLOW, nullptr);
    // 2) 顶部横幅说明现状，别让用户以为一切正常
    ShowBanner(L"当前以管理员权限运行。推荐以普通权限启动，拖放与右键菜单会更稳定。");
}
```

两条一起做：补丁保证"能用"，横幅保证"用户知道为什么可能不对劲"。只做补丁不留提示，出问题时没人想得起来是自己提权启动的。

**必须处理的输入边缘情况**：

| 拖入内容                      | 行为                                                         |
| ------------------------- | ---------------------------------------------------------- |
| 单个文件                      | 直接加入                                                       |
| 多选（文件+文件夹混合）              | 全部加入，文件夹标记为"目录"                                            |
| 文件夹                       | 加入列表本身，**不预展开**（可能几十万文件），解锁时按需递归 + 进度条                     |
| 快捷方式 `.lnk`               | `IShellLink` 解析出目标后加入；解析出的目标仍要过黑名单（防"建个快捷方式指向 ntoskrnl"绕过） |
| UNC 网络路径 `\\server\share` | 拒绝并说明：跨机器无法枚举对端句柄                                          |
| 压缩包内的虚拟文件                 | 拿不到真实路径，拒绝                                                 |
| 回收站里的项目                   | 拒绝（走系统还原，不走解锁）                                             |
| 已存在的重复项                   | 按 `CanonicalPath` 去重，不重复加入                                 |
| 拖入 0 个（空）                 | 忽略，不弹错                                                     |

**视觉反馈**：`WM_DROPFILES` 没有 DragEnter/DragLeave 事件，做不了"拖入时高亮"。折中做法是空列表时在窗口中央画一个静态虚线框 + 文案（`WM_PAINT` 自绘），有列表后隐藏。够用，也省掉 `IDropTarget` 的复杂度。

#### 4.7.3 软件内选择（F17）

两个按钮，不要试图用一个对话框同时选文件和文件夹——`IFileOpenDialog` 不支持混合选择，硬做只会得到"选了文件夹却返回文件"的诡异行为。

```cpp
std::vector<std::wstring> PickFiles(HWND owner) {
    std::vector<std::wstring> out;
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&dlg)))) return out;

    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_ALLOWMULTISELECT | FOS_FILEMUSTEXIST
                         | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    // 两个过滤器：全部文件 + 驱动/模块，后者便于快速定位常见占用场景
    COMDLG_FILTERSPEC filters[] = {
        { L"所有文件", L"*.*" },
        { L"驱动与模块 (*.sys;*.dll)", L"*.sys;*.dll" }
    };
    dlg->SetFileTypes(ARRAYSIZE(filters), filters);
    dlg->SetTitle(L"选择要解锁的文件");

    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItemArray* items = nullptr;
        if (SUCCEEDED(dlg->GetResults(&items))) {
            DWORD n = 0; items->GetCount(&n);
            for (DWORD i = 0; i < n; ++i) {
                IShellItem* it = nullptr;
                PWSTR p = nullptr;
                if (SUCCEEDED(items->GetItemAt(i, &it)) &&
                    SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
                    out.emplace_back(p);
                    CoTaskMemFree(p);
                }
                if (it) it->Release();
            }
            items->Release();
        }
    }
    dlg->Release();
    return out;
}
```

要点：

- **`FOS_PICKFOLDERS` 单独一套**，点"添加文件夹"时用，别和文件选择混在一个函数里
- 需要 `CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)`，且 UI 线程必须是 STA
- `SIGDN_FILESYSPATH` 在虚拟位置（如"此电脑"、库）上会失败，返回空就跳过，不要当作错误弹窗
- 记住上次目录：`SetDefaultFolder` / 持久化到注册表 `HKCU\Software\Unlocker\LastDir`
- 提权进程里调用这个对话框是正常的（同 session 的映射盘符可见），但**用户从普通权限切到管理员令牌后看到的是同一个桌面**，这点不用担心

#### 4.7.4 加入列表时就裁决，不要等到点按钮

拖入/选择的瞬间就跑一次 `PolicyGuard::Evaluate()`：

| 裁决      | 列表表现             | 可操作性                                       |
| ------- | ---------------- | ------------------------------------------ |
| Deny    | 红色行 + 状态列"✕ 已阻止" | 复选框禁用，无法勾选                                 |
| Confirm | 黄色行 + 状态列"需确认"   | 可勾选；**确认框由 worker 弹**（见 4.10.3(4)），UI 侧不代问 |
| Allow   | 正常               | 可勾选                                        |

好处：用户在拖入的**第一秒**就知道"这个不能动"，而不是点完按钮再看一个错误弹窗。这一步几乎不花成本（归一化 + 查表，单文件 < 1ms），体验差异却很大。

#### 4.7.5 为什么 UI 必须留在中完整性（已定案）

这个问题在 v0.3 里是个待验证的分叉，现已定案：**主程序 `asInvoker`，提权下放到 worker**。记录理由，免得后面有人图省事改回去。

| 方案            | 拖放                                                                                | 其它副作用                                            | 判定     |
| ------------- | --------------------------------------------------------------------------------- | ------------------------------------------------ | ------ |
| A 单进程提权       | 靠 `ChangeWindowMessageFilterEx` 放行 `WM_DROPFILES`，社区经验而非官方保证，且 `IDropTarget` 完全无救 | Shell 扩展（跑在 explorer 里，中完整性）无法把路径交给高完整性主程序，得再绕一层 | 否决     |
| **B 双进程（采用）** | 原生，零补丁                                                                            | 首次操作多一次 UAC                                      | **采用** |
| C 常驻 broker   | 原生                                                                                | 等于常驻提权后门，杀软必报                                    | 否决     |

选 B 的连带收益，值得单列：

- 拖放、`IFileOpenDialog`、托盘图标、主题跟随全部正常工作，不用跟 UIPI 搏斗
- **右键菜单终于顺了**：扩展跑在 explorer（中完整性）里，直接和同为中完整性的 UI 通信即可；若主程序是高完整性，这条链路根本连不通
- UI 崩溃不会带着管理员权限一起崩；worker 死了 UI 还能提示重试

代价只有一个：UAC。缓解手段见 4.10.6。

> 一句话原则：**权限属于"动作"，不属于"窗口"**。窗口拿着管理员令牌却只用来画画，是纯负担。

#### 4.7.6 其余 UI 要点

- 长任务不能锁 UI：`scan` / 操作全部在 worker 里跑，UI 只收进度，进度通过 `WM_APP+n` 回传；`IProgressDialog` 可用于删除大文件
- 每个失败项必须给出**可读原因**：`ERROR_SHARING_VIOLATION` → "被 notepad.exe (PID 812) 占用"
- 批量操作要有"全部取消"（发 `cancel` 给 worker），且已完成的项要能回滚提示（哪些删了、哪些没动）
- 拖入后焦点落到列表第一行，键盘可操作（Del 删项、Ctrl+A 全选、空格切换勾选）
- **worker 未就绪时按钮禁用**，不要让用户点了没反应；首次点击触发提权，期间按钮显示"正在请求管理员权限…"
- 用户点了 UAC 的"否"：不能当作错误弹窗轰炸，显示一行"已取消提权，无法执行操作" + 重试按钮

#### 4.7.7 占用详情的展示（F19）

**两级列表，各管一件事**：上面一行是"文件"，下面一组是"这个文件被谁占着"。不要合成一张大表——多文件时列会打架。

| 层级      | 列                         | 说明                                       |
| ------- | ------------------------- | ---------------------------------------- |
| 文件维度（上） | 路径 / 大小 / 占用者 / 状态        | "占用者"列做聚合：`notepad.exe (+2)`，一眼看出是独一还是多头 |
| 进程维度（下） | 进程名 / PID / 描述 / 句柄数 / 状态 | 选中上方某行才刷新，不做全量铺开                         |

**聚合很重要**：同一个 notepad 对同一个文件可能持有 3 个句柄。如果平铺成 3 行，用户会以为是 3 个进程在抢，反而更慌。显示 `×3`，点开才看句柄明细。

**状态列配色**（要能一眼分辨严重度）：

| 状态               | 颜色  | 文案            |
| ---------------- | --- | ------------- |
| `Locked`         | 红   | 已锁定           |
| `ImageMapped`    | 橙   | 映像占用          |
| `AccessDenied`   | 黄   | 权限不足          |
| `Opened`         | 灰   | 已打开           |
| `Free`           | 绿   | 可操作           |
| `LockedByKernel` | 紫/灰 | 驱动锁定          |
| `NotFound`       | 灰   | 文件不存在（直接移除该行） |
| `Error`          | 深灰  | 无法访问 + 原始错误码  |

**交互**：

- 双击进程行 → 打开该进程所在目录并选中 exe（`SHOpenFolderAndSelectItems`），用户能立刻知道"这是哪个软件"
- 右键进程行 → 复制 PID / 复制完整路径 / 结束该进程（走 Level 4，**必须先过黑名单**，Deny 项菜单置灰）
- 多选文件 → 下方显示并集，并按进程归并，避免重复列同一个 explorer.exe
- 空状态：文件处于 `Locked` 但枚举为空 → 显示"未找到持有进程，可能由安全软件或文件系统驱动锁定"，**不要显示"无人占用"**

**渐进渲染，别等全部扫完**：扫描可能 1.5 秒，界面不能白屏。每命中一项就 `PostMessage(WM_APP_OWNER_FOUND)` 追加一行。用户看到"notepad.exe 已出现"的心理感受，和盯着转圈 1.5 秒完全不同。

**图标与描述的加载时机**：只对已显示的进程补全（通常 1-5 个），在 UI 线程直接同步查版本资源也够快（每个 <2ms）。不要开线程池去预扫 200 个进程——那是给"进程管理器"用的策略，不是这里。

**刷新语义**：用户在界面上停留期间，占用关系可能变化（进程退出、句柄释放）。提供一个"重新扫描"按钮（F5），并在窗口重新激活时轻量校验一次（`Probe` 一遍路径即可，不重扫句柄表）。别做后台定时轮询——那是无谓的 CPU 和杀软敏感行为。

### 4.8 Shell 扩展（F9）

- 实现 `IShellExtInit` + `IContextMenu`，注册到：
  ```
  HKCR\*\shellex\ContextMenuHandlers\Unlocker       = {GUID}
  HKCR\Directory\shellex\ContextMenuHandlers\Unlocker = {GUID}
  ```
- 32/64 位各一份 DLL，注册表分别写 `HKCR\CLSID` 与 `HKCR\Wow6432Node\CLSID`
- **Win11 新菜单**的真相：不放 MSIX 包（sparse package）的话，只能在"显示更多选项"里看到。文档里要写清，别当 bug 修。
- 点击后不要自己干活，而是把路径丢给主程序：
  ```cpp
  // 通道 A：Shell 扩展 -> UI，中完整性到中完整性，不走 UAC
  //   固定管道名 \\.\pipe\Unlocker.UI.{SessionId}，配 RegisterWindowMessage 唤醒主窗口
  // 通道 B：UI -> worker，走提权，见 4.10（随机管道名 + 128 位令牌）
  // 单实例：CreateMutexW(L"Local\\Unlocker.Instance")
  ```
- **两条管道别混**：A 是"低危唤醒"（只传路径），B 是"高危执行"（能删文件）。混成一个，等于把 Shell 扩展也纳入了提权通道的信任边界
- 单实例互斥体用 `Local\` 而不是 `Global\`：**单实例本来就是按会话语义**——同一用户在两个会话（快速用户切换、RDP）里各开一个、互不干扰才是对的。顺带纠正一个常见说法：`SeCreateGlobalPrivilege` 只约束**文件映射对象和符号链接**，创建 `Global\` 互斥体并不需要它
- 扩展里绝不能做重活，否则资源管理器卡死会被用户骂

### 4.9 CLI（F10）

```
Unlocker.exe [选项] <路径...>

  /u            仅解锁（关句柄），不执行文件操作
  /d            解锁并删除
  /r <新名>     解锁并重命名
  /m <目标目录> 解锁并移动
  /c <目标目录> 解锁并复制
  --force       强制模式（终止进程）
  --reboot      失败时排队到重启
  --list        只列出占用者，不做任何修改（安全模式）
  --json        机器可读输出
  --quiet       静默

退出码
  0  成功
  1  参数错误
  2  未找到占用者
  3  权限不足
  4  被内置黑名单拦截（BL-xxx，--force 也无法覆盖）
  5  解锁失败
  6  需要重启才能完成
  7  需要二次确认但运行在 --quiet 下（无人可确认，安全中止）
  8  需要提权但运行在 --quiet 下（不允许弹 UAC，安全中止）
```

> 所有同类工具的 CLI 参数都是各家自己定的，没有标准。上面这套是自洽设计，不要照抄某些"IObit CLI 参数表"——那些表格在官方文档里查不到。
>
> 一条硬规则：**`--force` 只是"允许杀进程"，不是"允许无视黑名单"。** 两者必须在代码里彻底分开，否则迟早有人把它们混成一个开关。

**CLI 怎么拿到权限**（别留悬念）：

| 场景                     | 做法                                                                        |
| ---------------------- | ------------------------------------------------------------------------- |
| 在**已提权**的终端里跑（管理员 cmd） | 直接在当前进程内调 `core`，不起 worker。链路最短，最适合脚本化                                    |
| 在**普通**终端里跑            | 拉起 worker（同 4.10 的 `--worker` 协议），本次命令结束即退出，不复用会话                         |
| `--list` 只想读占用信息       | 先跑 L1（免提权）；遇到打不开的进程时**不自动提权**，在输出里标注 `"incomplete": true` 与 `skippedPids` |
| `--quiet`              | **禁止任何形式的 UAC**。需要提权时直接以退出码 8 结束，绝不弹窗——脚本场景下弹出 UAC 会让整个流水线卡死等人            |

> CLI 不做"自动重新提权自己"这种花活（`ShellExecuteEx(runas)` 拉起自身再转发参数），那会让退出码传递变得不可靠。要么用户自己用管理员终端，要么走 worker 把结果回传，二选一。



### 4.10 提权 Worker 与 IPC 协议（方案 B 核心）

#### 4.10.1 整体时序

```
UI(中完整性)                          worker(高完整性)
    │                                      │
    │ 用户点"解锁并删除"                      │
    │── ShellExecuteEx(lpVerb="runas") ───►│  弹 UAC
    │                                      │  校验 --pipe / --parent-pid
    │   lpParameters = --worker             │  EnablePrivileges()
    │                  --pipe=\\.\pipe\Unlocker.W.{128 位随机}
    │                  --parent-pid={UI 的 PID}
    │                                      │
    │◄──────── 管道连接 + 握手 ─────────────│  CreateNamedPipe(ACL + SACL:ML;;NW;;;ME)
    │── {"op":"scan","items":[...]} ───────►│
    │◄── {"evt":"progress",...} × N ────────│  扫句柄
    │◄── {"ok":true,"items":[...]} ─────────│
    │  渲染占用进程列表                        │
    │── {"op":"delete","items":[...]} ─────►│  重新归一化 + 重新裁决 → 执行
    │◄── {"ok":true,"results":[...]} ───────│
    │                                      │  空闲 5 分钟 → 自动退出
```

**单次操作一次 UAC，不是每个文件一次。** worker 起来后常驻到空闲超时，后续操作复用同一条连接。

注意这条链路不是"拖入就必然发生"：普通文件在 UI 里跑完 L1 就能出结果（见 4.2 两级扫描）。**只有深扫和写操作才会走到上面这张图。**

#### 4.10.2 消息格式

长度前缀 + UTF-8 JSON，最省事且不易出帧错：

```
[4 字节 little-endian 长度][JSON 正文]
```

```jsonc
// 请求（UI → worker）
{ "id": 7, "op": "delete", "items": [ {"path": "C:\\Temp\\a.log"}, ... ] }

// 响应（worker → UI）
{ "id": 7, "evt": "progress", "cur": 3, "total": 10 }
{ "id": 7, "ok": true, "results": [
    { "path": "C:\\Temp\\a.log", "ok": true, "level": 2, "state_after": "Free",
      "owners": [ {"pid":812,"name":"notepad.exe","handles":3} ] },
    { "path": "C:\\Windows\\System32\\x.dll", "ok": false, "code": 4, "rule": "BL-016",
      "msg": "系统目录下的文件，需确认" }
] }
```

**第一帧必须是 `hello`，不是 `ping`。** 版本不匹配（`ver` ≠ 编进 worker 的 `kProtocolVersion`）就断连并返回明确错误——用户升级后旧 worker 残留（比如会话复用期间换了 exe）是最常见的协议错位来源，而且症状是"发出去的请求石沉大海"，不查版本根本想不到。握手失败时 UI 先杀掉旧 worker 再重拉一次，不要直接报错给用户。

字段名只有一套：**路径一律放在 `items[].path`**，不存在 `paths` 这种写法。请求体只认 `id` / `op` / `items` / `force` 四个字段，多余字段忽略但记日志。管道名、父进程 PID、UI 用户 SID 都属于**启动上下文**（`WorkerContext`），不随请求传输。

`op` 枚举固定，**白名单式**，收到的任何其它 `op` 一律拒绝并记日志：

| op                                    | 说明                                                       | 是否危险 |
| ------------------------------------- | -------------------------------------------------------- | ---- |
| `hello`                               | **首帧握手**：`{op:"hello",ver:3}`，worker 回 `{ok:true,ver:3}` | 否    |
| `ping`                                | 存活探测                                                     | 否    |
| `scan`                                | 只查占用者，不改文件                                               | 否    |
| `unlock`                              | 关句柄                                                      | 是    |
| `unlock`                              | 关句柄（F2）                                                  | 是    |
| `delete` / `rename` / `move` / `copy` | 对应 F3-F6                                                 | 是    |
| `reboot`                              | 排队到重启                                                    | 是    |
| `cancel`                              | 中止当前任务                                                   | —    |
| `shutdown`                            | worker 主动退出                                              | —    |

> 没有任何 `exec`、`shell`、`run` 类操作。这条要写进代码注释和 review checklist——一旦有人加进来，整个工具就变成了提权跳板。

#### 4.10.3 管道的安全边界（重点，别抄错）

worker 是高完整性进程，**任何能连上它的进程都等于拿到了管理员的手**。所以：

**(1) ACL 只给当前用户 + 强制完整性标签压到 Medium**

这里有个**一定会在联调第一天爆掉**的坑：管道对象默认继承创建者的完整性标签。worker 是高完整性，管道标签就是 High，并且默认带 NO-WRITE-UP 策略。中完整性的 UI 来连接时必然请求写权限（双向管道），会被内核直接拒——`CreateFile` 返回 `ACCESS_DENIED`，现象和"SDDL 写错了"一模一样，很容易查错方向。

所以 SDDL 里必须补一条 SACL，把对象标签压到 Medium：

```cpp
// 关键：不能只放行"自己的 SID"。
// 标准用户点操作、在弹出的框里输入管理员凭据时，worker 是以【那个管理员账户】运行的，
// GetTokenInformation(TokenUser) 拿到的是管理员的 SID；而连管道的 UI 仍然是
// 原来的标准用户。只放行自己 → UI 连不上，而且现象和 A1 的 SACL 问题一模一样。
//
// 正确做法：从父进程（UI）的令牌里读它真正的用户 SID，两个都放行。
std::wstring uiSid = GetUserSidOfProcess(expectedUiPid);   // 见下方说明
std::wstring sddl  = L"D:(A;;GA;;;" + uiSid + L")(A;;GA;;;" + SelfUserSidString() + L")"
                     L"S:(ML;;NW;;;ME)";
PSECURITY_DESCRIPTOR sd = nullptr;
ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr);
SECURITY_ATTRIBUTES sa{ sizeof(sa), sd, FALSE };
HANDLE hPipe = CreateNamedPipeW(pipeName,
                                PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_REJECT_REMOTE_CLIENTS,
                                1, 64 * 1024, 64 * 1024, 0, &sa);
```

三处要点：

- `S:(ML;;NW;;;ME)` 是**功能必需**，不是安全加固。少了它，UI 连不上 worker，而报错信息（`ACCESS_DENIED`）会把人往权限方向带偏
- **DACL 必须同时放行"UI 侧的用户 SID"**。这是跨用户提权（标准用户 + 输入管理员凭据）才会暴露的问题：worker 跑在管理员账户下，`CurrentUserSidString()` 是管理员的 SID，只放行它的话，标准用户的 UI 连自己的管道都被拒——T14 那条用例点完"输入凭据"之后必然失败
- `FILE_FLAG_FIRST_PIPE_INSTANCE` 不能省——否则别的进程可以抢先占住这个管道名冒充 worker，UI 就被劫持了

`uiSid` 的取法（**用父进程令牌，不要信命令行**）：

```cpp
std::wstring GetUserSidOfProcess(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);  // 管理员身份够用
    if (!h) return {};
    HANDLE tok = nullptr;
    OpenProcessToken(h, TOKEN_QUERY, &tok);
    std::vector<BYTE> buf(64); DWORD len = 0;
    GetTokenInformation(tok, TokenUser, buf.data(), (DWORD)buf.size(), &len);
    // 转成 S-dd-... 字符串
    ...
}
```

UI 传 `--ui-sid` 也能用（SID 不是秘密，它是授权目标而非凭证），但**worker 必须自己从父进程令牌核对一遍**，否则恶意客户端可以塞一个任意 SID 来放宽自己的门。

**(2) 客户端身份校验 —— 强制，不是"辅助"**

```cpp
ULONG clientPid = 0;
GetNamedPipeClientProcessId(hPipe, &clientPid);
if (clientPid != expectedUiPid) { DisconnectAndExit(); }   // 硬失败，不给机会

// 三重校验，缺一不可
// a. PID 必须等于启动参数里的 --parent-pid
// b. 该 PID 的完整映像路径必须等于本程序的 exe 路径（QueryFullProcessImageNameW）
// c. 该进程的创建时间必须早于 worker 自己（GetProcessTimes），防 PID 复用
```

**先把威胁模型说清楚，免得设计跑偏**：ACL 是按用户 SID 放行的，所以**同用户的中完整性进程是能连上这个管道的**——ACL 挡的是"别的用户"和"更低的完整性"，挡不住同一用户自己的恶意代码。这一点如果搞反了，测试用例会写成"ACL 拒连"，然后在真机上一试就发现根本没有拒。

**令牌的位置也调整了**：不再放命令行，改为嵌进管道名 `\\.\pipe\Unlocker.W.{128 位随机}`。这不改变"可枚举"的事实（`\\.\pipe\` 目录能列出来），但命令行的可见性明显更高——`Win32_Process` 一类的工具默认就在展示它，管道名需要主动去枚举。**降低可见性，别当保密用。**

所以：**(1) 和 (2) 都不是安全边界，真正兜底的是 (3)。**

但这句话得说准确，别给它过强的承诺：**三重校验挡的是"冒充 UI"，挡不住"UI 本身被注入"。** 真 UI 被注入后，PID、映像路径、进程创建时间全部合法，worker 会照单执行。此时剩下的约束只有两条：

1. 黑名单（`Deny` 项一律拒绝）
2. `Confirm` 项的弹框（用户能看见发生了什么）

也就是说，一个被注入的中完整性 UI 能指使 worker 删掉**不在黑名单、也没有 Confirm 规则覆盖的高权限位置**（其他用户的 profile、`ProgramData` 深处、某些服务的配置目录）。这已经超出了一个中等完整性进程本来能做的事。

这是方案 B 的固有成本：**为了让拖放和对话框稳定，把 UI 留在了低权限一侧，就必须接受"UI 是被信任的"这个前提。** 想收紧只有一个办法——把黑名单从"防误删"扩展到"防越权"（比如默认拒绝用户目录之外的一切），但那样工具就没法用了。接受它，并写进风险表。

**(3) worker 端重新做一遍全部校验（这一条最重要）**

UI 传上来的路径**一律视为不可信输入**：

```cpp
for (auto& raw : req.items) {
    std::wstring canonical = CanonicalPath(raw.path);   // 重新取句柄真身
    auto v = PolicyGuard::Evaluate(canonical);          // 重新裁决
    if (v.verdict == Verdict::Deny) { 拒绝并返回规则 ID }
    // 不允许路径里出现 ..、不允许 UNC、不允许设备路径
}
```

原因很直接：UI 是中完整性进程，同用户的中完整性进程就能给它注入代码、伪造请求。**worker 不做重校验，这台机器就有了一个"任意删文件"的提权接口。**

同理，`PolicyGuard` 的规则表在 worker 端也必须做一次 CRC 自检（见 4.6.6），不能只信 UI 传来的"已校验"标志。

**(4) Confirm 级确认必须由 worker 自己弹**

这是个容易漏的地方：如果二次确认只是"UI 弹个框，然后把 `confirmed: true` 发给 worker"，那这个字段毫无意义——伪造的客户端直接置 true 就绕过了。

正确做法：**凡是裁决结果为 `Confirm` 的操作，对话框由 worker 弹**。UI 只负责发起请求，worker 在真正执行前自己弹框、自己收结果。

- worker 虽然是 `SW_HIDE` 启动的，但同一会话里创建对话框完全正常
- 对话框里要带足够的信息让人能做判断：完整路径、规则 ID、占用进程、后果
- 用户在 worker 对话框上点"取消" → 该项返回 `code=7`，其余项继续
- `--quiet` 模式下 worker 不弹框，直接返回 `code=7`（对应退出码 7）

> 一句话：**权限在哪，确认就在哪。** 让低权限的一侧替高权限的一侧做决定，等于没做确认。

**(5) 不做的事**

- 不把 worker 注册成服务、不加开机自启、不监听网络
- 不允许 worker 在无 `--pipe` 参数时进入交互模式（否则 `Unlocker.exe --worker` 就成了裸工具）
- 不给 worker 加"记住授权"逻辑

#### 4.10.4 拉起 worker

```cpp
SHELLEXECUTEINFOW sei{ sizeof(sei) };
sei.fMask       = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
sei.lpVerb      = L"runas";                 // 触发 UAC
sei.lpFile      = selfPath;                 // 必须是绝对路径，不能用 argv[0]
sei.lpParameters = params.c_str();          // --worker --pipe=\\.\pipe\Unlocker.W.{128 位随机} --parent-pid=...
                                            // 注意：令牌已并入管道名，不再单列参数
sei.nShow       = SW_HIDE;                  // worker 无窗口
if (!ShellExecuteExW(&sei)) {
    DWORD e = GetLastError();
    if (e == ERROR_CANCELLED) ShowInfo(L"已取消提权，无法执行操作");
    else                      ShowError(L"无法启动执行组件", e);
    return false;
}
// sei.hProcess 留着，用于监控 worker 异常退出
```

**坑**：

- `lpFile` 用绝对路径。用 `argv[0]` 在从 Shell 扩展拉起时会拿到相对路径或错误目录，UAC 直接失败
- `SEE_MASK_NOASYNC` 必须加，否则函数返回后 `sei` 结构体不能再用
- 不要用 `CreateProcess` + `runas`——`ShellExecuteEx` 才是走 UAC 的正路
- 用户取消（`ERROR_CANCELLED`）是**正常路径**，不能当异常处理
- **必须在工作线程调用**：UAC 弹窗会一直阻塞到用户点确认，放 UI 线程上界面直接假死（还会被系统判定"无响应"变灰）。界面这边要立刻切到"正在请求管理员权限…"状态
- 既然令牌并进了管道名，`--pipe` 参数里就带着敏感串，启动命令行的暴露面只剩这一处；日志里必须**打码**（只留前 8 位），否则日志文件本身就成了泄露源

#### 4.10.5 失败与超时

| 情况                      | UI 表现                                                         |
| ----------------------- | ------------------------------------------------------------- |
| UAC 被取消                 | 一行提示 + 重试按钮，不弹错误框                                             |
| worker 启动后 5 秒内没连上管道    | 杀掉 worker 进程，提示"执行组件启动失败"，附日志路径                               |
| 管道读写超时（默认 30 秒，扫描大目录另算） | 发 `cancel`，超时即断开并提示                                           |
| worker 中途崩溃             | `WaitForSingleObject(sei.hProcess)` 收到退出，提示"执行组件异常退出（0x%08X）" |
| 单条消息 > 16 MB            | 拒绝（防止畸形请求打爆内存），记日志                                            |

#### 4.10.6 UAC 会在什么时候弹

两级扫描（F20）落地后，UAC 的次数比 v0.6 少了一个量级：

| 用户动作                   | 是否弹 UAC    | 说明                  |
| ---------------------- | ---------- | ------------------- |
| 拖入 / 选择文件              | **不弹**     | L1 在 UI 进程内跑完       |
| 查看占用者（普通进程占用）          | **不弹**     | L1 就够               |
| 查看占用者（被管理员程序占用）        | 提示按钮，用户点才弹 | "以管理员身份深扫"          |
| 解锁 / 删除 / 改名 / 移动 / 复制 | **弹一次**    | 写操作一律走 worker，会话内复用 |
| 同一批文件的后续操作             | 不弹         | worker 空闲 5 分钟才退出   |

四条配套规则：

1. **worker 会话复用**：首次提权后常驻，空置 5 分钟自退。一批文件（解锁 + 删除 + 改名）只弹一次
2. **惰性提权**：不要在启动时就提权。用户只是拖个文件看看谁占着，不该被打断
3. **状态栏明说**：常驻一行"执行组件：未启动 / 运行中（管理员）"，用户随时知道现在处在哪一档
4. **`--quiet` 下永不弹 UAC**：需要提权时直接退出码 8。脚本里挂个 UAC 弹窗会让人等到天荒地老

> 反面做法：为了省 UAC 去做"计划任务持久化提权"或"服务常驻"。两条都是 UAC 绕过的经典手法，杀软零容忍，不碰。

---

## 5. 关键数据结构

```cpp
// NtApi.h
typedef struct _UNICODE_STRING { USHORT Length, MaximumLength; PWSTR Buffer; } UNICODE_STRING;

typedef struct _SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX {
    PVOID     Object;
    ULONG_PTR UniqueProcessId;
    HANDLE    HandleValue;
    ULONG     GrantedAccess;
    USHORT    CreatorBackTraceIndex;
    USHORT    ObjectTypeIndex;
    ULONG     HandleAttributes;
    ULONG     Reserved;
} SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX;

typedef struct _SYSTEM_HANDLE_INFORMATION_EX {
    ULONG_PTR NumberOfHandles;
    ULONG_PTR Reserved;
    SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX Handles[1];
} SYSTEM_HANDLE_INFORMATION_EX;

// 业务模型
enum class OccupancyState { Free, Opened, Locked, ImageMapped, AccessDenied,
                            LockedByKernel, NotFound, Error };

struct ProcessInfo {                 // 进程身份（F19）
    DWORD        pid = 0;
    std::wstring name, fullPath, description, company;
    bool         elevated      = false;
    bool         protectedProc = false;   // OpenProcess 失败
    HICON        icon          = nullptr; // 仅 UI 进程内有效，不参与序列化
};

struct HandleOwner {                 // 一个具体句柄
    ProcessInfo  proc;
    HANDLE       handleValue;
    ULONG        grantedAccess;
    bool         isImageMapping;     // GrantedAccess 含 SECTION_MAP_EXECUTE
};

struct FileReport {                  // 一个目标文件的完整结论，UI 直接拿它渲染
    std::wstring             path;
    std::wstring             canonical;
    OccupancyState           state    = OccupancyState::Free;
    PolicyResult             policy;              // 见下文定义，加入列表时就算好
    uint64_t                 size     = 0;
    std::vector<HandleOwner> owners;              // 按 pid 分组的原始句柄
};

struct UnlockResult {
    bool     success;
    DWORD    win32Error;        // 或 NTSTATUS
    int      level;             // 实际用到的策略层级
    std::wstring message;       // 面向用户的中文说明
    std::vector<HandleOwner> owners;
};

// IPC（F18）
enum class IpcOp { Hello, Ping, Scan, Unlock, Delete, Rename, Move, Copy, Reboot, Cancel, Shutdown };

struct IpcItem { std::wstring path; };   // 与线格式 items[].path 一一对应

struct IpcRequest {
    uint64_t             id    = 0;
    IpcOp                op    = IpcOp::Ping;
    std::vector<IpcItem> items;    // 原始路径，worker 端必须重新归一化
    bool                 force = false;
};

// 启动上下文只属于 worker 进程，不进请求体（否则每个请求都在重传管道名和身份信息）
struct WorkerContext {
    std::wstring pipeName;         // 来自 --pipe
    DWORD        expectedUiPid = 0;// 来自 --parent-pid
    std::wstring uiUserSid;        // 从父进程令牌读出，用于构建 DACL
};
// 响应统一为 JSON：{id, evt|ok, results|progress}
// 手写 JSON 即可（字段少，避免引入依赖），但转义必须做全：
//   \ 转成 \\      " 转成 \"     控制字符转成 \u00XX     非 ASCII 直接输出 UTF-8
// 最容易被忽略的是路径里的反斜杠和文件名里的引号——一个没转义就整条消息解析失败，
// 而这类文件名恰好是用户最需要处理的"疑难文件"

// 黑名单裁决（F15）
enum class Verdict { Allow, Confirm, Deny };

struct PolicyResult {
    Verdict      verdict;
    RuleId       ruleId;        // 命中的规则，Allow 时为 BL_NONE
    Severity     severity;      // INFO / HIGH / CRIT
    std::wstring canonical;     // 归一化后的真身路径（日志与提示都用它）
    std::wstring reason;        // 已本地化，直接可显示
};
```

> **`ProcessInfo` 过不了 IPC，这点很容易踩**：扫描在 worker，渲染在 UI，`HICON` 是进程内内核对象句柄，序列化过去就是野值。做法是 worker 只传 `pid / name / fullPath / description`，**UI 侧拿到 `fullPath` 后自己 `SHGetFileInfoW` 取图标**；受保护进程取不到就用系统默认图标。别想着把图标塞进 JSON。

---

## 6. 里程碑

| 阶段     | 内容                                                                                          | 交付物                  | 验收                                                    | 工作量    |
| ------ | ------------------------------------------------------------------------------------------- | -------------------- | ----------------------------------------------------- | ------ |
| **M0** | 工程骨架：CMake/MSVC、**manifest 用 asInvoker**、`main.cpp` 三态分流、日志、RAII 句柄、错误映射、`Blacklist.def` 骨架 | 能编译运行的空壳             | 双击**不弹** UAC；`--worker` 无参时直接退出                       | 1.5 人日 |
| **M1** | HandleScanner + PathResolver + **CanonicalPath** + **Probe 状态判定（F19）**                      | `--list` 可用          | 对记事本占用的 txt 正确列出 PID 与状态；对大目录 1.5s 内                  | 3.5 人日 |
| **M2** | FileOps（删/改名/移动/复制）+ Level 0/1                                                              | 无占用文件全部操作正常          | 只读文件、跨卷移动、中文长路径全通过                                    | 2 人日   |
| **M3** | LockBreaker Level 2/4 + **PolicyGuard 裁决引擎 + 文件 ID 表 + fail-closed**                        | 真正能解锁，且删不动系统文件       | 关闭句柄后 explorer 立即能删；T17-T24 全绿                        | 4 人日   |
| **M4** | **提权 worker + 命名管道（ACL+SACL）+ 协议 + 三重身份校验 + 重校验 + 空闲退出**                                    | worker 可被 UI 拉起并完成任务 | T32-T38、T51、T52 全绿；伪造请求被拒；`S:(ML;;NW;;;ME)` 缺省时能复现连不上 | 2.5 人日 |
| **M5** | GUI：拖入（F16）+ 文件选择（F17）+ 两级扫描（F20）+ 两级列表与占用详情（F19）+ 提权自查兜底（B7）                               | 可用界面                 | T25-T31、T48-T50 全绿；普通文件拖入不弹 UAC；被手动提权时拖放仍可用并有横幅       | 3.5 人日 |
| **M6** | Shell 扩展 + CLI + 单实例                                                                        | 右键可用                 | 多选、文件夹、Win11 二级菜单均验证；扩展→UI 链路通                        | 2.5 人日 |
| **M7** | Level 5 重启兜底 + 加固 + 全量测试                                                                    | Release 包            | 测试矩阵全绿；蓝屏用例 0                                         | 3 人日   |

**关键路径**：M1 → M3 → M4。M1 做不准，后面全是白搭；M3 是唯一的"真解锁"能力来源；**M4 是所有 GUI 功能的前置**——UI 从第一天起就通过 `PipeClient` 调 `core`，不存在"先在进程内跑通再拆出去"的过渡版本。

> 这个顺序是方案 B 的必然结果，也是它的隐性好处：`core` 从第一次提交就必须是"可以被远程调用"的干净接口，不会写出依赖全局状态、依赖窗口句柄的脏代码。
>
> M5/M6 可以并行。M0 的 `asInvoker` 一旦写错（比如顺手加了 `requireAdministrator`），M5 的拖放会静默失效，所以 M0 的验收项里专门有一条"双击不弹 UAC"。

---



## 7. 测试矩阵

| #   | 用例            | 构造方式                   | 期望                                                                             |
| --- | ------------- | ---------------------- | ------------------------------------------------------------------------------ |
| T1  | 记事本打开 txt     | 手动                     | 列出 notepad.exe，解锁后可删                                                           |
| T2  | 目录被当工作目录      | `cmd /k cd /d D:\test` | 找到 cmd.exe，关闭目录句柄后目录可删                                                         |
| T3  | DLL 被加载            | 写个宿主 exe LoadLibrary        | 句柄关了仍删不掉 → 提示需强制模式 |
| T4  | EXE 正在运行           | 直接运行                     | 同上，不得崩溃系统 |
| T5  | 只读属性             | `attrib +r`                 | Level 1 直接成功 |
| T6  | ACL 拒绝        | 移除当前用户 DELETE 权限       | 启用 **SeRestorePrivilege** 后能拿到 DELETE 访问（`SeBackupPrivilege` 只豁免读，对 DELETE 无效） |
| T7  | 跨卷移动             | C: → D:                    | 复制+删源，MD5 一致 |
| T8  | 超长路径          | >260 字符                | `\\?\` 前缀处理正确                                                                  |
| T9  | 符号链接          | `mklink /D`            | 比较真身路径，不误删链接目标                                                                 |
| T10 | 系统关键文件        | 尝试删 `ntoskrnl.exe` 副本  | PolicyGuard 拦截（BL-003），不蓝屏                                                     |
| T11 | 白名单进程         | 尝试杀 lsass              | 拒绝并解释                                                                          |
| T12 | 性能            | 150k 句柄机器              | < 1.5s 出结果                                                                     |
| T13 | 重启兜底             | 锁定 pagefile 同级场景          | 重启后文件消失，注册表项清空 |
| T14 | 普通用户账户        | 用标准账户（非管理员组）运行         | UI 正常启动不弹 UAC；点操作时弹出凭据输入框                                                      |
| T15 | x86 进程        | 32 位宿主                 | x64 主程序能枚举并关其句柄                                                                |
| T16 | Win10 / Win11 | 双系统                    | 右键菜单位置符合预期                                                                     |

**黑名单专项（F15，全部必须在 M3 通过）**

| #   | 用例           | 构造方式                                                                    | 期望                              |
| --- | ------------ | ----------------------------------------------------------------------- | ------------------------------- |
| T17 | 直接命中硬禁       | 解锁 `System32\drivers\` 下任意 `.sys`                                       | BL-001 Deny，UI 显示三段式提示          |
| T18 | `..` 相对穿越    | `C:\Windows\System32\..\System32\ntoskrnl.exe`                          | 归一化后仍 Deny                      |
| T19 | NT 前缀 / 大小写  | `\\?\C:\WINDOWS\system32\NTOSKRNL.EXE`                                  | 剥前缀 + 大写后 Deny                  |
| T20 | 8.3 短名       | `dir /x` 取短名后传参                                                         | `GetLongPathName` 展开后 Deny      |
| T21 | junction 重定向 | `mklink /J C:\tmp\win C:\Windows` 后传 `C:\tmp\win\System32\ntoskrnl.exe` | 句柄真身解析后 Deny                    |
| T22 | 硬链接          | `mklink /H C:\tmp\k.exe C:\Windows\System32\ntoskrnl.exe`               | `FILE_ID_INFO` 命中 Deny          |
| T23 | 误伤反例         | 自建 `D:\MyApp\System32\a.dll` 并被占用                                       | Allow，正常解锁                      |
| T24 | 规则表篡改        | patch 二进制 `.rdata` 改一条规则                                                | CRC 校验失败 → 全表 Deny（fail-closed） |

**输入入口专项（F16 / F17，M5 通过）**

| #   | 用例          | 构造方式                          | 期望                                           |
| --- | ----------- | ----------------------------- | -------------------------------------------- |
| T25 | 拖放（中完整性 UI） | 正常启动（不弹 UAC），从资源管理器拖 1 个文件进窗口 | 能收到；若换成 requireAdministrator 则应复现失效，反向确认架构决策 |
| T26 | 多选混合拖入      | 同时选中 2 个文件和 1 个文件夹拖入          | 三者都进列表，文件夹标记为目录                              |
| T27 | 快捷方式        | 拖入指向黑名单文件的 `.lnk`             | 解析目标后仍判 Deny                                 |
| T28 | 不支持的位置      | 拖入 UNC 路径 / 压缩包内文件 / 回收站项     | 明确提示不支持，不崩溃、不静默忽略                            |
| T29 | 重复拖入        | 同一文件拖两次                       | 按 canonical 去重，列表仍只有 1 条                     |
| T30 | 内置选择器       | 点"添加文件"多选 5 个；点"添加文件夹"选 1 个   | 分别返回正确集合；记住上次目录                              |
| T31 | 虚拟位置        | 选择器里点"此电脑"/"库"并确定             | `SIGDN_FILESYSPATH` 失败被忽略，不弹错误               |

**造锁工具**：先写个 30 行的 `MakeLock.exe`（`CreateFile` 不带 `FILE_SHARE_DELETE` 然后 `Sleep`），比手动找场景快 10 倍，列为 M1 附属交付物。

> T17-T24 建议做成一个 `policy_tests.exe` 断言集，每次改 `.def` 跑一遍。黑名单的回归成本极低，价值极高——它挡的是蓝屏。

**提权与 IPC 专项（F18，M4 通过）**

| #   | 用例        | 构造方式                                                                                                                              | 期望                                                          |
| --- | --------- | --------------------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------- |
| T32 | UAC 取消    | 弹窗后点"否"                                                                                                                           | 一行提示 + 重试按钮，不弹错误框、不留残留进程                                    |
| T33 | 会话复用      | 连续做扫描→解锁→删除                                                                                                                       | 只弹**一次** UAC                                                |
| T34 | 空闲退出      | 提权后放置 5 分钟                                                                                                                        | worker 自行退出，任务管理器无残留                                        |
| T35 | 同用户伪造客户端  | 以同一用户的中完整性进程连管道（**能连上，ACL 挡不住同用户**），发 `{"op":"delete","items":[{"path":"C:\\Windows\\System32\\drivers\\x.sys"}]}`（JSON 里反斜杠必须转义） | 三重身份校验失败 → 断连；即便伪造 `--parent-pid`，worker 重校验仍返回 BL-001 Deny |
| T36 | 畸形请求      | 发 4 GB 长度头 / 非 JSON / 未知 `op`                                                                                                     | 拒绝并断连，worker 不崩                                             |
| T37 | worker 崩溃 | 任务管理器杀掉 worker                                                                                                                    | UI 收到退出码并提示，可重新拉起                                           |
| T38 | 裸跑 worker | `Unlocker.exe --worker`（不带 pipe 参数）                                                                                               | 立即退出，不产生任何行为                                                |

**占用详情专项（F19，M1/M5 通过）**

| #   | 用例       | 构造方式                               | 期望                                              |
| --- | -------- | ---------------------------------- | ----------------------------------------------- |
| T39 | 可删的"被打开" | 自写程序以 `FILE_SHARE_DELETE` 打开 txt   | 显示"已打开"而非"锁定"，直接删能成                             |
| T40 | 真锁定      | `MakeLock` 打开 txt（不含 SHARE_DELETE） | 显示"锁定"，并列出 notepad/MakeLock 名称与 PID             |
| T41 | 映像映射     | 宿主 exe `LoadLibrary` 某 dll         | 状态"映像占用"，提示关句柄无效、需杀进程                           |
| T42 | 受保护进程    | 删被 System(PID 4) 持有的文件             | 显示"另有 N 个受保护进程无法检查"汇总行；**不得声称是它在占用**（做不到），也不得隐身 |
| T43 | 驱动锁定     | 杀软实时扫描中删文件                         | 显示"未找到持有进程"，不得误报"无人占用"                          |
| T44 | 同进程多句柄   | 同一进程对同一文件开 3 个句柄                   | 进程维度聚合为一行 ×3，不平铺 3 行                            |
| T45 | 中文描述回退   | 打开 notepad 占用的文件                   | 描述列显示"记事本"（`080404B0` 语言块回退成功）                  |
| T46 | 自我排除     | 本工具正在操作时重扫                         | 列表内不出现 Unlocker 自身进程                            |
| T47 | 文件已被删    | 扫描后、操作前手动删掉该文件                     | 显示"文件不存在"并移除该项，不进入解锁流程（验证 `NotFound` 分支）        |

**两级扫描与提权兜底专项（F20 / B7，M4-M5 通过）**

| #   | 用例       | 构造方式                                    | 期望                                                                                                    |
| --- | -------- | --------------------------------------- | ----------------------------------------------------------------------------------------------------- |
| T48 | UI 被手动提权 | 右键"以管理员身份运行"UI                          | 拖放**仍能收到**（补丁生效）+ 顶部横幅提示；未打补丁时应能复现失效                                                                  |
| T49 | 免提权扫描    | 拖入被 notepad 占用的文件                       | **不弹 UAC**，直接列出占用者（L1 生效）                                                                             |
| T50 | 深扫入口     | 拖入被管理员程序占用的文件                           | 同时出现两条提示："未找到持有进程" + "另有 N 个进程无法检查"；给出"以管理员身份深扫"按钮，点了才弹 UAC。若 L1 探测为 `AccessDenied`，提示改为"需要管理员权限才能访问" |
| T51 | 管道完整性标签  | 临时去掉 SDDL 里的 `S:(ML;;NW;;;ME)`          | UI 连接返回 `ACCESS_DENIED` —— 反向确认这条 SACL 是功能必需                                                          |
| T52 | 跨用户提权    | 用标准账户启动 UI，操作时在 UAC 框里输入**另一个管理员账户**的凭据 | UI 能正常连上 worker 并拿到结果（DACL 已放行 UI 侧 SID）；反向：把 DACL 改回只放行自身 SID 时应连不上                                  |

---

## 8. 风险清单

| 风险                            | 影响                     | 缓解                                                                                         |
| ----------------------------- | ---------------------- | ------------------------------------------------------------------------------------------ |
| 强关句柄导致目标进程崩溃                  | 用户数据丢失                 | 默认不杀进程；关句柄前明确提示；对含未保存数据的进程（Office/IDE）建议先保存                                                |
| 误杀进程导致蓝屏                      | 系统重启                   | PolicyGuard 白名单 + 黑名单硬禁 + 系统盘保护 + 二次确认                                                     |
| 黑名单被绕过（路径伪装）                  | 删掉系统文件 → 蓝屏            | 句柄真身归一化 + `FILE_ID_INFO` 双表；T17-T24 作为发布门禁                                                 |
| 黑名单误伤用户文件                     | 用户骂娘                   | 只对 `<Windows>`、`drivers\`、hive 等窄范围硬禁；用户目录显式 Allow 规则（BL-030 一类）                           |
| 用户嫌麻烦去用别的工具                   | 流失                     | 拒绝提示必须说清原因和后果，而不是干巴巴一句"不允许"                                                                |
| **L1 漏报被误当成"没人占用"**           | 用户反复删除失败，认为软件不准        | L1 结果必须带 `skippedPids`；Locked 且空列表时强制提示深扫入口；T50 作为门禁                                       |
| 用户手动以管理员运行 UI                 | 拖放与右键菜单静默失效            | 启动自查 `TokenElevation` → 打 `ChangeWindowMessageFilterEx` + 顶部横幅（T48）                        |
| **worker 被当作提权跳板**            | 中完整性进程借 worker 拿到管理员权限 | 管道 ACL 只给本人 + `FILE_FLAG_FIRST_PIPE_INSTANCE` + **worker 端完整重校验**（4.10.3）；op 白名单无 exec 类操作 |
| **UI 被注入后指使 worker**          | 可删黑名单之外的高权限位置（越权）      | 方案 B 的固有成本：UI 被信任。剩余约束仅黑名单 + worker 弹框（4.10.3(2)）；UI 侧不做任何文件操作、不加载第三方代码、不开调试端口             |
| **worker 被 ATL/杀软判定为 UAC 绕过** | 直接查杀                   | 不做服务常驻、不做计划任务、不做免提权持久化；签名；文档公开安全设计                                                         |
| 拖入文件夹导致递归删除过深                 | 误删大片文件                 | 文件夹不预展开；递归前显示"将处理 N 个对象"并要求确认                                                              |
| UAC 太频繁导致用户放弃                 | 流失                     | 两级扫描把只读场景全部免提权；写操作靠 worker 会话复用（空闲 5 分钟）；状态栏常驻显示当前权限档                                      |
| 占用状态误判（把可删的报成锁定）              | 用户白紧张、白杀进程             | 先 Probe 再枚举（4.2）；T39 专门验证 `FILE_SHARE_DELETE` 场景                                           |
| 受保护进程查不到就隐身                   | 用户误以为无人占用，反复失败         | 末尾汇总提示"另有 N 个进程无法检查" + 解释文案；T42 作为门禁                                                       |
| 版本资源读取拖慢首次渲染                  | 打开卡顿                   | 只补全实际占用者，不全量预扫；渐进追加行                                                                       |
| 图标 GDI 句柄泄漏                   | 长时间运行后界面异常             | `SHGetFileInfo` 的 HICON 必须 `DestroyIcon`；列表项销毁时统一回收                                        |
| 杀软误报                          | 无法分发                   | 代码签名；不写驱动；行为模式贴近正常工具；提交误报申诉                                                                |
| HVCI / 内核隔离                   | 能力退化                   | 用户态方案不受影响（这也是不写驱动的理由之一）                                                                    |
| 句柄扫描被 AV 当行为监控                | 卡顿/拦截                  | `NtQuerySystemInformation` 是常规调用，避免高频轮询                                                    |
| worker 与 UI 版本不一致             | 协议错位，请求石沉大海            | 首帧强制 `hello` 交换 `ver`，不匹配即断连；UI 侧先杀旧 worker 再重拉一次（4.10.2）                                  |
| Win11 右键菜单位置不符预期              | 投诉                     | 安装时提供"是否安装 MSIX 包"选项，文档写清                                                                  |

---

## 9. 与 IObit Unlocker 的能力对照

| 能力       | IObit     | 本方案                          | 差异说明                        |
| -------- | --------- | ---------------------------- | --------------------------- |
| 找占用进程    | ✅         | ✅                            | 同级                          |
| 关句柄不杀进程  | ✅         | ✅                            | 同级                          |
| 强制模式     | 终止关联进程    | 终止进程 + 白名单                   | 本方案更保守                      |
| 解锁并复制    | ✅         | ✅                            | 同级                          |
| 内核级锁定    | 可能靠驱动     | ❌                            | 明确的能力缺口                     |
| 卸 DLL 映像 | 推测有       | P2 可选                        | 优先级不同                       |
| 重启兜底     | ✅         | ✅                            | 同级                          |
| 内置黑名单    | 不可见       | ✅ 三级裁决 + 规则 ID 可追溯           | 本方案更透明                      |
| 拖入文件     | ✅         | ✅                            | 同级；本方案额外处理快捷方式/UNC/去重       |
| 软件内选择文件  | ✅         | ✅                            | 同级；文件与文件夹分两个按钮              |
| 占用者详情    | 进程名 + PID | 进程名 + PID + 描述 + 句柄数 + 8 种状态 | 本方案给出"能不能删"的结论，不只是"谁占着"     |
| 提权模型     | 主程序直接提权   | UI 中完整性 + 提权 worker + 两级扫描   | 本方案拖放/对话框更稳，且只在写操作时才要一次 UAC |

---

## 10. 附录

### 10.1 常量速查

| 名称                                                | 值    | 用途                     |
| ------------------------------------------------- | ---- | ---------------------- |
| `SystemExtendedHandleInformation`                 | 64   | 句柄枚举                   |
| `ObjectNameInformation`                           | 1    | 取句柄名                   |
| `FileDispositionInfoEx`                           | 21   | POSIX 删除               |
| `FileRenameInfo`                                  | 3    | 改名/移动                  |
| `MOVEFILE_DELAY_UNTIL_REBOOT`                     | 0x4  | 重启兜底                   |
| `DUPLICATE_CLOSE_SOURCE`                          | 0x1  | 关远程句柄的关键               |
| `FileIdInfo`                                      | 18   | 取 `FILE_ID_INFO`，硬链接识别 |

### 10.2 内置黑名单规则表（初始版，共 20 条）

`<Windows>` 为占位符，运行时展开。同一路径命中多条时取最严格者。

> **所有模式都必须以 `<Windows>\`、`<ProgramFiles>\` 或盘符开头。** 写成裸的 `System32\...` 会静默失效——归一化后是完整路径 `C:\WINDOWS\SYSTEM32\NTOSKRNL.EXE`，`EXACT` 做的是全等比较，不带前缀永远匹配不上，结果就是"规则写了、但什么都没拦"。这类错误不会报错、不会崩溃，只会让保护悄悄失效。建议在 `.def` 生成时加一条编译期断言：模式串首段必须是占位符或 `X:\`。

> `BL-008` / `BL-009` 的"任意卷根"没法写成静态模式，必须在启动时枚举所有卷（`GetLogicalDriveStringsW` + `GetVolumeNameForVolumeMountPoint`）展开成具体路径再入表。文档里这两条是**逻辑规则**，不是可以直接抄进 `.def` 的字面模式。

| ID     | 匹配          | 模式                                                                                                                                                    | 裁决       | 原因                                      |
| ------ | ----------- | ----------------------------------------------------------------------------------------------------------------------------------------------------- | -------- | --------------------------------------- |
| BL-001 | PREFIX      | `<Windows>\System32\drivers\`                                                                                                                         | **Deny** | 驱动目录，关句柄易致蓝屏                            |
| BL-002 | PREFIX      | `<Windows>\System32\config\`                                                                                                                          | **Deny** | 注册表 hive 与日志                            |
| BL-003 | EXACT       | `<Windows>\System32\{ntoskrnl,ntkrnlmp,ntkrnlpa,ntoskrnlmp}.exe`                                                                                      | **Deny** | 内核主文件                                   |
| BL-004 | EXACT       | `<Windows>\System32\{hal,ci,bootvid}.dll`                                                                                                             | **Deny** | 硬件抽象层与代码完整性                             |
| BL-005 | GLOB        | `<Windows>\System32\win32k*.sys`                                                                                                                      | **Deny** | 内核图形子系统                                 |
| BL-006 | GLOB        | `<Windows>\System32\*.sys`（其余内核驱动）                                                                                                                    | **Deny** | 任何内核模块                                  |
| BL-007 | VOLUME_ROOT | 任意卷根目录                                                                                                                                                | **Deny** | 整卷操作不是本工具职责                             |
| BL-008 | EXACT       | 任意卷根 `{pagefile,hiberfil,swapfile}.sys`、`DumpStack.log.tmp`                                                                                           | **Deny** | 内存/休眠文件                                 |
| BL-009 | EXACT       | 卷根 `bootmgr`、`BOOTNXT`、`Boot\BCD`                                                                                                                     | **Deny** | 引导链                                     |
| BL-010 | PREFIX      | `<Windows>\WinSxS\`                                                                                                                                   | **Deny** | 组件存储，删了系统装不上补丁                          |
| BL-011 | PREFIX      | `<Windows>\servicing\`                                                                                                                                | **Deny** | 服务栈                                     |
| BL-012 | PREFIX      | `\System Volume Information\`                                                                                                                         | **Deny** | 卷影/还原点                                  |
| BL-013 | FILE_ID     | 硬禁清单内所有具体文件的 FileId 集合                                                                                                                                | **Deny** | 硬链接伪装                                   |
| BL-014 | SELF        | 本程序自身 exe 及其目录                                                                                                                                        | **Deny** | 自杀保护                                    |
| BL-015 | VOLUME      | EFI 系统分区、系统保留分区挂载点                                                                                                                                    | **Deny** | 按 `IOCTL_DISK_GET_PARTITION_INFO_EX` 判定 |
| BL-016 | PREFIX      | `<Windows>\`（未被上面命中的其余部分）                                                                                                                             | Confirm  | 系统目录，需二次确认                              |
| BL-017 | PREFIX      | `<ProgramFiles>\`、`<ProgramFilesX86>\`                                                                                                                | Confirm  | 已安装程序目录，命中即需确认                          |
| BL-018 | COUNT       | 单次批量 > 500 个对象                                                                                                                                        | Confirm  | 批量误删风险                                  |
| BL-019 | PREFIX      | `<Windows>\SysWOW64\`                                                                                                                                 | **Deny** | 32 位子系统目录，删了 32 位程序全废                   |
| BL-020 | EXACT       | `<Windows>\System32\{ntdll,kernel32,kernelbase,user32,win32u,gdi32,advapi32,rpcrt4,combase,ucrtbase,msvcrt,shell32,ole32,bcrypt,crypt32,sechost}.dll` | **Deny** | 根目录核心 DLL，删掉任一个都进不了桌面                   |

**显式放行（Allow）**——防止过严误伤，写规则时同样重要：

| ID     | 匹配     | 模式                                         | 说明            |
| ------ | ------ | ------------------------------------------ | ------------- |
| BL-030 | PREFIX | `<Windows>\Temp\`                          | 系统临时目录，本来就是垃圾 |
| BL-031 | PREFIX | `<Windows>\SoftwareDistribution\Download\` | 更新缓存，常见占用场景   |
| BL-032 | PREFIX | `<Windows>\Prefetch\`                      | 预读文件          |

> 用户目录（含 `D:\MyApp\System32\` 这种同名目录）不在任何规则范围内，天然 Allow。**别为了"保险"把规则写成"路径含 System32 就拦"**，那是误伤的重灾区。

**参考**：`NtQuerySystemInformation` / `NtQueryObject` 文档（ntinternals 系列）、Windows Internals 第 7 版 上册第 3 章（对象与句柄表）、`winbase.h` 中 `FILE_DISPOSITION_INFO_EX` 定义、`GetFinalPathNameByHandleW` / `FILE_ID_INFO` 文档。

---

## 11. 下一步

M0 可以直接开工。建议第一件事不是写 UI，而是**写 30 行的 `MakeLock.exe`**——没有稳定的锁源，后面每个功能都只能靠猜。

第二件事是把 `Blacklist.def` 的 20 条规则先写死，哪怕 `PolicyGuard` 还没接上调用点。**先有刹车，再装发动机**：功能可以慢慢补，蓝屏一次就够了。

第三件事，方案 B 定案后值得单独做的 **IPC 空跑验证**：UI 只发 `ping`，worker 只回 `pong`，中间套一层 UAC。**这件事两小时能做完，但决定了后面所有功能是"通的"还是"不通的"**。别等 M4 才第一次跑通提权链路——那时候 UI、协议、业务逻辑同时出问题，排查会非常痛苦。

推荐的开工顺序：

1. `MakeLock.exe`（30 行，半天）
2. `main.cpp` 三态分流 + `asInvoker` manifest（验 T14/T38）
3. `pipe ping/pong` 空跑，含 UAC（验 T32/T33/T34）
4. `Blacklist.def` 20 条 + `PolicyGuard` 纯函数版（验 T17-T24）
5. 再回到 M1 的句柄扫描

前四步都是"没它就得返工"的地基，加起来约 3.5 天，但能省掉后面至少一周的排查。

