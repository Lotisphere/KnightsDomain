// Knights' Domain — ImGui overlay: D3D11 Present/ResizeBuffers hooks, WndProc subclass with input swallowing,
// CJK font, toasts, world labels and the F5 window with module tabs.
//
// Threads:
//   * Present / ResizeBuffers run on UE's RHI (or render) thread. ALL ImGui calls happen there.
//   * The WndProc runs on the window thread (the game thread in UE4). It never calls ImGui directly: messages ImGui
//     needs are queued and replayed through ImGui_ImplWin32_WndProcHandler at the start of the next ImGui frame.
//   * Toast / PublishWorldLabels / AddMenuTab / SetMenuOpen are thread-safe.
#include "qdcoop/ui/Overlay.h"
#include "qdcoop/core/Core.h"

#include "qdcoop/core/Config.h"
#include "qdcoop/core/Log.h"
#include "qdcoop/ui/I18n.h"

#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <imm.h>
#pragma comment(lib, "imm32.lib")

#include <MinHook.h>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace qd::ui {
void ApplyPanels();
namespace {

constexpr const char* kTag = "overlay";

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

constexpr int kPresentIndex = 8;
constexpr int kResizeBuffersIndex = 13;
constexpr size_t kMaxToasts = 5;
constexpr UINT kMsgRawDelta = WM_APP + 0x5D1;   // queued (render side): raw mouse delta for the virtual cursor
constexpr UINT kMsgFocusReset = WM_APP + 0x5D2; // queued (render side): menu closed -> release ImGui key state
constexpr UINT kMsgImeToggle = WM_APP + 0x5D3;  // posted to the window: wParam 1 = menu opened, 0 = closed

// ---- hook state
PresentFn       g_origPresent = nullptr;
ResizeBuffersFn g_origResize = nullptr;
void*           g_presentTarget = nullptr;
void*           g_resizeTarget = nullptr;
std::atomic<bool> g_hooksInstalled{false};
std::atomic<bool> g_disabled{false};     // not D3D11 or fatal init error: pass-through forever
std::atomic<bool> g_shuttingDown{false};

// ---- render-thread state (guarded by g_renderMx)
std::mutex                g_renderMx;
bool                      g_imguiReady = false;
IDXGISwapChain*           g_swapChain = nullptr;
ID3D11Device*             g_device = nullptr;
ID3D11DeviceContext*      g_context = nullptr;
ID3D11RenderTargetView*   g_rtv = nullptr;
HWND                      g_hwnd = nullptr;
ImVec2                    g_virtualMouse{-1, -1};
bool                      g_useVirtualMouse = false;
std::chrono::steady_clock::time_point g_lastRealMouseMove{};

// ---- window-thread state
WNDPROC                 g_origWndProc = nullptr;
std::atomic<bool>       g_wndProcInstalled{false};
std::atomic<UINT>       g_menuVk{VK_F5};
std::atomic<bool>       g_menuOpen{false};      // input captured (F5 menu OR ~ console is open)
std::atomic<bool>       g_panelMenu{false};     // F5 menu visible
std::atomic<bool>       g_panelConsole{false};  // ~ console visible

#if QD_DEV
std::mutex                                     g_consoleMx;
std::deque<std::string>                        g_consoleLines;
std::function<void(const std::string&)>        g_consoleHandler;
char                                           g_consoleInput[256] = {};
bool                                           g_consoleFocus = false;
bool                                           g_consoleScroll = false;
#endif

bool IsDebugTab(const std::string& id) { return id.rfind("debug", 0) == 0 || id == "enemies"; }
std::atomic<HWND>       g_targetHwnd{nullptr};
bool                    g_imeWasDisabled = false;   // window thread only
// Modifier state sampled on the window thread (GetKeyState is per-thread; the render thread would always read "up").
std::atomic<bool>       g_modCtrl{false}, g_modShift{false}, g_modAlt{false};

// ---- cross-thread queues
struct QueuedMsg {
    HWND   hwnd;
    UINT   msg;
    WPARAM wp;
    LPARAM lp;
};
std::mutex             g_inputMx;
std::vector<QueuedMsg> g_inputQueue;

struct Tab {
    std::string           id, title;
    std::function<void()> draw;
    int                   order = 100;
    uint64_t              seq = 0;
};
std::mutex       g_tabsMx;
std::vector<Tab> g_tabs;
uint64_t         g_tabSeq = 0;

struct ToastItem {
    std::string text;
    ToastKind   kind = ToastKind::Info;
    float       seconds = 4.0f;
    std::chrono::steady_clock::time_point start;
};
std::mutex            g_toastMx;
std::deque<ToastItem> g_toasts;

std::mutex              g_labelsMx;
std::vector<WorldLabel> g_labels;

// ------------------------------------------------------------------ helpers

UINT ParseMenuKey(std::string key) {
    for (auto& c : key) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    if (key.size() >= 2 && key[0] == 'F') {
        int n = 0;
        try { n = std::stoi(key.substr(1)); } catch (...) { n = 0; }
        if (n >= 1 && n <= 24) return VK_F1 + (n - 1);
    }
    if (key == "INSERT" || key == "INS") return VK_INSERT;
    if (key == "HOME") return VK_HOME;
    if (key == "END") return VK_END;
    if (key == "DELETE" || key == "DEL") return VK_DELETE;
    if (key == "PAGEUP" || key == "PGUP") return VK_PRIOR;
    if (key == "PAGEDOWN" || key == "PGDN") return VK_NEXT;
    if (key == "`" || key == "~" || key == "TILDE" || key == "GRAVE") return VK_OEM_3;
    if (key.size() == 1 && ((key[0] >= 'A' && key[0] <= 'Z') || (key[0] >= '0' && key[0] <= '9'))) return key[0];
    QD_WARN(kTag, "unknown menuKey \"{}\", using F5", key);
    return VK_F5;
}

void QueueMsg(HWND h, UINT m, WPARAM w, LPARAM l) {
    std::lock_guard lk(g_inputMx);
    if (g_inputQueue.size() < 4096) g_inputQueue.push_back({h, m, w, l});
}

bool IsKeyboardMsg(UINT m) { return m >= WM_KEYFIRST && m <= WM_KEYLAST; }
bool IsMouseMsg(UINT m) { return m >= WM_MOUSEFIRST && m <= WM_MOUSELAST; }
bool IsReleaseMsg(UINT m) {
    return m == WM_KEYUP || m == WM_SYSKEYUP || m == WM_LBUTTONUP || m == WM_RBUTTONUP || m == WM_MBUTTONUP ||
           m == WM_XBUTTONUP;
}

ImU32 ToastColor(ToastKind k) {
    switch (k) {
        case ToastKind::Success: return IM_COL32(126, 214, 150, 255);
        case ToastKind::Warning: return IM_COL32(244, 196, 98, 255);
        case ToastKind::Error:   return IM_COL32(236, 104, 112, 255);
        default:                 return IM_COL32(242, 167, 195, 255);   // accent pink
    }
}

void ReleaseRtv() {
    if (g_rtv) {
        g_rtv->Release();
        g_rtv = nullptr;
    }
}

bool CreateRtv() {
    if (!g_swapChain || !g_device) return false;
    ID3D11Texture2D* back = nullptr;
    if (FAILED(g_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back))) || !back) return false;
    const HRESULT hr = g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
    back->Release();
    if (FAILED(hr)) {
        g_rtv = nullptr;
        return false;
    }
    return true;
}

