cmake_minimum_required(VERSION 3.20)
project(reclient LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(miniz STATIC third_party/miniz-src/miniz.c)
target_include_directories(miniz PUBLIC third_party/miniz-src)
if(MSVC)
    target_compile_definitions(miniz PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

set(LUA_SOURCES
    lapi.c lauxlib.c lbaselib.c lcode.c ldblib.c ldebug.c ldo.c ldump.c lfunc.c lgc.c
    linit.c liolib.c llex.c lmathlib.c lmem.c loadlib.c lobject.c lopcodes.c loslib.c
    lparser.c lstate.c lstring.c lstrlib.c ltable.c ltablib.c ltm.c lundump.c lvm.c lzio.c
)
list(TRANSFORM LUA_SOURCES PREPEND third_party/lua-5.1.5/src/)
add_library(lua51 STATIC ${LUA_SOURCES})
target_include_directories(lua51 PUBLIC third_party/lua-5.1.5/src)
if(MSVC)
    target_compile_definitions(lua51 PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

add_library(reclient_core
    src/Sha1.cpp
    src/BigInt.cpp
    src/Srp6.cpp
    src/AuthCrypt.cpp
    src/Rc4.cpp
    src/Warden.cpp
    src/Socket.cpp
    src/World.cpp
    src/Client.cpp
    src/Mpq.cpp
    src/Formats.cpp
    src/FrameXml.cpp
    src/Sound.cpp
    src/Diag.cpp
)
target_include_directories(reclient_core PUBLIC src ${CMAKE_SOURCE_DIR}/third_party/minimp3)
target_link_libraries(reclient_core PUBLIC miniz lua51)
if(WIN32)
    target_link_libraries(reclient_core PUBLIC ws2_32)
endif()

set(SDL3_ROOT ${CMAKE_SOURCE_DIR}/third_party/sdl3/SDL3-3.4.18)
add_executable(reclient src/main.cpp src/App.cpp src/Scene.cpp)
target_include_directories(reclient PRIVATE ${SDL3_ROOT}/include)
target_link_libraries(reclient PRIVATE reclient_core ${SDL3_ROOT}/lib/x64/SDL3.lib opengl32)
add_custom_command(TARGET reclient POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SDL3_ROOT}/lib/x64/SDL3.dll $<TARGET_FILE_DIR:reclient>)

add_executable(reclient_tests tests/test_crypto.cpp)
target_link_libraries(reclient_tests PRIVATE reclient_core)
add_executable(reclient_mpq tests/test_mpq.cpp)
target_link_libraries(reclient_mpq PRIVATE reclient_core)
add_executable(reclient_formats tests/test_formats.cpp)
target_link_libraries(reclient_formats PRIVATE reclient_core)
add_executable(reclient_ui tests/test_ui.cpp)
target_link_libraries(reclient_ui PRIVATE reclient_core)

enable_testing()
add_test(NAME crypto COMMAND reclient_tests)
