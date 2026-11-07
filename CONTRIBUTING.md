# 贡献指南

## 环境

| 依赖 | 版本 | 备注 |
| --- | --- | --- |
| Visual Studio 2022 | Community 即可 | 需要「使用 C++ 的桌面开发」工作负载 |
| CMake | ≥ 3.20 | `build.cmd` 会用 PATH 里的 cmake，也会探测 VS 自带的 |
| Windows SDK | 10.0.26100 | 清单、资源脚本与清单工具需要 |
| Python | 3.8+ | 规则表 CRC、图标生成、语言表生成都用它，无第三方依赖 |

## 构建与测试

```bat
build.cmd             :: 常规构建（动态 CRT）+ 单元测试
build.cmd single      :: 单文件版（静态 CRT）
```

改完代码至少跑一遍：

```bat
build\Release\policy_tests.exe
build\Release\ipc_tests.exe
build\Release\subtree_tests.exe
```

## 代码约定

这些是踩过坑之后定下来的，**别改回去**（README 实现细节章节有原因）：

1. `src/core` 不依赖 UI、不依赖 IPC；`src/ipc` 只搬数据不做决策。新增逻辑先想清楚该放哪层
2. 文案一律走 `ul::T(Str::X)` / `ul::Tf(Str::X, ...)`：先在 `tools/gen_lang.py` 的 `ROWS` 里加一行（5 个语种），
   再执行 `python tools/gen_lang.py` 生成 `LangTable.cpp`，不要直接写裸字符串，也不要在窗口代码里堆字面量
3. 尺寸一律走 `theme::Scale(96 DPI 逻辑值)`，不要写死像素；新控件要在 `ApplyThemeChange()` 里能跟着换主题重绘
4. 新增文件要同时加到 `CMakeLists.txt`
5. 批处理文件保持 CRLF（`.gitattributes` 已钉死）

## 提交

- 一个提交做一件事，提交信息首行用 `feat:` / `fix:` / `docs:` / `chore:` 前缀
- 改行为的提交请在正文说明「为什么」，不是「改了什么」——后者看 diff 就行
- 涉及删除/提权路径的改动，请在 PR 描述里写明你如何验证（最好附一份测试用例）

## PR

- 小而聚焦，别顺手重构无关文件
- 新增功能请补测试；`tests/` 下是自带的轻量框架，看 `policy_tests.cpp` 开头即可
- 涉及 UI 的改动麻烦附一张截图（中英各一张更好）

## 发布

发布完全由 CI 完成，本地不需要手工打包：

```bash
git tag -a v0.9.0 -m "v0.9.0"
git push origin v0.9.0        # 触发 release.yml：跑测试 → 编静态 CRT 单文件版 → 建 Release
```

`build.yml` 在每次 push / PR 时跑构建与三套单元测试；
`release.yml` 在此之上再编一次静态 CRT 单文件版，产物是 `Unlocker-Single.exe`。

改完 `.github/workflows/` 之后，可以用 API 空跑一次验证语法（不会建 Release）：

```bash
curl -X POST -H "Authorization: Bearer <token>" \
  https://api.github.com/repos/LLLEIHH/Unlocker/actions/workflows/<id>/dispatches \
  -d '{"ref":"main"}'
```

工作流文件被 GitHub 判无效时，run 不会产生任何 job、名字直接显示文件路径——
这种时候上面的接口会返回 422 并在 `message` 里给出具体行号，比翻网页快。

## 安全问题

见 [SECURITY.md](SECURITY.md)，不要开公开 issue。
