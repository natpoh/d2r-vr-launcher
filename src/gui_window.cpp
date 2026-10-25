// gui_window.cpp - the launcher window: plain Win32, no library besides what Windows ships (comctl32 v6, GDI+).
//
// What is on it, top to bottom: a header (logo, name, version) - the game folder and what is known about it - the mods of that game as cards
// - the OpenXR runtime for this start - Play, Settings, Logs, Uninstall, About - the last message and the log of the session.
//
// How it is built, in the few places that are not obvious:
//   * Everything is laid out in device-independent units (dp = 1/96 inch) times the window's DPI, so it is sharp at 100 / 125 / 150 / 200 %
//     and re-laid out when the window moves to another monitor (WM_DPICHANGED). `--dpi N` forces a DPI for the layout (a test aid).
//   * The mod cards are one child window ("the list") that paints the card backgrounds, pictures and text itself and owns a real radio
//     button and a real link per card, so keyboard and screen readers see ordinary controls. The list scrolls when there are many mods.
//     Radio buttons and links are transparent controls: they ask their parent for the background (WM_ERASEBKGND with their own DC), and
//     the list answers by painting the cards under them.
//   * Tab and Enter work as in a dialog: the message loop calls IsDialogMessage, and DM_GETDEFID makes Enter press Play.
//   * Anything slow (hashing D2RLoader's three files, ~50 MB) runs on a worker thread and posts its result to the window.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <windowsx.h>

#include <algorithm>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

// gdiplus.h uses min and max: take them from the standard library
using std::max;
using std::min;
#include <gdiplus.h>

#include "game.h"
#include "gui_draw.h"
#include "gui_install_model.h"
#include "gui_install_view.h"
#include "gui_uninstall.h"
#include "gui_window.h"
#include "install_run.h"
#include "resource.h"

