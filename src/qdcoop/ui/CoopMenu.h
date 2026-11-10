// QD Coop — the F5 window's "联机 / 我的名字 / 调试" tabs.
#pragma once

namespace qd::ui {

// Game thread, after coop::Init(). Registers the tabs and a ticker that snapshots co-op state for the render thread.
void InitCoopMenu();
void ShutdownCoopMenu();

}  // namespace qd::ui
