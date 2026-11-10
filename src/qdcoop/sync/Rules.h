// QD Coop — co-op rules: while in a session the world never stops — no real pause and no
// "enemy freeze" time dilation. Outside a session every hook passes through, so the original behaviour returns by
// itself. Game thread only.
#pragma once

namespace qd::sync {

void RulesInit();       // registers hooks + the per-frame ticker
void RulesShutdown();

}  // namespace qd::sync