namespace vrml {
namespace gui {

namespace {

// ------------------------------------------------------------------ small helpers

std::wstring W(const std::string& s) {
    if (s.empty()) return L"";
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(std::max(n, 0)), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return "";
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(std::max(n, 0)), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

// A path as a Windows API wants it: wide, with backslashes.
std::wstring WPath(const fs::path& p) {
    fs::path q = p;
    q.make_preferred();
    return q.wstring();
}

// "a & b" for a button or label: '&' would make a mnemonic.
std::wstring NoMnemonic(std::wstring s) {
    for (size_t i = 0; i < s.size(); ++i)
        if (s[i] == L'&') s.insert(i++, 1, L'&');
    return s;
}

// StartMod's warnings name files as {game}/D2RLoader.exe and quote hashes: the log gets them with the folder prefix dropped, the message line
// under the buttons without the bracketed technical detail ("... not the reviewed 1.3.1-beta ones (...hash...). The mods were made for ...").
std::string ForLog(std::string w) {
    const std::string tag = "{game}/";
    for (size_t at; (at = w.find(tag)) != std::string::npos;) w.erase(at, tag.size());
    return w;
}

std::string ForMessage(const std::string& warning) {
    std::string w = warning;
    const size_t open = w.find(" (");
    if (open != std::string::npos) {
        int depth = 0;
        for (size_t i = open + 1; i < w.size(); ++i) {
            if (w[i] == '(') ++depth;
            else if (w[i] == ')' && --depth == 0) {
                const std::string inner = w.substr(open, i - open + 1);
                if (inner.find("SHA-256") != std::string::npos || inner.find("{game}") != std::string::npos) w.erase(open, i - open + 1);
                break;
            }
        }
    }
    return w;
}

std::string JoinArgs(const std::vector<std::string>& args) {
    std::string s;
    for (const std::string& a : args) s += (s.empty() ? "" : " ") + a;
    return s;
}

// A web link from a manifest is opened only when it is http(s): the manifest is a file in the mod's folder, not the launcher's own data.
bool IsWebUrl(const std::string& u) { return StartsWith(Lower(u), "https://") || StartsWith(Lower(u), "http://"); }

// ------------------------------------------------------------------ look

const COLORREF kBg = RGB(243, 243, 243);          // the window
const COLORREF kCard = RGB(255, 255, 255);
const COLORREF kCardBorder = RGB(205, 205, 205);
const COLORREF kCardSelected = RGB(240, 247, 253);
const COLORREF kAccent = RGB(15, 108, 189);       // the selected card, Play
const COLORREF kSlate = RGB(71, 85, 105);         // the mod's settings button: as heavy as Play, another colour
const COLORREF kSlatePressed = RGB(45, 56, 72);
const COLORREF kText = RGB(27, 27, 27);
const COLORREF kMuted = RGB(92, 92, 92);
const COLORREF kOk = RGB(14, 112, 14);
const COLORREF kWarn = RGB(150, 86, 0);           // amber, dark enough to read on the light window
const COLORREF kError = RGB(190, 30, 20);

COLORREF ToneColor(Tone t) {
    switch (t) {
        case Tone::Ok: return kOk;
        case Tone::Warn: return kWarn;
        case Tone::Error: return kError;
        default: return kText;
    }
}

// sizes in dp (1/96 inch)
constexpr int kHeaderH = 68;
constexpr int kMargin = 16;
constexpr int kMinClientW = 560;
constexpr int kMinClientH = 630;
constexpr int kDefaultClientW = 640;
constexpr int kDefaultClientH = 800;
constexpr int kListMinH = 96;
constexpr int kLogMinH = 56;

// ------------------------------------------------------------------ ids and messages

enum : int {
    IDC_GAME_LABEL = 100, IDC_GAME_COMBO, IDC_GAME_BROWSE, IDC_GAME_STATUS,
    IDC_MODS_LABEL, IDC_MOD_LIST,
    IDC_RT_LABEL, IDC_RT_COMBO, IDC_RT_BROWSE, IDC_RT_DETAIL,
    IDC_CLOSE_CHECK, IDC_PLAY, IDC_PLAY_REASON, IDC_SETTINGS, IDC_LOGS, IDC_RESERVED_UNINSTALL /* was the bottom-row button: each installed card has its own now */, IDC_ABOUT,
    IDC_MESSAGE, IDC_LOG, IDC_INSTALL_VIEW, IDC_RESERVED_UPDATE_CHECK /* was "Check for updates when the launcher starts": since 0.3.6 the window always looks */,
    IDC_CARD_RADIO = 1000,        // + the card's number
    IDC_CARD_LINK = 2000,         // the card's links: "Website" and its support place ("Discord" / "Support"), one SysLink
    IDC_CARD_INSTALL = 3000,      // the card's "Install" / "Update" button
    IDC_CARD_UNINSTALL = 4000,    // the card's "Uninstall" button (an installed D2R VR Authentic / Unleashed)
};
constexpr UINT WM_APP_LOADER = WM_APP + 1;     // lParam: LoaderResult*, the hashing of D2RLoader's files is done
constexpr UINT WM_APP_REFRESH = WM_APP + 2;    // an uninstaller has ended: look at the disk again
constexpr UINT WM_APP_SHOT = WM_APP + 3;       // take the screenshot now
constexpr UINT WM_APP_UPDATE = WM_APP + 4;     // lParam: UpdateNote*, the quiet look at one mod's newest release is done
constexpr UINT WM_APP_OPEN_INSTALL = WM_APP + 5;   // --install-view: open the install view now
constexpr UINT WM_APP_OFFER = WM_APP + 6;      // first run: offer the install view
constexpr UINT WM_APP_OPEN_CARD = WM_APP + 7;  // wParam: a card's Install / Update button was pressed (posted: its own click is over before the view opens)
constexpr UINT WM_APP_UPDATE_AGAIN = WM_APP + 8;   // the quiet check is over and another game folder was chosen while it ran: ask again
constexpr UINT_PTR kTimerPoll = 1;
constexpr UINT_PTR kTimerShotGuard = 2;
constexpr UINT kPollMs = 1500;
constexpr ULONGLONG kStartGraceMs = 10000;     // after a start: no second Play until the game shows up (or this long)
constexpr ULONGLONG kUpdateRecheckMs = 60ull * 60 * 1000;   // a window left open asks the servers again when it comes back to the front this long after the last look

const wchar_t kMainClass[] = L"D2RVRLauncherWindow";
const wchar_t kListClass[] = L"D2RVRLauncherModList";

struct LoaderResult {
    unsigned generation;
    LoaderStatus status;
};

struct UpdateNote {                // a mod's newest release, from the quiet check at start (a worker's copy: the window applies it)
    Piece piece;
    UpdateInfo info;
};

struct Fonts {
    HFONT ui = nullptr, bold = nullptr, title = nullptr, fine = nullptr, play = nullptr;
    int ui_h = 0, bold_h = 0, fine_h = 0;     // line heights
    void Destroy() {
        for (HFONT* f : {&ui, &bold, &title, &fine, &play})
            if (*f) DeleteObject(*f);
        *this = Fonts();
    }
};

// One mod's card. All rectangles are in the list's content coordinates (0,0 = the top left of the whole list, scrolled or not).
struct Card {
    int mod = -1;                  // index into Model::mods()
    bool compact = false;          // an "other" D2RLoader mod: one line
    RECT rc{}, icon{}, radio{}, desc{}, meta{}, note{}, badge{}, state{}, install{}, uninstall{}, link{}, version{};
    HWND radio_wnd = nullptr, link_wnd = nullptr, install_wnd = nullptr, uninstall_wnd = nullptr;
    std::wstring name, shown_name, description, meta_text, note_text, tip;
    std::wstring badge_text;               // amber: "Update available: 0.167.0 -> 0.168.0"
    std::wstring install_label;            // "Install" / "Update", empty = no button
    std::wstring uninstall_label;          // "Uninstall" on an installed D2R VR mod, empty = no button
    Piece piece = Piece::Loader;           // which piece the Install / Update button asks the install view for
    Gdiplus::Bitmap* picture = nullptr;      // the mod's own picture; shared (not owned) for Authentic's built-in one
    bool owns_picture = false;
    wchar_t letter = L'?';
    bool installed = false;
    bool has_link = false;
    std::vector<CardLink> links;           // Website, then Discord / Support: the links of link_wnd, in order (NMLINK's iLink)
    bool links_stacked = false;            // too wide for one line beside the card: one link under the other
};

// ------------------------------------------------------------------ the window

class Window {
public:
    Window(HINSTANCE instance, Model& model) : inst_(instance), model_(model), opt_(model.options()) {}
    ~Window();
    int Run();

private:
    static LRESULT CALLBACK MainProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK ListProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT OnMain(UINT, WPARAM, LPARAM);
    LRESULT OnList(UINT, WPARAM, LPARAM);

    int S(int dp) const { return MulDiv(dp, dpi_, 96); }

    // setup
    bool Init();
    void MakeFonts();
    void ApplyFonts();
    void LoadPictures();
    void CreateControls();
    HWND Static(int id, const wchar_t* text, DWORD style = 0);
    HWND Button(int id, const wchar_t* text, DWORD style = BS_PUSHBUTTON);

    // what the controls show
    void FillGameCombo();
    void FillRuntimeCombo();
    void RebuildCards();
    void UpdateGameLine();
    void UpdateModUi();                   // after another mod was chosen
    void UpdateRuntimeDetail();
    void UpdateButtons();
    PlayState CurrentPlayState() const;
    void Say(Tone tone, const std::string& text, bool log = true);
    void SayReady();
    void AppendLog(const std::string& text);
    void SetStatic(HWND h, const std::string& text);
    void AfterGameChange();
    void FullRefresh();

    // layout and painting
    void Layout();
    int MeasureStatic(HWND h, int width) const;
    void LayoutCards();
    int ComputeCards(int width);
    void PositionCardChildren();
    void UpdateScrollBar();
    void ScrollTo(int y);
    void EnsureCardVisible(int index);
    void PaintMain(HDC dc);
    void PaintList(HDC dc, int dx, int dy, bool foreground);
    void DrawCard(HDC dc, const Card& c, int dx, int dy, bool selected, bool foreground);
    void DrawBigButton(const DRAWITEMSTRUCT* d, const std::wstring& text, COLORREF fill_normal, COLORREF fill_pressed);
    void DrawRuntimeItem(const DRAWITEMSTRUCT* d);
    std::wstring Ellipsize(HDC dc, HFONT font, const std::wstring& s, int maxWidth) const;
    int CardAt(int x, int y) const;       // in client coordinates of the list
    void AddTooltips();

    // actions
    void OnCommand(int id, int code, HWND from);
    void OnCardChosen(int cardIndex, bool focus);
    void DoPlay();
    bool AskStartAnyway();
    void ShowOutcome(const StartOutcome& so);
    void DoSettings();
    void DoLogs();
    void OnCardUninstall(int cardIndex);
    void DoAbout();
    void OpenInstallView(const std::vector<Piece>& tick, bool explicitTick);
    void OnCardInstall(int cardIndex);
    void StartUpdateCheck(bool now);      // now: whatever the last look was (another game folder); else at most once per kUpdateRecheckMs
    void OnUpdate(UpdateNote* note);
    void UpdateOffers();                  // a card gets (or loses) its Install / Update button and badge when the answers say so
    void OfferInstallView();
    void BrowseGame();
    void BrowseRuntime();
    void OnGameComboChanged();
    void OnRuntimeComboChanged();
    void StartLoaderCheck();
    void OnLoader(LoaderResult* r);
    void Poll();
    void MaybeShot();
    void TakeShot();
    void SaveIfDirty();
    bool PickFolder(const wchar_t* title, const fs::path& start, fs::path* out);
    bool PickRuntimeFile(const fs::path& start, fs::path* out);

    HINSTANCE inst_;
    Model& model_;
    const Options& opt_;
    HWND hwnd_ = nullptr;
    int dpi_ = 96;
    Fonts fonts_;
    HBRUSH bg_brush_ = nullptr, white_brush_ = nullptr;

    HWND lbl_game_ = nullptr, cmb_game_ = nullptr, btn_game_browse_ = nullptr, st_game_ = nullptr, lbl_mods_ = nullptr, list_ = nullptr;
    HWND lbl_rt_ = nullptr, cmb_rt_ = nullptr, btn_rt_browse_ = nullptr, st_rt_ = nullptr, chk_close_ = nullptr;
    HWND btn_play_ = nullptr, st_reason_ = nullptr, btn_installview_ = nullptr, btn_settings_ = nullptr, btn_logs_ = nullptr, btn_about_ = nullptr;
    HWND st_msg_ = nullptr, edt_log_ = nullptr, tooltip_ = nullptr;

    Gdiplus::Bitmap* logo_ = nullptr;
    Gdiplus::Bitmap* authentic_icon_ = nullptr;
    Gdiplus::Bitmap* unleashed_icon_ = nullptr;   // bearstonem's icon, for D2R VR Unleashed without its own picture file

    std::vector<Card> cards_;
    RECT others_heading_{};            // "Other D2RLoader mods", content coordinates; empty when there are none
    bool has_others_ = false;
    int content_h_ = 0;
    int scroll_y_ = 0;

    Tone msg_tone_ = Tone::Normal;
    Tone game_tone_ = Tone::Normal;
    bool game_running_ = false;
    ULONGLONG grace_until_ = 0;
    bool in_list_paint_ = false;
    bool laying_out_ = false;
    bool dirty_ = false;               // settings changed in memory and not saved yet

    unsigned loader_generation_ = 0;
    bool loader_done_ = true;
    bool in_install_view_ = false;
    ULONGLONG update_asked_at_ = 0;        // when the quiet check last asked the servers (0 = not yet)
    int update_pending_ = 0;               // answers of the quiet check still to come
    bool update_again_ = false;            // another game folder was chosen while the servers were being asked: ask again for it
    int paints_ = 0;
    bool shot_requested_ = false;
    bool shot_done_ = false;
    int exit_code_ = 0;
};

Window::~Window() {
    fonts_.Destroy();
    if (bg_brush_) DeleteObject(bg_brush_);
    if (white_brush_) DeleteObject(white_brush_);
    for (Card& c : cards_)
        if (c.owns_picture) delete c.picture;
    delete logo_;
    delete authentic_icon_;
    delete unleashed_icon_;
}

// ------------------------------------------------------------------ start

int Window::Run() {
    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_STANDARD_CLASSES | ICC_LINK_CLASS | ICC_BAR_CLASSES | ICC_WIN95_CLASSES};
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = MainProc;
    wc.hInstance = inst_;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = static_cast<HICON>(LoadImageW(inst_, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0));
    wc.hIconSm = static_cast<HICON>(LoadImageW(inst_, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
    wc.lpszClassName = kMainClass;
    RegisterClassExW(&wc);
    WNDCLASSEXW lc{};
    lc.cbSize = sizeof lc;
    lc.lpfnWndProc = ListProc;
    lc.hInstance = inst_;
    lc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    lc.lpszClassName = kListClass;
    RegisterClassExW(&lc);

    // the size of the first window: the default client size at the DPI we start with, centred on the work area, never bigger than it
    const int dpi0 = opt_.dpi ? opt_.dpi : static_cast<int>(GetDpiForSystem());
    const DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
    RECT want{0, 0, MulDiv(opt_.width_dp ? opt_.width_dp : kDefaultClientW, dpi0, 96), MulDiv(opt_.height_dp ? opt_.height_dp : kDefaultClientH, dpi0, 96)};
    AdjustWindowRectExForDpi(&want, style, FALSE, 0, static_cast<UINT>(dpi0));
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    const int workW = mi.rcWork.right - mi.rcWork.left, workH = mi.rcWork.bottom - mi.rcWork.top;
    const int w = std::min<int>(want.right - want.left, workW), h = std::min<int>(want.bottom - want.top, workH);
    const int x = mi.rcWork.left + (workW - w) / 2, y = mi.rcWork.top + (workH - h) / 2;

    std::wstring title = L"D2R VR Launcher";
    if (opt_.dry_run) title += L" (dry run)";
    hwnd_ = CreateWindowExW(0, kMainClass, title.c_str(), style, x, y, w, h, nullptr, nullptr, inst_, this);
    if (!hwnd_) return 1;
    // a test run must not steal the keyboard from whoever is working
    ShowWindow(hwnd_, opt_.screenshot.empty() ? SW_SHOW : SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd_);
    if (opt_.install_view) PostMessageW(hwnd_, WM_APP_OPEN_INSTALL, 0, 0);
    else if (opt_.screenshot.empty() && !opt_.dry_run && model_.ShouldOfferInstallView()) PostMessageW(hwnd_, WM_APP_OFFER, 0, 0);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(hwnd_, &msg)) continue;      // Tab, arrows, Enter (= Play), mnemonics
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return exit_code_;
}

bool Window::Init() {
    dpi_ = opt_.dpi ? opt_.dpi : static_cast<int>(GetDpiForWindow(hwnd_));
    bg_brush_ = CreateSolidBrush(kBg);
    white_brush_ = CreateSolidBrush(kCard);
    MakeFonts();
    LoadPictures();
    CreateControls();
    ApplyFonts();

    FillGameCombo();
    FillRuntimeCombo();
    Button_SetCheck(chk_close_, model_.settings().close_on_start ? BST_CHECKED : BST_UNCHECKED);
    RebuildCards();
    UpdateGameLine();
    UpdateModUi();

    // the log of the session starts with what a support request needs
    AppendLog(std::string("D2R VR Launcher ") + kLauncherVersion);
    AppendLog("Launcher folder: " + DisplayPath(model_.home()) + (model_.first_run() ? "  (first start: nothing is remembered yet)" : ""));
    if (model_.game()) AppendLog("Game folder: " + DisplayPath(model_.game()->install.dir));
    for (const ModEntry& m : model_.mods())
        AppendLog("Mod: " + m.name + (m.installed ? (m.found_version.empty() ? (m.known ? std::string(", version not known") : std::string()) : " " + m.found_version + (m.found_from == "manifest" ? "" : m.found_from == "plugin" ? " (from its plugin)" : " (from its manifest, which is not valid)")) +
                                                        " (installed" + (m.source == "built-in" ? "" : ", " + m.source) + ")" : " (not installed)"));
    for (const std::string& n : model_.startup_notes()) AppendLog("warning: " + n);
    if (!model_.startup_notes().empty()) Say(Tone::Warn, model_.startup_notes().front(), false);
    else SayReady();

    game_running_ = model_.GameRunning();
    UpdateButtons();
    Layout();
    StartLoaderCheck();
    StartUpdateCheck(true);
    // the keyboard starts on Play when it can be pressed (Enter plays from anywhere), else on the game folder
    SetFocus(IsWindowEnabled(btn_play_) ? btn_play_ : IsWindowEnabled(cmb_game_) ? cmb_game_ : btn_game_browse_);
    SetTimer(hwnd_, kTimerPoll, kPollMs, nullptr);
    if (!opt_.screenshot.empty() && !opt_.install_view) {      // with --install-view the view takes the screenshot
        shot_requested_ = true;
        SetTimer(hwnd_, kTimerShotGuard, 20000, nullptr);      // a screenshot that never comes is a failure, not a hang
    }
    return true;
}

void Window::MakeFonts() {
    fonts_.Destroy();
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof ncm;
    if (!SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, static_cast<UINT>(dpi_))) {
        ncm.lfMessageFont = LOGFONTW{};
        ncm.lfMessageFont.lfHeight = -MulDiv(9, dpi_, 72);
    }
    LOGFONTW lf = ncm.lfMessageFont;
    wcscpy_s(lf.lfFaceName, L"Segoe UI");
    lf.lfQuality = CLEARTYPE_QUALITY;
    const int base = lf.lfHeight ? lf.lfHeight : -MulDiv(9, dpi_, 72);
    lf.lfHeight = base;
    lf.lfWeight = FW_NORMAL;
    fonts_.ui = CreateFontIndirectW(&lf);
    lf.lfWeight = FW_SEMIBOLD;
    fonts_.bold = CreateFontIndirectW(&lf);
    lf.lfWeight = FW_NORMAL;
    lf.lfHeight = base * 11 / 12;           // small: a notch under the message font
    fonts_.fine = CreateFontIndirectW(&lf);
    lf.lfWeight = FW_SEMIBOLD;
    lf.lfHeight = -MulDiv(18, dpi_, 72);    // the title
    fonts_.title = CreateFontIndirectW(&lf);
    lf.lfHeight = -MulDiv(12, dpi_, 72);    // Play
    fonts_.play = CreateFontIndirectW(&lf);

    HDC dc = GetDC(hwnd_);
    TEXTMETRICW tm{};
    auto height = [&](HFONT f) {
        HGDIOBJ old = SelectObject(dc, f);
        GetTextMetricsW(dc, &tm);
        SelectObject(dc, old);
        return static_cast<int>(tm.tmHeight);
    };
    fonts_.ui_h = height(fonts_.ui);
    fonts_.bold_h = height(fonts_.bold);
    fonts_.fine_h = height(fonts_.fine);
    ReleaseDC(hwnd_, dc);
}

void Window::ApplyFonts() {
    auto set = [&](HWND h, HFONT f) {
        if (h) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(f), TRUE);
    };
    for (HWND h : {lbl_game_, cmb_game_, btn_game_browse_, st_game_, lbl_mods_, lbl_rt_, cmb_rt_, btn_rt_browse_, st_rt_, chk_close_, st_reason_,
                   btn_installview_, btn_settings_, btn_logs_, btn_about_, st_msg_, edt_log_})
        set(h, fonts_.ui);
    set(lbl_game_, fonts_.bold);
    set(lbl_mods_, fonts_.bold);
    set(lbl_rt_, fonts_.bold);
    set(btn_play_, fonts_.play);
    set(st_reason_, fonts_.ui);
    set(list_, fonts_.ui);
    // the combo boxes: the height of the closed box and of every row of the list follow the font
    const int row = fonts_.ui_h + S(8);
    for (HWND c : {cmb_game_, cmb_rt_}) {
        SendMessageW(c, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), row);
        SendMessageW(c, CB_SETITEMHEIGHT, 0, row);
    }
    for (Card& c : cards_) {
        set(c.radio_wnd, fonts_.bold);
        set(c.link_wnd, fonts_.ui);
        set(c.install_wnd, fonts_.ui);
    }
}

void Window::LoadPictures() {
    logo_ = LoadResourcePng(IDB_LOGO);
    authentic_icon_ = LoadResourcePng(IDB_AUTHENTIC);
    unleashed_icon_ = LoadResourcePng(IDB_UNLEASHED);
}

HWND Window::Static(int id, const wchar_t* text, DWORD style) {
    return CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT | style, 0, 0, 10, 10, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst_, nullptr);
}

HWND Window::Button(int id, const wchar_t* text, DWORD style) {
    return CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | style, 0, 0, 10, 10, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst_, nullptr);
}

