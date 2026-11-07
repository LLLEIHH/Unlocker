# 生成 Lang.h 的 Str 枚举与 LangTable.cpp 的字符串表。
# 用法：python tools/gen_lang.py（在仓库根目录执行）
# 加一种语言 = 给 ROWS 里每一行补一列，再在 LANGS 处同步 Lang 枚举。
import io

# (枚举名, 简体, 繁體台灣, 繁體香港, 繁體澳門, English)
ROWS = [
    ("AppSubtitle", "文件占用解除工具", "檔案佔用解除工具", "檔案佔用解除工具", "檔案佔用解除工具", "Unlock files in use"),

    ("BtnAddFile", "添加文件", "新增檔案", "新增檔案", "新增檔案", "Add file"),
    ("BtnAddFolder", "添加文件夹", "新增資料夾", "新增資料夾", "新增資料夾", "Add folder"),
    ("BtnAdminScan", "管理员深扫", "管理員深層掃描", "管理員深層掃描", "管理員深層掃描", "Admin scan"),
    ("BtnForce", "强制模式", "強制模式", "強制模式", "強制模式", "Force mode"),
    ("BtnUnlock", "解锁", "解除鎖定", "解除鎖定", "解除鎖定", "Unlock"),
    ("BtnUnlockDelete", "解锁并删除", "解除鎖定並刪除", "解除鎖定並刪除", "解除鎖定並刪除", "Unlock & delete"),
    ("BtnUnlockRename", "解锁并改名", "解除鎖定並重新命名", "解除鎖定並重新命名", "解除鎖定並重新命名", "Unlock & rename"),
    ("BtnUnlockMove", "解锁并移动", "解除鎖定並移動", "解除鎖定並移動", "解除鎖定並移動", "Unlock & move"),
    ("BtnUnlockCopy", "解锁并复制", "解除鎖定並複製", "解除鎖定並複製", "解除鎖定並複製", "Unlock & copy"),
    ("BtnRescan", "重新扫描", "重新掃描", "重新掃描", "重新掃描", "Rescan"),
    ("BtnMinimize", "最小化", "最小化", "最小化", "最小化", "Minimize"),
    ("BtnMaximize", "最大化", "最大化", "最大化", "最大化", "Maximize"),
    ("BtnRestore", "还原", "還原", "還原", "還原", "Restore"),
    ("BtnClose", "关闭", "關閉", "關閉", "關閉", "Close"),
    ("BtnSettings", "设置", "設定", "設定", "設定", "Settings"),

    ("ColPath", "路径", "路徑", "路徑", "路徑", "Path"),
    ("ColSize", "大小", "大小", "大小", "大小", "Size"),
    ("ColOwner", "占用者", "佔用者", "佔用者", "佔用者", "Owner"),
    ("ColStatus", "状态", "狀態", "狀態", "狀態", "Status"),
    ("ColProcess", "进程", "處理程序", "處理程序", "處理程序", "Process"),
    ("ColPid", "PID", "PID", "PID", "PID", "PID"),
    ("ColDesc", "描述", "描述", "描述", "描述", "Description"),
    ("ColHandles", "句柄", "控制代碼", "控制代碼", "控制代碼", "Handles"),

    ("HintDrop", "把文件或文件夹拖到这里\\n（也可以点击上方「添加文件 / 添加文件夹」）",
                 "將檔案或資料夾拖曳到這裡\\n（也可以點選上方的「新增檔案 / 新增資料夾」）",
                 "將檔案或資料夾拖放到這裡\\n（亦可點選上方的「新增檔案 / 新增資料夾」）",
                 "將檔案或資料夾拖放到這裡\\n（亦可點選上方的「新增檔案 / 新增資料夾」）",
                 "Drop files or folders here\\n(or use Add file / Add folder above)"),

    ("BannerFailClosed", "规则表自检失败：已进入保护模式，全部写操作都会被拒绝。",
                         "規則表自我檢查失敗：已進入保護模式，所有寫入操作都會被拒絕。",
                         "規則表自我檢查失敗：已進入保護模式，所有寫入操作均會被拒絕。",
                         "規則表自我檢查失敗：已進入保護模式，所有寫入操作均會被拒絕。",
                         "Rule check failed: protection mode is on, every write operation is refused."),
    ("BannerElevated", "当前以管理员权限运行。推荐以普通权限启动，拖放与右键菜单会更稳定。",
                       "目前以系統管理員權限執行。建議以一般權限啟動，拖放與右鍵選單會更穩定。",
                       "現時以系統管理員權限執行。建議以一般權限啟動，拖放與右鍵選單會更穩定。",
                       "現時以系統管理員權限執行。建議以一般權限啟動，拖放及右鍵選單會更穩定。",
                       "Running as administrator. Launching without elevation keeps drag & drop "
                       "and the context menu more reliable."),

    ("StatusIdle", "执行组件：未启动（首次写操作时会请求管理员权限）",
                   "執行元件：未啟動（首次寫入需管理員權限）",
                   "執行元件：未啟動（首次寫入需管理員權限）",
                   "執行元件：未啟動（首次寫入需管理員權限）",
                   "Worker idle (UAC on write)"),
    ("StatusIdleElevated", "执行组件：运行中（管理员权限 UI）",
                           "執行元件：執行中（系統管理員權限介面）",
                           "執行元件：執行中（系統管理員權限介面）",
                           "執行元件：執行中（系統管理員權限介面）",
                           "Worker: running (admin)"),
    ("StatusFailClosed", "规则表自检失败：已进入保护模式，全部操作被拒绝",
                         "規則表自我檢查失敗：已進入保護模式，所有操作被拒絕",
                         "規則表自我檢查失敗：已進入保護模式，所有操作均被拒絕",
                         "規則表自我檢查失敗：已進入保護模式，所有操作均被拒絕",
                         "Protection mode on"),
    ("StatusScanning", "正在扫描占用进程…", "正在掃描佔用處理程序…", "正在掃描佔用中的處理程序…",
                       "正在掃描佔用中的處理程序…", "Scanning for owning processes…"),
    ("StatusRescanning", "重新扫描中…", "重新掃描中…", "重新掃描中…", "重新掃描中…", "Rescanning…"),
    ("StatusScanDone", "扫描完成", "掃描完成", "掃描完成", "掃描完成", "Scan complete"),
    ("StatusDeepRequest", "正在请求管理员权限以深扫…", "正在要求系統管理員權限以進行深層掃描…",
                          "正在要求系統管理員權限以進行深層掃描…", "正在要求系統管理員權限以進行深層掃描…",
                          "Requesting admin rights for deep scan…"),
    ("StatusDeepProgress", "深扫中 %d/%d", "深層掃描中 %d/%d", "深層掃描中 %d/%d", "深層掃描中 %d/%d",
                           "Deep scanning %d/%d"),
    ("StatusDeepFailed", "深扫失败", "深層掃描失敗", "深層掃描失敗", "深層掃描失敗", "Deep scan failed"),
    ("StatusDeepDone", "深扫完成（管理员）", "深層掃描完成（系統管理員）", "深層掃描完成（系統管理員）",
                       "深層掃描完成（系統管理員）", "Deep scan complete (admin)"),
    ("StatusOpRequest", "正在请求管理员权限…（首次操作会弹出 UAC）",
                        "正在要求系統管理員權限…（首次操作會顯示 UAC 提示）",
                        "正在要求系統管理員權限…（首次操作會顯示 UAC 提示）",
                        "正在要求系統管理員權限…（首次操作會顯示 UAC 提示）",
                        "Requesting admin rights… (UAC on first use)"),
    ("StatusOpProgress", "处理中 %d/%d", "處理中 %d/%d", "處理中 %d/%d", "處理中 %d/%d", "Processing %d/%d"),
    ("StatusOpRunning", "执行组件：运行中（管理员）", "執行元件：執行中（系統管理員）",
                        "執行元件：執行中（系統管理員）", "執行元件：執行中（系統管理員）",
                        "Worker: running (administrator)"),
    ("StatusOpDone", "操作完成", "操作完成", "操作完成", "操作完成", "Operation completed"),
    ("StatusOpFailed", "操作失败", "操作失敗", "操作失敗", "操作失敗", "Operation failed"),
    ("StatusNothingToAdd", "没有可加入的路径（UNC / 回收站 / 无法解析的快捷方式已拒绝）",
                           "沒有可加入的路徑（UNC / 資源回收筒 / 無法解析的捷徑已拒絕）",
                           "沒有可加入的路徑（UNC / 資源回收筒 / 無法解析的捷徑已被拒絕）",
                           "沒有可加入的路徑（UNC / 資源回收筒 / 無法解析的捷徑已被拒絕）",
                           "Nothing to add (UNC / Recycle Bin / bad shortcuts rejected)"),

    ("StateBlocked", "✕ 已阻止", "✕ 已封鎖", "✕ 已封鎖", "✕ 已封鎖", "✕ Blocked"),
    ("StateConfirm", "需确认", "需要確認", "需要確認", "需要確認", "Confirm"),
    ("StateConfirmPrefix", "需确认 · ", "需要確認 · ", "需要確認 · ", "需要確認 · ", "Confirm · "),
    ("StateScanning", "扫描中…", "掃描中…", "掃描中…", "掃描中…", "Scanning…"),
    ("StatePending", "待扫描", "待掃描", "待掃描", "待掃描", "Pending"),
    ("EntryIsDir", "<目录>", "<目錄>", "<目錄>", "<目錄>", "<dir>"),
    ("OwnerChildPrefix", "（占用子项 ", "（佔用子項 ", "（佔用子項 ", "（佔用子項 ", " (child "),
    ("OwnerChildSuffix", "）", "）", "）", "）", ")"),

    ("ProcProtected", "受保护", "受保護", "受保護", "受保護", "Protected"),
    ("ProcMapped", "映射", "對應", "對應", "對應", "Mapped"),
    ("ProcChildLock", "子项占用", "子項佔用", "子項佔用", "子項佔用", "Child lock"),
    ("ProcInUse", "占用", "佔用", "佔用", "佔用", "In use"),
    ("ProcUnknown", "未知", "未知", "未知", "未知", "Unknown"),
    ("ProcSkippedMany", "另有 %d 个受保护进程无法检查（可能是系统进程或安全软件）",
                        "另有 %d 個受保護處理程序無法檢查（可能是系統處理程序或安全軟體）",
                        "另有 %d 個受保護處理程序無法檢查（可能是系統處理程序或保安軟件）",
                        "另有 %d 個受保護處理程序無法檢查（可能是系統處理程序或保安軟件）",
                        "%d protected process(es) could not be inspected "
                        "(system processes or security software)"),

    ("DlgTooMany", "一次拖入了 %zu 个对象，超过单批 500 的上限。请分批处理，避免误删整棵目录。",
                   "一次拖曳了 %zu 個物件，超過單批 500 的上限。請分批處理，以免誤刪整個目錄。",
                   "一次拖放了 %zu 個物件，超出單批 500 的上限。請分批處理，以免誤刪整個目錄。",
                   "一次拖放了 %zu 個物件，超出單批 500 的上限。請分批處理，以免誤刪整個目錄。",
                   "%zu objects were dropped at once, above the 500-per-batch limit. "
                   "Please split them into smaller batches."),
    ("DlgNoTargetsDenied", "选中的项目全部被内置黑名单阻止，无法操作。",
                           "選取的項目全部被內建黑名單封鎖，無法操作。",
                           "已選取的項目全部被內建黑名單封鎖，無法操作。",
                           "已選取的項目全部被內建黑名單封鎖，無法操作。",
                           "All selected items are blocked by the built-in rules."),
    ("DlgNoTargets", "没有可操作的项目，请先添加文件。", "沒有可操作的項目，請先新增檔案。",
                     "沒有可操作的項目，請先新增檔案。", "沒有可操作的項目，請先新增檔案。",
                     "Nothing to operate on. Add files first."),
    ("DlgElevationCancelled", "已取消提权，无法执行操作。\\n再次点击按钮可以重试。",
                              "已取消提高權限，無法執行操作。\\n再次點選按鈕即可重試。",
                              "已取消提升權限，無法執行操作。\\n再按一次按鈕即可重試。",
                              "已取消提升權限，無法執行操作。\\n再按一次按鈕即可重試。",
                              "Elevation was cancelled, so nothing was done.\\n"
                              "Click the button again to retry."),
    ("DlgTitlePartial", "Unlocker · 未完成", "Unlocker · 未完成", "Unlocker · 未完成",
                        "Unlocker · 未完成", "Unlocker · Not completed"),
    ("DlgNothingToScan", "没有可扫描的项目。", "沒有可掃描的項目。", "沒有可掃描的項目。",
                         "沒有可掃描的項目。", "There is nothing to scan."),
    ("DlgBusyQuit", "操作正在进行中，确定要退出吗？", "操作正在進行中，確定要結束嗎？",
                    "操作正在進行中，確定要退出嗎？", "操作正在進行中，確定要退出嗎？",
                    "An operation is still running. Quit anyway?"),
    ("DlgDeniedTitle", "已被内置黑名单阻止", "已被內建黑名單封鎖", "已被內建黑名單封鎖",
                       "已被內建黑名單封鎖", "Blocked by the built-in rules"),
    ("DlgDeniedSuffix", " · 已阻止", " · 已封鎖", " · 已封鎖", " · 已封鎖", " · blocked"),
    ("DlgDeniedFooter", "\\n此规则为内置保护，无法关闭。", "\\n此規則為內建保護，無法關閉。",
                        "\\n此規則為內建保護，無法關閉。", "\\n此規則為內建保護，無法關閉。",
                        "\\nThis rule is part of the built-in protection and cannot be turned off."),

    ("OpSummary", "成功 %d，失败 %d，跳过 %d", "成功 %d，失敗 %d，略過 %d", "成功 %d，失敗 %d，略過 %d",
                  "成功 %d，失敗 %d，略過 %d", "Succeeded %d, failed %d, skipped %d"),
    ("OpRebootNote", "\\n有项目已排队到重启后执行。", "\\n有項目已排入重新啟動後執行。",
                     "\\n有項目已排定於重新啟動後執行。", "\\n有項目已排定於重新啟動後執行。",
                     "\\nSome items were queued to run after the next reboot."),
    ("OpDetailSkipped", "[跳过] ", "[略過] ", "[略過] ", "[略過] ", "[skipped] "),
    ("OpDetailFailed", "[失败] ", "[失敗] ", "[失敗] ", "[失敗] ", "[failed] "),
    ("OpDetailRule", "    规则 ", "    規則 ", "    規則 ", "    規則 ", "    rule "),

    ("SettingsTitle", "设置", "設定", "設定", "設定", "Settings"),
    ("SettingsAppearance", "外观", "外觀", "外觀", "外觀", "Appearance"),
    ("ThemeSystem", "跟随系统", "跟隨系統", "跟隨系統", "跟隨系統", "Use system setting"),
    ("ThemeLight", "浅色", "淺色", "淺色", "淺色", "Light"),
    ("ThemeDark", "深色", "深色", "深色", "深色", "Dark"),

    ("SettingsLanguage", "界面语言", "介面語言", "介面語言", "介面語言", "Interface language"),
    ("SettingsHint", "切换后立即生效，下次启动会沿用这个选择。",
                     "切換後立即生效，下次啟動會沿用這個選擇。",
                     "切換後即時生效，下次啟動會沿用這個選擇。",
                     "切換後即時生效，下次啟動會沿用這個選擇。",
                     "Changes apply immediately and are remembered for the next launch."),
    ("SettingsShellMenu", "资源管理器右键菜单", "檔案總管右鍵選單", "檔案總管右鍵選單",
                            "檔案總管右鍵選單", "Explorer context menu"),
    ("SettingsShellMenuHint", "在文件和文件夹上右键即可打开 Unlocker",
                              "在檔案和資料夾上按右鍵即可開啟 Unlocker",
                              "在檔案和資料夾上按右鍵即可開啟 Unlocker",
                              "在檔案和資料夾上按右鍵即可開啟 Unlocker",
                              "Right-click a file or folder to open Unlocker"),

    ("ShellMenuVerb", "Unlocker 解锁占用", "Unlocker 解除佔用", "Unlocker 解除佔用",
                      "Unlocker 解除佔用", "Unlock with Unlocker"),
    ("ShellPromptTitle", "添加右键菜单", "新增右鍵選單", "新增右鍵選單", "新增右鍵選單",
                         "Add context menu entry"),
    ("ShellPromptBody", "把「%s」加到文件和文件夹的右键菜单里，之后可以直接查看占用并解锁。"
                        "随时可以在设置里关掉。",
                        "把「%s」加到檔案和資料夾的右鍵選單裡，之後可以直接查看佔用並解除鎖定。"
                        "隨時可以在設定裡關閉。",
                        "把「%s」加到檔案和資料夾的右鍵選單裡，之後可以直接查看佔用並解除鎖定。"
                        "隨時可以在設定裡關閉。",
                        "把「%s」加到檔案和資料夾的右鍵選單裡，之後可以直接查看佔用並解除鎖定。"
                        "隨時可以在設定裡關閉。",
                        "Add \u201c%s\u201d to the context menu of files and folders, so you can "
                        "check and release locks right from Explorer. You can turn it off in "
                        "Settings at any time."),
    ("ShellPromptAdd", "添加", "新增", "新增", "新增", "Add"),
    ("ShellPromptLater", "暂不", "暫時不要", "暫時不要", "暫時不要", "Not now"),
    ("ShellInstallFailed", "写入注册表失败，可能被安全软件拦截。",
                           "寫入登錄檔失敗，可能被安全軟體攔截。",
                           "寫入登錄檔失敗，可能被保安軟件攔截。",
                           "寫入登錄檔失敗，可能被保安軟件攔截。",
                           "Could not update the registry. Security software may have blocked it."),

    ("SettingsAbout", "关于", "關於", "關於", "關於", "About"),
    ("SettingsLicense", "MIT 许可证", "MIT 授權條款", "MIT 授權條款",
                        "MIT 授權條款", "MIT License"),

    ("OccupancyReady", "可操作", "可操作", "可操作", "可操作", "Ready"),
    ("OccupancyOpen", "已打开", "已開啟", "已開啟", "已開啟", "Open"),
    ("OccupancyLocked", "已锁定", "已鎖定", "已鎖定", "已鎖定", "Locked"),
    ("OccupancyMapped", "映像占用", "映像佔用", "映像佔用", "映像佔用", "Mapped"),
    ("OccupancyDenied", "权限不足", "權限不足", "權限不足", "權限不足", "Denied"),
    ("OccupancyKernelLock", "驱动锁定", "驅動程式鎖定", "驅動程式鎖定", "驅動程式鎖定", "Kernel lock"),
    ("OccupancyMissing", "文件不存在", "檔案不存在", "檔案不存在", "檔案不存在", "Missing"),
    ("OccupancyInaccessible", "无法访问", "無法存取", "無法存取", "無法存取", "Inaccessible"),
    ("OccupancyUnknown", "未知", "未知", "未知", "未知", "Unknown"),

    ("ExplainFree", "没有进程占用，可直接操作", "沒有處理程序佔用，可直接操作",
                    "沒有處理程序佔用，可直接操作", "沒有處理程序佔用，可直接操作",
                    "No process is using it; you can act on it directly"),
    ("ExplainOpen", "有进程打开了它，但允许删除", "有處理程序開啟了它，但允許刪除",
                    "有處理程序開啟了它，但允許刪除", "有處理程序開啟了它，但允許刪除",
                    "A process has it open, but deletion is allowed"),
    ("ExplainLocked", "有进程不允许删除，需要先释放句柄", "有處理程序不允許刪除，需要先釋放控制代碼",
                      "有處理程序不允許刪除，需要先釋放控制代碼", "有處理程序不允許刪除，需要先釋放控制代碼",
                      "A process blocks deletion; the handle must be released first"),
    ("ExplainMapped", "映像已被映射到进程，关闭句柄无效，只能结束进程或重启后删除",
                      "映像已對應到處理程序，關閉控制代碼無效，只能結束處理程序或重新啟動後刪除",
                      "映像已對應到處理程序，關閉控制代碼無效，只能結束處理程序或重新啟動後刪除",
                      "映像已對應到處理程序，關閉控制代碼無效，只能結束處理程序或重新啟動後刪除",
                      "The image is mapped into a process; closing handles won't help, "
                      "end the process or delete after reboot"),
    ("ExplainDenied", "权限或 ACL 问题，不是占用，需要管理员权限",
                      "權限或 ACL 問題，不是佔用，需要系統管理員權限",
                      "權限或 ACL 問題，不是佔用，需要系統管理員權限",
                      "權限或 ACL 問題，不是佔用，需要系統管理員權限",
                      "Access/ACL problem rather than a lock; administrator rights required"),
    ("ExplainKernelLock", "未找到持有进程，可能由安全软件或文件系统驱动锁定",
                          "找不到持有處理程序，可能由安全軟體或檔案系統驅動程式鎖定",
                          "找不到持有處理程序，可能由保安軟件或檔案系統驅動程式鎖定",
                          "找不到持有處理程序，可能由保安軟件或檔案系統驅動程式鎖定",
                          "No owner process found; likely locked by security software "
                          "or a file system driver"),
    ("ExplainMissing", "文件已经不存在", "檔案已經不存在", "檔案已經不存在", "檔案已經不存在",
                       "The file no longer exists"),
    ("ExplainError", "路径非法、设备未就绪或未知错误", "路徑無效、裝置未就緒或未知錯誤",
                     "路徑無效、裝置未就緒或未知錯誤", "路徑無效、裝置未就緒或未知錯誤",
                     "Invalid path, device not ready, or unknown error"),

    ("ErrSuccess", "成功", "成功", "成功", "成功", "Success"),
    ("ErrFileNotFound", "文件不存在", "檔案不存在", "檔案不存在", "檔案不存在", "File not found"),
    ("ErrPathNotFound", "路径不存在", "路徑不存在", "路徑不存在", "路徑不存在", "Path not found"),
    ("ErrAccessDenied", "拒绝访问（权限不足）", "存取被拒（權限不足）", "存取被拒（權限不足）",
                        "存取被拒（權限不足）", "Access denied"),
    ("ErrSharing", "文件正被占用（共享冲突）", "檔案正在使用中（共用衝突）",
                   "檔案正在使用中（共享衝突）", "檔案正在使用中（共享衝突）",
                   "File is in use (sharing violation)"),
    ("ErrLock", "文件区域被锁定", "檔案區域被鎖定", "檔案區域被鎖定", "檔案區域被鎖定",
                "File region is locked"),
    ("ErrInvalidName", "文件名或路径格式非法", "檔名或路徑格式無效", "檔名或路徑格式無效",
                       "檔名或路徑格式無效", "Invalid file name or path"),
    ("ErrBadPath", "路径无效", "路徑無效", "路徑無效", "路徑無效", "Invalid path"),
    ("ErrInvalidParam", "参数无效", "參數無效", "參數無效", "參數無效", "Invalid parameter"),
    ("ErrDirNotEmpty", "目录非空", "目錄不是空的", "目錄不是空的", "目錄不是空的",
                       "Directory is not empty"),
    ("ErrNotSameDevice", "不在同一个卷上", "不在同一個磁碟區上", "不在同一個磁碟區上",
                         "不在同一個磁碟區上", "Not on the same volume"),
    ("ErrDiskFull", "磁盘空间不足", "磁碟空間不足", "磁碟空間不足", "磁碟空間不足", "Disk is full"),
    ("ErrWriteProtect", "介质写保护", "媒體受寫入保護", "媒體受寫入保護", "媒體受寫入保護",
                        "Media is write-protected"),
    ("ErrCancelled", "操作已取消", "操作已取消", "操作已取消", "操作已取消", "Operation cancelled"),
    ("ErrPrivilege", "缺少所需特权", "缺少所需權限", "缺少所需權限", "缺少所需權限",
                     "Required privilege not held"),
    ("ErrInvalidHandle", "句柄无效", "控制代碼無效", "控制代碼無效", "控制代碼無效", "Invalid handle"),
    ("ErrProcessAborted", "进程已终止", "處理程序已終止", "處理程序已終止", "處理程序已終止",
                          "Process was terminated"),
    ("ErrFileTooLarge", "文件过大", "檔案過大", "檔案過大", "檔案過大", "File is too large"),
    ("ErrNotSupported", "该操作不受支持", "不支援此操作", "不支援此操作", "不支援此操作",
                        "Not supported"),
    ("ErrPathTooLong", "路径过长", "路徑太長", "路徑太長", "路徑太長", "Path is too long"),
    ("ErrUnknown", "未知错误", "未知錯誤", "未知錯誤", "未知錯誤", "Unknown error"),
    ("ErrElevationCancelled", "已取消提权，无法执行操作", "已取消提高權限，無法執行操作",
                             "已取消提升權限，無法執行操作", "已取消提升權限，無法執行操作",
                             "Elevation was cancelled"),
    ("ErrWithCodeFmt", "%s（%s）", "%s（%s）", "%s（%s）", "%s（%s）", "%s (%s)"),
]

