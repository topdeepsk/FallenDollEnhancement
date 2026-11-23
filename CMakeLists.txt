cmake_minimum_required(VERSION 3.20)

project(GameEnhancement VERSION 1.0.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "GameEnhancement 仅支持 Windows（依赖 Win32 注入与低级键盘钩子）。")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 静态链接 CRT：注入到游戏里的 DLL 不依赖 VC 运行库，
# 目标机器（哪怕是别的机器）不需要额外安装 Redistributable。
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# 所有产物统一落在 build/bin，exe 与 dll 相邻（注入器默认就在同目录找 DLL）。
set(GE_OUTPUT_DIR "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${GE_OUTPUT_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${GE_OUTPUT_DIR}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

# 多配置生成器（VS）需要逐配置指定，否则会落到 bin/Debug 子目录里。
foreach(_cfg ${CMAKE_CONFIGURATION_TYPES})
    string(TOUPPER "${_cfg}" _CFG)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${_CFG} "${GE_OUTPUT_DIR}")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${_CFG} "${GE_OUTPUT_DIR}")
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${_CFG} "${CMAKE_BINARY_DIR}/lib")
endforeach()

add_subdirectory(src/Common)
add_subdirectory(src/Dll)
add_subdirectory(src/Injector)