// The controls are created in the order Tab visits them.
void Window::CreateControls() {
    auto id = [](int i) { return reinterpret_cast<HMENU>(static_cast<INT_PTR>(i)); };
    lbl_game_ = Static(IDC_GAME_LABEL, L"&Game folder");
    cmb_game_ = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST | CBS_HASSTRINGS, 0, 0, 10, 10, hwnd_, id(IDC_GAME_COMBO), inst_, nullptr);
    btn_game_browse_ = Button(IDC_GAME_BROWSE, L"Browse...");
    st_game_ = Static(IDC_GAME_STATUS, L"", SS_NOPREFIX);
    lbl_mods_ = Static(IDC_MODS_LABEL, L"Mod to play", SS_NOPREFIX);
    list_ = CreateWindowExW(WS_EX_CONTROLPARENT, kListClass, L"Mods", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_CLIPCHILDREN, 0, 0, 10, 10, hwnd_, id(IDC_MOD_LIST), inst_, this);
    lbl_rt_ = Static(IDC_RT_LABEL, L"OpenXR &runtime for this start (the system setting is never changed)");
    cmb_rt_ = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS, 0, 0, 10, 10, hwnd_, id(IDC_RT_COMBO), inst_, nullptr);
    btn_rt_browse_ = Button(IDC_RT_BROWSE, L"Browse...");
    st_rt_ = Static(IDC_RT_DETAIL, L"", SS_NOPREFIX);
    btn_play_ = Button(IDC_PLAY, L"&Play", BS_OWNERDRAW);
    btn_settings_ = Button(IDC_SETTINGS, L"&Settings", BS_OWNERDRAW);      // the mod's settings: as big as Play, right after it (and so right after it in the Tab order)
    st_reason_ = Static(IDC_PLAY_REASON, L"", SS_NOPREFIX);
    chk_close_ = Button(IDC_CLOSE_CHECK, L"&Close the launcher when the game starts", BS_AUTOCHECKBOX);
    btn_installview_ = Button(IDC_INSTALL_VIEW, L"&Install / update mods...");
    btn_logs_ = Button(IDC_LOGS, L"&Logs");
    btn_about_ = Button(IDC_ABOUT, L"&About");
    st_msg_ = Static(IDC_MESSAGE, L"", SS_NOPREFIX);
    edt_log_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 0, 0, 10, 10, hwnd_, id(IDC_LOG), inst_, nullptr);
    SendMessageW(edt_log_, EM_SETLIMITTEXT, 400000, 0);
}

// ------------------------------------------------------------------ what the controls show

void Window::FillGameCombo() {
    SendMessageW(cmb_game_, CB_RESETCONTENT, 0, 0);
    if (model_.games().empty()) {
        SendMessageW(cmb_game_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"(no game folder found)"));
        SendMessageW(cmb_game_, CB_SETCURSEL, 0, 0);
        EnableWindow(cmb_game_, FALSE);
        return;
    }
    EnableWindow(cmb_game_, TRUE);
    HDC dc = GetDC(hwnd_);
    HGDIOBJ old = SelectObject(dc, fonts_.ui);
    int widest = 0;
    for (const GameChoice& g : model_.games()) {
        const std::wstring label = W(g.label);
        SendMessageW(cmb_game_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        SIZE sz{};
        GetTextExtentPoint32W(dc, label.c_str(), static_cast<int>(label.size()), &sz);
        widest = std::max<int>(widest, sz.cx);
    }
    SelectObject(dc, old);
    ReleaseDC(hwnd_, dc);
    SendMessageW(cmb_game_, CB_SETDROPPEDWIDTH, static_cast<WPARAM>(widest + S(32)), 0);   // a long path is readable in the list
    SendMessageW(cmb_game_, CB_SETCURSEL, static_cast<WPARAM>(std::max(model_.game_index(), 0)), 0);
}

void Window::FillRuntimeCombo() {
    SendMessageW(cmb_rt_, CB_RESETCONTENT, 0, 0);
    for (const RuntimeChoice& r : model_.runtimes()) {
        // the one a player cannot use says so in its row (the row is also drawn grey)
        const std::wstring label = W(r.label + (r.available ? "" : " (not installed)"));
        SendMessageW(cmb_rt_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    }
    SendMessageW(cmb_rt_, CB_SETCURSEL, static_cast<WPARAM>(model_.runtime_index()), 0);
}

void Window::UpdateGameLine() {
    const std::string text = model_.GameLine(&game_tone_);
    SetStatic(st_game_, text);
}

void Window::SetStatic(HWND h, const std::string& text) {
    const std::wstring w = W(text);
    SetWindowTextW(h, w.c_str());
    InvalidateRect(h, nullptr, TRUE);
}

void Window::UpdateRuntimeDetail() {
    const int idx = model_.runtime_index();
    std::string text;
    if (idx >= 0 && idx < static_cast<int>(model_.runtimes().size())) {
        const RuntimeChoice& r = model_.runtimes()[static_cast<size_t>(idx)];
        if (r.id == "file") text = DisplayPath(model_.runtime_file());
        else if (r.id == "system") text = "Windows' own choice: " + (r.detail.find(":/") != std::string::npos ? DisplayPath(P(r.detail)) : std::string("the active OpenXR runtime"));
        else text = r.detail.find(":/") != std::string::npos ? DisplayPath(P(r.detail)) : r.detail;
    }
    // a chosen runtime that is gone says why the combo shows another
    const ModEntry* m = model_.mod();
    if (m) {
        auto it = model_.settings().mods.find(m->id);
        if (it != model_.settings().mods.end() && !it->second.runtime.empty() && it->second.runtime != "system") {
            const RuntimeChoice& shown = model_.runtimes()[static_cast<size_t>(idx)];
            if (shown.id != it->second.runtime) text = "The runtime chosen before (" + it->second.runtime + ") is not available now. " + text;
        }
    }
    SetStatic(st_rt_, text);
}

void Window::UpdateModUi() {
    for (size_t i = 0; i < cards_.size(); ++i)
        if (cards_[i].radio_wnd) Button_SetCheck(cards_[i].radio_wnd, cards_[i].mod == model_.mod_index() ? BST_CHECKED : BST_UNCHECKED);
    SendMessageW(cmb_rt_, CB_SETCURSEL, static_cast<WPARAM>(model_.runtime_index()), 0);
    UpdateRuntimeDetail();
    UpdateButtons();
    if (list_) RedrawWindow(list_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

PlayState Window::CurrentPlayState() const {
    PlayState p = model_.CanPlay(game_running_);
    if (p.can && GetTickCount64() < grace_until_) {
        p.can = false;
        p.reason = "Starting the game...";
    }
    return p;
}

void Window::UpdateButtons() {
    const PlayState ps = CurrentPlayState();
    EnableWindow(btn_play_, ps.can);
    SetStatic(st_reason_, ps.can ? "" : ps.reason);
    const ModTools tools = model_.Tools();
    const ModEntry* m = model_.mod();
    EnableWindow(btn_settings_, m && m->installed && !tools.settings_exe.empty() && IsFile(tools.settings_exe));
    {
        // "<mod> Settings" (the mod's own label, else its name), with S of "Settings" as the mnemonic; a mod with no settings app: a greyed "Settings"
        std::wstring text = tools.settings_label.empty() ? L"&Settings" : W(tools.settings_label);
        const size_t at = text.rfind(L"Settings");
        if (!tools.settings_label.empty() && at != std::wstring::npos && at + 8 == text.size()) text.insert(at, L"&");
        wchar_t now[256] = L"";
        GetWindowTextW(btn_settings_, now, 256);
        if (text != now) {
            SetWindowTextW(btn_settings_, text.c_str());
            InvalidateRect(btn_settings_, nullptr, TRUE);
        }
    }
    EnableWindow(btn_logs_, m && m->installed && !tools.logs.empty());
    const int idx = model_.runtime_index();
    EnableWindow(btn_rt_browse_, idx >= 0 && idx < static_cast<int>(model_.runtimes().size()) && model_.runtimes()[static_cast<size_t>(idx)].id == "file");
    InvalidateRect(btn_play_, nullptr, TRUE);
    Layout();
}

void Window::AppendLog(const std::string& text) {
    if (!edt_log_) return;
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char stamp[16];
    std::strftime(stamp, sizeof stamp, "%H:%M:%S  ", &tm);
    std::string line;
    for (const std::string& part : Split(text, '\n')) {
        if (part.empty()) continue;
        line += stamp + part + "\r\n";
    }
    if (line.empty()) return;
    const std::wstring w = W(line);
    const int len = GetWindowTextLengthW(edt_log_);
    SendMessageW(edt_log_, EM_SETSEL, static_cast<WPARAM>(len), static_cast<LPARAM>(len));
    SendMessageW(edt_log_, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(w.c_str()));
    SendMessageW(edt_log_, EM_SCROLLCARET, 0, 0);
}

// The last message: shown under the buttons and written to the log.
void Window::Say(Tone tone, const std::string& text, bool log) {
    msg_tone_ = tone;
    SetStatic(st_msg_, text);
    if (log) AppendLog(std::string(tone == Tone::Error ? "error: " : tone == Tone::Warn ? "warning: " : "") + text);
    if (hwnd_) Layout();
}

// ------------------------------------------------------------------ the cards

// What a mod's picture is: its own file, Authentic's built-in one, or a plain square with a letter (nothing of anyone's brand).
void Window::RebuildCards() {
    if (tooltip_) {
        DestroyWindow(tooltip_);
        tooltip_ = nullptr;
    }
    for (Card& c : cards_) {
        if (c.radio_wnd) DestroyWindow(c.radio_wnd);
        if (c.link_wnd) DestroyWindow(c.link_wnd);
        if (c.install_wnd) DestroyWindow(c.install_wnd);
        if (c.uninstall_wnd) DestroyWindow(c.uninstall_wnd);
        if (c.owns_picture) delete c.picture;
    }
    cards_.clear();
    has_others_ = false;
    scroll_y_ = 0;

    const std::vector<ModEntry>& mods = model_.mods();
    // the known VR mods first (DiscoverMods lists them first), then the others in a smaller size
    for (size_t i = 0; i < mods.size(); ++i) {
        const ModEntry& m = mods[i];
        Card c;
        c.mod = static_cast<int>(i);
        c.compact = !m.known;
        c.installed = m.installed;
        c.name = W(m.name);
        c.description = W(m.description);
        // what is installed: the version found in the mod's folder (its manifest, else its plugin), never the built-in profile's
        if (m.installed && !m.found_version.empty()) c.meta_text = L"Version " + W(m.found_version);
        else if (m.installed && m.known) c.meta_text = L"Version not known";
        if (!m.publisher.empty()) c.meta_text += (c.meta_text.empty() ? L"by " : L"  -  by ") + W(m.publisher);
        if (!m.notes.empty()) c.note_text = W(m.notes.front());
        if (m.id == "d2r-vr" && authentic_icon_) {
            c.picture = authentic_icon_;
        } else if (!m.icon.empty()) {
            c.picture = LoadPictureFile(m.icon);
            c.owns_picture = c.picture != nullptr;
        }
        // D2R VR Unleashed: its own picture when its manifest names one that loads, else bearstonem's icon built in
        if (!c.picture && m.id == "d2r-pcvr" && unleashed_icon_) c.picture = unleashed_icon_;
        // the letter: the word that tells D2R VR Authentic from D2R VR Unleashed, else the first letter of the name
        std::wstring base = c.name;
        if (base.rfind(L"D2R VR ", 0) == 0 && base.size() > 7) base = base.substr(7);
        for (wchar_t ch : base)
            if (iswalnum(ch)) {
                c.letter = static_cast<wchar_t>(towupper(ch));
                break;
            }
        c.links = CardLinks(m);                                         // the small "Website" and "Discord" / "Support" links
        c.has_link = !c.links.empty();
        const Model::CardOffer offer = model_.OfferFor(m);
        if (offer.kind != Model::CardOffer::Kind::None) c.install_label = W(offer.label);
        c.badge_text = W(offer.badge);
        c.uninstall_label = W(model_.UninstallFor(m).label);
        PieceOfModId(m.id, &c.piece);
        c.tip = c.name;
        if (m.installed && !m.found_version.empty()) c.tip += L" " + W(m.found_version);
        if (!m.publisher.empty()) c.tip += L"\nby " + W(m.publisher);
        c.tip += L"\n" + W(DisplayPath(m.dir));
        for (const std::string& n : m.notes) c.tip += L"\n" + W(n);
        const std::string help = CardInstallHelp(m);                   // a known mod that is not installed: its author's words about installing it
        if (!help.empty()) c.tip += L"\n\n" + W(help);
        cards_.push_back(std::move(c));
        if (!m.known) has_others_ = true;
    }

    // real controls: the radio buttons first (they form the group), then the links (each a tab stop of its own)
    for (size_t i = 0; i < cards_.size(); ++i) {
        Card& c = cards_[i];
        DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON | BS_NOTIFY;
        if (i == 0) style |= WS_GROUP;
        c.radio_wnd = CreateWindowExW(0, L"BUTTON", NoMnemonic(c.name).c_str(), style, 0, 0, 10, 10, list_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_CARD_RADIO + static_cast<int>(i))), inst_, nullptr);
    }
    // then the Install / Update buttons, then the Website links: the first of them ends the radio group
    bool group_ended = false;
    for (size_t i = 0; i < cards_.size(); ++i) {
        Card& c = cards_[i];
        if (c.install_label.empty()) continue;
        DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON;
        if (!group_ended) style |= WS_GROUP;
        group_ended = true;
        c.install_wnd = CreateWindowExW(0, L"BUTTON", c.install_label.c_str(), style, 0, 0, 10, 10, list_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_CARD_INSTALL + static_cast<int>(i))), inst_, nullptr);
    }
    for (size_t i = 0; i < cards_.size(); ++i) {
        Card& c = cards_[i];
        if (c.uninstall_label.empty()) continue;
        DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON;
        if (!group_ended) style |= WS_GROUP;
        group_ended = true;
        c.uninstall_wnd = CreateWindowExW(0, L"BUTTON", c.uninstall_label.c_str(), style, 0, 0, 10, 10, list_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_CARD_UNINSTALL + static_cast<int>(i))), inst_, nullptr);
    }
    for (size_t i = 0; i < cards_.size(); ++i) {
        Card& c = cards_[i];
        if (!c.has_link) continue;
        DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | LWS_TRANSPARENT;
        if (!group_ended) style |= WS_GROUP;
        group_ended = true;
        std::wstring markup;
        for (const CardLink& k : c.links) markup += (markup.empty() ? L"" : L"   ") + (L"<a>" + W(k.label) + L"</a>");
        c.link_wnd = CreateWindowExW(0, WC_LINK, markup.c_str(), style, 0, 0, 10, 10, list_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_CARD_LINK + static_cast<int>(i))), inst_, nullptr);
    }
    for (Card& c : cards_) {
        SendMessageW(c.radio_wnd, WM_SETFONT, reinterpret_cast<WPARAM>(fonts_.bold), TRUE);
        if (c.link_wnd) SendMessageW(c.link_wnd, WM_SETFONT, reinterpret_cast<WPARAM>(fonts_.ui), TRUE);
        if (c.install_wnd) SendMessageW(c.install_wnd, WM_SETFONT, reinterpret_cast<WPARAM>(fonts_.ui), TRUE);
        if (c.uninstall_wnd) SendMessageW(c.uninstall_wnd, WM_SETFONT, reinterpret_cast<WPARAM>(fonts_.ui), TRUE);
    }
    UpdateModUi();
    LayoutCards();          // how tall the cards are ...
    Layout();               // ... decides how tall the list is, and the list's size lays the cards out again
    AddTooltips();
}

