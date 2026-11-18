// Knights' Domain: a "Co-op" entry in the title screen's own menu, under Play Game. It is the game's own button
// widget (FS_Button_C, same font, sounds and controller navigation) with a text block styled like its neighbours;
// pressing it opens the co-op window (the same one as F5). Game thread only.
//
// The title menu is TitleScreenUI_C's VerticalBox_4: Play Game, Options, Credits, Quit. UE 4.27 has no runtime
// InsertChildAt, so the buttons below Play Game are taken out and added back after ours, with their slot settings.
//
// Include order: SDK headers FIRST (see GameQueries.cpp for why).
#include "SDK/Engine_classes.hpp"
#include "SDK/Engine_parameters.hpp"
#include "SDK/UMG_classes.hpp"
#include "SDK/UMG_parameters.hpp"
#include "SDK/CommonUI_classes.hpp"
#include "SDK/CommonUI_parameters.hpp"
#include "SDK/TitleScreenUI_classes.hpp"
#include "SDK/FS_Button_classes.hpp"

#include "qdcoop/ui/TitleButton.h"

#include "qdcoop/core/Core.h"
#include "qdcoop/core/Hooks.h"
#include "qdcoop/core/Log.h"
#include "qdcoop/core/Unreal.h"
#include "qdcoop/ui/I18n.h"
#include "qdcoop/ui/Overlay.h"

#include <cstring>
#include <string>
#include <vector>

