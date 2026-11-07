cmake_minimum_required(VERSION 3.20)
project(Unlocker VERSION 0.9 LANGUAGES CXX)

if(NOT MSVC)
    message(FATAL_ERROR "Unlocker 只支持 MSVC（Win32 API / NTAPI）。")
endif()

option(UNLOCKER_BUILD_APP "构建主程序、右键扩展等完整交付物" ON)
# 单文件分发：把 MSVC 运行库静态链进产物，成品不依赖 vcruntime140/msvcp140
# 必须在任何 target 之前设置：unlocker_core 静态库和 exe 必须是同一套 CRT，否则 LNK2038
option(UNLOCKER_STATIC_CRT "静态链接 MSVC 运行库（单文件分发）" OFF)
if(UNLOCKER_STATIC_CRT)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

add_compile_definitions(
    UNICODE _UNICODE
    WIN32_LEAN_AND_MEAN NOMINMAX
    _WIN32_WINNT=0x0A00
    WINVER=0x0A00
)

# rc.exe 靠 include 路径找 Resource.rc 里引用的 unlocker.ico（工具生成，见 tools/make_icon.py）
include_directories("${CMAKE_CURRENT_SOURCE_DIR}/assets/icons")

add_compile_options(/utf-8 /W4 /permissive- /Zc:__cplusplus /EHsc)
add_compile_options($<$<CONFIG:Release>:/O2>)

set(UNLOCKER_COMMON_SRC
    src/common/NtApi.cpp
    src/common/StringUtil.cpp
    src/common/Log.cpp
    src/common/ErrorMap.cpp
    src/common/Lang.cpp
    src/common/LangTable.cpp
)

set(UNLOCKER_CORE_SRC
    src/core/Privilege.cpp
    src/core/PathResolver.cpp
    src/core/PolicyGuard.cpp
    src/core/HandleScanner.cpp
    src/core/LockBreaker.cpp
    src/core/FileOps.cpp
    src/core/RebootScheduler.cpp
)

set(UNLOCKER_IPC_SRC
    src/ipc/Json.cpp
    src/ipc/Protocol.cpp
    src/ipc/PipeClient.cpp
    src/ipc/PipeServer.cpp
    src/ipc/WorkerMain.cpp
)

set(UNLOCKER_LIB_SRC ${UNLOCKER_COMMON_SRC} ${UNLOCKER_CORE_SRC})
if(UNLOCKER_BUILD_APP)
    list(APPEND UNLOCKER_LIB_SRC ${UNLOCKER_IPC_SRC})
endif()

# ---------------------------------------------------------------- 核心静态库
add_library(unlocker_core STATIC ${UNLOCKER_LIB_SRC})
target_include_directories(unlocker_core PUBLIC src)
target_link_libraries(unlocker_core PUBLIC
    advapi32 shell32 shlwapi ole32 oleaut32 uuid comctl32 version psapi user32 gdi32 bcrypt
)

# 规则表 CRC 由构建期从 Blacklist.def 生成，用于 fail-closed 自检（4.6.6）
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/generated/BlacklistCrc.h
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_rules_crc.py
                ${CMAKE_CURRENT_SOURCE_DIR}/src/core/rules/Blacklist.def
                ${CMAKE_CURRENT_BINARY_DIR}/generated/BlacklistCrc.h
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/core/rules/Blacklist.def
                ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_rules_crc.py
        COMMENT "生成规则表 CRC（BlacklistCrc.h）"
        VERBATIM
    )
    add_custom_target(gen_rules_crc DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/generated/BlacklistCrc.h)
    add_dependencies(unlocker_core gen_rules_crc)
else()
    message(WARNING "未找到 Python3：跳过规则表 CRC 生成，本次构建无 fail-closed 自检值。")
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/generated)
    if(NOT EXISTS ${CMAKE_CURRENT_BINARY_DIR}/generated/BlacklistCrc.h)
        file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/generated/BlacklistCrc.h
             "#pragma once\n#define UNLOCKER_RULES_CRC 0u\n#define UNLOCKER_RULES_CRC_VALID 0\n")
    endif()
endif()
target_include_directories(unlocker_core PUBLIC ${CMAKE_CURRENT_BINARY_DIR}/generated)

# ---------------------------------------------------------------- 主程序
if(UNLOCKER_BUILD_APP)
    add_executable(Unlocker WIN32
        src/main.cpp
        src/ui/MainWindow.cpp
        src/ui/Theme.cpp
        src/ui/FilePicker.cpp
        src/ui/ElevationCheck.cpp
        src/ui/UiIpc.cpp
        src/ui/Resource.rc
        src/cli/CliMain.cpp
        src/shell/ShellIntegration.cpp
        src/app.manifest
    )
    # gdiplus：抗锯齿圆角；dwmapi：圆角/深色标题；uxtheme：深色滚动条（序号动态取，失败即降级）
    # shell32：SHChangeNotify；advapi32：右键菜单的 HKCU 注册表读写
    target_link_libraries(Unlocker PRIVATE unlocker_core gdiplus dwmapi uxtheme shell32 advapi32)
    # 版本号只有一个来源：project(Unlocker VERSION ...)，关于框与 CLI 都读这个宏
    target_compile_definitions(Unlocker PRIVATE UNLOCKER_VERSION_STR=L"${PROJECT_VERSION}")
    set_target_properties(Unlocker PROPERTIES
        OUTPUT_NAME "Unlocker"
        VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    )

    add_library(UnlockerShell SHARED src/shell/ContextMenu.cpp src/shell/ContextMenu.def)
    target_link_libraries(UnlockerShell PRIVATE shell32 shlwapi ole32 advapi32 user32 unlocker_core)
    target_compile_definitions(UnlockerShell PRIVATE UNLOCKER_SHELL_DLL)
    set_target_properties(UnlockerShell PROPERTIES OUTPUT_NAME "UnlockerShell")
endif()

# ---------------------------------------------------------------- 造锁工具
add_executable(MakeLock tools/MakeLock/MakeLock.cpp)
set_target_properties(MakeLock PROPERTIES OUTPUT_NAME "MakeLock")

# ---------------------------------------------------------------- 测试
add_executable(policy_tests tests/policy_tests.cpp)
target_link_libraries(policy_tests PRIVATE unlocker_core)
set_target_properties(policy_tests PROPERTIES OUTPUT_NAME "policy_tests")

# 子树占用回归（依赖 MakeLock.exe 当跨进程锁源，两者必须同目录）
add_executable(subtree_tests tests/subtree_tests.cpp)
target_link_libraries(subtree_tests PRIVATE unlocker_core)
add_dependencies(subtree_tests MakeLock)
set_target_properties(subtree_tests PROPERTIES OUTPUT_NAME "subtree_tests")

if(UNLOCKER_BUILD_APP)
    add_executable(ipc_tests tests/ipc_tests.cpp)
    target_link_libraries(ipc_tests PRIVATE unlocker_core)
    set_target_properties(ipc_tests PROPERTIES OUTPUT_NAME "ipc_tests")
    add_test(NAME ipc_tests COMMAND ipc_tests)
endif()

enable_testing()
add_test(NAME policy_tests COMMAND policy_tests)
add_test(NAME subtree_tests COMMAND subtree_tests)

if(UNLOCKER_BUILD_APP)
    install(TARGETS Unlocker MakeLock UnlockerShell RUNTIME DESTINATION .)
endif()