void Window::AddTooltips() {
    // one tooltip window, a tool per card: the card's full name, where it is and what is wrong with it (the manifest's notes)
    tooltip_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, 0, 0, 0, 0, list_, nullptr, inst_, nullptr);
    if (!tooltip_) return;
    SendMessageW(tooltip_, TTM_SETMAXTIPWIDTH, 0, S(380));
    SendMessageW(tooltip_, TTM_SETDELAYTIME, TTDT_INITIAL, 500);
    for (size_t i = 0; i < cards_.size(); ++i) {
        TTTOOLINFOW ti{};
        ti.cbSize = sizeof ti;
        ti.uFlags = TTF_SUBCLASS;
        ti.hwnd = list_;
        ti.uId = i;
        ti.lpszText = const_cast<LPWSTR>(cards_[i].tip.c_str());
        ti.rect = cards_[i].rc;
        SendMessageW(tooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
    }
}

std::wstring Window::Ellipsize(HDC dc, HFONT font, const std::wstring& s, int maxWidth) const {
    HGDIOBJ old = SelectObject(dc, font);
    SIZE sz{};
    GetTextExtentPoint32W(dc, s.c_str(), static_cast<int>(s.size()), &sz);
    std::wstring out = s;
    if (sz.cx > maxWidth) {
        size_t n = s.size();
        while (n > 1) {
            --n;
            out = s.substr(0, n) + L"…";
            GetTextExtentPoint32W(dc, out.c_str(), static_cast<int>(out.size()), &sz);
            if (sz.cx <= maxWidth) break;
        }
    }
    SelectObject(dc, old);
    return out;
}

// The geometry of every card for a list `width` px wide; returns the height of all of it.
int Window::ComputeCards(int width) {
    HDC dc = GetDC(list_);
    const int gap = S(8);
    int y = S(2);
    bool heading_done = false;
    for (Card& c : cards_) {
        if (c.compact && !heading_done) {
            heading_done = true;
            others_heading_ = RECT{0, y + S(6), width, y + S(6) + fonts_.fine_h + S(6)};
            y = others_heading_.bottom;
        }
        const int pad = c.compact ? S(8) : S(10);
        const int pic = c.compact ? S(28) : S(56);
        const int right_w = c.compact ? S(78) : S(116);
        c.rc = RECT{S(1), y, width - S(1), y};
        const int x0 = c.rc.left + pad;
        c.icon = RECT{x0, y + pad, x0 + pic, y + pad + pic};
        const int text_l = c.icon.right + S(12);
        const int text_r = std::max<int>(text_l + S(80), c.rc.right - pad - right_w - S(8));
        const int indent = S(20);                                  // under the radio's own label, not under its circle
        const int radio_h = std::max(fonts_.bold_h + S(4), S(20));
        c.radio = RECT{text_l, y + pad - (c.compact ? 0 : S(2)), text_r, y + pad - (c.compact ? 0 : S(2)) + radio_h};
        int bottom = c.radio.bottom;
        c.desc = c.meta = c.note = RECT{text_l + indent, bottom, text_r, bottom};
        if (!c.compact) {
            if (!c.description.empty()) {
                RECT r{0, 0, text_r - text_l - indent, 0};
                HGDIOBJ old = SelectObject(dc, fonts_.ui);
                DrawTextW(dc, c.description.c_str(), -1, &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);
                SelectObject(dc, old);
                const int h = std::min<int>(r.bottom, fonts_.ui_h * 2);       // two lines at most, the rest is an ellipsis
                c.desc = RECT{text_l + indent, bottom + S(1), text_r, bottom + S(1) + h};
                bottom = c.desc.bottom;
            }
            if (!c.meta_text.empty()) {
                c.meta = RECT{text_l + indent, bottom + S(3), text_r, bottom + S(3) + fonts_.fine_h};
                bottom = c.meta.bottom;
            }
            if (!c.note_text.empty()) {
                c.note = RECT{text_l + indent, bottom + S(3), text_r, bottom + S(3) + fonts_.fine_h};
                bottom = c.note.bottom;
            }
            if (!c.badge_text.empty()) {
                c.badge = RECT{text_l + indent, bottom + S(3), text_r, bottom + S(3) + fonts_.ui_h};
                bottom = c.badge.bottom;
            }
        } else if (!c.note_text.empty()) {
            // what is wrong with a smaller card's mod (a damaged manifest): one small line, in full in the tooltip
            c.note = RECT{text_l + indent, bottom + S(1), text_r, bottom + S(1) + fonts_.fine_h};
            bottom = c.note.bottom;
        }
        const int right_x = c.rc.right - pad - right_w;
        c.state = RECT{right_x, y + pad, c.rc.right - pad, y + pad + fonts_.ui_h};
        c.version = c.state;
        // under the state: the Install / Update button, then the small Website link
        int below = c.state.bottom + S(4);
        const int btn_h = fonts_.ui_h + S(10);
        c.install = RECT{right_x, below, right_x + S(90), below + btn_h};
        if (!c.install_label.empty()) below = c.install.bottom + S(4);
        c.uninstall = RECT{right_x, below, right_x + S(90), below + btn_h};
        if (!c.uninstall_label.empty()) below = c.uninstall.bottom + S(4);
        // the links: side by side when they fit the column, else one under the other (the SysLink wraps at the spaces between them)
        int links_w = 0, widest = 0;
        if (c.has_link) {
            HGDIOBJ old = SelectObject(dc, fonts_.ui);
            for (size_t k = 0; k < c.links.size(); ++k) {
                const std::wstring t = W(c.links[k].label);
                SIZE sz{};
                GetTextExtentPoint32W(dc, t.c_str(), static_cast<int>(t.size()), &sz);
                widest = std::max<int>(widest, sz.cx);
                links_w += sz.cx;
                if (k > 0) {
                    GetTextExtentPoint32W(dc, L"   ", 3, &sz);
                    links_w += sz.cx;
                }
            }
            SelectObject(dc, old);
        }
        const int link_line = fonts_.ui_h + S(4);
        c.links_stacked = c.links.size() > 1 && links_w + S(6) > right_w;
        const int link_w = c.links_stacked ? widest + S(6) : std::max<int>(S(70), links_w + S(6));
        c.link = RECT{right_x, below, right_x + link_w, below + (c.links_stacked ? static_cast<int>(c.links.size()) * fonts_.ui_h + S(4) : link_line)};
        int card_bottom = std::max<int>(c.icon.bottom, bottom) + pad;
        if (!c.install_label.empty()) card_bottom = std::max<int>(card_bottom, c.install.bottom + pad);
        if (!c.uninstall_label.empty()) card_bottom = std::max<int>(card_bottom, c.uninstall.bottom + pad);
        if (c.has_link) card_bottom = std::max<int>(card_bottom, c.link.bottom + pad);
        c.rc.bottom = card_bottom;
        y = card_bottom + gap;
    }
    ReleaseDC(list_, dc);
    return y - gap + S(2);
}

