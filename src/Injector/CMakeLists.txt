add_executable(ge_inject
    main.cpp
    Injector.cpp
    PeExport.cpp
)

target_link_libraries(ge_inject PRIVATE ge_common)

# 注入器默认在自身目录找 GameEnhancement.dll，所以 DLL 必须先构建（并已落到 bin）。
add_dependencies(ge_inject GameEnhancement)

set_target_properties(ge_inject PROPERTIES OUTPUT_NAME "ge_inject")

if(MSVC)
    # 控制台程序：不要弹窗，但也不要 console 之外的子系统设置
    target_link_options(ge_inject PRIVATE /SUBSYSTEM:CONSOLE)
endif()
