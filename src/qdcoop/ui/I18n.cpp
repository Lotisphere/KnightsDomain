// Knights' Domain — string tables. English is the reference text; Chinese is the translation.
#include "qdcoop/ui/I18n.h"

#include "qdcoop/core/Config.h"
#include "qdcoop/core/Core.h"
#include "qdcoop/core/Log.h"
#include "qdcoop/core/Ownership.h"

#include <atomic>
#include <string_view>
#include <unordered_map>

namespace qd::ui {
namespace {

constexpr char kTag[] = "i18n";

struct Entry {
    const char* key;
    const char* en;
    const char* zh;   // nullptr = fall back to English
    const char* ja = nullptr;   // Japanese; nullptr = fall back to English
};

// clang-format off
constexpr Entry kTable[] = {
    // ---- F5 window
    {"menu.footer",            "Press {} to close this menu",                       "按 {} 关闭菜单",
                               "{} でメニューを閉じる"},
    {"menu.empty",             "(No pages available yet)",                          "（还没有可用的页面）",
                               "（表示できるページがまだありません）"},
    {"tab.coop",               "Co-op",                                             "联机",
                               "協力プレイ"},
    {"tab.name",               "My Name",                                           "我的名字",
                               "名前"},
    {"tab.summon",             "Teleport",                                          "传送",
                               "テレポート"},
    {"tab.about",              "About",                                             "关于",
                               "このMODについて"},

    // ---- update banner + what's-new notice
    {"update.available",       "Version {} is available (installed: {}).",        "有新版本 {}（当前 {}）",
                               "バージョン {} が利用できます（現在 {}）"},
    {"update.button",          "Update now",                                        "立即更新",
                               "今すぐ更新"},
    {"update.downloading",     "Downloading the update… {}%",                       "正在下载更新… {}%",
                               "更新をダウンロード中… {}%"},
    {"update.installed",       "Updated to {}. Restart the game to finish.",       "已更新到 {}，重启游戏后生效",
                               "{} に更新しました。ゲームを再起動すると有効になります"},
    {"update.failed",          "The update could not be installed ({}). You can try again, or download it from GitHub or the website.",
                               "更新安装失败（{}）。可以再试一次，或到网站 / GitHub 手动下载",
                               "更新をインストールできませんでした（{}）。もう一度試すか、GitHub / サイトからダウンロードしてください"},
    {"update.retry",           "Try again",                                         "再试一次",
                               "もう一度試す"},
    {"notice.title",           "What's new in {}",                                  "{} 更新公告",
                               "{} の更新内容"},
    {"notice.thanks",          "Thank you for supporting this co-op mod! I maintain and update it on my own, and I will keep polishing it and fixing bugs as well as I can. In this update:",
                               "感谢大家对本联机 MOD 的支持！作为独立维护和更新这款 MOD 的作者，我会尽可能完善优化各个方面并修复 bug。以下是本次更新报告：",
                               "この協力プレイMODを応援していただきありがとうございます！個人で保守・更新していますが、できる限り改善と不具合修正を続けていきます。今回の更新内容："},
    {"notice.1",               "1. Smoother blending between your friends' attack, run and walk animations",
                               "1. 玩家模型的攻击、奔跑、走路等动画衔接更加流畅",
                               "1. 仲間モデルの攻撃・走り・歩きなどのアニメーションがより滑らかにつながるように"},
    {"notice.2",               "2. Fixed boss and some enemy health not staying in sync between players",
                               "2. 修复 BOSS 与部分怪物的血量数据不同步的问题",
                               "2. ボスと一部の敵のHPがプレイヤー間で同期しない問題を修正"},
    {"notice.3",               "3. NEW: online updates. From now on you can update to the latest version right here.",
                               "3. 【重点】增加在线更新功能，下次可以直接在这里更新到最新版本",
                               "3. 【注目】オンライン更新に対応。次回からここで最新版に更新できます"},
    {"notice.4",               "4. Fixed several performance costs",
                               "4. 修复了若干性能开销问题",
                               "4. いくつかのパフォーマンス負荷を修正"},
    {"notice.5",               "5. Lower network latency and better handling of packet loss",
                               "5. 网络延迟和丢包改善",
                               "5. ネットワーク遅延とパケットロスへの対応を改善"},
    {"notice.6",               "6. Fixed phase changes of several bosses not matching between players",
                               "6. 修复了若干 BOSS 转阶段无法匹配的问题",
                               "6. 複数のボスでフェーズ移行が一致しない問題を修正"},
    {"notice.disclaimer",      "This mod has no connection with the developer of Queen's Domain. If you run into a bug while using it, please do not report it to the official developer. Thank you!",
                               "本 MOD 与 Queen's Domain 开发商没有任何关系。如果使用本 MOD 时发生任何 bug，请不要提交给官方，谢谢配合！",
                               "このMODは Queen's Domain の開発元とは一切関係ありません。使用中に不具合が起きても、公式には報告しないでください。ご協力ありがとうございます！"},
    {"notice.ok",              "Got it",                                            "知道了",
                               "了解"},
    {"toast.game_updated",     "Queen's Domain was updated and this version of Knights' Domain does not know it yet. Co-op may misbehave; please update the mod.",
                               "Queen's Domain 已更新，当前版本的 Knights' Domain 还不认识它，联机可能出现异常，请更新 MOD",
                               "Queen's Domain が更新されました。この版の Knights' Domain は未対応のため、協力プレイに不具合が出る場合があります。MOD を更新してください"},
    {"toast.boss_talk",        "{} is speaking with a boss. The fight starts when they finish.",
                               "{} 正在和 Boss 对话，对话结束后开战",
                               "{} がボスと会話中。終わると戦闘が始まります"},
    // ---- Co-op page
    {"coop.loading",           "Loading…",                                          "加载中…",
                               "読み込み中…"},
    {"coop.language",          "Language",                                          "语言",
                               "言語"},
    {"coop.init_failed",       "The co-op module failed to start. See KnightsDomain.log.",
                               "联机模块没有初始化成功，详见 KnightsDomain.log。",
                               "協力プレイ機能を起動できませんでした。KnightsDomain.log を確認してください。"},
    {"coop.verifying",         "Checking your Steam copy of Queen's Domain…",       "正在验证 Steam 正版 Queen's Domain…",
                               "Steam 版 Queen's Domain を確認しています…"},
    {"coop.starting",          "Waiting for Steam…",                                "正在等待 Steam…",
                               "Steam を待っています…"},
    {"coop.status",            "Status: ",                                          "状态：",
                               "状態："},
    {"coop.state.idle",        "Not in a session",                                  "未联机",
                               "未接続"},
    {"coop.state.creating",    "Creating room…",                                    "正在创建房间…",
                               "ルームを作成中…"},
    {"coop.state.hosting_n",   "Host ({} players)",                                 "房主（{} 人）",
                               "ホスト（{} 人）"},
    {"coop.state.hosting_wait","Host (waiting for friends to join)",                "房主（等待好友加入）",
                               "ホスト（フレンドの参加を待っています）"},
    {"coop.state.joining",     "Joining…",                                          "正在加入…",
                               "参加中…"},
    {"coop.state.connected",   "Connected",                                         "已连接",
                               "接続済み"},
    {"coop.state.handshake",   "Connected (handshaking…)",                          "已连接（握手中…）",
                               "接続済み（認証中…）"},
    {"coop.state.failed",      "Failed: {}",                                        "失败：{}",
                               "失敗：{}"},
    {"common.unknown_error",   "unknown error",                                     "未知错误",
                               "不明なエラー"},
    {"coop.host",              "Host a co-op room",                                 "创建联机房间",
                               "協力プレイのルームを作成"},
    {"coop.leave",             "Leave",                                             "离开",
                               "退出"},
    {"coop.members",           "Members",                                           "成员",
                               "メンバー"},
    {"coop.col.slot",          "Slot",                                              "槽位",
                               "スロット"},
    {"coop.col.name",          "Name",                                              "名字",
                               "名前"},
    {"coop.col.ping",          "Ping",                                              "延迟",
                               "Ping"},
    {"coop.host_tag",          "Host",                                              "房主",
                               "ホスト"},
    {"coop.you",               "{} (you)",                                          "{}（你）",
                               "{}（あなた）"},
    {"coop.friends",           "Friends' rooms",                                    "好友的房间",
                               "フレンドのルーム"},
    {"coop.refresh",           "Refresh",                                           "刷新",
                               "更新"},
    {"coop.invite",            "Friend invite received",                            "收到好友邀请",
                               "フレンドから招待が届きました"},
    {"coop.accept",            "Accept invite",                                     "接受邀请",
                               "招待を受ける"},
    {"coop.no_lobbies",        "(No friends are hosting right now. Click Refresh to check again.)",
                               "（没有好友正在开房，点「刷新」再看看）",
                               "（ルームを開いているフレンドはいません。「更新」で再確認できます）"},
    {"coop.col.host",          "Host",                                              "房主",
                               "ホスト"},
    {"coop.col.players",       "Players",                                           "人数",
                               "人数"},
    {"coop.unknown_host",      "(unknown)",                                         "（未知）",
                               "（不明）"},
    {"coop.version_diff",      "Different version",                                 "版本不同",
                               "バージョン違い"},
    {"coop.join",              "Join",                                              "加入",
                               "参加"},

    // ---- ownership
    {"own.failed",             "Knights' Domain needs Queen's Domain launched from Steam with an owned copy. Co-op is disabled.",
                               "Knights' Domain 需要从 Steam 启动并拥有正版的 Queen's Domain。联机功能已停用。",
                               "Knights' Domain は、所有している Queen's Domain を Steam から起動したときに使えます。協力プレイは無効になっています。"},

    // ---- My Name page
    {"name.current",           "Current name: ",                                    "当前名字：",
                               "現在の名前："},
    {"name.slot",              "Current save slot: slot{}",                         "当前存档槽：slot{}",
                               "現在のセーブスロット：slot{}"},
    {"name.save",              "Save",                                              "保存",
                               "保存"},
    {"name.reset",             "Reset to default",                                  "恢复默认",
                               "初期設定に戻す"},
    {"name.help1",             "Your name belongs to the current save slot: other save slots keep their own name. If none is set, your Steam name is used.",
                               "名字跟当前存档槽绑定：换到别的存档槽会用那个槽自己的名字。没有设置时用 Steam 昵称。",
                               "名前は現在のセーブスロットごとに保存されます。別のスロットではそのスロットの名前を使います。未設定のときは Steam の名前を使います。"},
    {"name.help2",             "Up to 24 characters. Renaming during co-op updates everyone right away.",
                               "最多 24 个字。联机中改名会立刻同步给其他人。",
                               "最大 24 文字。協力プレイ中に変更すると、すぐに全員に反映されます。"},
    {"name.saved",             "Name saved: {}",                                    "名字已保存：{}",
                               "名前を保存しました：{}"},
    {"name.save_failed",       "Name applied, but profiles.json could not be written",
                               "名字已生效，但写入 profiles.json 失败",
                               "名前は反映されましたが、profiles.json に保存できませんでした"},
    {"name.default",           "Player",                                            "玩家",
                               "プレイヤー"},

    // ---- handshake / roster toasts
    {"reject.full",            "The room is full",                                  "房间已满",
                               "ルームが満員です"},
    {"reject.mod",             "Different mod version. Both players need the same version of Knights' Domain.",
                               "MOD 版本不一致，请双方更新到同一版本的 Knights' Domain",
                               "MOD のバージョンが違います。全員が同じバージョンの Knights' Domain を使ってください。"},
    {"reject.game",            "Different game version. Both players need the same version of Queen's Domain.",
                               "游戏版本不一致，请双方把游戏更新到同一版本",
                               "ゲームのバージョンが違います。全員が同じバージョンの Queen's Domain を使ってください。"},
    {"reject.protocol",        "Different co-op protocol. Both players need to update Knights' Domain.",
                               "联机协议版本不一致，请双方更新 MOD",
                               "通信プロトコルが違います。全員が Knights' Domain を更新してください。"},
    {"reject.kicked",          "The host removed you from the room",                "你被房主移出了房间",
                               "ホストによってルームから外されました"},
    {"reject.unknown",         "unknown reason",                                    "未知原因",
                               "不明な理由"},
    {"toast.rejected_join",    "Declined a join request: {}",                       "拒绝了一个加入请求：{}",
                               "参加リクエストを断りました：{}"},
    {"toast.joined",           "{} joined",                                         "{} 加入了",
                               "{} が参加しました"},
    {"toast.left",             "{} left",                                           "{} 离开了",
                               "{} が退出しました"},
    {"toast.renamed",          "{} is now called {}",                               "{} 改名为 {}",
                               "{} が {} に名前を変更しました"},
    {"toast.host_left",        "The host left",                                     "房主已离开",
                               "ホストが退出しました"},
    {"toast.joined_room",      "Joined {}'s room",                                  "已加入 {} 的房间",
                               "{} のルームに参加しました"},
    {"toast.host_default",     "the host",                                          "房主",
                               "ホスト"},
    {"toast.cannot_join",      "Cannot join: {}",                                   "无法加入：{}",
                               "参加できません：{}"},
    {"toast.room_created",     "Co-op room created. Waiting for friends to join.",  "联机房间已创建，等待好友加入",
                               "協力プレイのルームを作成しました。フレンドの参加を待っています。"},
    {"toast.session_failed",   "Co-op failed: {}",                                  "联机失败：{}",
                               "協力プレイに失敗しました：{}"},
    {"toast.host_timeout",     "Could not reach the host (no response)",            "连接房主超时（没有收到回应）",
                               "ホストに接続できませんでした（応答なし）"},
    {"toast.init_failed",      "The co-op module failed to start ({}). See KnightsDomain.log.",
                               "联机模块初始化失败（{}），详见 KnightsDomain.log",
                               "協力プレイ機能を起動できませんでした（{}）。KnightsDomain.log を確認してください。"},
    {"toast.left_session",     "You left the co-op session",                        "已离开联机",
                               "協力プレイから退出しました"},

    // ---- Teleport page / toasts
    {"summon.done",            "Teleported to the host",                            "已传送到房主身边",
                               "ホストのところへテレポートしました"},
    {"summon.only_client",     "You can only teleport to the host after joining someone's room",
                               "只有加入别人的房间时才能传送到房主身边",
                               "ホストへのテレポートは、ほかの人のルームに参加しているときだけ使えます"},
    {"summon.no_world",        "You are not in the game world yet",                 "还没有进入游戏世界",
                               "まだゲームの世界に入っていません"},
    {"summon.dead",            "You can't teleport while dead",                     "死亡状态下不能传送",
                               "死亡中はテレポートできません"},
    {"summon.no_pos",          "The host's position hasn't arrived yet. Please try again in a moment.",
                               "还没收到房主的位置，请稍后再试",
                               "ホストの位置をまだ受信していません。少し待ってからもう一度試してください。"},
    {"summon.other_map",       "The host is on another map, so you can't teleport there directly yet",
                               "房主在另一张地图，暂时不能直接传送",
                               "ホストは別のマップにいるため、今は直接テレポートできません"},
    {"summon.failed",          "Teleport failed (see KnightsDomain.log)",           "传送失败（详见 KnightsDomain.log）",
                               "テレポートに失敗しました（KnightsDomain.log を確認してください）"},
    {"summon.in_progress",     "Teleporting to the host…",                          "正在传送到房主身边…",
                               "ホストのところへテレポート中…"},
    {"summon.back_at_save",    "You're back at your save point. F5 > Teleport takes you back to the host.",
                               "已回到存档点，F5 → 传送 可以回到房主身边",
                               "セーブ地点に戻りました。F5 → テレポートでホストのところへ戻れます。"},
    {"summon.desc",            "Teleport next to the host (1.5 m to their right). You can move again once the destination has loaded.",
                               "传送到房主身边（在房主右侧 1.5 米）。传送后会等目的地区域加载完再恢复移动。",
                               "ホストの右 1.5 m にテレポートします。移動先の読み込みが終わると動けるようになります。"},
    {"summon.button",          "Teleport to the host",                              "传送到房主身边",
                               "ホストのところへテレポート"},
    {"summon.busy",            "Teleporting…",                                      "正在传送…",
                               "テレポート中…"},
    {"summon.you_host",        "You are the host. Players who joined you can teleport to you here.",
                               "你是房主。客机可以在这里传送到你身边。",
                               "あなたはホストです。参加したプレイヤーはここからあなたのところへテレポートできます。"},
    {"summon.solo",            "After joining someone's room you can teleport to the host here.",
                               "加入别人的房间后，可以在这里传送到房主身边。",
                               "ほかの人のルームに参加すると、ここからホストのところへテレポートできます。"},

    // ---- teammate model
    {"puppet.mesh_failed",     "Couldn't load the teammate model; showing a cylinder instead (see KnightsDomain.log)",
                               "化身模型加载失败，暂时用圆柱体代替（详见 KnightsDomain.log）",
                               "仲間のモデルを読み込めなかったため、代わりに円柱を表示しています（KnightsDomain.log を確認してください）"},
    {"puppet.anims_failed",    "Some teammate animations failed to load (see KnightsDomain.log)",
                               "部分化身动画加载失败（详见 KnightsDomain.log）",
                               "仲間のアニメーションの一部を読み込めませんでした（KnightsDomain.log を確認してください）"},

    // ---- About page (no store link)
    {"about.version",          "Version",                                           "版本",
                               "バージョン"},
    {"about.author",           "Author",                                            "作者",
                               "作者"},
    {"about.desc",             "A free fan-made co-op mod for Queen's Domain. Play the whole game together with up to 3 friends.",
                               "一个免费的 Queen's Domain 粉丝自制联机 MOD。最多可以和 3 位好友一起玩完整个游戏。",
                               "Queen's Domain のための無料のファンメイド協力プレイ MOD です。最大 3 人のフレンドと一緒にゲーム全体を遊べます。"},
    {"about.disclaimer",       "Not affiliated with the developer of Queen's Domain.",
                               "本 MOD 与 Queen's Domain 的开发商没有任何关联。",
                               "本 MOD は Queen's Domain の開発者とは関係ありません。"},
};
// clang-format on

struct Tables {
    std::unordered_map<std::string_view, const char*> en, zh, ja;
    Tables() {
        for (const Entry& e : kTable) {
            if (e.en) en.emplace(e.key, e.en);
            if (e.zh) zh.emplace(e.key, e.zh);
            if (e.ja) ja.emplace(e.key, e.ja);
        }
    }
};
const Tables& T() {
    static const Tables t;   // thread-safe initialisation; read-only afterwards
    return t;
}

std::atomic<Lang> g_lang{Lang::En};

const char* LangCode(Lang l) { return l == Lang::Zh ? "zh" : l == Lang::Ja ? "ja" : "en"; }

// First-run detection (game thread ticker).
gamethread::TickerId g_detectTicker = 0;
float                g_detectTimer = 0;
float                g_detectElapsed = 0;

void DetectTick(float dt) {
    g_detectTimer -= dt;
    g_detectElapsed += dt;
    if (g_detectTimer > 0) return;
    g_detectTimer = 2.0f;
    const std::string steamLang = core::SteamGameLanguage();
    if (!steamLang.empty()) {
        const Lang l = (steamLang == "schinese" || steamLang == "tchinese") ? Lang::Zh
                     : steamLang == "japanese"                                ? Lang::Ja
                                                                              : Lang::En;
        QD_INFO(kTag, "first run: Steam game language '{}' -> UI language {}", steamLang, LangCode(l));
        SetLangAndSave(l);
    } else if (g_detectElapsed < 90.0f) {
        return;   // Steam not up yet
    } else {
        QD_INFO(kTag, "first run: Steam game language unavailable after 90 s; keeping {}", LangCode(GetLang()));
    }
    gamethread::RemoveTicker(g_detectTicker);
    g_detectTicker = 0;
}

}  // namespace

const char* Tr(const char* key) {
    if (!key) return "";
    const Tables& t = T();
    const Lang lang = g_lang.load(std::memory_order_relaxed);
    if (lang == Lang::Zh) {
        if (auto it = t.zh.find(key); it != t.zh.end()) return it->second;
    } else if (lang == Lang::Ja) {
        if (auto it = t.ja.find(key); it != t.ja.end()) return it->second;
    }
    if (auto it = t.en.find(key); it != t.en.end()) return it->second;
    return key;
}

void SetLang(Lang lang) { g_lang.store(lang, std::memory_order_relaxed); }
Lang GetLang() { return g_lang.load(std::memory_order_relaxed); }

void InitI18n() {
    const auto& cfg = config::Get();
    SetLang(cfg.language == "zh" ? Lang::Zh : cfg.language == "ja" ? Lang::Ja : Lang::En);
    QD_INFO(kTag, "UI language {} ({})", LangCode(GetLang()),
            cfg.languageFromFile ? "from config.json" : "default; detecting from Steam");
    if (!cfg.languageFromFile && g_detectTicker == 0) {
        g_detectTimer = 0;
        g_detectElapsed = 0;
        g_detectTicker = gamethread::AddTicker("i18n-detect", [](float dt) { DetectTick(dt); });
    }
}

void SetLangAndSave(Lang lang) {
    SetLang(lang);
    config::SetLanguage(LangCode(lang));
    // A manual choice also ends the first-run detection.
    if (g_detectTicker) {
        gamethread::RemoveTicker(g_detectTicker);
        g_detectTicker = 0;
    }
}

}  // namespace qd::ui
