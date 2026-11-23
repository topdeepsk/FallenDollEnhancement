add_library(GameEnhancement SHARED
    dllmain.cpp
    Config.cpp
    Player.cpp
    KeyHook.cpp
)

target_link_libraries(GameEnhancement PRIVATE
    ge_common
    user32      # SetWindowsHookEx / UnhookWindowsHookEx
)

target_compile_definitions(GameEnhancement PRIVATE GE_BUILD_DLL=1)

set_target_properties(GameEnhancement PROPERTIES
    PREFIX ""
    OUTPUT_NAME "GameEnhancement"
)
