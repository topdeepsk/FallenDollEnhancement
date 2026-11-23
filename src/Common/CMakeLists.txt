add_library(ge_common STATIC
    Win32Util.cpp
)

# 以 src 作为包含根：各处统一写 "Common/X.h"、"Dll/Y.h"，避免相对路径满天飞。
target_include_directories(ge_common PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_compile_features(ge_common PUBLIC cxx_std_17)

if(MSVC)
    # /utf-8：源码与中文字面量都按 UTF-8 处理，避免控制台乱码。
    target_compile_options(ge_common PUBLIC /utf-8 /W4 /permissive- /Zc:__cplusplus)
endif()
