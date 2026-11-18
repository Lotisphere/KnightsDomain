// Knights' Domain — engine addresses found at run time.
//
// The SDK headers carry the addresses of a handful of engine symbols (the object array, FName::AppendString, GWorld,
// ...) as they were in the game build the SDK headers were made for. Every game update moves them, and with stale values
// the mod cannot even see the engine. ResolveEngineOffsets() looks each symbol up by a byte pattern in the game's code
// and overwrites the SDK value, so a game update that leaves the engine code itself alone keeps working.
//
// Call once, before the first SDK call (any thread; touches nothing but the SDK's offset table and the log).
#pragma once

namespace qd::core {

// True when every symbol was found by pattern. Symbols that were not keep the value from the SDK headers.
bool ResolveEngineOffsets();

}  // namespace qd::core