// ------------------------------------------------------------------ WndProc (window thread)

// Window thread. While the menu is open the window gets the default IME context so Chinese names can be typed
// (UE4 usually disassociates IME during gameplay); on close the previous "disabled" state is restored.
void ApplyImeForMenu(HWND h, bool open) {
    if (open) {
        HIMC cur = ImmGetContext(h);
        g_imeWasDisabled = cur == nullptr;
        if (cur) ImmReleaseContext(h, cur);
        if (g_imeWasDisabled) ImmAssociateContextEx(h, nullptr, IACE_DEFAULT);
    } else if (g_imeWasDisabled) {
        ImmAssociateContext(h, nullptr);
        g_imeWasDisabled = false;
    }
}

bool IsImeMsg(UINT m) {
    return m == WM_IME_STARTCOMPOSITION || m == WM_IME_ENDCOMPOSITION || m == WM_IME_COMPOSITION || m == WM_IME_NOTIFY ||
           m == WM_IME_REQUEST || m == WM_IME_COMPOSITIONFULL || m == WM_IME_SELECT;
}

LRESULT CALLBACK HookWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == kMsgImeToggle) {
        ApplyImeForMenu(h, w != 0);
        return 0;
    }
    if (!g_shuttingDown) {
        if (IsKeyboardMsg(m)) {
            g_modCtrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            g_modShift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            g_modAlt = (GetKeyState(VK_MENU) & 0x8000) != 0;
        }
        const UINT menuVk = g_menuVk.load();
        if ((m == WM_KEYDOWN || m == WM_SYSKEYDOWN) && w == menuVk) {
            if ((l & (1LL << 30)) == 0) SetMenuOpen(!g_panelMenu.load());   // ignore auto-repeat
            return 0;
        }
        if ((m == WM_KEYUP || m == WM_SYSKEYUP) && w == menuVk) return 0;
#if QD_DEV
        if ((m == WM_KEYDOWN || m == WM_SYSKEYDOWN) && w == VK_OEM_3) {   // ~ : debug console (dev flavour only)
            if ((l & (1LL << 30)) == 0) SetConsoleOpen(!g_panelConsole.load());
            return 0;
        }
        if ((m == WM_KEYUP || m == WM_SYSKEYUP) && w == VK_OEM_3) return 0;
        if (m == WM_CHAR && (w == 0x60 || w == 0x7E || w == 0xB7)) return 0;   // the ~ key's own character
#endif

        if (g_menuOpen.load()) {
            if (m == WM_INPUT) {
                // Raw mouse deltas drive a virtual cursor in case the game keeps the OS cursor locked.
                RAWINPUT ri{};
                UINT size = sizeof(ri);
                if (GetRawInputData(reinterpret_cast<HRAWINPUT>(l), RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) != UINT(-1) &&
                    ri.header.dwType == RIM_TYPEMOUSE && (ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0 &&
                    (ri.data.mouse.lLastX != 0 || ri.data.mouse.lLastY != 0)) {
                    QueueMsg(h, kMsgRawDelta, static_cast<WPARAM>(static_cast<int32_t>(ri.data.mouse.lLastX)),
                             static_cast<LPARAM>(static_cast<int32_t>(ri.data.mouse.lLastY)));
                }
                return DefWindowProcW(h, m, w, l);   // swallowed; DefWindowProc does the RIM_INPUT cleanup
            }
            if (m == WM_IME_CHAR) {   // composed character (UTF-16 unit on a Unicode window) -> ImGui text input
                QueueMsg(h, WM_CHAR, w, l);
                return 0;
            }
            if (IsImeMsg(m)) return DefWindowProcW(h, m, w, l);   // default IME UI, bypassing the game's handler
            if (IsKeyboardMsg(m) || IsMouseMsg(m)) {
                QueueMsg(h, m, w, l);
                const bool altF4 = m == WM_SYSKEYDOWN && w == VK_F4;
                // Releases always reach the game so nothing stays "held" there; Alt+F4 keeps working.
                if (!IsReleaseMsg(m) && !altF4) return 0;
            } else if (m == WM_SETCURSOR && LOWORD(l) == HTCLIENT) {
                SetCursor(nullptr);   // ImGui draws its own cursor (io.MouseDrawCursor)
                return TRUE;
            }
        }
        switch (m) {   // window state ImGui needs even while the menu is closed
            case WM_SETFOCUS:
            case WM_KILLFOCUS:
            case WM_SIZE:
            case WM_DISPLAYCHANGE:
            case WM_ACTIVATEAPP:
                QueueMsg(h, m, w, l);
                break;
            default:
                break;
        }
    }
    return CallWindowProcW(g_origWndProc, h, m, w, l);
}