namespace qd::ui {
namespace {

constexpr const char* kTag = "titlebutton";
constexpr const char* kTitleClass = "/Game/UI/Title/TitleScreenUI.TitleScreenUI_C";
constexpr const char* kButtonClass = "/Game/UI/UI_Style/FS_Button.FS_Button_C";
constexpr const char* kButtonClicked = "/Game/UI/UI_Style/FS_Button.FS_Button_C:BP_OnClicked";
constexpr float kCheckSec = 0.5f;

struct State {
    bool inited = false;
    gamethread::TickerId ticker = 0;
    float checkIn = 0;
    SDK::UObject* title = nullptr;      // the title widget our button was added to
    SDK::UObject* button = nullptr;     // our button
    bool failedFor = false;             // gave up on this title widget (logged once)
};
State& S() {
    static State s;
    return s;
}

template <class P>
bool Call(SDK::UObject* obj, const char* fn, P& params) {
    SDK::UFunction* f = obj ? ue::FindFunction(obj, fn) : nullptr;
    if (!f) {
        QD_WARN(kTag, "{} not found on {}", fn, obj ? ue::ClassName(obj) : std::string("null"));
        return false;
    }
    ue::CallFunction(obj, f, &params);
    return true;
}

SDK::UObject* FindTitle() {
    SDK::UClass* cls = ue::FindClass(kTitleClass);
    if (!cls) return nullptr;
    auto& objs = SDK::UObject::GObjects;
    for (int i = 0; i < objs->Num(); ++i) {
        SDK::UObject* o = objs->GetByIndex(i);
        if (o && !o->IsDefaultObject() && o->IsA(cls) && ue::IsValid(o)) return o;
    }
    return nullptr;
}

// Play Game's label: the title screen binds it into the button's named slot from its own widget tree
// ("UseButtontext_2"), so it is looked up there by name.
SDK::UCommonTextBlock* PlayLabel(SDK::UObject* title) {
    SDK::UClass* cls = SDK::UCommonTextBlock::StaticClass();
    auto& objs = SDK::UObject::GObjects;
    for (int i = 0; i < objs->Num(); ++i) {
        SDK::UObject* o = objs->GetByIndex(i);
        if (!o || o->IsDefaultObject() || !o->IsA(cls) || ue::Name(o) != "UseButtontext_2") continue;
        for (SDK::UObject* out = o->Outer; out; out = out->Outer)
            if (out == title) return static_cast<SDK::UCommonTextBlock*>(o);
    }
    return nullptr;
}

// A text block for the button's label, styled like the label of Play Game.
SDK::UCommonTextBlock* MakeLabel(SDK::UCommonTextBlock* model, SDK::UObject* outer) {
    SDK::UObject* o = SDK::UGameplayStatics::SpawnObject(SDK::UCommonTextBlock::StaticClass(), outer);
    if (!o) return nullptr;
    auto* label = static_cast<SDK::UCommonTextBlock*>(o);
    if (model) {
        label->Style = model->Style;   // DefaultTextStyle_C: the menu's pixel font rendering comes from the style
        SDK::Params::CommonTextBlock_SetStyle st{};
        st.InStyle = model->Style;
        Call(label, "SetStyle", st);
        label->RenderTransform = model->RenderTransform;   // the menu's labels sit 3 px lower
        label->RenderTransformPivot = model->RenderTransformPivot;
        label->Justification = model->Justification;
        label->bDisplayAllCaps = model->bDisplayAllCaps;
        // field by field: FSlateFontInfo and FSlateColor also hold engine shared pointers that must not be copied
        SDK::FSlateFontInfo& f = label->Font;
        const SDK::FSlateFontInfo& m = model->Font;
        f.FontObject = m.FontObject;
        f.FontMaterial = m.FontMaterial;
        f.OutlineSettings.OutlineSize = m.OutlineSettings.OutlineSize;
        f.OutlineSettings.bSeparateFillAlpha = m.OutlineSettings.bSeparateFillAlpha;
        f.OutlineSettings.bApplyOutlineToDropShadows = m.OutlineSettings.bApplyOutlineToDropShadows;
        f.OutlineSettings.OutlineMaterial = m.OutlineSettings.OutlineMaterial;
        f.OutlineSettings.OutlineColor = m.OutlineSettings.OutlineColor;
        f.TypefaceFontName = m.TypefaceFontName;
        f.Size = m.Size;
        f.LetterSpacing = m.LetterSpacing;
        f.PixelateConfig = m.PixelateConfig;
        label->ColorAndOpacity.SpecifiedColor = model->ColorAndOpacity.SpecifiedColor;
        label->ColorAndOpacity.ColorUseRule = model->ColorAndOpacity.ColorUseRule;
        label->ShadowOffset = model->ShadowOffset;
        label->ShadowColorAndOpacity = model->ShadowColorAndOpacity;
        label->Visibility = model->Visibility;
    }
    SDK::Params::TextBlock_SetText tx{};
    const std::string text = Tr("tab.coop");
    tx.InText = SDK::UKismetTextLibrary::Conv_StringToText(SDK::FString(std::wstring(text.begin(), text.end()).c_str()));
    Call(label, "SetText", tx);
    return label;
}

bool AddButton(SDK::UObject* titleObj) {
    auto* title = static_cast<SDK::UTitleScreenUI_C*>(titleObj);
    SDK::UFS_Button_C* play = title->LoadGameButton;
    if (!play || !play->Slot || !play->Slot->Parent) return false;
    SDK::UPanelWidget* box = play->Slot->Parent;
    if (!box->IsA(SDK::UVerticalBox::StaticClass())) return false;

    // our button, owned by the same player as the title screen
    SDK::UClass* cls = ue::FindClass(kButtonClass);
    if (!cls) return false;
    SDK::Params::WidgetBlueprintLibrary_Create cr{};
    cr.WorldContextObject = titleObj;
    cr.WidgetType = cls;
    cr.OwningPlayer = ue::LocalController();
    if (!Call(SDK::UWidgetBlueprintLibrary::GetDefaultObj(), "Create", cr) || !cr.ReturnValue) return false;
    auto* button = static_cast<SDK::UFS_Button_C*>(cr.ReturnValue);
    button->SpecialSounds = play->SpecialSounds;
    button->Style = play->Style;
    SDK::UCommonTextBlock* model = PlayLabel(titleObj);
    if (!model) QD_WARN(kTag, "Play Game's label not found; the co-op label keeps the default text style");
    if (SDK::UCommonTextBlock* label = MakeLabel(model, button->WidgetTree ? static_cast<SDK::UObject*>(button->WidgetTree)
                                                                           : static_cast<SDK::UObject*>(button))) {
        label->Justification = SDK::ETextJustify::Center;
        if (button->Subcontent) {
            SDK::Params::ContentWidget_SetContent sc{};
            sc.Content = label;
            Call(button->Subcontent, "SetContent", sc);
            // centre the label like the menu's own: the named slot fills the button's width and the text centres in it
            if (SDK::UPanelSlot* ns = button->Subcontent->Slot) {
                struct { SDK::EHorizontalAlignment a; } h{SDK::EHorizontalAlignment::HAlign_Fill};
                if (SDK::UFunction* f = ue::FindFunction(ns, "SetHorizontalAlignment")) ue::CallFunction(ns, f, &h);
            }
            if (sc.ReturnValue) {
                struct { SDK::EHorizontalAlignment a; } h{SDK::EHorizontalAlignment::HAlign_Fill};
                if (SDK::UFunction* f = ue::FindFunction(sc.ReturnValue, "SetHorizontalAlignment")) ue::CallFunction(sc.ReturnValue, f, &h);
            }
        }
    }

    // take out everything below Play Game, add ours, put them back (same padding, size and alignment)
    struct Kept { SDK::UWidget* w; SDK::FSlateChildSize size; SDK::FMargin pad; SDK::EHorizontalAlignment h; SDK::EVerticalAlignment v; };
    std::vector<Kept> below;
    bool after = false;
    SDK::UVerticalBoxSlot* playSlot = static_cast<SDK::UVerticalBoxSlot*>(play->Slot);
    for (int i = 0; i < box->Slots.Num(); ++i) {
        SDK::UPanelSlot* sl = box->Slots[i];
        if (!sl || !sl->Content) continue;
        if (after) {
            auto* vs = static_cast<SDK::UVerticalBoxSlot*>(sl);
            below.push_back({sl->Content, vs->Size, vs->Padding, vs->HorizontalAlignment, vs->VerticalAlignment});
        }
        if (sl->Content == play) after = true;
    }
    for (const Kept& k : below) {
        SDK::Params::PanelWidget_RemoveChild rm{};
        rm.Content = k.w;
        Call(box, "RemoveChild", rm);
    }
    auto add = [&](SDK::UWidget* w, const SDK::FSlateChildSize& size, const SDK::FMargin& pad, SDK::EHorizontalAlignment h,
                   SDK::EVerticalAlignment v) {
        SDK::Params::VerticalBox_AddChildToVerticalBox ad{};
        ad.Content = w;
        if (!Call(box, "AddChildToVerticalBox", ad) || !ad.ReturnValue) return;
        SDK::Params::VerticalBoxSlot_SetSize ss{};
        ss.InSize = size;
        Call(ad.ReturnValue, "SetSize", ss);
        SDK::Params::VerticalBoxSlot_SetPadding sp{};
        sp.InPadding = pad;
        Call(ad.ReturnValue, "SetPadding", sp);
        SDK::Params::VerticalBoxSlot_SetHorizontalAlignment sh{};
        sh.InHorizontalAlignment = h;
        Call(ad.ReturnValue, "SetHorizontalAlignment", sh);
        SDK::Params::VerticalBoxSlot_SetVerticalAlignment sv{};
        sv.InVerticalAlignment = v;
        Call(ad.ReturnValue, "SetVerticalAlignment", sv);
    };
    add(button, playSlot->Size, playSlot->Padding, playSlot->HorizontalAlignment, playSlot->VerticalAlignment);
    for (const Kept& k : below) add(k.w, k.size, k.pad, k.h, k.v);

    auto& s = S();
    s.title = titleObj;
    s.button = button;
    QD_INFO(kTag, "co-op button added to the title menu ({} buttons moved below it)", below.size());
    return true;
}

void Tick(float dt) {
    auto& s = S();
    s.checkIn -= dt;
    if (s.checkIn > 0) return;
    s.checkIn = kCheckSec;
    SDK::UObject* title = FindTitle();
    if (!title) {
        s.title = s.button = nullptr;
        s.failedFor = false;
        return;
    }
    if (title == s.title && ue::IsValid(s.button)) return;
    auto* t = static_cast<SDK::UTitleScreenUI_C*>(title);
    if (!t->LoadGameButton || !ue::IsValid(t->LoadGameButton)) return;   // the menu is built after the intro
    if (s.failedFor) return;
    if (!AddButton(title)) {
        s.failedFor = true;
        QD_WARN(kTag, "could not add the co-op button to the title menu; F5 still opens the co-op window");
    }
}

}  // namespace

void TitleButtonInit() {
    auto& s = S();
    if (s.inited) return;
    s.inited = true;
    hooks::AddPost(kButtonClicked, [](hooks::CallInfo& ci) {
        auto& st = S();
        if (st.button && ci.object == st.button) {
            SetMenuOpen(true);
            QD_INFO(kTag, "co-op window opened from the title menu");
        }
    });
    s.ticker = gamethread::AddTicker("titlebutton", [](float dt) { Tick(dt); });
    QD_INFO(kTag, "initialized");
}

}  // namespace qd::ui