LANGS = ["zhCN", "zhTW", "zhHK", "zhMO", "en"]

# ---------------- Lang.h ----------------
enum_lines = []
for r in ROWS:
    enum_lines.append(f"    {r[0]},")
enum_block = "\n".join(enum_lines)

header = f'''// 多语言文案：所有可翻译字符串集中在 LangTable.cpp 的一张二维表里，
// 加一种语言 = 给表里每一行多补一列（编译器会检查列数），再加一个 Lang 枚举值。
// 调用点只引用 Str::Xxx，不再把文案写死在业务代码里。
#pragma once

#include <string>

namespace ul {{

enum class Lang {{
    ZhCN = 0,   // 简体中文
    ZhTW,       // 繁體中文（台灣）
    ZhHK,       // 繁體中文（香港）
    ZhMO,       // 繁體中文（澳門）
    En,
    Count
}};

enum class Str : int {{
{enum_block}
    Count
}};

Lang GetLang();
void SetLang(Lang lang);

// 系统首选 UI 语言 → 界面语言。zh-Hant 按地区细分到台/港/澳，其余中文用简体，非中文用英文。
Lang DetectSystemLang();

// 读 HKCU\\Software\\Unlocker\\Language（"zh-CN" / "zh-TW" / "zh-HK" / "zh-MO" / "en"）
Lang LoadLangPreference();
void SaveLangPreference(Lang lang);

// 语言自身的名称（用于下拉框，永远用该语言自己的写法）
const wchar_t* LangNativeName(Lang lang);
// 稳定的存储标识（注册表用）
const wchar_t* LangTag(Lang lang);

const wchar_t* T(Str id);              // 当前语言
const wchar_t* T(Str id, Lang lang);   // 指定语言
std::wstring Tf(Str fmt, ...);         // 带参数（按当前语言取格式串）

}}  // namespace ul
'''