void Window::LayoutCards() {
    if (!list_) return;
    RECT client{}, full{};
    GetClientRect(list_, &client);
    GetWindowRect(list_, &full);
    const int view_h = client.bottom;
    // Is there room for all the cards without a scroll bar? If not, they are narrower by the bar. (Worked out from the window's width, not
    // the client's, so showing the bar does not change the answer.)
    const int full_w = full.right - full.left;
    int content = ComputeCards(full_w);
    if (content > view_h) content = ComputeCards(std::max(S(100), full_w - GetSystemMetricsForDpi(SM_CXVSCROLL, static_cast<UINT>(dpi_))));
    content_h_ = content;
    UpdateScrollBar();
    RedrawWindow(list_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

void Window::UpdateScrollBar() {
    RECT client{};
    GetClientRect(list_, &client);
    const int view_h = client.bottom;
    scroll_y_ = std::max(0, std::min(scroll_y_, std::max(0, content_h_ - view_h)));
    SCROLLINFO si{};
    si.cbSize = sizeof si;
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = std::max(0, content_h_ - 1);
    si.nPage = static_cast<UINT>(view_h);
    si.nPos = scroll_y_;
    SetScrollInfo(list_, SB_VERT, &si, TRUE);
    PositionCardChildren();
}

// The radio buttons and links sit at their card's place minus the scroll.
void Window::PositionCardChildren() {
    HDWP dwp = BeginDeferWindowPos(static_cast<int>(cards_.size()) * 4 + 1);
    for (Card& c : cards_) {
        const int rw = c.radio.right - c.radio.left, rh = c.radio.bottom - c.radio.top;
        if (dwp && c.radio_wnd) dwp = DeferWindowPos(dwp, c.radio_wnd, nullptr, c.radio.left, c.radio.top - scroll_y_, rw, rh, SWP_NOZORDER | SWP_NOACTIVATE);
        if (dwp && c.link_wnd) dwp = DeferWindowPos(dwp, c.link_wnd, nullptr, c.link.left, c.link.top - scroll_y_, c.link.right - c.link.left, c.link.bottom - c.link.top, SWP_NOZORDER | SWP_NOACTIVATE);
        if (dwp && c.install_wnd) dwp = DeferWindowPos(dwp, c.install_wnd, nullptr, c.install.left, c.install.top - scroll_y_, c.install.right - c.install.left, c.install.bottom - c.install.top, SWP_NOZORDER | SWP_NOACTIVATE);
        if (dwp && c.uninstall_wnd) dwp = DeferWindowPos(dwp, c.uninstall_wnd, nullptr, c.uninstall.left, c.uninstall.top - scroll_y_, c.uninstall.right - c.uninstall.left, c.uninstall.bottom - c.uninstall.top, SWP_NOZORDER | SWP_NOACTIVATE);
        // the radio's label is cut with an ellipsis here, not by the control's edge
        HDC dc = GetDC(list_);
        const std::wstring shown = Ellipsize(dc, fonts_.bold, c.name, std::max(S(40), rw - S(22)));
        ReleaseDC(list_, dc);
        if (shown != c.shown_name) {
            c.shown_name = shown;
            SetWindowTextW(c.radio_wnd, NoMnemonic(shown).c_str());
        }
    }
    if (dwp) EndDeferWindowPos(dwp);
    if (tooltip_) {
        for (size_t i = 0; i < cards_.size(); ++i) {
            TTTOOLINFOW ti{};
            ti.cbSize = sizeof ti;
            ti.hwnd = list_;
            ti.uId = i;
            ti.rect = cards_[i].rc;
            OffsetRect(&ti.rect, 0, -scroll_y_);
            SendMessageW(tooltip_, TTM_NEWTOOLRECTW, 0, reinterpret_cast<LPARAM>(&ti));
        }
    }
}

void Window::ScrollTo(int y) {
    RECT client{};
    GetClientRect(list_, &client);
    y = std::max(0, std::min(y, std::max(0, content_h_ - static_cast<int>(client.bottom))));
    if (y == scroll_y_) return;
    scroll_y_ = y;
    UpdateScrollBar();
    RedrawWindow(list_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

void Window::EnsureCardVisible(int index) {
    if (index < 0 || index >= static_cast<int>(cards_.size())) return;
    RECT client{};
    GetClientRect(list_, &client);
    const Card& c = cards_[static_cast<size_t>(index)];
    if (c.rc.top < scroll_y_) ScrollTo(c.rc.top - S(2));
    else if (c.rc.bottom > scroll_y_ + client.bottom) ScrollTo(c.rc.bottom - client.bottom + S(2));
}

int Window::CardAt(int x, int y) const {
    const int cy = y + scroll_y_;
    for (size_t i = 0; i < cards_.size(); ++i) {
        const RECT& r = cards_[i].rc;
        if (x >= r.left && x < r.right && cy >= r.top && cy < r.bottom) return static_cast<int>(i);
    }
    return -1;
}

// ------------------------------------------------------------------ layout of the window

int Window::MeasureStatic(HWND h, int width) const {
    const int n = GetWindowTextLengthW(h);
    if (n <= 0) return 0;
    std::wstring text(static_cast<size_t>(n) + 1, L'\0');
    GetWindowTextW(h, &text[0], n + 1);
    text.resize(static_cast<size_t>(n));
    HDC dc = GetDC(h);
    HGDIOBJ old = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(h, WM_GETFONT, 0, 0)));
    RECT r{0, 0, std::max(width, 10), 0};
    DrawTextW(dc, text.c_str(), -1, &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);
    SelectObject(dc, old);
    ReleaseDC(h, dc);
    return r.bottom;
}

void Window::Layout() {
    if (!hwnd_ || !list_ || laying_out_) return;
    laying_out_ = true;
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const int W_ = rc.right, H_ = rc.bottom;
    const int m = S(kMargin), w = std::max(S(240), W_ - 2 * m);
    const int line = fonts_.ui_h, label_h = fonts_.bold_h + S(2);
    const int combo_h = fonts_.ui_h + S(8) + S(2);
    // all the moves in one go: the controls repaint once, not once each
    HDWP defer = BeginDeferWindowPos(28);
    auto put = [&](HWND h, int x, int y, int ww, int hh) {
        if (defer) defer = DeferWindowPos(defer, h, nullptr, x, y, ww, hh, SWP_NOZORDER | SWP_NOACTIVATE);
    };

    // ---- above the list
    int y = S(kHeaderH) + S(14);
    put(lbl_game_, m, y, w, label_h);
    y += label_h + S(4);
    const int browse_w = S(96);
    put(cmb_game_, m, y, w - browse_w - S(8), S(300));      // the height of a combo box is that of its dropped list; the closed box follows CB_SETITEMHEIGHT
    put(btn_game_browse_, m + w - browse_w, y - S(1), browse_w, combo_h + S(2));
    y += combo_h + S(6);
    const int game_h = std::max(line, MeasureStatic(st_game_, w));
    put(st_game_, m, y, w, game_h);
    y += game_h + S(12);
    put(lbl_mods_, m, y, w, label_h);
    y += label_h + S(4);
    const int list_top = y;

    // ---- below the list, measured first: the list and the log share what is left
    const int rt_label_h = std::max(label_h, MeasureStatic(lbl_rt_, w));
    const int rt_detail_h = MeasureStatic(st_rt_, w);
    const int check_h = line + S(8);
    const int play_w = S(140), play_h = S(44);
    // the mod's settings button: as high as Play, as wide as its name needs (never less than Play), after Play on the same row
    int set_w = play_w;
    {
        wchar_t label[256] = L"";
        GetWindowTextW(btn_settings_, label, 256);
        std::wstring text;
        for (const wchar_t* c = label; *c; ++c)
            if (*c != L'&') text += *c;
        if (HDC mdc = GetDC(hwnd_)) {
            HGDIOBJ old = SelectObject(mdc, fonts_.play);
            SIZE sz{};
            if (GetTextExtentPoint32W(mdc, text.c_str(), static_cast<int>(text.size()), &sz)) set_w = std::max(play_w, static_cast<int>(sz.cx) + S(40));
            SelectObject(mdc, old);
            ReleaseDC(hwnd_, mdc);
        }
        set_w = std::min(set_w, std::max(play_w, w - play_w - S(10)));
    }
    // why Play cannot be pressed (when it cannot) goes under the row, over the width; then the checkbox
    const int reason_h = MeasureStatic(st_reason_, w);
    const int checks_h = check_h;
    const int play_row_h = play_h + S(6) + (reason_h ? reason_h + S(4) : 0) + checks_h;
    const int btn_h = combo_h + S(4);
    const int msg_h = std::min(MeasureStatic(st_msg_, w), line * 4);
    const int below = S(12) + rt_label_h + S(4) + combo_h + (rt_detail_h ? S(2) + rt_detail_h : 0) + S(10) + play_row_h + S(10) + btn_h + S(12) +
                      std::max(msg_h, line) + S(6);
    const int bottom_margin = S(12);
    const int free = std::max(0, H_ - list_top - below - bottom_margin);

    // the list takes what its cards need (more is empty space), the log the rest; neither gets less than its minimum
    const int need = content_h_ + S(2);
    int list_h = std::max(S(kListMinH), std::min(need, free - S(kLogMinH) - S(8)));
    int log_h = std::max(S(kLogMinH), free - list_h - S(8));
    put(list_, m, list_top, w, list_h);
    y = list_top + list_h + S(12);

    put(lbl_rt_, m, y, w, rt_label_h);
    y += rt_label_h + S(4);
    put(cmb_rt_, m, y, w - browse_w - S(8), S(260));
    put(btn_rt_browse_, m + w - browse_w, y - S(1), browse_w, combo_h + S(2));
    y += combo_h;
    if (rt_detail_h) {
        y += S(2);
        put(st_rt_, m, y, w, rt_detail_h);
        y += rt_detail_h;
    } else {
        put(st_rt_, m, y, w, 0);
    }
    y += S(10);
    put(btn_play_, m, y, play_w, play_h);
    put(btn_settings_, m + play_w + S(10), y, set_w, play_h);
    int cy = y + play_h + S(6);
    put(st_reason_, m, cy, w, reason_h);
    if (reason_h) cy += reason_h + S(4);
    put(chk_close_, m, cy, w, check_h);
    y += play_row_h + S(10);
    int x = m;
    const int widths[] = {S(168), S(64), S(68)};
    HWND row_buttons[] = {btn_installview_, btn_logs_, btn_about_};
    for (int i = 0; i < 3; ++i) {
        put(row_buttons[i], x, y, widths[i], btn_h);
        x += widths[i] + S(8);
    }
    y += btn_h + S(12);
    put(st_msg_, m, y, w, std::max(msg_h, line));
    y += std::max(msg_h, line) + S(6);
    put(edt_log_, m, y, w, std::max(S(kLogMinH), H_ - y - bottom_margin));
    (void)log_h;
    if (defer) EndDeferWindowPos(defer);

    laying_out_ = false;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// ------------------------------------------------------------------ painting

void Window::PaintMain(HDC dc) {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    // header: white band, logo, name, version
    RECT band{0, 0, rc.right, S(kHeaderH)};
    HBRUSH white = white_brush_;
    FillRect(dc, &band, white);
    RECT line{0, band.bottom - 1, rc.right, band.bottom};
    HBRUSH rule = CreateSolidBrush(RGB(222, 222, 222));
    FillRect(dc, &line, rule);
    DeleteObject(rule);
    const int m = S(kMargin);
    int text_x = m;
    if (logo_) {
        const int h = S(kHeaderH) - S(10);
        const int w = std::max(1, static_cast<int>(static_cast<double>(logo_->GetWidth()) * h / std::max<UINT>(1, logo_->GetHeight())));
        RECT dst{m, S(5), m + w, S(5) + h};
        DrawPicture(dc, logo_, dst);
        text_x = dst.right + S(16);
    }
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, fonts_.title);
    SetTextColor(dc, kText);
    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    const int title_h = tm.tmHeight;
    const int ty = (S(kHeaderH) - (title_h + S(2) + fonts_.fine_h)) / 2;
    RECT tr{text_x, ty, rc.right - m, ty + title_h};
    DrawTextW(dc, L"D2R VR Launcher", -1, &tr, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, fonts_.fine);
    SetTextColor(dc, kMuted);
    const std::wstring ver = L"Version " + W(kLauncherVersion);
    RECT vr{text_x, ty + title_h + S(2), rc.right - m, ty + title_h + S(2) + fonts_.fine_h};
    DrawTextW(dc, ver.c_str(), -1, &vr, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
}

// The cards, at `dx`,`dy` offset (the origin of a child's own DC when it asks for the background) minus the scroll.
void Window::PaintList(HDC dc, int dx, int dy, bool foreground) {
    RECT client{};
    GetClientRect(list_, &client);
    RECT all{dx, dy, dx + client.right, dy + client.bottom};
    FillRect(dc, &all, bg_brush_);
    const int sel = model_.mod_index();
    for (const Card& c : cards_) DrawCard(dc, c, dx, dy - scroll_y_, c.mod == sel, foreground);
    if (foreground && cards_.empty()) {
        // no game folder, or one that is not a game: say what will be here
        RECT t{dx + S(16), dy + S(8), dx + client.right - S(16), dy + client.bottom - S(8)};
        SetBkMode(dc, TRANSPARENT);
        SelectObject(dc, fonts_.ui);
        SetTextColor(dc, kMuted);
        DrawTextW(dc, L"The mods in the game folder will be listed here.", -1, &t, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    if (foreground && has_others_) {
        RECT h = others_heading_;
        OffsetRect(&h, dx, dy - scroll_y_);
        SetBkMode(dc, TRANSPARENT);
        SelectObject(dc, fonts_.fine);
        SetTextColor(dc, kMuted);
        DrawTextW(dc, L"Other D2RLoader mods", -1, &h, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    }
}

void Window::DrawCard(HDC dc, const Card& c, int dx, int dy, bool selected, bool foreground) {
    auto off = [&](RECT r) {
        OffsetRect(&r, dx, dy);
        return r;
    };
    const RECT card = off(c.rc);
    DrawRoundRect(dc, card, S(6), selected ? kCardSelected : kCard, selected ? kAccent : kCardBorder, selected ? std::max(2, S(2)) : std::max(1, S(1)));
    if (!foreground) return;
    SetBkMode(dc, TRANSPARENT);
    const RECT icon = off(c.icon);
    if (c.picture) DrawPicture(dc, c.picture, icon);
    else DrawPlaceholder(dc, icon, c.letter);
    auto text = [&](const std::wstring& s, HFONT f, COLORREF col, RECT r, UINT flags) {
        if (s.empty()) return;
        SelectObject(dc, f);
        SetTextColor(dc, col);
        DrawTextW(dc, s.c_str(), -1, &r, flags | DT_NOPREFIX);
    };
    text(c.description, fonts_.ui, kMuted, off(c.desc), DT_LEFT | DT_WORDBREAK | DT_EDITCONTROL | DT_END_ELLIPSIS);
    text(c.meta_text, fonts_.fine, kMuted, off(c.meta), DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    text(c.note_text, fonts_.fine, kWarn, off(c.note), DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    text(c.badge_text, fonts_.ui, kWarn, off(c.badge), DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);      // "Update available: 0.167.0 -> 0.168.0"
    if (c.compact) {
        // another mod: its version, small, on the right (a D2RLoader mod without data of its own has none to show)
        const ModEntry& m = model_.mods()[static_cast<size_t>(c.mod)];
        if (m.source != "plain") text(W(m.version), fonts_.fine, kMuted, off(c.version), DT_RIGHT | DT_SINGLELINE | DT_END_ELLIPSIS);
        return;
    }
    RECT st = off(c.state);
    const int dot = std::max(6, S(8));
    RECT dotr{st.left, st.top + (fonts_.ui_h - dot) / 2, st.left + dot, st.top + (fonts_.ui_h - dot) / 2 + dot};
    DrawDot(dc, dotr, c.installed ? kOk : RGB(150, 150, 150));
    st.left += dot + S(6);
    text(c.installed ? L"Installed" : L"Not installed", fonts_.ui, c.installed ? kOk : kMuted, st, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void Window::DrawBigButton(const DRAWITEMSTRUCT* d, const std::wstring& text, COLORREF fill_normal, COLORREF fill_pressed) {
    const bool disabled = (d->itemState & ODS_DISABLED) != 0, pressed = (d->itemState & ODS_SELECTED) != 0, focus = (d->itemState & ODS_FOCUS) != 0 && (d->itemState & ODS_NOFOCUSRECT) == 0;
    RECT r = d->rcItem;
    FillRect(d->hDC, &r, bg_brush_);
    const COLORREF fill = disabled ? RGB(218, 218, 218) : pressed ? fill_pressed : fill_normal;
    DrawRoundRect(d->hDC, r, S(6), fill, fill, 0);
    SetBkMode(d->hDC, TRANSPARENT);
    SelectObject(d->hDC, fonts_.play);
    SetTextColor(d->hDC, disabled ? RGB(132, 132, 132) : RGB(255, 255, 255));
    if (pressed) OffsetRect(&r, 0, 1);
    DrawTextW(d->hDC, text.c_str(), -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    if (focus && !disabled) {
        RECT f = d->rcItem;
        InflateRect(&f, -S(4), -S(4));
        DrawFocusRect(d->hDC, &f);
    }
}

void Window::DrawRuntimeItem(const DRAWITEMSTRUCT* d) {
    if (static_cast<int>(d->itemID) < 0 || d->itemID >= model_.runtimes().size()) {
        if (d->itemAction & ODA_FOCUS) DrawFocusRect(d->hDC, &d->rcItem);
        return;
    }
    const RuntimeChoice& r = model_.runtimes()[d->itemID];
    const bool selected = (d->itemState & ODS_SELECTED) != 0;
    const bool in_edit = (d->itemState & ODS_COMBOBOXEDIT) != 0;
    HBRUSH b = GetSysColorBrush(selected && !in_edit ? COLOR_HIGHLIGHT : COLOR_WINDOW);
    FillRect(d->hDC, &d->rcItem, b);
    COLORREF col = GetSysColor(selected && !in_edit ? COLOR_HIGHLIGHTTEXT : COLOR_WINDOWTEXT);
    if (!r.available) col = selected && !in_edit ? RGB(200, 200, 200) : GetSysColor(COLOR_GRAYTEXT);     // greyed: this one cannot be used
    SetBkMode(d->hDC, TRANSPARENT);
    SetTextColor(d->hDC, col);
    SelectObject(d->hDC, fonts_.ui);
    wchar_t buf[256] = L"";
    SendMessageW(d->hwndItem, CB_GETLBTEXT, d->itemID, reinterpret_cast<LPARAM>(buf));
    RECT t = d->rcItem;
    t.left += S(6);
    DrawTextW(d->hDC, buf, -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    if ((d->itemState & ODS_FOCUS) && !in_edit) DrawFocusRect(d->hDC, &d->rcItem);
}

// ------------------------------------------------------------------ actions

void Window::OnCommand(int id, int code, HWND from) {
    switch (id) {
        case IDC_GAME_COMBO:
            if (code == CBN_SELCHANGE) OnGameComboChanged();
            break;
        case IDC_GAME_BROWSE:
            if (code == BN_CLICKED) BrowseGame();
            break;
        case IDC_RT_COMBO:
            if (code == CBN_SELCHANGE) OnRuntimeComboChanged();
            break;
        case IDC_RT_BROWSE:
            if (code == BN_CLICKED) BrowseRuntime();
            break;
        case IDC_CLOSE_CHECK:
            if (code == BN_CLICKED) {
                model_.settings().close_on_start = Button_GetCheck(chk_close_) == BST_CHECKED;
                dirty_ = true;
                SaveIfDirty();
            }
            break;
        case IDC_INSTALL_VIEW:
            if (code == BN_CLICKED) OpenInstallView({}, false);
            break;
        case IDOK:                       // Enter in a dialog-like window: the default button, Play
        case IDC_PLAY:
            DoPlay();
            break;
        case IDC_SETTINGS: DoSettings(); break;
        case IDC_LOGS: DoLogs(); break;
        case IDC_ABOUT: DoAbout(); break;
        default: break;
    }
    (void)from;
}

void Window::SaveIfDirty() {
    if (dirty_ && !opt_.dry_run) model_.SaveSettings();
    dirty_ = false;
}

void Window::OnGameComboChanged() {
    const int sel = static_cast<int>(SendMessageW(cmb_game_, CB_GETCURSEL, 0, 0));
    if (sel < 0 || sel == model_.game_index()) return;
    model_.SelectGame(sel);
    dirty_ = true;
    AfterGameChange();
}

void Window::AfterGameChange() {
    AppendLog(model_.game() ? "Game folder: " + DisplayPath(model_.game()->install.dir) : std::string("No game folder"));
    RebuildCards();
    UpdateGameLine();
    UpdateModUi();
    game_running_ = model_.GameRunning();
    grace_until_ = 0;
    UpdateButtons();
    StartLoaderCheck();
    StartUpdateCheck(true);              // what is installed in this folder decides what counts as newer
    SayReady();
}

// The message under the buttons when nothing has happened yet (what the game line above says in red or amber is not repeated).
void Window::SayReady() {
    if (!model_.game() || !model_.game()->is_game) Say(Tone::Normal, "Pick the folder Diablo II: Resurrected is installed in, then a mod, then press Play.", false);
    else Say(Tone::Normal, "Choose a mod and press Play.", false);
}

// the disk changed under an open window (a mod was installed or uninstalled)
void Window::FullRefresh() {
    if (in_install_view_) return;          // the view is open (it may be in the middle of a click on a card button): looked at again when it closes
    if (!model_.Refresh()) return;
    RebuildCards();
    UpdateGameLine();
    UpdateModUi();
    if (!model_.loader_hashed()) StartLoaderCheck();
    AppendLog("The mods in the game folder changed.");
}

void Window::BrowseGame() {
    fs::path chosen;
    const fs::path start = model_.game() ? model_.game()->install.dir.parent_path() : fs::path();
    if (!PickFolder(L"Pick the folder Diablo II: Resurrected is installed in", start, &chosen)) return;
    std::string why;
    const int idx = model_.AddGame(chosen, &why);
    if (idx < 0) {
        Say(Tone::Error, why);
        return;
    }
    dirty_ = true;
    FillGameCombo();
    AfterGameChange();
}

void Window::BrowseRuntime() {
    fs::path chosen;
    if (!PickRuntimeFile(model_.runtime_file().parent_path(), &chosen)) return;
    int file_index = -1;
    for (size_t i = 0; i < model_.runtimes().size(); ++i)
        if (model_.runtimes()[i].id == "file") file_index = static_cast<int>(i);
    std::string why;
    if (file_index < 0 || !model_.SetRuntime(file_index, chosen, &why)) {
        Say(Tone::Warn, why.empty() ? "That file cannot be used." : why);
        return;
    }
    dirty_ = true;
    UpdateModUi();
}

void Window::OnRuntimeComboChanged() {
    const int sel = static_cast<int>(SendMessageW(cmb_rt_, CB_GETCURSEL, 0, 0));
    if (sel < 0 || sel >= static_cast<int>(model_.runtimes().size())) return;
    const RuntimeChoice& r = model_.runtimes()[static_cast<size_t>(sel)];
    std::string why;
    fs::path file = model_.runtime_file();
    bool ok = true;
    if (!r.available) {
        why = r.label + " is not installed on this PC.";
        ok = false;
    } else if (r.id == "file" && !IsFile(file)) {
        // nothing remembered yet: choosing "a manifest file" means choosing one now
        ok = PickRuntimeFile(file.parent_path(), &file);
        if (!ok) why = "";
    }
    if (ok) ok = model_.SetRuntime(sel, file, &why);
    if (!ok && !why.empty()) Say(Tone::Warn, why);
    if (ok) dirty_ = true;
    UpdateModUi();        // puts the combo back on the choice that stands
}

void Window::OnCardChosen(int index, bool focus) {
    if (index < 0 || index >= static_cast<int>(cards_.size())) return;
    const Card& c = cards_[static_cast<size_t>(index)];
    model_.SelectMod(c.mod);
    UpdateModUi();
    if (focus && c.radio_wnd) SetFocus(c.radio_wnd);
    EnsureCardVisible(index);
}

void Window::DoPlay() {
    const PlayState ps = CurrentPlayState();
    if (!ps.can) {
        if (!ps.reason.empty()) Say(Tone::Warn, ps.reason);
        return;
    }
    const HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));      // checking D2RLoader's files takes a moment
    StartOutcome so = model_.Play(opt_.dry_run);
    SetCursor(old);
    if (so.code == "E_FIRST_START" && AskStartAnyway()) {
        SetCursor(LoadCursorW(nullptr, IDC_WAIT));
        so = model_.Play(opt_.dry_run, true);
        SetCursor(old);
    }
    ShowOutcome(so);
}

// The game has never been started without mods (no Settings.json in Saved Games): OK stops, "Start anyway" starts as before (for when the check is wrong).
bool Window::AskStartAnyway() {
    constexpr int kStartAnyway = 100;
    const TASKDIALOG_BUTTON buttons[] = {{kStartAnyway, L"Start anyway"}};
    const std::wstring text = W(kFirstStartMessage);
    TASKDIALOGCONFIG tc = {};
    tc.cbSize = sizeof(tc);
    tc.hwndParent = hwnd_;
    tc.dwFlags = TDF_POSITION_RELATIVE_TO_WINDOW;
    tc.dwCommonButtons = TDCBF_OK_BUTTON;
    tc.pszWindowTitle = L"D2R VR Launcher";
    tc.pszMainIcon = TD_INFORMATION_ICON;
    tc.pszContent = text.c_str();
    tc.cButtons = 1;
    tc.pButtons = buttons;
    tc.nDefaultButton = IDOK;
    int pressed = IDOK;
    if (FAILED(TaskDialogIndirect(&tc, &pressed, nullptr, nullptr))) return false;
    return pressed == kStartAnyway;
}

void Window::ShowOutcome(const StartOutcome& so) {
    const ModEntry* m = model_.mod();
    const std::string name = m ? m->name : "the mod";
    if (!so.code.empty()) {
        for (const std::string& w : so.warnings) AppendLog("warning: " + ForLog(w));
        AppendLog("error [" + so.code + "]: " + so.message);
        msg_tone_ = Tone::Error;
        SetStatic(st_msg_, so.message);
        Layout();
        return;
    }
    std::string text;
    Tone tone = Tone::Ok;
    if (opt_.dry_run) {
        text = "Dry run: " + name + " would be started as shown in the log. Nothing was started.";
        AppendLog(so.plan.Describe());
    } else {
        text = "Started " + name + (m && !m->version.empty() ? " " + m->version : "") + ".";
        AppendLog("Started " + name + ": " + so.plan.exe + (so.plan.args.empty() ? "" : " " + JoinArgs(so.plan.args)) + "  (runtime " + so.plan.runtime + ", " + so.plan.store + " edition)");
    }
    for (const std::string& w : so.warnings) {
        text += " " + ForMessage(w);
        tone = Tone::Warn;
        AppendLog("warning: " + ForLog(w));
    }
    msg_tone_ = tone;
    SetStatic(st_msg_, text);
    if (!opt_.dry_run) {
        dirty_ = false;                                   // Model::Play saved what was used
        grace_until_ = GetTickCount64() + kStartGraceMs;  // no second Play until the game shows up in the process list
        UpdateButtons();
        if (model_.settings().close_on_start) {
            AppendLog("Closing the launcher (\"Close the launcher when the game starts\").");
            PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        }
    }
    Layout();
}

void Window::DoSettings() {
    const ModTools t = model_.Tools();
    if (t.settings_exe.empty() || !IsFile(t.settings_exe) || !model_.game()) return;
    std::wstring params;
    for (const std::string& a : t.settings_args) params += (params.empty() ? L"\"" : L" \"") + W(a) + L"\"";
    const std::wstring dir = WPath(model_.game()->install.dir);
    const HINSTANCE r = ShellExecuteW(hwnd_, L"open", WPath(t.settings_exe).c_str(), params.empty() ? nullptr : params.c_str(), dir.c_str(), SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(r) <= 32) Say(Tone::Error, "Could not start " + DisplayPath(t.settings_exe) + ".");
    else AppendLog("Opened the settings: " + DisplayPath(t.settings_exe));
}

void Window::DoLogs() {
    const ModTools t = model_.Tools();
    for (const fs::path& f : t.logs) {
        if (!IsFile(f)) continue;
        const std::wstring args = L"/select,\"" + WPath(f) + L"\"";
        ShellExecuteW(hwnd_, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
        AppendLog("Showing " + DisplayPath(f));
        return;
    }
    const ModEntry* m = model_.mod();
    Say(Tone::Warn, std::string("There is no log file yet") + (m ? " for " + m->name : "") + ": the mod writes it when the game has run.");
}

// The card's Uninstall: the mod's own uninstaller (never a folder deleted here). Refused while the game runs; a question that names the mod comes first; the mod's own
// uninstaller (Unleashed's without /SILENT) then asks its own; a copy with no uninstaller (a ZIP) gets the words for removing it by hand.
void Window::OnCardUninstall(int index) {
    if (index < 0 || index >= static_cast<int>(cards_.size())) return;
    const ModEntry& m = model_.mods()[static_cast<size_t>(cards_[static_cast<size_t>(index)].mod)];
    const Model::UninstallOffer o = model_.UninstallFor(m);
    if (o.kind == Model::UninstallOffer::Kind::None) return;
    const std::string refusal = model_.UninstallRefusal();
    if (!refusal.empty()) {
        Say(Tone::Warn, refusal);
        AppendLog("Not uninstalling " + m.name + ": " + refusal);
        return;
    }
    if (o.kind == Model::UninstallOffer::Kind::Explain) {
        AppendLog(m.name + " has no uninstaller here: " + DisplayPath(m.dir));
        MessageBoxW(hwnd_, W(o.explanation).c_str(), (L"Uninstall " + W(m.name)).c_str(), MB_OK | MB_ICONINFORMATION);
        return;
    }
    const std::wstring ask = W(UninstallQuestion(m, o));
    if (MessageBoxW(hwnd_, ask.c_str(), (L"Uninstall " + W(m.name)).c_str(), MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return;
    // when the uninstaller ends the disk is looked at again: the mod is gone from the list
    bool declined = false;
    if (!StartUninstaller(hwnd_, o, hwnd_, WM_APP_REFRESH, &declined)) {
        Say(Tone::Error, declined ? "The uninstaller was cancelled." : "Could not start the uninstaller.");
        return;
    }
    AppendLog("Running the uninstaller of " + m.name + ": " + DisplayPath(o.exe));
}

void Window::DoAbout() {
    std::wstring text = L"D2R VR Launcher " + W(kLauncherVersion) +
                        L"\n\nStarts Diablo II: Resurrected with one of the D2RLoader VR mods: D2R VR Authentic or D2R VR Unleashed. "
                        L"Choosing a mod is the whole switch: the launcher moves no files and never changes the system's OpenXR runtime "
                        L"(the runtime you pick applies to that start only).\n\nIt remembers its choices in:\n" +
                        W(DisplayPath(model_.home() / "settings.json")) + L"\nand writes its log to:\n" + W(DisplayPath(model_.home() / "logs" / "launcher.log"));
    MessageBoxW(hwnd_, text.c_str(), L"About D2R VR Launcher", MB_OK | MB_ICONINFORMATION);
}

// ------------------------------------------------------------------ installing and updating

// The install view (gui_install_view.cpp) over this window. `tick`: the pieces ticked at first (a card's Install / Update); else the boxes follow the facts.
void Window::OpenInstallView(const std::vector<Piece>& tick, bool explicitTick) {
    if (in_install_view_) return;
    if (!model_.settings().install_view_offered && opt_.screenshot.empty()) {
        model_.settings().install_view_offered = true;         // the player has seen it (the Setup's finish page, the button, the offer): no first-run offer on top of that
        dirty_ = true;
        SaveIfDirty();
    }
    in_install_view_ = true;
    const InstallViewResult r = ShowInstallView(inst_, hwnd_, model_, tick, explicitTick);
    in_install_view_ = false;
    if (r.shot_failed) exit_code_ = 5;
    if (!opt_.screenshot.empty() && opt_.install_view) {          // the screenshot of the view is the whole run
        DestroyWindow(hwnd_);
        return;
    }
    FullRefresh();                                // what the view installed (or the Setups it ran)
    if (r.changed) StartLoaderCheck();            // D2RLoader's files may have been replaced: look at them again
    UpdateOffers();                               // the view asked the servers too
    UpdateGameLine();
    UpdateButtons();
}

void Window::OnCardInstall(int index) {
    if (index < 0 || index >= static_cast<int>(cards_.size())) return;
    OnCardChosen(index, false);
    // not from inside the button's own click: the cards are made again when the view closes
    PostMessageW(hwnd_, WM_APP_OPEN_CARD, static_cast<WPARAM>(index), 0);
}

// A card has an Install / Update button and a badge when the model says so: when the servers' answers change that, the cards are made again (once).
void Window::UpdateOffers() {
    bool differ = false;
    for (const Card& c : cards_) {
        const ModEntry& m = model_.mods()[static_cast<size_t>(c.mod)];
        const Model::CardOffer o = model_.OfferFor(m);
        if (W(o.label) != c.install_label || W(o.badge) != c.badge_text || W(model_.UninstallFor(m).label) != c.uninstall_label) differ = true;
    }
    if (!differ) return;
    HWND f = GetFocus();
    const int fid = f ? GetDlgCtrlID(f) : 0;
    RebuildCards();
    if (fid >= IDC_CARD_RADIO && fid < IDC_CARD_RADIO + static_cast<int>(cards_.size()) && cards_[static_cast<size_t>(fid - IDC_CARD_RADIO)].radio_wnd)
        SetFocus(cards_[static_cast<size_t>(fid - IDC_CARD_RADIO)].radio_wnd);
}

// The quiet look: one request per source, in the background - the same question the install view asks. Always at start (it is not a setting since 0.3.6:
// a player who had unticked the old box saw no update on the cards while the install view showed them), again when another game folder is chosen, and when
// a window left open comes back to the front an hour or more after the last look. Nothing is shown when it fails.
void Window::StartUpdateCheck(bool now) {
    if (!model_.UpdateCheckWanted()) return;
    // a screenshot run asks the internet only when a test server was named (VRML_ALLOW_HTTP_HOST): tests never use the real one; and only once
    if (!opt_.screenshot.empty() && (TestHttpHost(model_.services().env).empty() || update_asked_at_)) return;
    if (update_pending_ > 0) {                          // the servers are being asked: the answers may be for the folder chosen before
        update_again_ = update_again_ || now;
        return;
    }
    if (!now && update_asked_at_ && GetTickCount64() - update_asked_at_ < kUpdateRecheckMs) return;
    update_asked_at_ = GetTickCount64();
    update_pending_ = 2;                                // a screenshot waits for both answers
    struct Job {
        HWND target;
        fs::path game;
        std::map<std::string, std::string> env;
    };
    auto* job = new Job{hwnd_, model_.game()->install.dir, model_.services().env};
    HANDLE th = CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
        std::unique_ptr<Job> j(static_cast<Job*>(p));
        for (Piece m : {Piece::Authentic, Piece::Unleashed}) {
            auto* n = new UpdateNote{m, CheckModRelease(m, j->game, j->env)};
            if (!PostMessageW(j->target, WM_APP_UPDATE, 0, reinterpret_cast<LPARAM>(n))) {
                delete n;
                return 0;
            }
        }
        return 0;
    }, job, 0, nullptr);
    if (th) {
        CloseHandle(th);
    } else {
        delete job;
        update_pending_ = 0;
    }
}

void Window::OnUpdate(UpdateNote* raw) {
    std::unique_ptr<UpdateNote> n(raw);
    const bool quiet = in_install_view_;               // the view is open: the cards are made again when it closes
    if (update_pending_ > 0) --update_pending_;
    if (update_pending_ == 0 && update_again_) {        // another game folder was chosen meanwhile: ask for it (this answer still counts until then)
        update_again_ = false;
        PostMessageW(hwnd_, WM_APP_UPDATE_AGAIN, 0, 0);
    }
    if (!n->info.checked) {
        model_.Log(std::string("update check: ") + PieceName(n->piece) + ": " + n->info.error);       // quiet: in the support log only
        MaybeShot();
        return;
    }
    model_.SetLatest(n->info);
    const Release& r = n->info.latest;
    model_.Log(std::string("update check: ") + PieceName(n->piece) + ": newest " + (r.up_to_date ? std::string("is what is installed") : r.version));
    for (const ModEntry& m : model_.mods()) {
        if (m.id != PieceId(n->piece)) continue;
        const Model::CardOffer o = model_.OfferFor(m);
        if (!o.badge.empty()) AppendLog(m.name + ": " + o.badge);
    }
    if (!quiet) UpdateOffers();
    MaybeShot();
}

// First run: no VR mod is installed in the game folder yet. Offered once (whatever the answer, it is not asked again).
void Window::OfferInstallView() {
    if (!model_.ShouldOfferInstallView()) return;
    model_.settings().install_view_offered = true;
    dirty_ = true;
    SaveIfDirty();
    const std::wstring ask = L"Neither D2R VR Authentic nor D2R VR Unleashed is installed in this game folder yet.\n\n"
                             L"Open the install view to choose what to install? It lists D2RLoader and both mods, downloads what you tick, checks it and "
                             L"installs it. You can open it later with \"Install / update mods...\".";
    if (MessageBoxW(hwnd_, ask.c_str(), L"Install a VR mod", MB_YESNO | MB_ICONQUESTION) == IDYES) OpenInstallView({}, false);
}

// ------------------------------------------------------------------ the loader check and the poll

void Window::StartLoaderCheck() {
    const GameChoice* g = model_.game();
    if (!g || !g->is_game || !IsFile(g->install.dir / "D2RLoader.exe")) {
        loader_done_ = true;
        MaybeShot();
        return;
    }
    loader_done_ = false;
    const unsigned gen = ++loader_generation_;
    const fs::path dir = g->install.dir;
    const HWND target = hwnd_;
    // ~50 MB of hashing: not on the thread that paints. (CreateThread, not std::thread: this project has its own process.h, which the
    // standard library's <thread> would pick up instead of the C runtime's.)
    struct Job {
        fs::path dir;
        unsigned gen;
        HWND target;
    };
    auto* job = new Job{dir, gen, target};
    HANDLE th = CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
        std::unique_ptr<Job> j(static_cast<Job*>(p));
        auto* r = new LoaderResult{j->gen, CheckLoader(j->dir, true)};
        if (!PostMessageW(j->target, WM_APP_LOADER, 0, reinterpret_cast<LPARAM>(r))) delete r;
        return 0;
    }, job, 0, nullptr);
    if (th) CloseHandle(th);
    else delete job;
}

void Window::OnLoader(LoaderResult* r) {
    std::unique_ptr<LoaderResult> own(r);
    if (own->generation != loader_generation_) return;            // another game was chosen meanwhile
    model_.SetLoaderStatus(own->status);
    loader_done_ = true;
    UpdateGameLine();
    for (const std::string& n : own->status.notes) AppendLog("D2RLoader: " + ForLog(n));
    UpdateButtons();
    MaybeShot();
}

void Window::Poll() {
    const bool running = model_.GameRunning();
    bool changed = running != game_running_;
    game_running_ = running;
    if (running) grace_until_ = 0;                                  // it showed up: the real state takes over
    else if (grace_until_ && GetTickCount64() >= grace_until_) {
        grace_until_ = 0;
        changed = true;
    }
    if (changed) UpdateButtons();
}

// ------------------------------------------------------------------ screenshot

void Window::MaybeShot() {
    if (!shot_requested_ || shot_done_ || !loader_done_ || update_pending_ > 0 || paints_ == 0) return;
    shot_done_ = true;
    PostMessageW(hwnd_, WM_APP_SHOT, 0, 0);
}

void Window::TakeShot() {
    // one more full paint, so what PrintWindow sees is final
    RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    std::string error;
    if (!SaveClientPng(hwnd_, fs::u8path(opt_.screenshot).wstring(), &error)) {
        exit_code_ = 5;
        MessageBeep(MB_ICONERROR);
        std::wstring m = L"screenshot failed: " + W(error);
        OutputDebugStringW(m.c_str());
    }
    DestroyWindow(hwnd_);
}

// ------------------------------------------------------------------ pickers

bool Window::PickFolder(const wchar_t* title, const fs::path& start, fs::path* out) {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return false;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dlg->SetTitle(title);
    if (!start.empty() && IsDir(start)) {
        IShellItem* si = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(WPath(start).c_str(), nullptr, IID_PPV_ARGS(&si)))) {
            dlg->SetFolder(si);
            si->Release();
        }
    }
    bool ok = false;
    if (SUCCEEDED(dlg->Show(hwnd_))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                *out = fs::path(path);
                CoTaskMemFree(path);
                ok = true;
            }
            item->Release();
        }
    }
    dlg->Release();
    return ok;
}

bool Window::PickRuntimeFile(const fs::path& start, fs::path* out) {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return false;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);
    dlg->SetTitle(L"Pick an OpenXR runtime manifest (a .json file)");
    const COMDLG_FILTERSPEC filters[] = {{L"OpenXR runtime manifest (*.json)", L"*.json"}, {L"All files", L"*.*"}};
    dlg->SetFileTypes(2, filters);
    if (!start.empty() && IsDir(start)) {
        IShellItem* si = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(WPath(start).c_str(), nullptr, IID_PPV_ARGS(&si)))) {
            dlg->SetFolder(si);
            si->Release();
        }
    }
    bool ok = false;
    if (SUCCEEDED(dlg->Show(hwnd_))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                *out = fs::path(path);
                CoTaskMemFree(path);
                ok = true;
            }
            item->Release();
        }
    }
    dlg->Release();
    return ok;
}

