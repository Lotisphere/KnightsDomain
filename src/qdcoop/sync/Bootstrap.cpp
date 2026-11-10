// Knights' Domain — module bootstrap (player sync, UI). Registers with core::OnReady during static initialization of KnightsDomain.dll,
// i.e. before the proxy calls KnightsDomain_Start(); the handler runs once on the game thread when the engine is ready.
// Order: overlay (so later failures can Toast) -> co-op hub -> menu tabs -> player sync.
#include "qdcoop/core/Core.h"
#include "qdcoop/core/Log.h"
#include "qdcoop/sync/Coop.h"
#include "qdcoop/sync/PlayerSync.h"
#include "qdcoop/ui/CoopMenu.h"
#include "qdcoop/ui/I18n.h"
#include "qdcoop/ui/Overlay.h"

namespace qd::sync {
namespace {

void StartWo3Modules() {
    ui::InitI18n();   // UI language before anything can Toast
    if (!ui::Init()) QD_ERROR("bootstrap", "overlay init failed - continuing without the F5 menu / toasts");
    coop::Init();
    ui::InitCoopMenu();
    PlayerSyncInit();
    QD_INFO("bootstrap", "player sync modules ready ({} {}, game build {})", core::kModName, core::kModVersion, core::GameBuild());
}

struct Registrar {
    Registrar() { core::OnReady(&StartWo3Modules); }
};
const Registrar g_registrar;

}  // namespace
}  // namespace qd::sync
