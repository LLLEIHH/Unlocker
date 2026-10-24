# Unlocker 执行方案评审意见

> 评审对象：`Unlocker-Execution-Plan.md`
> 第一轮：v0.5 / 2026-10-22；第二轮：v0.7 / 2026-10-22（见 §六）
> 评审结论：**骨架不动**。第一轮 17 条已全部正确落实；第二轮新发现 2 个设计 bug、2 处规范矛盾，修完即可开工。
> 分级：A = 会直接跑不通/跑错的真 bug；B = 安全/设计漏洞；C = 覆盖缺口；D = 小毛病。

---

## 0. 总评

整体质量高。以下判断都是老手才会写的，予以确认：

- 方案 B（asInvoker 主程序 + 提权 worker）的 UIPI 论证成立
- `DUPLICATE_CLOSE_SOURCE` 关远程句柄的正名
- `NtQueryObject` 卡死用线程 + 超时兜底
- ObjectTypeIndex 运行时探测（不硬编码 25/47）
- 黑名单 fail-closed 自检、worker 端独立重校验
- "先 Probe 再枚举" 的顺序
- X-Macro 驱动规则表、三段式拒绝文案
- MakeLock 优先于真机测试用例

问题集中在三处：**管道完整性标签（A1）会在 M4 联调第一天爆掉**；**junction 解析逻辑（A2）会让 T21 落空且恰是最需要拦的绕过手法**；**IPC 令牌传递（B6）存在真实的提权链**。

---

## 一、A 类：真 bug（必须在 v0.6 修掉）

### A1. §4.10.3 管道缺少强制完整性标签（SACL），UI 大概率连不上 worker

高完整性进程创建的命名管道，默认继承创建者的完整性标签（High）。中完整性的 UI 前来连接需要读写权限，会被 "no write up" 策略拦下，`CreateFile` 连管道直接 `ACCESS_DENIED` —— 正好撞上 §4.10.5 里 "5 秒连不上就杀 worker" 那条。

**修复**：SDDL 补一条 SACL 把标签压到 Medium：

```
D:(A;;GA;;;<用户SID>)S:(ML;;NW;;;ME)
```

### A2. §4.6.4 自相矛盾：`FILE_FLAG_OPEN_REPARSE_POINT` 打开的句柄，`GetFinalPathNameByHandleW` 解不开 junction

用 `FILE_FLAG_OPEN_REPARSE_POINT` 打开，句柄指向的是**链接本身**，`GetFinalPathNameByHandleW` 返回的是链接路径（如 `C:\tmp\win\System32\...`），不会解析到目标（`C:\Windows\...`）。T21 的期望会落空 —— 而这恰恰是黑名单最需要拦的绕过手法。

**修复**：
- 黑名单归一化：**不带** `FILE_FLAG_OPEN_REPARSE_POINT` 打开（跟随 reparse），或显式 `FSCTL_GET_REPARSE_POINT` 查目标
- §4.2 Probe 里用 `OPEN_REPARSE_POINT` 是对的（删链接删的就是链接本身）—— 两处语义不同，文档要分开写清楚

### A3. §4.2 ImageMapped 判定用了错误的位

`SECTION_MAP_EXECUTE`（0x8）是**节对象**的访问位；文件句柄的 GrantedAccess 里 0x8 是 `FILE_READ_EA`，两个域不一样，按此判断会误报。

**修复**：主判据改为 `EnumProcessModules` 对比模块路径（文中提了一句，应升为主判据）；`FILE_EXECUTE`（0x20）位检查只做辅助。

### A4. §4.2 Probe 的 default 分支返回 `Free`

`ERROR_FILE_NOT_FOUND` / `ERROR_PATH_NOT_FOUND` / `ERROR_INVALID_NAME` 都落到 default，被当成"可操作"，路径敲错的文件显示绿灯。

**修复**：增加 `NotExist / Invalid` 状态单独返回。

### A5. §4.4 Level 2 代码小 bug

`DUPLICATE_CLOSE_SOURCE` 时目标句柄不创建，`lpTargetHandle` 应传 `nullptr`；现在的 `dup != INVALID_HANDLE_VALUE` 检查无意义，且 `dup` 未初始化时可能误 `CloseHandle` 一个野值。

---

## 二、B 类：安全设计漏洞（建议升级方案）

### B6. 令牌走命令行 + "读命令行需要管理员"的论断不成立

§4.10.3(2) 称 "读命令行需要管理员，恰好卡住一层" —— **不对**。同用户的中完整性进程可以通过 WMI `Win32_Process.CommandLine` 读到高完整性 worker 的完整命令行（已知信息泄露面，Chrome 传密钥改用继承句柄即为此）。

攻击链：读到令牌 → 管道 ACL 按用户放行（同用户中完整性进程**能连上**）→ 过握手 → 对非黑名单位置提权操作。且 revalidation 只挡黑名单；`Confirm` 级若靠消息里的确认标志，伪造客户端直接置 true。

