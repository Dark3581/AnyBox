add_library(gameinput_core STATIC Log.cpp Gamepad.cpp Injection.cpp Sources.cpp Objects.cpp)
target_include_directories(gameinput_core PUBLIC ${ANYBOX_LIBS_INCLUDE})
target_link_libraries(gameinput_core PUBLIC user32)

anybox_add_xdll(gameinput SOURCES GameInput.cpp LINK gameinput_core)
