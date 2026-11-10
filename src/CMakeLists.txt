# Knights' Domain (internal codename QD Coop) build.
# Configure (release flavour, the default):  cmake -S src -B build -G "Visual Studio 17 2022" -A x64
# Configure (dev flavour):                   cmake -S src -B build-dev -G "Visual Studio 17 2022" -A x64 -DQD_DEV_BUILD=ON
# Build:                                     cmake --build build --config Release
cmake_minimum_required(VERSION 3.25)
project(QDCoop LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(QD_ROOT    ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(QD_VENDOR  ${QD_ROOT}/vendor)
set(QD_SDK     ${QD_ROOT}/sdk/CppSDK CACHE PATH "Folder with the game's C++ SDK headers (not part of this repository)")

# build flavour. OFF = public release (no ~ console, no dev commands, no UDP transport, no -qdcoop-* switches,
# Steam ownership check enforced). ON = developer build (everything enabled). Passed to the code as QD_DEV=1/0.
option(QD_DEV_BUILD "Developer build (~ console, dev commands, UDP transport, -qdcoop-* switches)" OFF)
if (QD_DEV_BUILD)
    set(QD_DEV_VALUE 1)
    message(STATUS "Knights' Domain: DEV build (QD_DEV=1)")
else()
    set(QD_DEV_VALUE 0)
    message(STATUS "Knights' Domain: RELEASE build (QD_DEV=0)")
endif()

add_compile_options(/utf-8 /MP /bigobj /permissive- /W3 /wd4244 /wd4267 /wd4018 /wd4005 /Zi)
add_link_options(/DEBUG /OPT:REF /OPT:ICF)   # PDBs for crash triage; not shipped
add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)

# ---------- third party ----------
add_library(minhook STATIC
    ${QD_VENDOR}/minhook/src/buffer.c ${QD_VENDOR}/minhook/src/hook.c ${QD_VENDOR}/minhook/src/trampoline.c
    ${QD_VENDOR}/minhook/src/hde/hde64.c)
target_include_directories(minhook PUBLIC ${QD_VENDOR}/minhook/include)

set(IMGUI ${QD_VENDOR}/imgui)
add_library(imgui STATIC
    ${IMGUI}/imgui.cpp ${IMGUI}/imgui_draw.cpp ${IMGUI}/imgui_tables.cpp ${IMGUI}/imgui_widgets.cpp
    ${IMGUI}/backends/imgui_impl_dx11.cpp ${IMGUI}/backends/imgui_impl_win32.cpp)
target_include_directories(imgui PUBLIC ${IMGUI} ${IMGUI}/backends)
# d3dcompiler.lib of the Windows 10/11 SDK is the import library of d3dcompiler_47.dll (shipped with Windows 10+).
target_link_libraries(imgui PUBLIC d3d11 dxgi d3dcompiler)

# ---------- net: no SDK, no game dependency ----------
file(GLOB QD_NET_SRC CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/qdcoop/net/*.cpp)

# Net library linked into the mod DLL. Release flavour: UdpTransport.cpp is not even compiled (Session.cpp does not
# reference it when QD_DEV=0, so a stray reference would be a link error instead of a silent dev feature).
set(QD_NET_MOD_SRC ${QD_NET_SRC})
if (NOT QD_DEV_BUILD)
    list(FILTER QD_NET_MOD_SRC EXCLUDE REGEX "/qdcoop/net/UdpTransport\\.cpp$")
endif()
add_library(qdcoop_net STATIC ${QD_NET_MOD_SRC} ${CMAKE_CURRENT_SOURCE_DIR}/qdcoop/core/Log.cpp)
target_include_directories(qdcoop_net PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${QD_VENDOR})
target_compile_definitions(qdcoop_net PUBLIC QD_DEV=${QD_DEV_VALUE})
if (QD_DEV_BUILD)
    target_link_libraries(qdcoop_net PUBLIC ws2_32)
endif()

# The net unit tests drive two Sessions over the UDP loopback transport, so they always link a dev-flavour copy of
# the net library (in the dev flavour that is qdcoop_net itself).
file(GLOB QD_NET_TEST_SRC CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/net/*.cpp)
if (QD_NET_TEST_SRC)
    if (QD_DEV_BUILD)
        set(QD_NET_TEST_LIB qdcoop_net)
    else()
        add_library(qdcoop_net_devtest STATIC ${QD_NET_SRC} ${CMAKE_CURRENT_SOURCE_DIR}/qdcoop/core/Log.cpp)
        target_include_directories(qdcoop_net_devtest PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${QD_VENDOR})
        target_compile_definitions(qdcoop_net_devtest PUBLIC QD_DEV=1)
        target_link_libraries(qdcoop_net_devtest PUBLIC ws2_32)
        set(QD_NET_TEST_LIB qdcoop_net_devtest)
    endif()
    add_executable(qdnet_tests ${QD_NET_TEST_SRC})
    target_link_libraries(qdnet_tests PRIVATE ${QD_NET_TEST_LIB})
endif()

# ---------- main mod DLL ----------
file(GLOB_RECURSE QD_MOD_SRC CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/qdcoop/*.cpp)
list(FILTER QD_MOD_SRC EXCLUDE REGEX "/qdcoop/net/|/qdcoop/core/Log\\.cpp$")
if (NOT QD_DEV_BUILD)
    # dev command file / ~ console commands and the M0 smoke test are not part of the release.
    list(FILTER QD_MOD_SRC EXCLUDE REGEX "/qdcoop/debug/(DevCommands|SmokeTest)\\.cpp$")
endif()
option(QD_EXCLUDE_ENEMY_SYNC "Temporarily leave enemy sync out of the build" OFF)
if (QD_EXCLUDE_ENEMY_SYNC)
    list(FILTER QD_MOD_SRC EXCLUDE REGEX "/sync/(EnemySync|Rules|Summon|EnemyBootstrap)")
endif()
# SDK translation units actually used (append when new SDK functions are called)
include(${CMAKE_CURRENT_SOURCE_DIR}/sdk_units.cmake OPTIONAL)
add_library(QDCoop SHARED ${QD_MOD_SRC} ${QD_SDK_UNITS})
# the mod DLL ships as KnightsDomain\KnightsDomain.dll (the target keeps its internal name).
set_target_properties(QDCoop PROPERTIES OUTPUT_NAME KnightsDomain)
target_include_directories(QDCoop PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${QD_VENDOR} ${QD_SDK})
target_compile_definitions(QDCoop PRIVATE QD_DEV=${QD_DEV_VALUE})
# wintrust + crypt32: Authenticode check of steam_api64.dll (core/Ownership.cpp).
target_link_libraries(QDCoop PRIVATE qdcoop_net minhook imgui wintrust crypt32 winhttp bcrypt)

# ---------- xinput1_3 proxy loader ----------
file(GLOB QD_PROXY_SRC CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/proxy/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/proxy/*.def)
if (QD_PROXY_SRC)
    add_library(xinput1_3 SHARED ${QD_PROXY_SRC})
    set_target_properties(xinput1_3 PROPERTIES OUTPUT_NAME xinput1_3)
    target_compile_definitions(xinput1_3 PRIVATE QD_DEV=${QD_DEV_VALUE})
endif()

# ---------- motion math tests ----------
file(GLOB QD_MOTION_TEST_SRC CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/motion/*.cpp)
if (QD_MOTION_TEST_SRC)
    add_executable(qdmotion_tests ${QD_MOTION_TEST_SRC} ${CMAKE_CURRENT_SOURCE_DIR}/qdcoop/sync/AttackMotion.cpp)
    target_include_directories(qdmotion_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(qdmotion_tests PRIVATE QD_DEV=${QD_DEV_VALUE})
endif()
