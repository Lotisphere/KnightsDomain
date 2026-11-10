// QD Coop — enemy module bootstrap. Registers with core::OnReady during static initialization of QDCoop.dll.
// OnReady handlers run in registration order but static-init order across TUs is unspecified, so this handler only
// posts one more game-thread task: by the time it runs, every OnReady handler (including the ui/coop init) is done.
// Order: rules -> enemy sync -> summon tab.
#include "qdcoop/core/Core.h"
#include "qdcoop/core/Log.h"
#include "qdcoop/sync/EnemySync.h"
#include "qdcoop/sync/Rules.h"
#include "qdcoop/sync/Summon.h"

namespace qd::sync {
namespace {

void StartWo4Modules() {
    RulesInit();
    EnemySyncInit();
    SummonInit();
    QD_INFO("bootstrap", "enemy modules ready (rules, enemy sync, summon)");
}

struct Registrar {
    Registrar() {
        core::OnReady([] { gamethread::Post(&StartWo4Modules); });
    }
};
const Registrar g_registrar;

}  // namespace
}  // namespace qd::sync