// ------------------------------------------------------------------ ImGui setup (render thread)

void ApplyStyle() {
    ImGuiStyle& st = ImGui::GetStyle();
    ImGui::StyleColorsDark(&st);
    st.WindowRounding = 8.0f;
    st.ChildRounding = 8.0f;
    st.FrameRounding = 8.0f;
    st.PopupRounding = 8.0f;
    st.GrabRounding = 8.0f;
    st.TabRounding = 8.0f;
    st.ScrollbarRounding = 8.0f;
    st.WindowPadding = ImVec2(14, 12);
    st.FramePadding = ImVec2(10, 5);
    st.ItemSpacing = ImVec2(10, 7);
    st.WindowBorderSize = 0.0f;

    const ImVec4 pink(0.949f, 0.655f, 0.765f, 1.00f);      // #F2A7C3
    const ImVec4 pinkSoft(0.949f, 0.655f, 0.765f, 0.55f);
    const ImVec4 pinkDim(0.949f, 0.655f, 0.765f, 0.30f);
    ImVec4* c = st.Colors;
    c[ImGuiCol_WindowBg]           = ImVec4(0.09f, 0.08f, 0.10f, 0.90f);
    c[ImGuiCol_ChildBg]            = ImVec4(0.12f, 0.11f, 0.13f, 0.60f);
    c[ImGuiCol_PopupBg]            = ImVec4(0.10f, 0.09f, 0.11f, 0.96f);
    c[ImGuiCol_TitleBg]            = ImVec4(0.14f, 0.11f, 0.13f, 0.95f);
    c[ImGuiCol_TitleBgActive]      = ImVec4(0.30f, 0.19f, 0.24f, 0.95f);
    c[ImGuiCol_FrameBg]            = ImVec4(0.18f, 0.16f, 0.19f, 0.85f);
    c[ImGuiCol_FrameBgHovered]     = pinkDim;
    c[ImGuiCol_FrameBgActive]      = pinkSoft;
    c[ImGuiCol_Button]             = ImVec4(0.26f, 0.20f, 0.24f, 0.90f);
    c[ImGuiCol_ButtonHovered]      = pinkSoft;
    c[ImGuiCol_ButtonActive]       = pink;
    c[ImGuiCol_Header]             = pinkDim;
    c[ImGuiCol_HeaderHovered]      = pinkSoft;
    c[ImGuiCol_HeaderActive]       = pink;
    c[ImGuiCol_Tab]                = ImVec4(0.20f, 0.16f, 0.19f, 0.90f);
    c[ImGuiCol_TabHovered]         = pinkSoft;
    c[ImGuiCol_TabSelected]        = ImVec4(0.55f, 0.33f, 0.43f, 1.00f);
    c[ImGuiCol_TabSelectedOverline] = pink;
    c[ImGuiCol_CheckMark]          = pink;
    c[ImGuiCol_SliderGrab]         = pinkSoft;
    c[ImGuiCol_SliderGrabActive]   = pink;
    c[ImGuiCol_Separator]          = ImVec4(0.35f, 0.28f, 0.32f, 0.60f);
    c[ImGuiCol_TextSelectedBg]     = pinkDim;
    c[ImGuiCol_NavCursor]          = pink;
    c[ImGuiCol_ResizeGrip]         = pinkDim;
    c[ImGuiCol_ResizeGripHovered]  = pinkSoft;
    c[ImGuiCol_ResizeGripActive]   = pink;
}

void LoadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    ImFontConfig cfg;
    cfg.OversampleH = 1;   // ~21k CJK glyphs: keep the atlas reasonable
    cfg.OversampleV = 1;
    cfg.PixelSnapH = true;
    // English + Chinese + Japanese (kana are in the ChineseFull ranges) + arrows / box symbols used in UI text.
    static ImVector<ImWchar> ranges;
    if (ranges.empty()) {
        ImFontGlyphRangesBuilder b;
        b.AddRanges(io.Fonts->GetGlyphRangesChineseFull());
        b.AddRanges(io.Fonts->GetGlyphRangesJapanese());
        static const ImWchar extra[] = {0x2190, 0x21FF, 0x2500, 0x25FF, 0};   // arrows, box drawing, geometric shapes
        b.AddRanges(extra);
        b.BuildRanges(&ranges);
    }
    const wchar_t* candidates[] = {L"C:\\Windows\\Fonts\\msyh.ttc", L"C:\\Windows\\Fonts\\YuGothM.ttc",
                                   L"C:\\Windows\\Fonts\\meiryo.ttc", L"C:\\Windows\\Fonts\\simhei.ttf"};
    const char*    narrow[]     = {"C:\\Windows\\Fonts\\msyh.ttc", "C:\\Windows\\Fonts\\YuGothM.ttc",
                                   "C:\\Windows\\Fonts\\meiryo.ttc", "C:\\Windows\\Fonts\\simhei.ttf"};
    for (int i = 0; i < 4; ++i) {
        if (GetFileAttributesW(candidates[i]) == INVALID_FILE_ATTRIBUTES) continue;   // ImGui asserts on missing files
        if (io.Fonts->AddFontFromFileTTF(narrow[i], 18.0f, &cfg, ranges.Data)) {
            QD_INFO(kTag, "font: {}", narrow[i]);
            return;
        }
    }
    QD_WARN(kTag, "no CJK font found, using ImGui default font (Chinese text will not render)");
    io.Fonts->AddFontDefault();
}

bool InitImGui(IDXGISwapChain* sc) {
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(sc->GetDesc(&desc)) || !desc.OutputWindow || !IsWindowVisible(desc.OutputWindow)) return false;

    ID3D11Device* device = nullptr;
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device))) || !device) {
        QD_WARN(kTag, "swap chain is not D3D11 (D3D12/-dx12?) - overlay disabled");
        g_disabled = true;
        return false;
    }
    g_device = device;
    g_device->GetImmediateContext(&g_context);
    g_swapChain = sc;
    g_hwnd = desc.OutputWindow;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;   // never fight the game's cursor; we draw our own
    io.ConfigInputTrickleEventQueue = false;                   // our late overrides (cursor, modifiers) must win
    LoadFonts();
    ApplyStyle();

    if (!ImGui_ImplWin32_Init(g_hwnd) || !ImGui_ImplDX11_Init(g_device, g_context)) {
        QD_ERROR(kTag, "ImGui backend init failed");
        g_disabled = true;
        return false;
    }

    g_imguiReady = true;
    g_targetHwnd = g_hwnd;
    g_origWndProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&HookWndProc)));
    g_wndProcInstalled = g_origWndProc != nullptr;
    if (!g_origWndProc) QD_ERROR(kTag, "WndProc subclass failed (GetLastError={})", GetLastError());

    QD_INFO(kTag, "ImGui {} ready on hwnd {:p} ({}x{})", IMGUI_VERSION, static_cast<void*>(g_hwnd), desc.BufferDesc.Width,
            desc.BufferDesc.Height);
    return true;
}

// ------------------------------------------------------------------ per-frame drawing (render thread)

