#pragma once
// worker 侧：建管道、鉴权、分发（方案 4.10.3）。
// 安全边界：ACL 只放行 UI 侧用户 SID + worker 自身 SID，SACL 压到 Medium（功能必需）；
// 客户端三重身份校验；请求里的路径一律视为不可信输入，逐条重新归一化 + 重新裁决。
#include <windows.h>
#include <atomic>
#include <string>

#include "ipc/Protocol.h"

namespace ul {

class PipeServer {
public:
    // 返回进程退出码
    static int Run(const WorkerContext& ctx);

    // 供测试注入：跳过 UAC / 身份校验（仅 policy_tests 与自检使用）
    static void SetTestMode(bool on);
};

}  // namespace ul