**修复**：
1. 客户端 PID + 映像路径校验从"辅助"升级为**强制**（比对映像必须是父进程同一路径，防 PID 复用）
2. 令牌不走命令行，改用**句柄继承**传递
3. `Confirm` 确认不能信消息里的 confirmed 字段，须由 worker 侧绑定 UI 会话独立确认
4. **T35 期望写反了**：ACL 挡不住同用户进程，真实威胁模型恰恰是同用户中完整性恶意代码，用例按此改写

### B7. UI 被用户手动"以管理员身份运行"的场景没覆盖

用户右键"以管理员运行"UI 后，UI 变高完整性，来自 explorer 的 `WM_DROPFILES` 同样被 UIPI 静默拦掉，右键菜单链路也断 —— 与 §4.7.2 警告的 manifest 问题同源，只是触发者是用户而不是开发者。

**修复**（二选一，别留空白）：
- 检测到 UI 已提权时打 `ChangeWindowMessageFilterEx` 补丁（此时已知风险可控）
- 文档明确写"不要以管理员身份运行 UI"，并在 UI 里检测告警

---

## 三、C 类：覆盖缺口（值得补进方案）

| # | 位置 | 问题 | 建议 |
|---|---|---|---|
| C8 | §10.2 规则表 | `SysWOW64\` 完全没规则，只有 BL-016 的 Confirm 兜底；System32 根目录的 `ntdll.dll`/`kernelbase.dll`/`user32.dll` 等也只有 Confirm，删掉 ntdll.dll 同样开不了机 | System32 根 + SysWOW64 至少给 Deny 或 CRIT 级 Confirm |
| C9 | §4.10.6 vs §2 F18 | 表述打架：F18 说"首次**操作**弹一次 UAC"，4.10.6 说"拖入后自动扫描就弹" | 统一产品决策并写死；另可考虑两级扫描：UI 先中完整性扫（覆盖 95% 场景，普通用户进程不需要 SeDebug），发现打不开的进程或执行危险操作时再提权，避免每拖一次文件弹一次 UAC |
| C10 | §4.6.6 | CRC 自检防的是意外损坏，不是防篡改 —— 能 patch `.rdata` 的人也能 patch 常量和跳转 | T24 当回归门禁没问题，但文档别暗示它是安全边界（真边界是签名） |
| C11 | §4.9 | CLI 在非提权控制台下的行为没写 | 补充：是否也走 worker 弹 UAC？`--quiet` 与 UAC 弹窗如何共存？ |

---

## 四、D 类：小毛病（顺手修）

| # | 位置 | 问题 |
|---|---|---|
| D12 | §4.2 坑 1 | `FILE_SHARE_ALL` 不是 SDK 常量，应写 `FILE_SHARE_READ\|FILE_SHARE_WRITE\|FILE_SHARE_DELETE` |
| D13 | §4.3 / §4.6.4 | 归一化大小写不统一（一处小写、一处大写），挑一个 |
| D14 | §5 | `ProcessInfo` 里的 `HICON` 过不了 IPC：扫描在 worker、渲染在 UI，图标须 UI 侧自己 `SHGetFileInfoW`（受保护进程拿不到就用占位），数据结构加注释 |
| D15 | §5 | 手写 JSON 记得转义：Windows 路径全是 `\`，还有 `"` 和控制字符，最容易踩 |
| D16 | §4.2 | 自我排除除了 worker 自身（`GetCurrentProcessId`），还要排除父进程 UI（归一化时短暂持有句柄） |
| D17 | §4.8 | 单实例 `Global\Unlocker.Instance` 建议 `Local\`（按会话），多用户快速切换时行为更合理 |
| D18 | §7 | 估时偏乐观：M4（IPC + 安全 + T32-T38 全绿）2 人日、M5 3 人日，方案 B 的复杂度恰好集中在 M4，建议各加 0.5-1 天缓冲 |

---

## 五、修订优先级建议

1. **M0 开工前必须修**：A1、A2、B6（M4 联调时一定爆、爆了还难查的类型）
2. **M2/M3 开发中修**：A3、A4、A5、B7、C8、C11
3. **随手改**：C9、C10、D12-D18

---

## 六、第二轮评审（针对 v0.7，2026-10-22）

### 6.1 第一轮 17 条修复确认

A1-A5、B6-B7、C8-C11、D12-D18 **全部落实且方向正确**，两级扫描（F20）的架构拆分尤其干净。两点补充说明：

- **A5 的反向修正（v0.7 对 DUPLICATE_CLOSE_SOURCE 的注释）是对的，我第一轮的说法过头了**。实际两种写法都合法：`lpTargetHandle` 传 `nullptr` = 只关源不复制（社区通行写法）；传 `&dup` = 复制 + 关源，目标句柄需自行 CloseHandle。v0.7 代码初始化 `nullptr` 再判空关闭，两种语义下都安全。
- 22.5 人日算术、SDDL 的 `S:(ML;;NW;;;ME)` 语法、八种匹配类型表、令牌并入管道名 + 三重身份校验的推理链，均核对无误。

### 6.2 新发现问题

#### A6（设计 bug）跨用户提权时管道 ACL 断链 —— 恰是 T14 验收场景

标准用户点操作、输入管理员凭据（over-the-shoulder）时，worker 以**管理员账户**运行，`CurrentUserSidString()` 取到的是管理员的 SID。DACL 只放行这个 SID，标准用户的 UI 连自己的管道都被拒——T14"点操作时弹出凭据输入框"之后必然失败。

**修复**：DACL 同时放行 **UI 侧用户 SID**。取法二选一：
- worker 启动时打开 `--parent-pid` 的进程令牌读取其用户 SID（管理员权限够用，最干净）
- 或 UI 传 `--ui-sid`（SID 不是秘密，它是授权目标而非凭证；三重身份校验不受影响）

#### A7（规则表 bug）BL-003~006、BL-020 缺 `<Windows>\` 前缀，按字面实现会让 ntoskrnl 从 Deny 降为 Confirm

§10.2 里 BL-001/002 带占位符，BL-003~006 和 BL-020 写的是裸 `System32\...`。`MATCH_EXACT` 对归一化全路径（`C:\WINDOWS\SYSTEM32\NTOSKRNL.EXE`）做完全相等比较，不带 `<Windows>\` 的模式**匹配不上**——新加的核心 DLL 保护（BL-020）和内核主文件（BL-003）全部落空，只剩 BL-016 Confirm 兜底。T10/T17 能拦住这个回归，但规则表是照抄进 `.def` 的，必须先把前缀补齐。BL-008/009 的"任意卷根"同理需要运行期逐卷展开成具体路径，建议加一句实现注记。

#### B8（规范矛盾）Confirm 级会被确认两次，两处未调和

§4.7.4：Confirm 项"点解锁时统一二次确认"（UI 弹）；§4.10.3(4)：Confirm "对话框由 worker 弹"。按字面执行，用户先被 UI 问一次、再被 worker 问一次。

**修复**：定死**所有 Confirm 弹框（含 §4.5 批量一次性确认）统一由 worker 出**，UI 侧只标注"需确认"、不收集决定——这也让 CLI / 右键菜单入口的行为天然一致。

#### B9（安全声明过强）"限制在中完整性本来就能做的范围"不成立

§4.10.3(2) 结尾这句需要改。三重身份校验挡的是"冒充 UI"，挡不住"UI 本身被注入"——真 UI 的 PID/映像/创建时间全部合法，worker 会照单执行。而 (3) 重校验只做黑名单，被注入的 UI 可以指使 worker 删**非黑名单、无 Confirm 规则覆盖**的高权限位置（其他用户 profile、ProgramData 深处等），超出中完整性本来的能力。实际剩余约束 = 黑名单 + Confirm 弹框（用户可见）。

**修复**：改措辞，并在 §8 风险表补一行："worker 信任 UI 进程本身；UI 被注入 = 提权面，剩余约束为黑名单与 worker 弹框"。

#### C12（归因错误，实践无碍）DELETE 探测的 ACL 绕过应归 SeRestorePrivilege

§4.2 AccessDenied 行与 T6 写"提权 + SeBackupPrivilege 重试"：backup 语义只豁免**读**；`DELETE`/写访问要靠 **SeRestorePrivilege**。worker 两个特权都启（§4.1），测试不会挂，但归因写错会误导实现。

#### C13（缺口）L1 的 AccessDenied 处置未定义

L1 在中完整性 UI 里跑，没有 SeBackup/SeRestore 可重试。Probe 遇到 AccessDenied 应直接给"需要提权（深扫/写操作）"提示，与 T50 的深扫入口呼应。另 T50 期望只写"未找到持有进程"，漏了 4.2 自己规定的"N 个无法检查"汇总行，两者应同时出现。

#### C14（缺口）握手/版本消息未定义

风险表承诺"worker 握手时回传版本号，不匹配则拒绝"，但 §4.10.2 的协议里没有任何握手或版本字段（op 表只有 ping）。补一条 `hello`（或扩展 ping）：`{op:"hello", ver:3}` → `{ok:true, ver:3}`，不匹配断连。

#### D19（理由不成立，结论正确）Local\ 的论证

§4.8 称"创建 Global\ 对象需要 SeCreateGlobalPrivilege，标准用户会失败"——该特权检查只针对**文件映射对象（和符号链接）**，互斥体/事件不受限，标准用户创建 `Global\` 互斥体并不失败。`Local\` 依然是对的，但理由是"单实例本来就按会话语义"。

#### D20（小毛病）

- T35 示例 JSON 里 `C:\Windows` 反斜杠未转义——正是 §5 自己强调的头号坑
- §2 功能矩阵 F20 排在 F19 前面
- §5 `IpcRequest` 里 `pipeName` 混在"请求"结构中（仅 worker 侧使用，应挪到 worker 启动上下文）；`items` 是 `vector<wstring>` 而线格式是 `items[].path` 对象数组，两处对齐

### 6.3 优先级

1. **M4 开工前必须修**：A6、B8（都在 M4 验收面上）；A7 在 M3 规则表落地时修
2. **随手改**：B9、C12、C13、C14、D19、D20