void ReplayInput() {
    std::vector<QueuedMsg> msgs;
    {
        std::lock_guard lk(g_inputMx);
        msgs.swap(g_inputQueue);
    }
    ImGuiIO& io = ImGui::GetIO();
    for (const auto& q : msgs) {
        if (q.msg == kMsgRawDelta) {
            if (g_virtualMouse.x < 0) g_virtualMouse = ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
            g_virtualMouse.x = std::clamp(g_virtualMouse.x + static_cast<float>(static_cast<int32_t>(q.wp)), 0.0f,
                                          std::max(io.DisplaySize.x - 1.0f, 0.0f));
            g_virtualMouse.y = std::clamp(g_virtualMouse.y + static_cast<float>(static_cast<int32_t>(q.lp)), 0.0f,
                                          std::max(io.DisplaySize.y - 1.0f, 0.0f));
            // Only take over when the OS cursor has not produced WM_MOUSEMOVE lately (i.e. it is locked by the game).
            if (std::chrono::steady_clock::now() - g_lastRealMouseMove > std::chrono::milliseconds(250))
                g_useVirtualMouse = true;
            continue;
        }
        if (q.msg == kMsgFocusReset) {
            io.AddFocusEvent(false);   // clears keys/buttons ImGui still thinks are held
            io.AddFocusEvent(true);
            continue;
        }
        if (q.msg == WM_MOUSEMOVE) {
            const auto x = static_cast<float>(static_cast<short>(LOWORD(q.lp)));
            const auto y = static_cast<float>(static_cast<short>(HIWORD(q.lp)));
            if (x != g_virtualMouse.x || y != g_virtualMouse.y) {
                g_lastRealMouseMove = std::chrono::steady_clock::now();
                g_useVirtualMouse = false;
            }
            g_virtualMouse = ImVec2(x, y);
        }
        ImGui_ImplWin32_WndProcHandler(q.hwnd, q.msg, q.wp, q.lp);
    }

    // These run after the backend's NewFrame, so they override its GetCursorPos / GetKeyState based values
    // (io.ConfigInputTrickleEventQueue is off: the last event of a frame wins).
    if (g_menuOpen.load()) {
        if (g_useVirtualMouse && g_virtualMouse.x >= 0) io.AddMousePosEvent(g_virtualMouse.x, g_virtualMouse.y);
        io.AddKeyEvent(ImGuiMod_Ctrl, g_modCtrl.load());
        io.AddKeyEvent(ImGuiMod_Shift, g_modShift.load());
        io.AddKeyEvent(ImGuiMod_Alt, g_modAlt.load());
    }
}

void DrawOutlinedText(ImDrawList* dl, ImVec2 pos, ImU32 color, float alpha, const std::string& text) {
    const auto a = static_cast<int>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f);
    const ImU32 shadow = IM_COL32(0, 0, 0, a);
    const ImU32 body = (color & 0x00FFFFFFu) | (static_cast<ImU32>(a * ((color >> 24) & 0xFF) / 255) << 24);
    const char* b = text.c_str();
    const char* e = b + text.size();
    dl->AddText(ImVec2(pos.x - 1, pos.y), shadow, b, e);
    dl->AddText(ImVec2(pos.x + 1, pos.y), shadow, b, e);
    dl->AddText(ImVec2(pos.x, pos.y - 1), shadow, b, e);
    dl->AddText(ImVec2(pos.x, pos.y + 1), shadow, b, e);
    dl->AddText(pos, body, b, e);
}

void DrawWorldLabels() {
    std::vector<WorldLabel> labels;
    {
        std::lock_guard lk(g_labelsMx);
        labels = g_labels;
    }
    if (labels.empty()) return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    for (const auto& l : labels) {
        if (l.alpha <= 0.01f || l.text.empty()) continue;
        const ImVec2 size = ImGui::CalcTextSize(l.text.c_str());
        DrawOutlinedText(dl, ImVec2(std::floor(l.screenX - size.x * 0.5f), std::floor(l.screenY - size.y)), l.color, l.alpha,
                         l.text);
    }
}

void DrawToasts() {
    const auto now = std::chrono::steady_clock::now();
    std::vector<ToastItem> items;
    {
        std::lock_guard lk(g_toastMx);
        while (!g_toasts.empty()) {
            const float age = std::chrono::duration<float>(now - g_toasts.front().start).count();
            if (age < g_toasts.front().seconds) break;
            g_toasts.pop_front();
        }
        // drop finished ones in the middle too (different durations)
        for (auto it = g_toasts.begin(); it != g_toasts.end();) {
            const float age = std::chrono::duration<float>(now - it->start).count();
            it = age >= it->seconds ? g_toasts.erase(it) : it + 1;
        }
        items.assign(g_toasts.begin(), g_toasts.end());
    }
    if (items.empty()) return;

    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const float wrap = 380.0f;
    const float margin = 16.0f;
    float y = margin;
    for (auto it = items.rbegin(); it != items.rend(); ++it) {   // newest on top
        const float age = std::chrono::duration<float>(now - it->start).count();
        const float fadeIn = std::clamp(age / 0.25f, 0.0f, 1.0f);
        const float fadeOut = std::clamp((it->seconds - age) / 0.5f, 0.0f, 1.0f);
        const float alpha = std::min(fadeIn, fadeOut);
        const ImVec2 ts = ImGui::CalcTextSize(it->text.c_str(), nullptr, false, wrap);
        const float w = ts.x + 30.0f, h = ts.y + 18.0f;
        const float slide = (1.0f - fadeIn) * 24.0f;
        const ImVec2 p0(io.DisplaySize.x - margin - w + slide, y);
        const ImVec2 p1(p0.x + w, p0.y + h);
        const ImU32 accent = ToastColor(it->kind);
        const int a = static_cast<int>(alpha * 255.0f);
        dl->AddRectFilled(p0, p1, IM_COL32(24, 20, 26, static_cast<int>(alpha * 225.0f)), 8.0f);
        dl->AddRectFilled(p0, ImVec2(p0.x + 5.0f, p1.y), (accent & 0x00FFFFFFu) | (static_cast<ImU32>(a) << 24), 8.0f,
                          ImDrawFlags_RoundCornersLeft);
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(p0.x + 16.0f, p0.y + 9.0f), IM_COL32(245, 240, 244, a),
                    it->text.c_str(), nullptr, wrap);
        y += h + 8.0f;
    }
}