// ------------------------------------------------------------------ the main window's messages

LRESULT CALLBACK Window::MainProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    Window* self = nullptr;
    if (m == WM_NCCREATE) {
        self = static_cast<Window*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = h;
    } else {
        self = reinterpret_cast<Window*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    }
    if (!self) return DefWindowProcW(h, m, w, l);
    return self->OnMain(m, w, l);
}

LRESULT Window::OnMain(UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_CREATE:
            return Init() ? 0 : -1;

        case WM_ERASEBKGND: {
            // also what a transparent child (the checkbox) asks for with its own DC
            RECT r{};
            GetClipBox(reinterpret_cast<HDC>(w), &r);
            FillRect(reinterpret_cast<HDC>(w), &r, bg_brush_);
            return 1;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd_, &ps);
            PaintMain(dc);
            EndPaint(hwnd_, &ps);
            ++paints_;
            MaybeShot();
            return 0;
        }
        case WM_PRINTCLIENT:
            PaintMain(reinterpret_cast<HDC>(w));
            return 0;

        case WM_SIZE:
            if (w != SIZE_MINIMIZED) Layout();
            return 0;

        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(l);
            RECT r{0, 0, S(kMinClientW), S(kMinClientH)};
            AdjustWindowRectExForDpi(&r, static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_STYLE)), FALSE, 0, static_cast<UINT>(dpi_));
            mmi->ptMinTrackSize.x = r.right - r.left;
            mmi->ptMinTrackSize.y = r.bottom - r.top;
            return 0;
        }

        case WM_DPICHANGED: {
            if (!opt_.dpi) dpi_ = HIWORD(w);
            const RECT* r = reinterpret_cast<const RECT*>(l);
            SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            MakeFonts();
            ApplyFonts();
            RebuildCards();
            Layout();
            InvalidateRect(hwnd_, nullptr, TRUE);
            return 0;
        }

        case WM_COMMAND:
            OnCommand(LOWORD(w), HIWORD(w), reinterpret_cast<HWND>(l));
            return 0;

        case DM_GETDEFID:
            // Enter presses Play while it can be pressed
            return IsWindowEnabled(btn_play_) ? MAKELONG(IDC_PLAY, DC_HASDEFID) : 0;

        case WM_DRAWITEM: {
            const auto* d = reinterpret_cast<const DRAWITEMSTRUCT*>(l);
            if (d->CtlID == IDC_PLAY) DrawBigButton(d, L"Play", kAccent, RGB(10, 78, 138));
            else if (d->CtlID == IDC_SETTINGS) {
                wchar_t buf[256] = L"";
                GetWindowTextW(d->hwndItem, buf, 256);
                std::wstring text;
                for (const wchar_t* c = buf; *c; ++c)       // the mnemonic marker is not drawn
                    if (*c != L'&') text += *c;
                DrawBigButton(d, text, kSlate, kSlatePressed);
            }
            else if (d->CtlID == IDC_RT_COMBO) DrawRuntimeItem(d);
            return TRUE;
        }
        case WM_MEASUREITEM: {
            auto* mi = reinterpret_cast<MEASUREITEMSTRUCT*>(l);
            if (mi->CtlID == IDC_RT_COMBO) mi->itemHeight = static_cast<UINT>(fonts_.ui_h + S(8));
            return TRUE;
        }

        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT: {
            HDC dc = reinterpret_cast<HDC>(w);
            const int id = GetDlgCtrlID(reinterpret_cast<HWND>(l));
            if (id == IDC_LOG) {
                SetTextColor(dc, kText);
                SetBkColor(dc, kCard);
                return reinterpret_cast<LRESULT>(white_brush_);
            }
            COLORREF col = kText;
            if (id == IDC_GAME_STATUS) col = ToneColor(game_tone_ == Tone::Normal ? Tone::Normal : game_tone_);
            else if (id == IDC_MESSAGE) col = ToneColor(msg_tone_);
            else if (id == IDC_RT_DETAIL) col = kMuted;
            else if (id == IDC_PLAY_REASON) col = kWarn;
            SetTextColor(dc, col);
            SetBkColor(dc, kBg);
            return reinterpret_cast<LRESULT>(bg_brush_);
        }

        case WM_TIMER:
            if (w == kTimerPoll) {
                Poll();
            } else if (w == kTimerShotGuard) {
                KillTimer(hwnd_, kTimerShotGuard);
                if (!shot_done_) {
                    exit_code_ = 5;
                    DestroyWindow(hwnd_);
                }
            }
            return 0;

        case WM_ACTIVATEAPP:
            if (w) {
                FullRefresh();               // came back from Explorer or an installer: the mods may have changed
                StartUpdateCheck(false);     // and left open for an hour or more: a release may have come out meanwhile
            }
            return 0;

        case WM_APP_LOADER:
            OnLoader(reinterpret_cast<LoaderResult*>(l));
            return 0;
        case WM_APP_REFRESH:
            FullRefresh();
            UpdateButtons();
            return 0;
        case WM_APP_SHOT:
            TakeShot();
            return 0;
        case WM_APP_UPDATE:
            OnUpdate(reinterpret_cast<UpdateNote*>(l));
            return 0;
        case WM_APP_OPEN_INSTALL: {
            std::vector<Piece> tick;
            for (const std::string& word : opt_.install_tick) {
                Piece p;
                if (ParsePiece(word, &p)) tick.push_back(p);
            }
            OpenInstallView(tick, !opt_.install_tick.empty());
            return 0;
        }
        case WM_APP_OPEN_CARD:
            if (w < cards_.size()) OpenInstallView({cards_[w].piece}, true);
            return 0;
        case WM_APP_OFFER:
            OfferInstallView();
            return 0;
        case WM_APP_UPDATE_AGAIN:
            StartUpdateCheck(true);
            return 0;

        case WM_CLOSE:
            SaveIfDirty();
            DestroyWindow(hwnd_);
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd_, kTimerPoll);
            PostQuitMessage(exit_code_);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd_, m, w, l);
}

