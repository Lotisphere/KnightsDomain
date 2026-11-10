// Knights' Domain — update check and in-place update.
//
// Start() asks a small JSON manifest for the latest version on a background thread. Players whose UI language is
// Chinese ask the kd.mangolotis.com mirror first and GitHub second; everyone else GitHub first and the mirror second.
// When a newer version exists the F5 menu offers it; BeginUpdate() downloads the two mod DLLs, checks each file's
// SHA-256 against the manifest, and swaps them in (the running DLLs are renamed to *.old — Windows allows renaming a
// loaded DLL — and the new ones take effect on the next launch). Nothing is downloaded or changed without the click.
//
// Only HTTPS, only the two known hosts for the manifest, only the two known file names are ever written, and only
// inside the game's Binaries\Win64 folder.
#pragma once
#include <string>

namespace qd::updater {

enum class State { Idle, Checking, UpToDate, Available, Downloading, Installed, Failed };

struct Status {
    State       state = State::Idle;
    std::string latest;     // version in the manifest (Available / Downloading / Installed)
    std::string error;      // short English reason (Failed) — also in the log
    int         percent = 0;   // Downloading
};

void   Start();          // once; safe to call again (no-op)
Status Get();            // thread-safe
void   BeginUpdate();    // only acts in State::Available or after a failed download

// "What's new" notice: shown once per mod version (a marker file in the mod's Saved folder).
bool NoticeSeen();
void MarkNoticeSeen();

// a < b for dotted numeric versions ("1.0.3" < "1.0.10"). Exposed for the unit test.
bool VersionLess(const std::string& a, const std::string& b);

}  // namespace qd::updater