void DrawMenu() {
    std::vector<Tab> tabs;
    {
        std::lock_guard lk(g_tabsMx);
        tabs = g_tabs;
    }
    std::stable_sort(tabs.begin(), tabs.end(), [](const Tab& a, const Tab& b) {
        return a.order != b.order ? a.order < b.order : a.seq < b.seq;
    });

    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(600, 480), ImGuiCond_FirstUseEver);
    bool open = true;
    // fixed English title in both languages ("###" keeps the window id stable across the rename).
    if (ImGui::Begin("Knights' Domain — by Mangolotis###qdcoop_main", &open, ImGuiWindowFlags_NoCollapse)) {
        if (tabs.empty()) {
            ImGui::TextDisabled("%s", Tr("menu.empty"));
        } else if (ImGui::BeginTabBar("##qdcoop_tabs")) {
            for (auto& t : tabs) {
                if (IsDebugTab(t.id)) continue;
                const std::string label = std::string(Tr(t.title.c_str())) + "###" + t.id;
                if (ImGui::BeginTabItem(label.c_str())) {
                    if (t.draw) t.draw();
                    ImGui::EndTabItem();
                }
            }
            ImGui::EndTabBar();
        }
        ImGui::Separator();
        ImGui::TextDisabled("%s", TrF("menu.footer", config::Get().menuKey).c_str());
    }
    ImGui::End();
    if (!open) SetMenuOpen(false);
}

#if QD_DEV
void DrawConsole() {
    std::vector<Tab> tabs;
    {
        std::lock_guard lk(g_tabsMx);
        for (auto& t : g_tabs)
            if (IsDebugTab(t.id)) tabs.push_back(t);
    }
    std::stable_sort(tabs.begin(), tabs.end(), [](const Tab& a, const Tab& b) {
        return a.order != b.order ? a.order < b.order : a.seq < b.seq;
    });
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(24, io.DisplaySize.y * 0.35f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(760, 520), ImGuiCond_FirstUseEver);
    bool open = true;
    if (ImGui::Begin("QD Coop 调试控制台###qdcoop_console", &open, ImGuiWindowFlags_NoCollapse)) {
        if (ImGui::BeginTabBar("##qdcoop_console_tabs")) {
            if (ImGui::BeginTabItem("指令###console_cmd")) {
                const float footer = ImGui::GetFrameHeightWithSpacing() + 4.0f;
                ImGui::BeginChild("##console_out", ImVec2(0, -footer), true, ImGuiWindowFlags_HorizontalScrollbar);
                bool scroll = false;
                {
                    std::lock_guard lk(g_consoleMx);
                    for (const auto& line : g_consoleLines) ImGui::TextUnformatted(line.c_str());
                    scroll = g_consoleScroll;
                    g_consoleScroll = false;
                }
                if (scroll) ImGui::SetScrollHereY(1.0f);
                ImGui::EndChild();
                ImGui::SetNextItemWidth(-1);
                if (g_consoleFocus) {
                    ImGui::SetKeyboardFocusHere();
                    g_consoleFocus = false;
                }
                if (ImGui::InputTextWithHint("##console_in", "输入指令后回车，help 查看全部指令", g_consoleInput,
                                             sizeof(g_consoleInput), ImGuiInputTextFlags_EnterReturnsTrue)) {
                    std::string line = g_consoleInput;
                    g_consoleInput[0] = 0;
                    g_consoleFocus = true;
                    if (!line.empty()) {
                        ConsolePrint("> " + line);
                        std::function<void(const std::string&)> h;
                        {
                            std::lock_guard lk(g_consoleMx);
                            h = g_consoleHandler;
                        }
                        if (h) gamethread::Post([h, line] { h(line); });
                        else ConsolePrint("（指令处理器还没就绪）");
                    }
                }
                ImGui::EndTabItem();
            }
            for (auto& t : tabs) {
                const std::string label = t.title + "###" + t.id;
                if (ImGui::BeginTabItem(label.c_str())) {
                    if (t.draw) t.draw();
                    ImGui::EndTabItem();
                }
            }
            ImGui::EndTabBar();
        }
        ImGui::TextDisabled("按 ~ 关闭控制台");
    }
    ImGui::End();
    if (!open) SetConsoleOpen(false);
}