// ------------------------------------------------------------------ the list's messages

LRESULT CALLBACK Window::ListProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    Window* self = nullptr;
    if (m == WM_NCCREATE) {
        self = static_cast<Window*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->list_ = h;                 // messages arrive during CreateWindowEx, before it returns the handle
    } else {
        self = reinterpret_cast<Window*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    }
    if (!self) return DefWindowProcW(h, m, w, l);
    return self->OnList(m, w, l);
}

LRESULT Window::OnList(UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_ERASEBKGND: {
            if (in_list_paint_) return 1;       // our own WM_PAINT paints everything in one go
            // a transparent child (a radio button, a link) asks for the background under itself, in its own DC: draw with the DC's origin
            // reset, so GDI and GDI+ agree on where things are
            HDC dc = reinterpret_cast<HDC>(w);
            POINT org{};
            GetViewportOrgEx(dc, &org);
            SetViewportOrgEx(dc, 0, 0, nullptr);
            PaintList(dc, org.x, org.y, false);
            SetViewportOrgEx(dc, org.x, org.y, nullptr);
            return 1;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            in_list_paint_ = true;
            HDC dc = BeginPaint(list_, &ps);
            in_list_paint_ = false;
            RECT client{};
            GetClientRect(list_, &client);
            HDC mem = CreateCompatibleDC(dc);
            HBITMAP bmp = CreateCompatibleBitmap(dc, client.right, client.bottom);
            HGDIOBJ old = SelectObject(mem, bmp);
            PaintList(mem, 0, 0, true);
            BitBlt(dc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left, ps.rcPaint.bottom - ps.rcPaint.top, mem, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
            SelectObject(mem, old);
            DeleteObject(bmp);
            DeleteDC(mem);
            EndPaint(list_, &ps);
            return 0;
        }
        case WM_PRINTCLIENT:
            PaintList(reinterpret_cast<HDC>(w), 0, 0, true);
            return 0;

        case WM_SIZE:
            LayoutCards();
            return 0;

        case WM_VSCROLL: {
            SCROLLINFO si{};
            si.cbSize = sizeof si;
            si.fMask = SIF_ALL;
            GetScrollInfo(list_, SB_VERT, &si);
            int y = scroll_y_;
            switch (LOWORD(w)) {
                case SB_LINEUP: y -= fonts_.ui_h * 3; break;
                case SB_LINEDOWN: y += fonts_.ui_h * 3; break;
                case SB_PAGEUP: y -= static_cast<int>(si.nPage); break;
                case SB_PAGEDOWN: y += static_cast<int>(si.nPage); break;
                case SB_THUMBTRACK:
                case SB_THUMBPOSITION: y = si.nTrackPos; break;
                case SB_TOP: y = 0; break;
                case SB_BOTTOM: y = content_h_; break;
                default: break;
            }
            ScrollTo(y);
            return 0;
        }
        case WM_MOUSEWHEEL: {
            UINT lines = 3;
            SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
            const int delta = GET_WHEEL_DELTA_WPARAM(w);
            ScrollTo(scroll_y_ - delta * static_cast<int>(lines) * fonts_.ui_h / WHEEL_DELTA);
            return 0;
        }

        case WM_LBUTTONDOWN: {
            const int i = CardAt(GET_X_LPARAM(l), GET_Y_LPARAM(l));
            if (i >= 0) OnCardChosen(i, true);
            return 0;
        }

        case WM_SETFOCUS:                        // never rests on the list itself
            for (Card& c : cards_)
                if (c.mod == model_.mod_index() && c.radio_wnd) SetFocus(c.radio_wnd);
            return 0;

        case WM_COMMAND: {
            const int id = LOWORD(w), code = HIWORD(w);
            if (id >= IDC_CARD_RADIO && id < IDC_CARD_RADIO + static_cast<int>(cards_.size())) {
                const int i = id - IDC_CARD_RADIO;
                if (code == BN_CLICKED) OnCardChosen(i, false);
                else if (code == BN_SETFOCUS) EnsureCardVisible(i);
            } else if (id >= IDC_CARD_UNINSTALL && id < IDC_CARD_UNINSTALL + static_cast<int>(cards_.size())) {
                if (code == BN_CLICKED) OnCardUninstall(id - IDC_CARD_UNINSTALL);
                else if (code == BN_SETFOCUS) EnsureCardVisible(id - IDC_CARD_UNINSTALL);
            } else if (id >= IDC_CARD_INSTALL && id < IDC_CARD_INSTALL + static_cast<int>(cards_.size())) {
                if (code == BN_CLICKED) OnCardInstall(id - IDC_CARD_INSTALL);
                else if (code == BN_SETFOCUS) EnsureCardVisible(id - IDC_CARD_INSTALL);
            }
            return 0;
        }
        case WM_NOTIFY: {
            const auto* nh = reinterpret_cast<const NMHDR*>(l);
            if ((nh->code == NM_CLICK || nh->code == NM_RETURN) && nh->idFrom >= static_cast<UINT_PTR>(IDC_CARD_LINK) &&
                nh->idFrom < static_cast<UINT_PTR>(IDC_CARD_LINK + static_cast<int>(cards_.size()))) {
                const Card& c = cards_[nh->idFrom - IDC_CARD_LINK];
                const int k = reinterpret_cast<const NMLINK*>(l)->item.iLink;          // which of the card's links: Website, then Discord / Support
                if (k >= 0 && static_cast<size_t>(k) < c.links.size() && IsWebUrl(c.links[static_cast<size_t>(k)].url)) {
                    const std::string& url = c.links[static_cast<size_t>(k)].url;
                    ShellExecuteW(hwnd_, L"open", W(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    AppendLog("Opened " + url);
                }
            }
            return 0;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
            SetBkMode(reinterpret_cast<HDC>(w), TRANSPARENT);
            return reinterpret_cast<LRESULT>(GetStockObject(NULL_BRUSH));
        default:
            break;
    }
    return DefWindowProcW(list_, m, w, l);
}

}  // namespace

int RunWindow(HINSTANCE instance, Model& model) {
    Window window(instance, model);
    return window.Run();
}

}  // namespace gui
}  // namespace vrml
