// Knights' Domain — extras of the co-op hub used by the menu; not part of the public Coop.h contract. Game thread only.
#pragma once
#include <string>

namespace qd::coop {

bool               SessionReady();          // Session::Init succeeded (and co-op was not disabled afterwards)
bool               CoopAllowed();           // SessionReady() && ownership check passed -> Host/Join may be used
bool               CoopDisabled();          // ownership check failed / Steam never came up: co-op off for this run
bool               UsingUdp();              // config transport == "udp" (always false in the release flavour)
int                ActiveSaveSlotCached();  // last polled save slot (0 when unknown / title screen)
const std::string& ProfileKey();            // "<SteamID64>" or "dev-profile-N"

}  // namespace qd::coop
