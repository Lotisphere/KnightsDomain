# Knights' Domain — source

Source code of the two DLLs that make up Knights' Domain. Release builds are published at
[Lotisphere/KnightsDomain](https://github.com/Lotisphere/KnightsDomain).

## Layout

| Path | Contents |
| --- | --- |
| `src/proxy` | Loader DLL. Forwards the XInput exports to the system library and loads the mod DLL from a worker thread. |
| `src/qdcoop/core` | Start-up, configuration, logging, function hooks, game-thread task queue, engine helpers, Steam ownership check, updater. |
| `src/qdcoop/net` | Session and peer liveness, wire protocol codec, Steam transport, UDP loopback transport for development. No engine dependency. |
| `src/qdcoop/sync` | State replication built on the net layer: sampling, snapshots, interpolation, ownership hand-over. |
| `src/qdcoop/ui` | Overlay rendering and input capture, menu pages, string tables (English, Chinese, Japanese). |
| `src/qdcoop/debug` | Development-only command file and smoke test. Not compiled into release builds. |
| `src/tests` | Unit tests for the net layer and the motion math. No test framework. |
| `vendor` | Third-party libraries, each under its own licence (see the licence file in each folder). |

## Technology

- C++23, MSVC, CMake. Static CRT, so the DLLs need no runtime redistributable.
- [MinHook](https://github.com/TsudaKageyu/minhook) for inline function hooks.
- [Dear ImGui](https://github.com/ocornut/imgui) with the Direct3D 11 and Win32 backends for the overlay, drawn from
  hooks on `IDXGISwapChain::Present` and `ResizeBuffers`.
- Steamworks lobbies and `ISteamNetworkingMessages` through the flat C API, resolved at run time from the
  `steam_api64.dll` the game already ships. The Steamworks SDK is not linked.
- A binary little-endian wire protocol with a version handshake, reliable and unreliable channels, and a snapshot
  rate that adapts to how much has changed.
- WinHTTP and BCrypt (SHA-256) for the update check and download. No third-party network or crypto library.
- WinTrust for the Authenticode check of `steam_api64.dll`.
- [nlohmann/json](https://github.com/nlohmann/json) for the configuration file and the update manifest.

## Building

Requirements: Visual Studio 2022 with the C++ workload, CMake 3.25 or newer, and the Windows 10/11 SDK.

The build also needs C++ SDK headers for the game. They are not part of this repository. Pass their location with
`QD_SDK` (the folder that contains `SDK.hpp` and the `SDK` directory); without it the build looks in `sdk/CppSDK`
next to `src`.

```
cmake -S src -B build -G "Visual Studio 17 2022" -A x64 -DQD_SDK=<path to the SDK headers>
cmake --build build --config Release
```

Add `-DQD_DEV_BUILD=ON` when configuring for the development flavour, which enables the debug console, the
development commands and the UDP loopback transport.

Outputs in `build/Release`:

- `xinput1_3.dll` — the loader
- `KnightsDomain.dll` — the mod
- `qdnet_tests.exe`, `qdmotion_tests.exe` — unit tests
