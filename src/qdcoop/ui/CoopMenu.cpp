// Knights' Domain — F5 window tabs: Co-op / My Name / About (+ the dev-only debug page).
// Draw callbacks run on the render thread: they only read a snapshot taken on the game thread (10 Hz ticker) and
// gamethread::Post() every action.
#include "qdcoop/ui/CoopMenu.h"

#include "qdcoop/core/Config.h"
#include "qdcoop/core/Core.h"
#include "qdcoop/core/Hooks.h"
#include "qdcoop/core/Log.h"
#include "qdcoop/core/Ownership.h"
#include "qdcoop/core/Updater.h"
#include "qdcoop/sync/Coop.h"
#include "qdcoop/sync/CoopInternal.h"
#include "qdcoop/sync/PlayerSync.h"
#include "qdcoop/sync/Profiles.h"
#include "qdcoop/sync/Puppet.h"
#include "qdcoop/ui/I18n.h"
#include "qdcoop/ui/Overlay.h"

#include "imgui.h"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace qd::ui {
namespace {

constexpr float kSnapshotInterval = 0.1f;

const ImVec4 kColOk(0.55f, 0.86f, 0.62f, 1.0f);
const ImVec4 kColWarn(0.96f, 0.77f, 0.38f, 1.0f);
const ImVec4 kColErr(0.93f, 0.42f, 0.45f, 1.0f);
const ImVec4 kColPink(0.949f, 0.655f, 0.765f, 1.0f);
const ImVec4 kColGold(1.0f, 0.843f, 0.47f, 1.0f);

struct MemberRow {
    uint8_t     slot = 0;
    std::string name;
    int         ping = -1;
    bool        isLocal = false;
};

struct Snapshot {
    bool                       taken = false;
    bool                       sessionOk = false;
    bool                       coopAllowed = false;    // session up + ownership check passed
    bool                       coopDisabled = false;   // ownership check failed
    core::Ownership            ownership = core::Ownership::NotChecked;
    bool                       udp = false;
    bool                       isHost = true;
    bool                       inSession = false;
    net::SessionState          state = net::SessionState::Idle;
    std::string                lastError;
    std::vector<MemberRow>     members;
    std::vector<net::LobbyInfo> lobbies;
    uint64_t                   pendingInvite = 0;
    std::string                localName;
    int                        saveSlot = 0;
    std::string                profileKey;
};

std::mutex           g_mx;
Snapshot             g_snap;
gamethread::TickerId g_ticker = 0;
float                g_accum = 0;

// ------------------------------------------------------------------ game thread

void TakeSnapshot() {
    Snapshot s;
    s.taken = true;
    s.sessionOk = coop::SessionReady();
    s.coopAllowed = coop::CoopAllowed();
    s.coopDisabled = coop::CoopDisabled();
    s.ownership = core::OwnershipState();
    s.udp = coop::UsingUdp();
    s.isHost = coop::IsHost();
    s.inSession = coop::InSession();
    s.localName = coop::LocalName();
    s.saveSlot = coop::ActiveSaveSlotCached();
    s.profileKey = coop::ProfileKey();
    if (s.sessionOk) {
        net::Session& sess = coop::Session();
        s.state = sess.State();
        s.lastError = sess.LastError();
        s.lobbies = sess.FriendLobbies();
        s.pendingInvite = sess.PendingInviteLobby();
        net::ITransport* t = sess.Transport();
        for (const auto& m : coop::Members()) {
            if (!m.present) continue;
            MemberRow r;
            r.slot = m.slot;
            r.name = m.name;
            r.isLocal = m.isLocal;
            const bool reachable = s.isHost || m.slot == 0;   // clients only have a link to the host
            if (!m.isLocal && reachable && t) r.ping = t->PingMs(m.peer);
            s.members.push_back(std::move(r));
        }
    } else {
        for (const auto& m : coop::Members()) {
            if (!m.present) continue;
            MemberRow r;
            r.slot = m.slot;
            r.name = m.name;
            r.isLocal = m.isLocal;
            s.members.push_back(std::move(r));
        }
    }
    std::lock_guard lk(g_mx);
    g_snap = std::move(s);
}

// ------------------------------------------------------------------ render thread

Snapshot Copy() {
    std::lock_guard lk(g_mx);
    return g_snap;
}

bool IsIdleLike(net::SessionState s) { return s == net::SessionState::Idle || s == net::SessionState::Failed; }

// English / 中文, applied at once (render thread reads the atomic) and saved on the game thread.
void DrawLanguageSwitch() {
    const Lang cur = GetLang();
    ImGui::TextDisabled("%s:", Tr("coop.language"));
    ImGui::SameLine();
    if (ImGui::RadioButton("English##lang_en", cur == Lang::En) && cur != Lang::En) {
        SetLang(Lang::En);
        gamethread::Post([] { SetLangAndSave(Lang::En); });
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("中文##lang_zh", cur == Lang::Zh) && cur != Lang::Zh) {
        SetLang(Lang::Zh);
        gamethread::Post([] { SetLangAndSave(Lang::Zh); });
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("日本語##lang_ja", cur == Lang::Ja) && cur != Lang::Ja) {
        SetLang(Lang::Ja);
        gamethread::Post([] { SetLangAndSave(Lang::Ja); });
    }
}

void StatusLine(const Snapshot& s) {
    std::string text;
    ImVec4 col = kColPink;
    switch (s.state) {
        case net::SessionState::Idle:          text = Tr("coop.state.idle"); col = ImVec4(0.75f, 0.72f, 0.76f, 1.0f); break;
        case net::SessionState::CreatingLobby: text = Tr("coop.state.creating"); col = kColWarn; break;
        case net::SessionState::Hosting:
            text = s.inSession ? TrF("coop.state.hosting_n", s.members.size()) : std::string(Tr("coop.state.hosting_wait"));
            col = kColGold;
            break;
        case net::SessionState::JoiningLobby:
        case net::SessionState::Connecting:    text = Tr("coop.state.joining"); col = kColWarn; break;
        case net::SessionState::Connected:
            text = s.inSession ? Tr("coop.state.connected") : Tr("coop.state.handshake");
            col = kColOk;
            break;
        case net::SessionState::Failed:
            text = TrF("coop.state.failed", s.lastError.empty() ? std::string(Tr("common.unknown_error")) : s.lastError);
            col = kColErr;
            break;
    }
    ImGui::TextUnformatted(Tr("coop.status"));
    ImGui::SameLine(0, 0);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(col, "%s", text.c_str());
    ImGui::PopTextWrapPos();
#if QD_DEV
    ImGui::TextDisabled("传输：%s", s.udp ? "UDP（开发用）" : "Steam");
#endif
}

void DrawMembers(const Snapshot& s) {
    ImGui::SeparatorText(Tr("coop.members"));
    if (ImGui::BeginTable("##members", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn(Tr("coop.col.slot"), ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn(Tr("coop.col.name"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(Tr("coop.col.ping"), ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableHeadersRow();
        for (const auto& m : s.members) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (m.slot == 0) ImGui::TextColored(kColGold, "%d %s", m.slot, Tr("coop.host_tag"));
            else ImGui::Text("%d", m.slot);
            ImGui::TableNextColumn();
            if (m.isLocal) ImGui::TextColored(kColPink, "%s", TrF("coop.you", m.name).c_str());
            else ImGui::TextUnformatted(m.name.c_str());
            ImGui::TableNextColumn();
            if (m.isLocal) ImGui::TextDisabled("-");
            else if (m.ping < 0) ImGui::TextDisabled("?");
            else ImGui::TextColored(m.ping < 80 ? kColOk : (m.ping < 180 ? kColWarn : kColErr), "%d ms", m.ping);
        }
        ImGui::EndTable();
    }
}

void DrawFriendLobbies(const Snapshot& s) {
    ImGui::SeparatorText(Tr("coop.friends"));
#if QD_DEV
    if (s.udp) {
        ImGui::TextDisabled("UDP 开发模式下没有好友房间列表，请到「调试」页加入。");
        return;
    }
#endif
    const bool idle = IsIdleLike(s.state);
    ImGui::BeginDisabled(!s.coopAllowed);
    if (ImGui::Button(Tr("coop.refresh"))) gamethread::Post([] {
            if (coop::CoopAllowed()) coop::Session().RefreshFriendLobbies();
        });
    ImGui::EndDisabled();

    if (s.pendingInvite != 0 && idle && s.coopAllowed) {
        ImGui::SameLine();
        ImGui::TextColored(kColPink, "%s", Tr("coop.invite"));
        ImGui::SameLine();
        const uint64_t id = s.pendingInvite;
        if (ImGui::SmallButton(Tr("coop.accept"))) gamethread::Post([id] {
                if (coop::CoopAllowed()) coop::Session().JoinLobby(id);
            });
    }

    if (s.lobbies.empty()) {
        ImGui::TextDisabled("%s", Tr("coop.no_lobbies"));
        return;
    }
    if (ImGui::BeginTable("##lobbies", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn(Tr("coop.col.host"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(Tr("coop.col.players"), ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        for (const auto& l : s.lobbies) {
            ImGui::PushID(static_cast<int>(l.lobbyId & 0x7FFFFFFF));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(l.hostName.empty() ? Tr("coop.unknown_host") : l.hostName.c_str());
            const bool versionOk = l.modVersion == core::kModVersion && l.modAuthor == core::kModAuthor &&
                                   l.gameBuild == core::GameBuild();
            if (!versionOk) {
                ImGui::SameLine();
                ImGui::TextColored(kColErr, "%s", Tr("coop.version_diff"));
            }
            ImGui::TableNextColumn();
            ImGui::Text("%d/%d", l.members, l.maxMembers);
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(!s.coopAllowed || !idle || !versionOk || l.members >= l.maxMembers);
            const uint64_t id = l.lobbyId;
            if (ImGui::SmallButton(Tr("coop.join"))) gamethread::Post([id] {
                    if (coop::CoopAllowed()) coop::Session().JoinLobby(id);
                });
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

// "a newer version exists" banner with the in-place update button.
void DrawUpdateBanner() {
    const updater::Status u = updater::Get();
    if (u.state != updater::State::Available && u.state != updater::State::Downloading &&
        u.state != updater::State::Installed && !(u.state == updater::State::Failed && !u.latest.empty()))
        return;
    ImGui::PushTextWrapPos(0.0f);
    switch (u.state) {
        case updater::State::Available:
            ImGui::TextColored(kColWarn, "%s", TrF("update.available", u.latest, core::kModVersion).c_str());
            if (ImGui::Button(Tr("update.button"))) updater::BeginUpdate();
            break;
        case updater::State::Downloading:
            ImGui::TextColored(kColWarn, "%s", TrF("update.downloading", u.percent).c_str());
            break;
        case updater::State::Installed:
            ImGui::TextColored(kColWarn, "%s", TrF("update.installed", u.latest).c_str());
            break;
        default:
            ImGui::TextColored(kColErr, "%s", TrF("update.failed", u.error).c_str());
            if (ImGui::Button(Tr("update.retry"))) updater::BeginUpdate();
            break;
    }
    ImGui::PopTextWrapPos();
    ImGui::Separator();
}

// what's-new notice, shown until the player dismisses it once for this version.
void DrawNotice() {
    static int seen = -1;   // -1 unknown, 0 show, 1 dismissed
    if (seen < 0) seen = updater::NoticeSeen() ? 1 : 0;
    if (seen != 0) return;
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(kColWarn, "%s", TrF("notice.title", core::kModVersion).c_str());
    ImGui::TextUnformatted(Tr("notice.thanks"));
    for (const char* key : {"notice.1", "notice.2", "notice.3", "notice.4", "notice.5", "notice.6"})
        ImGui::TextUnformatted(Tr(key));
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    // The closing line stands out: framed and coloured.
    ImGui::PushStyleColor(ImGuiCol_Border, kColWarn);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.5f);
    if (ImGui::BeginChild("##notice_disclaimer", ImVec2(0, 0), ImGuiChildFlags_Border | ImGuiChildFlags_AutoResizeY)) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(kColWarn, "%s", Tr("notice.disclaimer"));
        ImGui::PopTextWrapPos();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    if (ImGui::Button(Tr("notice.ok"))) {
        seen = 1;
        updater::MarkNoticeSeen();
    }
    ImGui::Separator();
}

void DrawCoopTab() {
    DrawLanguageSwitch();
    ImGui::Separator();
    DrawUpdateBanner();
    DrawNotice();

    const Snapshot s = Copy();
    if (!s.taken) {
        ImGui::TextDisabled("%s", Tr("coop.loading"));
        return;
    }
    ImGui::PushTextWrapPos(0.0f);
    if (s.coopDisabled || s.ownership == core::Ownership::Failed) {
        ImGui::TextColored(kColErr, "%s", Tr("own.failed"));   // red text at the top of the co-op page
    } else if (!s.sessionOk) {
        ImGui::TextColored(kColWarn, "%s", Tr("coop.starting"));
    } else if (!s.coopAllowed) {
        ImGui::TextColored(kColWarn, "%s", Tr("coop.verifying"));
    }
    ImGui::PopTextWrapPos();
    StatusLine(s);

    const bool idle = IsIdleLike(s.state);
    ImGui::Spacing();
    ImGui::BeginDisabled(!s.coopAllowed || !idle);
    if (ImGui::Button(Tr("coop.host"))) gamethread::Post([] {
            if (coop::CoopAllowed()) coop::Session().Host();
        });
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!s.sessionOk || s.state == net::SessionState::Idle);
    if (ImGui::Button(Tr("coop.leave"))) {
        gamethread::Post([] {
            if (!coop::SessionReady()) return;
            coop::Session().Leave();
            ui::Toast(Tr("toast.left_session"), ui::ToastKind::Info);
        });
    }
    ImGui::EndDisabled();

    DrawFriendLobbies(s);
    DrawMembers(s);
}

// ---- My Name

char        g_nameBuf[net::kMaxNameBytes + 1] = {};
std::string g_nameSynced;
bool        g_nameDirty = false;

int NameEditCallback(ImGuiInputTextCallbackData* d) {
    if (d->EventFlag != ImGuiInputTextFlags_CallbackEdit) return 0;
    size_t cps = 0;
    for (int i = 0; i < d->BufTextLen; ++i) {
        if ((static_cast<unsigned char>(d->Buf[i]) & 0xC0) == 0x80) continue;
        if (cps == profiles::kMaxNameChars) {
            d->DeleteChars(i, d->BufTextLen - i);
            break;
        }
        ++cps;
    }
    return 0;
}

void DrawNameTab() {
    const Snapshot s = Copy();
    if (!g_nameDirty && g_nameSynced != s.localName) {
        g_nameSynced = s.localName;
        std::snprintf(g_nameBuf, sizeof(g_nameBuf), "%s", s.localName.c_str());
    }
    ImGui::TextUnformatted(Tr("name.current"));
    ImGui::SameLine(0, 0);
    ImGui::TextColored(kColPink, "%s", s.localName.c_str());
    ImGui::TextDisabled("%s", TrF("name.slot", s.saveSlot).c_str());
    ImGui::Spacing();

    ImGui::SetNextItemWidth(320.0f);
    if (ImGui::InputText("##name", g_nameBuf, sizeof(g_nameBuf), ImGuiInputTextFlags_CallbackEdit, NameEditCallback))
        g_nameDirty = true;
    ImGui::SameLine();
    ImGui::TextDisabled("%zu/%zu", profiles::CountCodepoints(g_nameBuf), profiles::kMaxNameChars);

    if (ImGui::Button(Tr("name.save"))) {
        const std::string name = g_nameBuf;
        g_nameDirty = false;
        gamethread::Post([name] { coop::SetLocalName(name); });
    }
    ImGui::SameLine();
    if (ImGui::Button(Tr("name.reset"))) {
        g_nameDirty = false;
        gamethread::Post([] { coop::SetLocalName(""); });
    }
    ImGui::Spacing();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", Tr("name.help1"));
    ImGui::TextDisabled("%s", Tr("name.help2"));
    ImGui::PopTextWrapPos();
}

// ---- About (no store link)

void DrawAboutTab() {
    ImGui::TextColored(kColPink, "%s", core::kModName);
    ImGui::Text("%s: %s", Tr("about.version"), core::kModVersion);
    ImGui::Text("%s: %s", Tr("about.author"), core::kModAuthor);
    ImGui::Spacing();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(Tr("about.desc"));
    ImGui::Spacing();
    ImGui::TextDisabled("%s", Tr("about.disclaimer"));
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    ImGui::Separator();
    DrawLanguageSwitch();
}

#if QD_DEV
// ---- 调试 (dev flavour only; lives in the ~ console, see Overlay IsDebugTab)

const char* const kLogLevels[] = {"debug", "info", "warn", "error"};
int  g_logLevelIdx = 1;
char g_traceClass[128] = "FS_PlayerChar_C";
float g_traceSeconds = 10.0f;
int  g_dummyMode = 0;
float g_dummyWalkSpeed = 150.0f;

void ApplyDummyMode() {
    switch (g_dummyMode) {
        case 0: sync::DebugSetDummyAction(0, 0.0f); break;
        case 1: sync::DebugSetDummyAction(0, g_dummyWalkSpeed); break;
        case 2: sync::DebugSetDummyAction(2, 400.0f); break;
        case 3: sync::DebugSetDummyAction(3, 0.0f); break;
        case 4: sync::DebugSetDummyAction(4, 0.0f); break;
        default: break;
    }
}

void DrawDebugTab() {
    const Snapshot s = Copy();
    const auto& cfg = config::Get();
    const bool idle = IsIdleLike(s.state);

    ImGui::TextDisabled("profile %d · transport %s · udp %s:%d · key %s", cfg.profile, cfg.transport.c_str(),
                        cfg.udpHostIp.c_str(), cfg.udpPort, s.profileKey.c_str());

    ImGui::SeparatorText("UDP 回环（同机双开）");
    if (!s.udp) ImGui::TextColored(kColWarn, "当前传输是 Steam。UDP 测试请用 -qdcoop-transport=udp 启动。");
    ImGui::BeginDisabled(!s.udp || !s.sessionOk || !idle);
    char hostLabel[64];
    std::snprintf(hostLabel, sizeof(hostLabel), "UDP 建房（端口 %d）", cfg.udpPort);
    if (ImGui::Button(hostLabel)) gamethread::Post([] { coop::Session().Host(); });
    ImGui::SameLine();
    char joinLabel[96];
    std::snprintf(joinLabel, sizeof(joinLabel), "UDP 加入 %s:%d", cfg.udpHostIp.c_str(), cfg.udpPort);
    if (ImGui::Button(joinLabel)) {
        const std::string ip = cfg.udpHostIp;
        const auto port = static_cast<uint16_t>(cfg.udpPort);
        gamethread::Post([ip, port] { coop::Session().JoinUdp(ip, port); });
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!s.sessionOk || s.state == net::SessionState::Idle);
    if (ImGui::Button("离开##udp")) gamethread::Post([] { coop::Session().Leave(); });
    ImGui::EndDisabled();

    ImGui::SeparatorText("日志");
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::Combo("日志级别", &g_logLevelIdx, kLogLevels, IM_ARRAYSIZE(kLogLevels))) {
        const int idx = g_logLevelIdx;
        gamethread::Post([idx] {
            log::SetMinLevel(static_cast<log::Level>(idx));
            config::Mutable().logLevel = kLogLevels[idx];
            QD_INFO("menu", "log level -> {}", kLogLevels[idx]);
        });
    }
    ImGui::SetNextItemWidth(260.0f);
    ImGui::InputText("类名##trace", g_traceClass, sizeof(g_traceClass));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    ImGui::SliderFloat("秒##trace", &g_traceSeconds, 1.0f, 60.0f, "%.0f");
    ImGui::SameLine();
    if (ImGui::Button("TraceClass")) {
        const std::string cls = g_traceClass;
        const float secs = g_traceSeconds;
        gamethread::Post([cls, secs] {
            hooks::TraceClass(cls.c_str(), secs);
            ui::Toast("开始记录 " + cls + " 的函数调用（见日志）", ui::ToastKind::Info);
        });
    }

    ImGui::SeparatorText("化身");
    const auto st = sync::GetPlayerSyncStats();
    const auto as = sync::GetPuppetAssetStatus();
    ImGui::Text("远端玩家 %d · 已生成化身 %d · 区域 %04X · 胶囊半高 %.1f", st.remotes, st.puppets, st.localAreaId,
                st.capsuleHalfHeight);
    if (!as.tried) ImGui::TextDisabled("资产：尚未加载（第一次生成化身时加载）");
    else if (!as.meshOk) ImGui::TextColored(kColErr, "资产：骑士模型加载失败，使用圆柱体");
    else ImGui::TextColored(as.animsOk == 5 ? kColOk : kColWarn, "资产：骑士模型 OK，动画 %d/5", as.animsOk);

    bool dummy = sync::DebugDummyEnabled();
    if (ImGui::Checkbox("在面前 3 米生成测试化身", &dummy)) {
        sync::DebugSetDummy(dummy);
        ApplyDummyMode();
    }
    ImGui::BeginDisabled(!dummy);
    bool changed = false;
    changed |= ImGui::RadioButton("待机", &g_dummyMode, 0); ImGui::SameLine();
    changed |= ImGui::RadioButton("走路", &g_dummyMode, 1); ImGui::SameLine();
    changed |= ImGui::RadioButton("冲刺", &g_dummyMode, 2); ImGui::SameLine();
    changed |= ImGui::RadioButton("死亡", &g_dummyMode, 3); ImGui::SameLine();
    changed |= ImGui::RadioButton("空中", &g_dummyMode, 4);
    if (g_dummyMode == 1) {
        ImGui::SetNextItemWidth(220.0f);
        changed |= ImGui::SliderFloat("走路速度 cm/s", &g_dummyWalkSpeed, 30.0f, 600.0f, "%.0f");
    }
    if (changed) ApplyDummyMode();
    if (ImGui::Button("攻击一次")) sync::DebugSetDummyAction(1, 0.0f);
    ImGui::EndDisabled();

    float yawOffset = sync::MeshYawOffset();
    ImGui::SetNextItemWidth(220.0f);
    if (ImGui::SliderFloat("网格朝向偏移", &yawOffset, -180.0f, 180.0f, "%.0f°")) sync::SetMeshYawOffset(yawOffset);
    ImGui::SameLine();
    if (ImGui::SmallButton("重置")) sync::SetMeshYawOffset(sync::kDefaultMeshYawOffset);
    ImGui::TextDisabled("化身应该面朝你（测试化身朝向 = 你的朝向 + 180°）；把实测值写回 Puppet.h 的 kDefaultMeshYawOffset。");

    // weapon placement on Weapon_R. Values live in Puppet.cpp (mutex-guarded), so reading/writing them from the
    // render thread is fine; puppets pick up changes on the game thread next frame.
    ImGui::SeparatorText("武器挂点");
    const sync::WeaponTuneInfo wt = sync::DebugWeaponTuneInfo();
    if (wt.path.empty()) {
        ImGui::TextDisabled("还没读到你手里的武器（进游戏、拿着武器时再看）。");
        return;
    }
    ImGui::PushTextWrapPos(0.0f);
    ImGui::Text("当前武器：%s", wt.path.c_str());
    ImGui::PopTextWrapPos();
    if (wt.overridden) {
        ImGui::SameLine();
        ImGui::TextColored(kColPink, "（已手动调整）");
    }
    sync::WeaponFit fit = wt.fit;
    bool fitChanged = false;
    ImGui::SetNextItemWidth(360.0f);
    fitChanged |= ImGui::SliderFloat3("位置偏移 cm##wfit", fit.loc, -60.0f, 60.0f, "%.1f");
    ImGui::SetNextItemWidth(360.0f);
    fitChanged |= ImGui::SliderFloat3("旋转 俯仰/偏航/翻滚##wfit", fit.rot, -180.0f, 180.0f, "%.0f°");
    ImGui::SetNextItemWidth(360.0f);
    fitChanged |= ImGui::SliderFloat("缩放##wfit", &fit.scale, 0.1f, 3.0f, "%.2f");
    if (fitChanged) sync::DebugSetWeaponFit(fit);
    if (ImGui::Button("复制为 C++##wfit")) {
        sync::DebugLogWeaponFitCpp();
        ui::Toast("已写进 KnightsDomain.log（搜 weapon fit）", ui::ToastKind::Info);
    }
    ImGui::SameLine();
    if (ImGui::Button("重置##wfit")) sync::DebugResetWeaponFit();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("调的是你自己手里的武器：测试化身拿的就是它，队友拿同一件武器时也会跟着变。满意后把日志里那行贴进 Puppet.cpp 的 kWeaponFits。");
    ImGui::PopTextWrapPos();
}
#endif   // QD_DEV

}  // namespace

void InitCoopMenu() {
    if (g_ticker) return;
    TakeSnapshot();
    g_ticker = gamethread::AddTicker("coop-menu", [](float dt) {
        g_accum += dt;
        if (g_accum < kSnapshotInterval) return;
        g_accum = 0;
        TakeSnapshot();
    });

    // Titles are i18n keys (translated per frame by the overlay).
    updater::Start();   // background version check
    AddMenuTab("coop", "tab.coop", DrawCoopTab, 10);
    AddMenuTab("name", "tab.name", DrawNameTab, 20);
    AddMenuTab("about", "tab.about", DrawAboutTab, 1000);   // always the last tab
#if QD_DEV
    const auto& cfg = config::Get();
    for (int i = 0; i < IM_ARRAYSIZE(kLogLevels); ++i)
        if (cfg.logLevel == kLogLevels[i]) g_logLevelIdx = i;
    AddMenuTab("debug", "调试", DrawDebugTab, 90);   // lives in the ~ console (Overlay IsDebugTab)
#endif
}

void ShutdownCoopMenu() {
    if (g_ticker) {
        gamethread::RemoveTicker(g_ticker);
        g_ticker = 0;
    }
}

}  // namespace qd::ui