io.open("src/common/Lang.h", "w", encoding="utf-8", newline="\n").write(header)

# ---------------- LangTable.cpp ----------------
rows_cpp = []
for r in ROWS:
    cells = ", ".join('L"%s"' % c for c in r[1:])
    rows_cpp.append(f"    /* {r[0]:<21} */ {{ {cells} }},")
table_body = "\n".join(rows_cpp)

table_cpp = f'''// 由 gen_lang.py 生成，手改请同步改脚本；结构与 Str 枚举一一对应。
#include "common/Lang.h"

namespace ul {{
namespace {{

// [Str][Lang]，列顺序：简体 / 繁體(台灣) / 繁體(香港) / 繁體(澳門) / English
const wchar_t* const kStrings[static_cast<int>(Str::Count)][static_cast<int>(Lang::Count)] = {{
{table_body}
}};

}}  // namespace

const wchar_t* T(Str id, Lang lang) {{
    const int s = static_cast<int>(id);
    const int l = static_cast<int>(lang);
    if (s < 0 || s >= static_cast<int>(Str::Count)) return L"";
    if (l < 0 || l >= static_cast<int>(Lang::Count)) return kStrings[s][static_cast<int>(Lang::En)];
    return kStrings[s][l];
}}

const wchar_t* T(Str id) {{ return T(id, GetLang()); }}

}}  // namespace ul
'''
io.open("src/common/LangTable.cpp", "w", encoding="utf-8", newline="\n").write(table_cpp)

print("rows:", len(ROWS), "languages:", len(LANGS))
print("Lang.h bytes:", len(header.encode("utf-8")))
print("LangTable.cpp bytes:", len(table_cpp.encode("utf-8")))