void ConsoleLogSink(log::Level level, const char* tag, const std::string& msg) {
    const bool dev = tag && std::strcmp(tag, "dev") == 0;
    if (!dev && level < log::Level::Warn) return;
    if (dev && msg.rfind("> ", 0) == 0) return;   // the console already echoed the command
    std::string line = dev ? msg
                           : std::string(level == log::Level::Error ? "[错误] " : "[警告] ") + (tag ? tag : "") + ": " + msg;
    ConsolePrint(line);
}
#endif   // QD_DEV

void RenderFrame(IDXGISwapChain* sc) {
    if (!g_imguiReady) {
        if (!InitImGui(sc)) return;
    }
    if (sc != g_swapChain) {
        // The game recreated its swap chain (fullscreen toggle etc.): follow it if it is the same window/device.
        DXGI_SWAP_CHAIN_DESC desc{};
        ID3D11Device* dev = nullptr;
        if (FAILED(sc->GetDesc(&desc)) || desc.OutputWindow != g_hwnd) return;
        if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&dev))) || !dev) return;
        const bool sameDevice = dev == g_device;
        dev->Release();
        if (!sameDevice) return;
        ReleaseRtv();
        g_swapChain = sc;
        QD_INFO(kTag, "following new swap chain");
    }
    if (!g_rtv && !CreateRtv()) return;

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ReplayInput();   // after the backend's NewFrame so our virtual cursor wins over its GetCursorPos fallback

    const bool menuOpen = g_menuOpen.load();
    ImGuiIO& io = ImGui::GetIO();
    io.MouseDrawCursor = menuOpen;

    ImGui::NewFrame();
    DrawWorldLabels();
    DrawToasts();
    if (g_panelMenu.load()) DrawMenu();
#if QD_DEV
    if (g_panelConsole.load()) DrawConsole();
#endif
    ImGui::Render();

    // Keep the game's render targets intact (and don't keep a ref on the back buffer bound across ResizeBuffers).
    ID3D11RenderTargetView* oldRtv = nullptr;
    ID3D11DepthStencilView* oldDsv = nullptr;
    g_context->OMGetRenderTargets(1, &oldRtv, &oldDsv);
    g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_context->OMSetRenderTargets(1, &oldRtv, oldDsv);
    if (oldRtv) oldRtv->Release();
    if (oldDsv) oldDsv->Release();
}

HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* sc, UINT syncInterval, UINT flags) {
    if (!g_disabled && !g_shuttingDown && !(flags & DXGI_PRESENT_TEST)) {
        std::unique_lock lk(g_renderMx, std::try_to_lock);
        if (lk.owns_lock()) RenderFrame(sc);
    }
    return g_origPresent(sc, syncInterval, flags);
}

HRESULT STDMETHODCALLTYPE HookResizeBuffers(IDXGISwapChain* sc, UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags) {
    {
        std::lock_guard lk(g_renderMx);
        if (sc == g_swapChain) ReleaseRtv();   // recreated lazily on the next Present
    }
    return g_origResize(sc, count, w, h, fmt, flags);
}

// ------------------------------------------------------------------ vtable discovery

bool FindSwapChainVtable(void*& present, void*& resize) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"QDCoopDummyDXGI";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) {
        QD_ERROR(kTag, "dummy window failed ({})", GetLastError());
        return false;
    }

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 1;
    sd.BufferDesc.Width = 64;
    sd.BufferDesc.Height = 64;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    IDXGISwapChain* sc = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    HRESULT hr = E_FAIL;
    for (D3D_DRIVER_TYPE type : {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP}) {
        hr = D3D11CreateDeviceAndSwapChain(nullptr, type, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd, &sc, &dev, nullptr, &ctx);
        if (SUCCEEDED(hr)) break;
    }
    bool ok = false;
    if (SUCCEEDED(hr) && sc) {
        void** vt = *reinterpret_cast<void***>(sc);
        present = vt[kPresentIndex];
        resize = vt[kResizeBuffersIndex];
        ok = present && resize;
    } else {
        QD_ERROR(kTag, "D3D11CreateDeviceAndSwapChain failed (0x{:08X})", static_cast<uint32_t>(hr));
    }
    if (sc) sc->Release();
    if (ctx) ctx->Release();
    if (dev) dev->Release();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return ok;
}

}  // namespace

// ================================================================== public contract

