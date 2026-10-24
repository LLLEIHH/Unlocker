cmake_minimum_required(VERSION 3.20)
project(Unlocker VERSION 0.8.0 LANGUAGES CXX)

if(NOT MSVC)
    message(FATAL_ERROR "Unlocker 只支持 MSVC（Win32 API / NTAPI）。")
endif()

option(UNLOCKER_BUILD_APP "构建主程序、右键扩展等完整交付物" ON)
option(UNLOCKER_BUILD_WINUI "构建 WinUI 3 版本（需先还原 winui/packages）" OFF)

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

add_compile_options(/utf-8 /W4 /permissive- /Zc:__cplusplus /EHsc)
add_compile_options($<$<CONFIG:Release>:/O2>)

set(UNLOCKER_COMMON_SRC
    src/common/NtApi.cpp
    src/common/StringUtil.cpp
    src/common/Log.cpp
    src/common/ErrorMap.cpp
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
        src/ui/FilePicker.cpp
        src/ui/ElevationCheck.cpp
        src/ui/UiIpc.cpp
        src/ui/Resource.rc
        src/cli/CliMain.cpp
        src/app.manifest
    )
    target_link_libraries(Unlocker PRIVATE unlocker_core)
    target_compile_definitions(Unlocker PRIVATE UNLOCKER_VERSION_STR=L"0.8.0")
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

if(UNLOCKER_BUILD_APP)
    add_executable(ipc_tests tests/ipc_tests.cpp)
    target_link_libraries(ipc_tests PRIVATE unlocker_core)
    set_target_properties(ipc_tests PROPERTIES OUTPUT_NAME "ipc_tests")
    add_test(NAME ipc_tests COMMAND ipc_tests)
endif()

enable_testing()
add_test(NAME policy_tests COMMAND policy_tests)

if(UNLOCKER_BUILD_APP)
    install(TARGETS Unlocker MakeLock UnlockerShell RUNTIME DESTINATION .)
endif()

# ---------------------------------------------------------------- WinUI 3 版本
# 说明：本机的安全策略不允许直接执行 MSBuild.exe，所以这里用 CMake 起一个 custom target
# 代为调用（与 VS 生成器内部调用 MSBuild 是同一条路径）。
if(UNLOCKER_BUILD_WINUI)
    set(MSBUILD_EXE
        "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/MSBuild/Current/Bin/MSBuild.exe"
        CACHE FILEPATH "MSBuild 可执行文件")
    # MSBUILD_EXE 三级探测：默认 BuildTools → Community → vswhere 动态定位。
    # 缓存值无效时依次回退，全部落空则显式报错（fail loud）。
    if(NOT EXISTS "${MSBUILD_EXE}")
        set(_msb "C:/Program Files/Microsoft Visual Studio/2022/Community/MSBuild/Current/Bin/MSBuild.exe")
        if(NOT EXISTS "${_msb}")
            find_program(_vswhere vswhere
                PATHS "C:/Program Files (x86)/Microsoft Visual Studio/Installer"
                NO_DEFAULT_PATH)
            if(_vswhere)
                execute_process(
                    COMMAND "${_vswhere}" -latest -products *
                            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64
                            -property installationPath
                    OUTPUT_VARIABLE _vsroot
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET)
                if(_vsroot)
                    set(_msb "${_vsroot}/MSBuild/Current/Bin/MSBuild.exe")
                endif()
            endif()
        endif()
        if(EXISTS "${_msb}")
            set(MSBUILD_EXE "${_msb}" CACHE FILEPATH "MSBuild 可执行文件" FORCE)
        else()
            message(FATAL_ERROR
                "找不到 MSBuild.exe：请安装 VS 2022（BuildTools 或 Community，含 C++ 工作负载），"
                "或用 -DMSBUILD_EXE=<路径> 显式指定。")
        endif()
    endif()
    message(STATUS "winui: MSBUILD_EXE = ${MSBUILD_EXE}")
    add_custom_target(winui
        COMMAND "${MSBUILD_EXE}" "${CMAKE_CURRENT_SOURCE_DIR}/winui/UnlockerApp.vcxproj"
                /p:Configuration=Release /p:Platform=x64 /m /nologo /v:m
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        USES_TERMINAL
        VERBATIM)
    add_dependencies(winui unlocker_core)

    # 没有 VS 的 UWP 工作负载时，C++ 的 XAML 构建规则（Microsoft.Windows.UI.Xaml.Cpp.Targets）缺失，
    # MarkupCompilePass1 不会自动挂到编译流程里。这里显式单独跑一遍生成 .g.h/.g.cpp。
    add_custom_target(winui_xaml
        COMMAND "${MSBUILD_EXE}" "${CMAKE_CURRENT_SOURCE_DIR}/winui/UnlockerApp.vcxproj"
                /t:MarkupCompilePass1 /p:Configuration=Release /p:Platform=x64 /nologo /v:m
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        USES_TERMINAL
        VERBATIM)
endif()