bool Init() {
#if QD_DEV
    log::SetSink(&ConsoleLogSink);
#endif
    if (g_hooksInstalled) return true;
    g_menuVk = ParseMenuKey(config::Get().menuKey);

    void* present = nullptr;
    void* resize = nullptr;
    if (!FindSwapChainVtable(present, resize)) return false;

    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
        QD_ERROR(kTag, "MH_Initialize: {}", MH_StatusToString(init));
        return false;
    }
    if (MH_CreateHook(present, reinterpret_cast<void*>(&HookPresent), reinterpret_cast<void**>(&g_origPresent)) != MH_OK ||
        MH_CreateHook(resize, reinterpret_cast<void*>(&HookResizeBuffers), reinterpret_cast<void**>(&g_origResize)) != MH_OK) {
        QD_ERROR(kTag, "MH_CreateHook on IDXGISwapChain failed");
        MH_RemoveHook(present);
        MH_RemoveHook(resize);
        return false;
    }
    g_presentTarget = present;
    g_resizeTarget = resize;
    if (MH_EnableHook(present) != MH_OK || MH_EnableHook(resize) != MH_OK) {
        QD_ERROR(kTag, "MH_EnableHook on IDXGISwapChain failed");
        MH_RemoveHook(present);
        MH_RemoveHook(resize);
        return false;
    }
    g_hooksInstalled = true;
    QD_INFO(kTag, "Present/ResizeBuffers hooked (menu key {})", config::Get().menuKey);
    return true;
}

void Shutdown() {
    if (!g_hooksInstalled) return;
    g_shuttingDown = true;
    if (g_wndProcInstalled && g_hwnd && g_origWndProc) {
        SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_origWndProc));
        g_wndProcInstalled = false;
    }
    MH_DisableHook(g_presentTarget);
    MH_DisableHook(g_resizeTarget);
    MH_RemoveHook(g_presentTarget);
    MH_RemoveHook(g_resizeTarget);
    g_hooksInstalled = false;

    std::lock_guard lk(g_renderMx);   // waits for an in-flight frame
    if (g_imguiReady) {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        g_imguiReady = false;
    }
    ReleaseRtv();
    if (g_context) g_context->Release(), g_context = nullptr;
    if (g_device) g_device->Release(), g_device = nullptr;
    g_swapChain = nullptr;
}

bool IsMenuOpen() { return g_menuOpen.load(); }

void SetMenuOpen(bool open) {
    g_panelMenu = open;
    ApplyPanels();
}

#if QD_DEV
bool IsConsoleOpen() { return g_panelConsole.load(); }

void SetConsoleOpen(bool open) {
    g_panelConsole = open;
    if (open) g_consoleFocus = true;
    ApplyPanels();
}

void SetConsoleHandler(std::function<void(const std::string& line)> handler) {
    std::lock_guard lk(g_consoleMx);
    g_consoleHandler = std::move(handler);
}

void ConsolePrint(const std::string& utf8) {
    std::lock_guard lk(g_consoleMx);
    g_consoleLines.push_back(utf8);
    while (g_consoleLines.size() > 300) g_consoleLines.pop_front();
    g_consoleScroll = true;
}
#endif   // QD_DEV

void ApplyPanels() {
    const bool open = g_panelMenu.load() || g_panelConsole.load();
    const bool was = g_menuOpen.exchange(open);
    if (was == open) return;
    HWND hwnd = g_targetHwnd.load();
    if (!open) {
        std::lock_guard lk(g_inputMx);
        g_inputQueue.push_back({hwnd, kMsgFocusReset, 0, 0});
    }
    if (hwnd && g_wndProcInstalled) PostMessageW(hwnd, kMsgImeToggle, open ? 1 : 0, 0);   // IME work must run on the window thread
}

void AddMenuTab(const char* id, const char* title, std::function<void()> draw, int order) {
    std::lock_guard lk(g_tabsMx);
    const std::string sid = id ? id : "";
    for (auto& t : g_tabs) {
        if (t.id == sid) {   // re-registering replaces
            t.title = title ? title : sid;
            t.draw = std::move(draw);
            t.order = order;
            return;
        }
    }
    Tab t;
    t.id = sid;
    t.title = title ? title : sid;
    t.draw = std::move(draw);
    t.order = order;
    t.seq = ++g_tabSeq;
    g_tabs.push_back(std::move(t));
}

void Toast(const std::string& utf8, ToastKind kind, float seconds) {
    QD_INFO(kTag, "toast[{}]: {}", static_cast<int>(kind), utf8);
    std::lock_guard lk(g_toastMx);
    ToastItem t;
    t.text = utf8;
    t.kind = kind;
    t.seconds = std::max(seconds, 0.8f);
    t.start = std::chrono::steady_clock::now();
    g_toasts.push_back(std::move(t));
    while (g_toasts.size() > kMaxToasts) g_toasts.pop_front();
}

void PublishWorldLabels(std::vector<WorldLabel> labels) {
    std::lock_guard lk(g_labelsMx);
    g_labels = std::move(labels);
}

}  // namespace qd::ui
