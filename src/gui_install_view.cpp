// gui_install_view.cpp - the install view (see gui_install_view.h). Plain Win32 like the main window, in device-independent units, one child window for the
// three cards (they paint themselves, the boxes are real check boxes), and two kinds of worker thread: the check (D2RLoader's files, the two servers) and the
// install itself (install_run.h). A worker never touches a control or the model: it posts a message with a copy of what it has to say; the UI thread
// applies it. The window does not close while an install runs (Cancel first); a Setup that is open is the player's to finish or close.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>
#include <objbase.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <ctime>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// gdiplus.h (used by gui_draw.h's callers) wants min and max
using std::max;
using std::min;

#include "gui_draw.h"
#include "gui_install_model.h"
#include "gui_install_view.h"
#include "gui_uninstall.h"
#include "install_run.h"
#include "resource.h"

namespace vrml {
namespace gui {

namespace {

std::wstring W(const std::string& s) {
    if (s.empty()) return L"";
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(std::max(n, 0)), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

std::wstring NoMnemonic(std::wstring s) {
    for (size_t i = 0; i < s.size(); ++i)
        if (s[i] == L'&') s.insert(i++, 1, L'&');
    return s;
}

// ------------------------------------------------------------------ look (the main window's)

const COLORREF kBg = RGB(243, 243, 243);
const COLORREF kCard = RGB(255, 255, 255);
const COLORREF kCardBorder = RGB(205, 205, 205);
const COLORREF kCardSelected = RGB(240, 247, 253);
const COLORREF kAccent = RGB(15, 108, 189);
const COLORREF kText = RGB(27, 27, 27);
const COLORREF kMuted = RGB(92, 92, 92);
const COLORREF kOk = RGB(14, 112, 14);
const COLORREF kWarn = RGB(150, 86, 0);
const COLORREF kError = RGB(190, 30, 20);

COLORREF ToneColor(Tone t, COLORREF normal = kText) {
    switch (t) {
        case Tone::Ok: return kOk;
        case Tone::Warn: return kWarn;
        case Tone::Error: return kError;
        default: return normal;
    }
}

// sizes in dp (1/96 inch)
constexpr int kHeaderH = 62;
constexpr int kMargin = 16;
constexpr int kMinClientW = 600;
constexpr int kMinClientH = 640;
constexpr int kDefaultClientW = 720;
constexpr int kDefaultClientH = 700;
constexpr int kRowButtonW = 108;       // each row's own buttons (Install / Update / Repair / Up to date and Uninstall), on the right of its card
constexpr int kLogMinH = 56;

enum : int {
    IDC_V_ROWS = 210, IDC_V_NOTICE, IDC_V_STEP, IDC_V_PROGRESS, IDC_V_LOG, IDC_V_CHECK, IDC_V_INSTALL,
    IDC_V_ROWCHECK = 1100,        // + the piece's number
    IDC_V_ROWACT = 1200,          // + the piece's number: the row's own Install / Update / Repair / Up to date button
    IDC_V_ROWUN = 1300,           // + the piece's number: the row's Uninstall button (an installed D2R VR Authentic / Unleashed)
};
// Close is IDCANCEL: Esc in a dialog-like window presses it
constexpr UINT WM_V_FACTS = WM_APP + 20;      // lParam: FactsMsg*
constexpr UINT WM_V_UPDATE = WM_APP + 21;     // UpdateMsg*
constexpr UINT WM_V_LOG = WM_APP + 22;        // std::string*
constexpr UINT WM_V_STAGE = WM_APP + 23;      // StageMsg*
constexpr UINT WM_V_PIECE = WM_APP + 24;      // PieceOutcome*
constexpr UINT WM_V_RUNDONE = WM_APP + 25;    // the worker has finished
constexpr UINT WM_V_SHOT = WM_APP + 26;
constexpr UINT WM_V_AUTO = WM_APP + 27;       // --install-now
constexpr UINT WM_V_UNINSTALLED = WM_APP + 28;   // a mod's own uninstaller has ended: look at the disk again
constexpr UINT_PTR kTimerPoll = 1, kTimerGuard = 2, kTimerShot = 3;
constexpr UINT kPollMs = 1500;

const wchar_t kViewClass[] = L"D2RVRLauncherInstallView";
const wchar_t kRowsClass[] = L"D2RVRLauncherInstallRows";

struct Fonts {
    HFONT ui = nullptr, bold = nullptr, title = nullptr, fine = nullptr;
    int ui_h = 0, bold_h = 0, fine_h = 0;
    void Destroy() {
        for (HFONT* f : {&ui, &bold, &title, &fine})
            if (*f) DeleteObject(*f);
        *this = Fonts();
    }
};

// ------------------------------------------------------------------ what the workers post

struct FactsMsg {
    unsigned gen;
    LoaderStatus loader;
    std::vector<ModEntry> mods;
};
struct UpdateMsg {
    unsigned gen;
    Piece piece;
    UpdateInfo info;
};
struct StageMsg {
    Piece piece;
    int index, count;
    std::string stage;
    uint64_t done, total;
};

struct CheckJob {
    HWND target;
    unsigned gen;
    fs::path game;
    std::map<std::string, std::string> env;
    std::string pin_file;
    bool network;
};

DWORD WINAPI CheckThread(LPVOID p) {
    std::unique_ptr<CheckJob> j(static_cast<CheckJob*>(p));
    LoaderStatus st;
    try {
        st = CheckLoaderAgainst(j->game, LoaderPlanFor(j->env, j->pin_file));
    } catch (const std::exception&) {
        st = CheckLoader(j->game, true);
    }
    auto* f = new FactsMsg{j->gen, st, DiscoverMods(j->game)};
    if (!PostMessageW(j->target, WM_V_FACTS, 0, reinterpret_cast<LPARAM>(f))) {
        delete f;
        return 0;
    }
    if (!j->network) return 0;
    for (Piece m : {Piece::Authentic, Piece::Unleashed}) {
        auto* u = new UpdateMsg{j->gen, m, CheckModRelease(m, j->game, j->env)};
        if (!PostMessageW(j->target, WM_V_UPDATE, 0, reinterpret_cast<LPARAM>(u))) {
            delete u;
            return 0;
        }
    }
    return 0;
}

struct RunJob {
    HWND target;
    fs::path game, home;
    std::map<std::string, std::string> env;
    ProcessProbe* probe;
    std::string pin_file;
    std::vector<PieceJob> jobs;
    std::atomic<bool>* cancel;
};

DWORD WINAPI RunThread(LPVOID p) {
    std::unique_ptr<RunJob> j(static_cast<RunJob*>(p));
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);     // ShellExecuteEx starts the Setup from this thread
    CliEnv env;
    env.env = j->env;
    env.probe = j->probe;
    RunContext ctx;
    ctx.game = j->game;
    ctx.home = j->home;
    ctx.env = &env;
    ctx.pin_file = j->pin_file;

    const HWND target = j->target;
    RunHooks hooks;
    hooks.log = [target](const std::string& line) {
        auto* s = new std::string(line);
        if (!PostMessageW(target, WM_V_LOG, 0, reinterpret_cast<LPARAM>(s))) delete s;
    };
    // the download reports for every piece of data: the window hears about it at most every 80 ms (and always when the stage changes or it is whole)
    struct Throttle {
        ULONGLONG last = 0;
        std::string stage;
        int index = -1;
    };
    auto throttle = std::make_shared<Throttle>();
    hooks.progress = [target, throttle](int index, int count, Piece piece, const std::string& stage, uint64_t done, uint64_t total) {
        const ULONGLONG now = GetTickCount64();
        const bool whole = total != 0 && done >= total;
        if (stage == throttle->stage && index == throttle->index && !whole && now - throttle->last < 80) return;
        throttle->stage = stage;
        throttle->index = index;
        throttle->last = now;
        auto* m = new StageMsg{piece, index, count, stage, done, total};
        if (!PostMessageW(target, WM_V_STAGE, 0, reinterpret_cast<LPARAM>(m))) delete m;
    };
    std::atomic<bool>* cancel = j->cancel;
    hooks.cancelled = [cancel] { return cancel->load(); };
    hooks.piece_done = [target](const PieceOutcome& o) {
        auto* m = new PieceOutcome(o);
        if (!PostMessageW(target, WM_V_PIECE, 0, reinterpret_cast<LPARAM>(m))) delete m;
    };
    RunInstallJobs(ctx, j->jobs, hooks);
    PostMessageW(target, WM_V_RUNDONE, 0, 0);
    if (SUCCEEDED(co)) CoUninitialize();
    return 0;
}

// ------------------------------------------------------------------ the view

struct RowGeo {
    RECT card{}, check{}, state{}, installed{}, latest{}, source{}, how{}, note{}, action{}, uninstall{};
};

class View {
public:
    View(HINSTANCE inst, HWND owner, Model& model, std::vector<Piece> tick, bool explicitTick)
        : inst_(inst), owner_(owner), model_(model), opt_(model.options()), tick_(std::move(tick)), explicit_tick_(explicitTick) {}
    ~View() {
        fonts_.Destroy();
        if (bg_brush_) DeleteObject(bg_brush_);
        if (white_brush_) DeleteObject(white_brush_);
        if (sel_brush_) DeleteObject(sel_brush_);
    }
    InstallViewResult Run();

private:
    static LRESULT CALLBACK ViewProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK RowsProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT OnView(UINT, WPARAM, LPARAM);
    LRESULT OnRows(UINT, WPARAM, LPARAM);

    int S(int dp) const { return MulDiv(dp, dpi_, 96); }

    bool Init();
    void MakeFonts();
    void CreateControls();
    void ApplyFonts();
    void AddTips();
    HWND Static(int id, const wchar_t* text, DWORD style = 0);
    HWND Button(int id, const wchar_t* text, DWORD style = BS_PUSHBUTTON);

    // what the controls show
    void UpdateUi();
    std::string NoticeText(Tone* tone) const;
    void SetStatic(HWND h, const std::string& text);
    void AppendLog(const std::string& text);
    void Sticky(Tone tone, const std::string& text);

    // layout and painting
    void Layout();
    int MeasureStatic(HWND h, int width) const;
    int ComputeRows(int width);
    void PaintHeader(HDC dc);
    void PaintRows(HDC dc, int dx, int dy, bool foreground);
    void PaintRow(HDC dc, size_t i, int dx, int dy, bool foreground);

    // work
    void StartChecks(bool network);
    void OnFacts(FactsMsg* m);
    void OnUpdate(UpdateMsg* m);
    void OnRowCheck(int piece);
    void OnRowAction(int piece);          // the row's own Install / Update / Repair: ticks only that piece and installs at once, the same path as Install selected
    void OnRowUninstall(int piece);       // the row's Uninstall: the mod's own uninstaller (never a folder deleted here)
    void OnUninstalled();
    const ModEntry* EntryOf(Piece p) const;
    void SetRowButton(HWND h, std::wstring* shown, const std::string& label, bool enabled);
    void OnInstall();
    void StartRun(std::vector<PieceJob> jobs);
    void OnStage(StageMsg* m);
    void OnPiece(PieceOutcome* o);
    void OnRunDone();
    void OnCancelOrClose();
    void Poll();
    void MaybeShot();
    void TakeShot();
    void MaybeAuto();
    // the owner is enabled again BEFORE this window goes: Windows then gives the activation back to it, not to some other program
    void Finish() {
        if (owner_) EnableWindow(owner_, TRUE);
        DestroyWindow(hwnd_);
    }

    HINSTANCE inst_;
    HWND owner_;
    Model& model_;
    const Options& opt_;
    std::vector<Piece> tick_;
    bool explicit_tick_;
    HWND hwnd_ = nullptr;
    int dpi_ = 96;
    Fonts fonts_;
    HBRUSH bg_brush_ = nullptr, white_brush_ = nullptr, sel_brush_ = nullptr;
    HWND rows_wnd_ = nullptr, st_notice_ = nullptr, st_step_ = nullptr, progress_ = nullptr, edt_log_ = nullptr, btn_check_ = nullptr, btn_install_ = nullptr, btn_close_ = nullptr;
    HWND check_[kPieceCount] = {};
    HWND tips_ = nullptr;                       // the tooltips of the buttons and boxes
    HWND focus_ = nullptr;                      // the control that had the keyboard when the window was left (a question, a Setup's wizard): it gets it back
    HWND action_[kPieceCount] = {}, unins_[kPieceCount] = {};
    std::wstring action_shown_[kPieceCount], unins_shown_[kPieceCount];     // the labels the buttons carry now (a button is not touched when nothing changed: no flicker)
    std::vector<ModEntry> mods_;                // the mods as the disk showed them at the last look (the Uninstall buttons read the uninstaller from here)
    // a mod's own uninstaller is running: Model::busy() (shared with the main window, which may have started it); the facts after it ended are about this piece
    Piece uninstalling_piece_ = Piece::Authentic;
    bool await_uninstall_facts_ = false;

    InstallModel inst_model_;
    std::vector<InstallRow> rows_;
    RowGeo geo_[kPieceCount];
    int rows_h_ = 0;
    bool in_rows_paint_ = false, laying_out_ = false;

    unsigned check_gen_ = 0;
    std::atomic<bool> cancel_{false};
    bool alive_ = true;
    bool ran_ = false;
    bool shot_requested_ = false, shot_done_ = false, auto_done_ = false;
    int paints_ = 0;
    int exit_shot_ = 0;
    std::string sticky_;
    Tone sticky_tone_ = Tone::Normal;
    Tone notice_tone_ = Tone::Normal;
    InstallViewResult result_;
};

HWND View::Static(int id, const wchar_t* text, DWORD style) {
    return CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT | style, 0, 0, 10, 10, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst_, nullptr);
}

HWND View::Button(int id, const wchar_t* text, DWORD style) {
    return CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | style, 0, 0, 10, 10, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst_, nullptr);
}

InstallViewResult View::Run() {
    static bool registered = false;
    if (!registered) {
        registered = true;
        INITCOMMONCONTROLSEX icc{sizeof icc, ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
        InitCommonControlsEx(&icc);
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof wc;
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = ViewProc;
        wc.hInstance = inst_;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = static_cast<HICON>(LoadImageW(inst_, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0));
        wc.hIconSm = static_cast<HICON>(LoadImageW(inst_, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
        wc.lpszClassName = kViewClass;
        RegisterClassExW(&wc);
        WNDCLASSEXW rc{};
        rc.cbSize = sizeof rc;
        rc.lpfnWndProc = RowsProc;
        rc.hInstance = inst_;
        rc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        rc.lpszClassName = kRowsClass;
        RegisterClassExW(&rc);
    }
    const int dpi0 = opt_.dpi ? opt_.dpi : static_cast<int>(owner_ ? GetDpiForWindow(owner_) : GetDpiForSystem());
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    RECT want{0, 0, MulDiv(kDefaultClientW, dpi0, 96), MulDiv(kDefaultClientH, dpi0, 96)};
    AdjustWindowRectExForDpi(&want, style, FALSE, 0, static_cast<UINT>(dpi0));
    HMONITOR mon = owner_ ? MonitorFromWindow(owner_, MONITOR_DEFAULTTOPRIMARY) : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(mon, &mi);
    const int workW = mi.rcWork.right - mi.rcWork.left, workH = mi.rcWork.bottom - mi.rcWork.top;
    const int w = std::min<int>(want.right - want.left, workW), h = std::min<int>(want.bottom - want.top, workH);
    const int x = mi.rcWork.left + (workW - w) / 2, y = mi.rcWork.top + (workH - h) / 2;

    hwnd_ = CreateWindowExW(0, kViewClass, L"Install / update mods - D2R VR Launcher", style, x, y, w, h, owner_, nullptr, inst_, this);
    if (!hwnd_) return result_;
    if (owner_) EnableWindow(owner_, FALSE);
    ShowWindow(hwnd_, opt_.screenshot.empty() ? SW_SHOW : SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd_);

    MSG msg;
    while (alive_ && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(hwnd_, &msg)) {
            if (!alive_) break;
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (alive_) {                       // WM_QUIT reached this loop: the application is ending
        PostQuitMessage(0);
        DestroyWindow(hwnd_);
    }
    if (owner_) EnableWindow(owner_, TRUE);
    result_.changed = ran_;
    result_.shot_failed = exit_shot_ != 0;
    return result_;
}

bool View::Init() {
    dpi_ = opt_.dpi ? opt_.dpi : static_cast<int>(GetDpiForWindow(hwnd_));
    bg_brush_ = CreateSolidBrush(kBg);
    white_brush_ = CreateSolidBrush(kCard);
    sel_brush_ = CreateSolidBrush(kCardSelected);
    MakeFonts();
    CreateControls();
    ApplyFonts();
    AddTips();

    const GameChoice* g = model_.game();
    inst_model_.SetGame(g ? g->install.dir : fs::path(), g && g->is_game);
    try {
        inst_model_.SetLoaderPinVersion(LoaderPlanFor(model_.services().env, opt_.pin_file).pin.version);
    } catch (const std::exception&) {
    }
    // what the main window already hashed counts, unless a test pin changes what "the reviewed files" are: then this view's own check says
    inst_model_.SetLoader(model_.loader(), model_.loader_hashed() && opt_.pin_file.empty());
    mods_ = model_.mods();
    for (const ModEntry& m : mods_) {
        Piece p;
        if (PieceOfModId(m.id, &p)) inst_model_.SetInstalled(p, m.installed, m.found_version, m.found_from);
    }
    inst_model_.SetGameRunning(model_.GameRunning());
    if (explicit_tick_) inst_model_.TickOnly(tick_);

    AppendLog(std::string("Install view. Game folder: ") + (g ? DisplayPath(g->install.dir) : std::string("none")));
    StartChecks(true);
    UpdateUi();
    Layout();
    focus_ = check_[0] && IsWindowEnabled(check_[0]) ? check_[0] : btn_close_;      // given again when the window is activated (WM_ACTIVATE)
    SetFocus(focus_);
    SetTimer(hwnd_, kTimerPoll, kPollMs, nullptr);
    if (!opt_.screenshot.empty()) {
        shot_requested_ = true;
        SetTimer(hwnd_, kTimerGuard, 40000, nullptr);       // a screenshot that never comes is a failure, not a hang
    }
    return true;
}

void View::MakeFonts() {
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
    lf.lfHeight = base * 11 / 12;
    fonts_.fine = CreateFontIndirectW(&lf);
    lf.lfWeight = FW_SEMIBOLD;
    lf.lfHeight = -MulDiv(16, dpi_, 72);
    fonts_.title = CreateFontIndirectW(&lf);

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

void View::CreateControls() {
    auto id = [](int i) { return reinterpret_cast<HMENU>(static_cast<INT_PTR>(i)); };
    rows_wnd_ = CreateWindowExW(WS_EX_CONTROLPARENT, kRowsClass, L"Pieces", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, 10, 10, hwnd_, id(IDC_V_ROWS), inst_, this);
    // per row: the box, then its own action button, then its Uninstall button (that is the Tab order)
    for (int i = 0; i < kPieceCount; ++i) {
        DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX;
        check_[i] = CreateWindowExW(0, L"BUTTON", NoMnemonic(W(PieceName(static_cast<Piece>(i)))).c_str(), style, 0, 0, 10, 10, rows_wnd_, id(IDC_V_ROWCHECK + i), inst_, nullptr);
        action_[i] = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 10, 10, rows_wnd_, id(IDC_V_ROWACT + i), inst_, nullptr);
        unins_[i] = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 10, 10, rows_wnd_, id(IDC_V_ROWUN + i), inst_, nullptr);
    }
    btn_check_ = Button(IDC_V_CHECK, L"&Check again");
    btn_install_ = Button(IDC_V_INSTALL, L"&Install selected", BS_DEFPUSHBUTTON);
    btn_close_ = Button(IDCANCEL, L"Close");
    st_notice_ = Static(IDC_V_NOTICE, L"", SS_NOPREFIX);
    st_step_ = Static(IDC_V_STEP, L"", SS_NOPREFIX);
    progress_ = CreateWindowExW(0, PROGRESS_CLASSW, nullptr, WS_CHILD | WS_VISIBLE | PBS_SMOOTH, 0, 0, 10, 10, hwnd_, id(IDC_V_PROGRESS), inst_, nullptr);
    SendMessageW(progress_, PBM_SETRANGE32, 0, 1000);
    edt_log_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 0, 0, 10, 10, hwnd_, id(IDC_V_LOG), inst_, nullptr);
    SendMessageW(edt_log_, EM_SETLIMITTEXT, 400000, 0);
}

void View::ApplyFonts() {
    auto set = [&](HWND h, HFONT f) {
        if (h) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(f), TRUE);
    };
    for (HWND h : {btn_check_, btn_install_, btn_close_, st_notice_, st_step_, edt_log_}) set(h, fonts_.ui);
    set(st_step_, fonts_.fine);
    for (HWND h : check_) set(h, fonts_.bold);
    for (HWND h : action_) set(h, fonts_.ui);
    for (HWND h : unins_) set(h, fonts_.ui);
}

// What the buttons do, in one sentence each (a disabled control shows no tooltip: Windows sends it no mouse messages).
void View::AddTips() {
    tips_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, 0, 0, 0, 0, hwnd_, nullptr, inst_, nullptr);
    if (!tips_) return;
    SendMessageW(tips_, TTM_SETMAXTIPWIDTH, 0, S(380));
    SendMessageW(tips_, TTM_SETDELAYTIME, TTDT_INITIAL, 500);
    auto add = [&](HWND control, const std::wstring& text) {
        if (!control) return;
        TTTOOLINFOW ti{};
        ti.cbSize = sizeof ti;
        ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        ti.hwnd = GetParent(control);
        ti.uId = reinterpret_cast<UINT_PTR>(control);
        ti.lpszText = const_cast<LPWSTR>(text.c_str());          // copied by the tooltip control
        SendMessageW(tips_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
    };
    for (int i = 0; i < kPieceCount; ++i) {
        const std::wstring name = W(PieceName(static_cast<Piece>(i)));
        add(check_[i], L"Tick to install or update " + name + L" with Install selected. Greyed out when there is nothing to do.");
        add(action_[i], L"Install, update or repair only " + name + L", right now.");
        add(unins_[i], L"Remove " + name + L" with its own uninstaller. You are asked first.");
    }
    add(btn_check_, L"Look at the game folder again and ask the servers for the newest versions.");
    add(btn_install_, L"Download, check and install everything that is ticked, D2RLoader first (Enter).");
    add(btn_close_, L"Close this window (Esc). While an install runs, this button cancels it at the next safe point.");
}

// ------------------------------------------------------------------ what the controls show

void View::SetStatic(HWND h, const std::string& text) {
    const std::wstring w = W(text);
    SetWindowTextW(h, w.c_str());
    InvalidateRect(h, nullptr, TRUE);
}

void View::AppendLog(const std::string& text) {
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

void View::Sticky(Tone tone, const std::string& text) {
    sticky_ = text;
    sticky_tone_ = tone;
}

// A row's own button: its label (hidden when there is none) and whether it can be pressed. Touched only when something changed.
void View::SetRowButton(HWND h, std::wstring* shown, const std::string& label, bool enabled) {
    if (!h) return;
    const std::wstring w = NoMnemonic(W(label));
    if (w != *shown) {
        *shown = w;
        SetWindowTextW(h, w.c_str());
        ShowWindow(h, w.empty() ? SW_HIDE : SW_SHOWNOACTIVATE);
    }
    const BOOL want = enabled && !w.empty() ? TRUE : FALSE;
    if (IsWindowEnabled(h) != want) EnableWindow(h, want);
}

const ModEntry* View::EntryOf(Piece p) const {
    for (const ModEntry& e : mods_)
        if (e.id == PieceId(p)) return &e;
    return nullptr;
}

std::string View::NoticeText(Tone* tone) const {
    if (tone) *tone = Tone::Normal;
    if (inst_model_.running()) return inst_model_.RunNotice(tone);
    if (model_.busy()) return "Waiting for the uninstaller of " + model_.busy_name() + " to finish (its window may be behind this one: look for it on the taskbar).";
    if (!sticky_.empty()) {
        if (tone) *tone = sticky_tone_;
        return sticky_;
    }
    const StartState ss = inst_model_.CanStart();
    if (!ss.can) {
        if (tone) *tone = ss.calm ? Tone::Ok : Tone::Warn;
        return ss.reason;
    }
    std::vector<std::string> names;
    for (const PieceJob& j : inst_model_.Jobs(false)) {
        std::string n = PieceName(j.piece);
        if (j.has_release && !j.release.version.empty()) n += " " + j.release.version;
        names.push_back(n);
    }
    std::string s = "Ready to install: ";
    for (size_t i = 0; i < names.size(); ++i) s += (i ? ", " : "") + names[i];
    return s + ". Each one is downloaded, checked and installed in turn; the Setups show their own windows.";
}

void View::UpdateUi() {
    rows_ = inst_model_.Rows();
    for (int i = 0; i < kPieceCount; ++i) {
        const InstallRow& r = rows_[static_cast<size_t>(i)];
        if (Button_GetCheck(check_[i]) != (r.checked ? BST_CHECKED : BST_UNCHECKED)) Button_SetCheck(check_[i], r.checked ? BST_CHECKED : BST_UNCHECKED);
        if (IsWindowEnabled(check_[i]) != (r.enabled ? TRUE : FALSE)) EnableWindow(check_[i], r.enabled);
        // the row's own buttons; while a mod's own uninstaller runs nothing else is started
        const bool idle = !model_.busy();
        SetRowButton(action_[i], &action_shown_[i], r.action, r.action_enabled && idle);
        SetRowButton(unins_[i], &unins_shown_[i], r.uninstall, r.uninstall_enabled && idle);
    }
    Tone tone = Tone::Normal;
    const std::string notice = NoticeText(&tone);
    notice_tone_ = tone;
    SetStatic(st_notice_, notice);
    const bool running = inst_model_.running();
    EnableWindow(btn_install_, !running && !model_.busy() && inst_model_.CanStart().can);
    EnableWindow(btn_check_, !running && inst_model_.has_game());
    const bool cancelling = running && inst_model_.cancel_requested();
    SetWindowTextW(btn_close_, running ? (cancelling ? L"Cancelling..." : L"Cancel") : L"Close");
    EnableWindow(btn_close_, !cancelling);
    SetStatic(st_step_, inst_model_.StepText());
    SendMessageW(progress_, PBM_SETPOS, static_cast<WPARAM>(inst_model_.Progress() * 1000.0), 0);
    InvalidateRect(rows_wnd_, nullptr, FALSE);
    Layout();
}

// ------------------------------------------------------------------ layout

int View::MeasureStatic(HWND h, int width) const {
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

// The cards' geometry for a panel `width` px wide, in the panel's own coordinates; returns the panel's height. Every card is as tall as the others and
// keeps a line for its note, so the window does not jump when a note comes and goes.
int View::ComputeRows(int width) {
    const int pad = S(10), gap = S(8);
    const int title_h = S(22);
    const int line = fonts_.ui_h, fine = fonts_.fine_h;
    // the three lines (Installed / Newest, where it comes from, how it runs) sit beside the row's own buttons; the note below them runs the whole width
    const int col_h = line + S(4) + fine + S(2) + fine;
    const int card_h = pad + title_h + S(3) + col_h + S(2) + fine + pad;
    const int mark_w = S(230);
    const int text_l = pad + S(24);
    const int btn_w = S(kRowButtonW);
    const int text_r = width - pad - btn_w - S(10);
    const int btn_gap = S(4);
    const int btn_h = (col_h - btn_gap) / 2;
    int y = 0;
    for (int i = 0; i < kPieceCount; ++i) {
        RowGeo& g = geo_[i];
        g.card = RECT{0, y, width, y + card_h};
        g.check = RECT{pad, y + pad, width - pad - mark_w - S(8), y + pad + title_h};
        g.state = RECT{width - pad - mark_w, y + pad, width - pad, y + pad + title_h};
        int ly = y + pad + title_h + S(3);
        const int col_top = ly;
        const int col1 = (text_r - text_l) * 46 / 100;
        g.installed = RECT{text_l, ly, text_l + col1, ly + line};
        g.latest = RECT{text_l + col1 + S(8), ly, text_r, ly + line};
        ly += line + S(4);
        g.source = RECT{text_l, ly, text_r, ly + fine};
        ly += fine + S(2);
        g.how = RECT{text_l, ly, text_r, ly + fine};
        ly += fine + S(2);
        g.note = RECT{text_l, ly, width - pad, ly + fine};
        // the buttons: the action on top, Uninstall under it; a row with only an action button has it in the middle of the column
        const bool has_unins = rows_.size() == static_cast<size_t>(kPieceCount) && !rows_[static_cast<size_t>(i)].uninstall.empty();
        const int top = has_unins ? col_top : col_top + (col_h - btn_h) / 2;
        g.action = RECT{width - pad - btn_w, top, width - pad, top + btn_h};
        g.uninstall = RECT{width - pad - btn_w, col_top + btn_h + btn_gap, width - pad, col_top + btn_h + btn_gap + btn_h};
        y += card_h + gap;
    }
    return y - gap;
}

void View::Layout() {
    if (!hwnd_ || !rows_wnd_ || laying_out_) return;
    laying_out_ = true;
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const int W_ = rc.right, H_ = rc.bottom;
    const int m = S(kMargin), w = std::max(S(240), W_ - 2 * m);
    const int line = fonts_.ui_h;
    HDWP defer = BeginDeferWindowPos(16);
    auto put = [&](HWND h, int x, int y, int ww, int hh) {
        if (defer) defer = DeferWindowPos(defer, h, nullptr, x, y, ww, hh, SWP_NOZORDER | SWP_NOACTIVATE);
    };
    int y = S(kHeaderH) + S(12);
    rows_h_ = ComputeRows(w);
    put(rows_wnd_, m, y, w, rows_h_);
    y += rows_h_ + S(10);
    const int notice_h = std::max(line, std::min(MeasureStatic(st_notice_, w), line * 3));
    put(st_notice_, m, y, w, notice_h);
    y += notice_h + S(8);
    const int step_h = fonts_.fine_h + S(2);
    put(st_step_, m, y, w, step_h);
    y += step_h + S(2);
    put(progress_, m, y, w, S(14));
    y += S(14) + S(8);
    const int btn_h = std::max(S(30), line + S(12));
    const int btn_y = H_ - S(12) - btn_h;
    put(edt_log_, m, y, w, std::max(S(kLogMinH), btn_y - S(10) - y));
    const int close_w = S(110), install_w = S(150), check_w = S(120);
    put(btn_close_, m + w - close_w, btn_y, close_w, btn_h);
    put(btn_install_, m + w - close_w - S(8) - install_w, btn_y, install_w, btn_h);
    put(btn_check_, m, btn_y, check_w, btn_h);
    if (defer) EndDeferWindowPos(defer);

    // the check boxes sit in their cards (the panel's coordinates)
    for (int i = 0; i < kPieceCount; ++i) {
        const RECT& c = geo_[i].check;
        SetWindowPos(check_[i], nullptr, c.left, c.top, c.right - c.left, c.bottom - c.top, SWP_NOZORDER | SWP_NOACTIVATE);
        const RECT& a = geo_[i].action;
        SetWindowPos(action_[i], nullptr, a.left, a.top, a.right - a.left, a.bottom - a.top, SWP_NOZORDER | SWP_NOACTIVATE);
        const RECT& u = geo_[i].uninstall;
        SetWindowPos(unins_[i], nullptr, u.left, u.top, u.right - u.left, u.bottom - u.top, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    laying_out_ = false;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// ------------------------------------------------------------------ painting

void View::PaintHeader(HDC dc) {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const int m = S(kMargin);
    RECT band{0, 0, rc.right, S(kHeaderH)};
    FillRect(dc, &band, white_brush_);
    RECT rule{0, band.bottom - 1, rc.right, band.bottom};
    HBRUSH b = CreateSolidBrush(RGB(222, 222, 222));
    FillRect(dc, &rule, b);
    DeleteObject(b);
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, fonts_.title);
    SetTextColor(dc, kText);
    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    const int title_h = tm.tmHeight;
    const int ty = (S(kHeaderH) - (title_h + S(2) + fonts_.fine_h)) / 2;
    RECT tr{m, ty, rc.right - m, ty + title_h};
    DrawTextW(dc, L"Install / update mods", -1, &tr, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, fonts_.fine);
    SetTextColor(dc, kMuted);
    const GameChoice* g = model_.game();
    const std::wstring sub = g ? L"Game folder: " + W(DisplayPath(g->install.dir)) : std::wstring(L"No game folder yet");
    RECT vr{m, ty + title_h + S(2), rc.right - m, ty + title_h + S(2) + fonts_.fine_h};
    DrawTextW(dc, sub.c_str(), -1, &vr, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_PATH_ELLIPSIS);
}

void View::PaintRow(HDC dc, size_t i, int dx, int dy, bool foreground) {
    const InstallRow& r = rows_[i];
    const RowGeo& g = geo_[i];
    auto off = [&](RECT x) {
        OffsetRect(&x, dx, dy);
        return x;
    };
    DrawRoundRect(dc, off(g.card), S(6), r.checked ? kCardSelected : kCard, r.checked ? kAccent : kCardBorder, r.checked ? std::max(2, S(2)) : std::max(1, S(1)));
    if (!foreground) return;
    SetBkMode(dc, TRANSPARENT);
    auto text = [&](const std::wstring& s, HFONT f, COLORREF col, RECT rc, UINT flags) {
        if (s.empty()) return;
        SelectObject(dc, f);
        SetTextColor(dc, col);
        DrawTextW(dc, s.c_str(), -1, &rc, flags | DT_NOPREFIX | DT_SINGLELINE | DT_END_ELLIPSIS);
    };
    // the mark on the right: what is happening / happened to this piece, else the amber badge
    RECT st = off(g.state);
    if (!r.state.empty()) {
        SelectObject(dc, fonts_.bold);
        SIZE sz{};
        const std::wstring ws = W(r.state);
        GetTextExtentPoint32W(dc, ws.c_str(), static_cast<int>(ws.size()), &sz);
        const int dot = std::max(6, S(8));
        const COLORREF col = r.state_tone == Tone::Normal ? kMuted : ToneColor(r.state_tone);
        const int avail = st.right - st.left - dot - S(6);
        const int tw = std::min<int>(sz.cx, avail);
        RECT dotr{st.right - tw - S(6) - dot, st.top + (st.bottom - st.top - dot) / 2, st.right - tw - S(6), st.top + (st.bottom - st.top - dot) / 2 + dot};
        DrawDot(dc, dotr, r.state_tone == Tone::Normal ? RGB(150, 150, 150) : col);
        text(ws, fonts_.bold, col, st, DT_RIGHT | DT_VCENTER);
    } else if (!r.badge.empty()) {
        text(W(r.badge), fonts_.bold, kWarn, st, DT_RIGHT | DT_VCENTER);
    }
    // "Installed: ...   Newest: ..."
    auto labelled = [&](const char* label, const std::string& value, Tone tone, RECT rc) {
        const std::wstring wl = W(label);
        SelectObject(dc, fonts_.ui);
        SIZE sz{};
        GetTextExtentPoint32W(dc, wl.c_str(), static_cast<int>(wl.size()), &sz);
        text(wl, fonts_.ui, kMuted, rc, DT_LEFT);
        RECT v = rc;
        v.left += sz.cx + S(6);
        text(W(value), fonts_.ui, ToneColor(tone), v, DT_LEFT);
    };
    labelled("Installed:", r.installed, r.installed_tone, off(g.installed));
    labelled("Newest:", r.latest, r.latest_tone, off(g.latest));
    text(W(r.source), fonts_.fine, kMuted, off(g.source), DT_LEFT);
    text(W(r.how), fonts_.fine, kMuted, off(g.how), DT_LEFT);
    text(W(r.note), fonts_.fine, ToneColor(r.note_tone, kMuted), off(g.note), DT_LEFT);
}

void View::PaintRows(HDC dc, int dx, int dy, bool foreground) {
    RECT client{};
    GetClientRect(rows_wnd_, &client);
    RECT all{dx, dy, dx + client.right, dy + client.bottom};
    FillRect(dc, &all, bg_brush_);
    for (size_t i = 0; i < rows_.size() && i < static_cast<size_t>(kPieceCount); ++i) PaintRow(dc, i, dx, dy, foreground);
}

// ------------------------------------------------------------------ work

void View::StartChecks(bool network) {
    const GameChoice* g = model_.game();
    if (!g || !g->is_game) return;
    const unsigned gen = ++check_gen_;
    if (network) inst_model_.SetChecking();
    auto* job = new CheckJob{hwnd_, gen, g->install.dir, model_.services().env, opt_.pin_file, network};
    HANDLE th = CreateThread(nullptr, 0, CheckThread, job, 0, nullptr);
    if (th) {
        CloseHandle(th);
    } else {
        delete job;
        AppendLog("error: could not start the check");
    }
}

void View::OnFacts(FactsMsg* raw) {
    std::unique_ptr<FactsMsg> m(raw);
    if (m->gen != check_gen_) return;
    inst_model_.SetLoader(m->loader, true);
    mods_ = m->mods;
    for (const ModEntry& e : mods_) {
        Piece p;
        if (PieceOfModId(e.id, &p)) inst_model_.SetInstalled(p, e.installed, e.found_version, e.found_from);
    }
    for (const std::string& n : m->loader.notes) AppendLog("D2RLoader: " + ForLog(n));
    if (await_uninstall_facts_) {
        // what the uninstaller left: the mod is gone, or its own question was answered No and it is still there
        await_uninstall_facts_ = false;
        const ModEntry* e = EntryOf(uninstalling_piece_);
        const std::string name = PieceName(uninstalling_piece_);
        if (e && e->installed) {
            AppendLog(name + " is still installed (its uninstaller ended without removing it).");
            Sticky(Tone::Warn, name + " is still installed: its uninstaller ended without removing it.");
        } else {
            AppendLog(name + " is no longer installed.");
            Sticky(Tone::Ok, name + " was uninstalled.");
        }
    }
    UpdateUi();
    MaybeShot();
    MaybeAuto();
}

void View::OnUpdate(UpdateMsg* raw) {
    std::unique_ptr<UpdateMsg> m(raw);
    if (m->gen != check_gen_) return;
    inst_model_.SetUpdate(m->piece, m->info);
    model_.SetLatest(m->info);
    if (m->info.checked) {
        const Release& r = m->info.latest;
        const bool older_named = r.up_to_date && !r.announced.empty() && IsNewer(r.current, r.announced);       // the site's newest is older than what is installed
        AppendLog(std::string(PieceName(m->piece)) + ": " +
                  (older_named ? "the site names " + r.announced + ", you have " + r.current + ": a newer version, nothing to update"
                               : r.up_to_date ? std::string("you have the newest version")
                                              : "newest is " + r.version + (r.sha256.empty() ? ", no published checksum" : ", checksum " + r.sha256_source)));
        for (const std::string& w : r.warnings) AppendLog("warning: " + w);
    } else {
        AppendLog(std::string("warning: ") + PieceName(m->piece) + ": could not find out the newest version: " + m->info.error);
    }
    UpdateUi();
    MaybeShot();
    MaybeAuto();
}

void View::OnRowCheck(int piece) {
    if (piece < 0 || piece >= kPieceCount) return;
    if (inst_model_.running() || model_.busy()) {
        UpdateUi();                    // the box toggled itself: back to what the model says
        return;
    }
    inst_model_.Select(static_cast<Piece>(piece), Button_GetCheck(check_[piece]) == BST_CHECKED);
    sticky_.clear();
    UpdateUi();
}

// The row's own Install / Update / Repair: only this piece is ticked (D2RLoader goes with a mod that needs it, as the cards' button does), then the same path as
// Install selected - the same refusals, the same question about a download without a published checksum.
void View::OnRowAction(int piece) {
    if (piece < 0 || piece >= kPieceCount || inst_model_.running() || model_.busy()) return;
    inst_model_.SetGameRunning(model_.GameRunning());
    inst_model_.TickOnly({static_cast<Piece>(piece)});
    sticky_.clear();
    AppendLog(std::string(PieceName(static_cast<Piece>(piece))) + ": " + (rows_.size() == static_cast<size_t>(kPieceCount) ? rows_[static_cast<size_t>(piece)].action : std::string("install")) + " pressed on its row.");
    UpdateUi();
    OnInstall();
}

// The row's Uninstall: the same as the card's button in the main window. Refused while the game runs; a question that names the mod; then the mod's own uninstaller
// (Authentic's unins000.exe, Unleashed's in %LOCALAPPDATA%\Programs\D2R PCVR, without /SILENT so its own question shows); with none on this computer, the words for
// removing it by hand. The launcher never deletes a mod folder itself. When the uninstaller ends the disk is looked at again and the row changes.
void View::OnRowUninstall(int piece) {
    if (piece < 0 || piece >= kPieceCount || inst_model_.running() || model_.busy()) return;
    const Piece p = static_cast<Piece>(piece);
    const ModEntry* found = EntryOf(p);
    if (!IsMod(p) || !found || !found->installed) return;
    // a copy: the questions below run a message loop, and the facts of a check that arrive in it replace mods_
    const ModEntry entry = *found;
    const ModEntry* e = &entry;
    const Model::UninstallOffer o = model_.UninstallFor(*e);
    if (o.kind == Model::UninstallOffer::Kind::None) return;
    const std::string refusal = model_.UninstallRefusal();
    if (!refusal.empty()) {
        Sticky(Tone::Warn, refusal);
        AppendLog("Not uninstalling " + e->name + ": " + refusal);
        UpdateUi();
        return;
    }
    sticky_.clear();
    const std::wstring title = L"Uninstall " + W(e->name);
    if (o.kind == Model::UninstallOffer::Kind::Explain) {
        AppendLog(e->name + " has no uninstaller here: " + DisplayPath(e->dir));
        MessageBoxW(hwnd_, W(o.explanation).c_str(), title.c_str(), MB_OK | MB_ICONINFORMATION);
        UpdateUi();
        return;
    }
    AppendLog("Asked before uninstalling " + e->name + ".");
    if (MessageBoxW(hwnd_, W(UninstallQuestion(*e, o)).c_str(), title.c_str(), MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) {
        AppendLog("Not uninstalling " + e->name + ": you said no. Nothing was run.");
        UpdateUi();
        return;
    }
    // the answer took a while: the game may have been started meanwhile
    const std::string late = model_.UninstallRefusal();
    if (!late.empty() || inst_model_.running()) {
        Sticky(Tone::Warn, late.empty() ? std::string("An install is running.") : late);
        UpdateUi();
        return;
    }
    bool declined = false;
    std::function<bool()> finished;
    if (!StartUninstaller(hwnd_, o, hwnd_, WM_V_UNINSTALLED, &declined, &finished)) {
        Sticky(declined ? Tone::Warn : Tone::Error, declined ? std::string("The uninstaller did not start: the Windows prompt was declined. Nothing was changed.")
                                                             : "Could not start the uninstaller " + DisplayPath(o.exe) + ".");
        AppendLog(declined ? "The Windows prompt for the uninstaller was declined." : "error: could not start " + DisplayPath(o.exe));
        UpdateUi();
        return;
    }
    model_.BeginUninstall(*e, finished);
    uninstalling_piece_ = p;
    AppendLog("Running the uninstaller of " + e->name + ": " + DisplayPath(o.exe));
    UpdateUi();
}

// The uninstaller has ended: look at the game folder again, and ask the servers again (an answer given while the mod was installed said "nothing newer than yours").
void View::OnUninstalled() {
    const std::string name = model_.busy_name();
    std::string id;
    if (!model_.PollBusy(&id)) return;              // still running, or nothing was (the poll already saw it end)
    Piece p;
    if (PieceOfModId(id, &p)) uninstalling_piece_ = p;
    AppendLog("The uninstaller of " + name + " has ended.");
    sticky_.clear();
    await_uninstall_facts_ = true;
    StartChecks(true);
    UpdateUi();
}

void View::OnInstall() {
    if (inst_model_.running()) return;
    if (model_.busy()) {
        Sticky(Tone::Warn, model_.BusyReason());
        UpdateUi();
        return;
    }
    inst_model_.SetGameRunning(model_.GameRunning());           // it may have been started since the last look
    const StartState ss = inst_model_.CanStart();
    if (!ss.can) {
        Sticky(Tone::Warn, ss.reason);
        AppendLog("Not started: " + ss.reason);
        UpdateUi();
        return;
    }
    bool accept = false;
    const std::vector<UnverifiedItem> items = inst_model_.UnverifiedTicked();
    if (!items.empty()) {
        if (opt_.accept_unverified) {
            accept = true;
            AppendLog("A release without a published checksum is accepted because of --accept-unverified (a test aid).");
        } else {
            const Prompt p = InstallModel::UnverifiedPrompt(items);
            AppendLog("Asked before downloading: " + p.title);
            // the default button is No: nothing is accepted by pressing Enter
            if (MessageBoxW(hwnd_, W(p.text).c_str(), W(p.title).c_str(), MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
                AppendLog("Not installed: a download without a published checksum was not accepted. Nothing was downloaded.");
                Sticky(Tone::Warn, "Nothing was downloaded: you did not accept a download without a published checksum. Untick it, or wait until a checksum is published.");
                UpdateUi();
                return;
            }
            accept = true;
            AppendLog("Accepted: install without a published checksum (the SHA-256 of the download will be shown here).");
        }
    }
    StartRun(inst_model_.Jobs(accept));
}

void View::StartRun(std::vector<PieceJob> jobs) {
    const GameChoice* g = model_.game();
    if (!g || jobs.empty()) return;
    sticky_.clear();
    cancel_ = false;
    inst_model_.BeginRun(jobs);
    ran_ = true;
    std::string names;
    for (const PieceJob& j : OrderedJobs(jobs)) names += (names.empty() ? "" : ", ") + std::string(PieceName(j.piece));
    AppendLog("Installing: " + names);
    auto* job = new RunJob{hwnd_, g->install.dir, model_.home(), model_.services().env, model_.services().probe, opt_.pin_file, jobs, &cancel_};
    HANDLE th = CreateThread(nullptr, 0, RunThread, job, 0, nullptr);
    if (th) {
        CloseHandle(th);
    } else {
        delete job;
        inst_model_.EndRun();
        AppendLog("error: could not start the install");
        Sticky(Tone::Error, "The install could not be started. Nothing was changed; close this window and try again.");
    }
    UpdateUi();
}

void View::OnStage(StageMsg* raw) {
    std::unique_ptr<StageMsg> m(raw);
    if (!inst_model_.running()) return;
    inst_model_.SetStage(m->piece, m->index, m->count, m->stage, m->done, m->total);
    UpdateUi();
}

void View::OnPiece(PieceOutcome* raw) {
    std::unique_ptr<PieceOutcome> o(raw);
    inst_model_.SetOutcome(*o);
    AppendLog(std::string(PieceName(o->piece)) + ": " + InstallModel::Meaning(*o));
    const std::string details = InstallModel::LogDetails(*o);
    if (!details.empty()) AppendLog("  " + details);
    if (o->status == JobStatus::Installed || o->status == JobStatus::AlreadyThere) AppendLog("  " + o->message);
    for (const std::string& n : o->notes) AppendLog("  note: " + n);
    if (!o->sha256.empty()) AppendLog("  SHA-256 of the installer: " + o->sha256 + (o->sha_verified ? " (matches the published one)" : " (no published checksum to compare with)"));
    UpdateUi();
}

void View::OnRunDone() {
    inst_model_.EndRun();
    Tone tone = Tone::Normal;
    const std::string head = inst_model_.Headline(&tone);
    Sticky(tone, head);
    AppendLog(head);
    cancel_ = false;
    StartChecks(false);          // what is installed now (the newest-version answers are kept); a screenshot waits for it
    UpdateUi();
}

void View::OnCancelOrClose() {
    if (inst_model_.running()) {
        if (inst_model_.cancel_requested()) return;
        inst_model_.RequestCancel();
        cancel_ = true;
        AppendLog("Cancel requested: the step that is running stops at the next safe point; a Setup window that is open is not closed.");
        UpdateUi();
        return;
    }
    Finish();
}

void View::Poll() {
    OnUninstalled();                                // one started from the main window's card ends while the view is open, or a message that was lost
    const bool running = model_.GameRunning();
    inst_model_.SetGameRunning(running);
    if (!inst_model_.running()) UpdateUi();
}

void View::MaybeShot() {
    if (!shot_requested_ || shot_done_ || paints_ == 0) return;
    if (!opt_.shot_now) {
        if (!inst_model_.AllChecksFinished() || inst_model_.running()) return;
        if (opt_.install_now && !auto_done_) return;
        if (opt_.install_now && inst_model_.outcomes().empty()) return;
    }
    shot_done_ = true;
    // Windows animates a button that was just enabled and a progress bar that moved: give them a moment to arrive, so the picture shows the final look
    SetTimer(hwnd_, kTimerShot, opt_.shot_now ? 30 : inst_model_.outcomes().empty() ? 500 : 900, nullptr);
}

void View::TakeShot() {
    RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    std::string error;
    if (!SaveClientPng(hwnd_, fs::u8path(opt_.screenshot).wstring(), &error)) {
        exit_shot_ = 5;
        std::wstring m = L"screenshot failed: " + W(error);
        OutputDebugStringW(m.c_str());
    }
    Finish();
}

// --install-now: the player's click on Install, once the checks are done (a test aid)
void View::MaybeAuto() {
    if (!opt_.install_now || auto_done_ || !inst_model_.AllChecksFinished() || inst_model_.running()) return;
    auto_done_ = true;
    PostMessageW(hwnd_, WM_V_AUTO, 0, 0);
}

// ------------------------------------------------------------------ messages

LRESULT CALLBACK View::ViewProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    View* self = nullptr;
    if (m == WM_NCCREATE) {
        self = static_cast<View*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = h;
    } else {
        self = reinterpret_cast<View*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    }
    if (!self) return DefWindowProcW(h, m, w, l);
    return self->OnView(m, w, l);
}

LRESULT View::OnView(UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_CREATE:
            return Init() ? 0 : -1;

        case WM_ERASEBKGND: {
            RECT r{};
            GetClipBox(reinterpret_cast<HDC>(w), &r);
            FillRect(reinterpret_cast<HDC>(w), &r, bg_brush_);
            return 1;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd_, &ps);
            PaintHeader(dc);
            EndPaint(hwnd_, &ps);
            ++paints_;
            MaybeShot();
            return 0;
        }
        case WM_PRINTCLIENT:
            PaintHeader(reinterpret_cast<HDC>(w));
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

        case WM_ACTIVATE:
            // as a dialog does: the keyboard comes back to the control that had it (after a question, a Setup's wizard, another program)
            if (LOWORD(w) == WA_INACTIVE) {
                HWND f = GetFocus();
                if (f && (IsChild(hwnd_, f))) focus_ = f;
            } else if (!HIWORD(w) && focus_ && IsWindow(focus_) && IsChild(hwnd_, focus_) && IsWindowVisible(focus_) && IsWindowEnabled(focus_)) {
                SetFocus(focus_);
                return 0;
            }
            break;

        case WM_DPICHANGED: {
            if (!opt_.dpi) dpi_ = HIWORD(w);
            const RECT* r = reinterpret_cast<const RECT*>(l);
            SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            MakeFonts();
            ApplyFonts();
            Layout();
            InvalidateRect(hwnd_, nullptr, TRUE);
            return 0;
        }

        case WM_COMMAND: {
            const int id = LOWORD(w), code = HIWORD(w);
            if (id == IDC_V_INSTALL && code == BN_CLICKED) OnInstall();
            else if (id == IDOK) {
                if (IsWindowEnabled(btn_install_)) OnInstall();
            } else if (id == IDC_V_CHECK && code == BN_CLICKED) {
                sticky_.clear();
                StartChecks(true);
                AppendLog("Checking again: D2RLoader's files and the newest versions.");
                UpdateUi();
            } else if (id == IDCANCEL) {
                OnCancelOrClose();
            }
            return 0;
        }

        case DM_GETDEFID:
            return IsWindowEnabled(btn_install_) ? MAKELONG(IDC_V_INSTALL, DC_HASDEFID) : 0;

        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT: {
            HDC dc = reinterpret_cast<HDC>(w);
            const int id = GetDlgCtrlID(reinterpret_cast<HWND>(l));
            if (id == IDC_V_LOG) {
                SetTextColor(dc, kText);
                SetBkColor(dc, kCard);
                return reinterpret_cast<LRESULT>(white_brush_);
            }
            COLORREF col = kText;
            if (id == IDC_V_NOTICE) col = ToneColor(notice_tone_);
            else if (id == IDC_V_STEP) col = kMuted;
            SetTextColor(dc, col);
            SetBkColor(dc, kBg);
            return reinterpret_cast<LRESULT>(bg_brush_);
        }

        case WM_TIMER:
            if (w == kTimerPoll) {
                Poll();
            } else if (w == kTimerShot) {
                KillTimer(hwnd_, kTimerShot);
                TakeShot();
            } else if (w == kTimerGuard) {
                KillTimer(hwnd_, kTimerGuard);
                if (!shot_done_) {
                    exit_shot_ = 5;
                    Finish();
                }
            }
            return 0;

        case WM_V_FACTS: OnFacts(reinterpret_cast<FactsMsg*>(l)); return 0;
        case WM_V_UPDATE: OnUpdate(reinterpret_cast<UpdateMsg*>(l)); return 0;
        case WM_V_LOG: {
            std::unique_ptr<std::string> s(reinterpret_cast<std::string*>(l));
            AppendLog(*s);
            return 0;
        }
        case WM_V_STAGE: OnStage(reinterpret_cast<StageMsg*>(l)); return 0;
        case WM_V_PIECE: OnPiece(reinterpret_cast<PieceOutcome*>(l)); return 0;
        case WM_V_RUNDONE: OnRunDone(); return 0;
        case WM_V_UNINSTALLED: OnUninstalled(); return 0;
        case WM_V_SHOT: TakeShot(); return 0;
        case WM_V_AUTO:
            OnInstall();
            return 0;

        case WM_CLOSE:
            if (inst_model_.running()) {
                // not while an install runs: Cancel first, or let it finish
                Sticky(Tone::Warn, "An install is running. Press Cancel to stop it at the next safe point, or wait until it has finished.");
                UpdateUi();
                return 0;
            }
            Finish();
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd_, kTimerPoll);
            alive_ = false;
            PostMessageW(owner_, WM_NULL, 0, 0);             // wakes the loop in Run()
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd_, m, w, l);
}

// ------------------------------------------------------------------ the cards' panel

LRESULT CALLBACK View::RowsProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    View* self = nullptr;
    if (m == WM_NCCREATE) {
        self = static_cast<View*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->rows_wnd_ = h;
    } else {
        self = reinterpret_cast<View*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    }
    if (!self) return DefWindowProcW(h, m, w, l);
    return self->OnRows(m, w, l);
}

LRESULT View::OnRows(UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_ERASEBKGND: {
            if (in_rows_paint_) return 1;
            // a check box asks for the background under itself, in its own DC: draw with the DC's origin reset, so GDI and GDI+ agree on where things are
            HDC dc = reinterpret_cast<HDC>(w);
            POINT org{};
            GetViewportOrgEx(dc, &org);
            SetViewportOrgEx(dc, 0, 0, nullptr);
            if (rows_.size() == static_cast<size_t>(kPieceCount)) PaintRows(dc, org.x, org.y, false);
            SetViewportOrgEx(dc, org.x, org.y, nullptr);
            return 1;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            in_rows_paint_ = true;
            HDC dc = BeginPaint(rows_wnd_, &ps);
            in_rows_paint_ = false;
            RECT client{};
            GetClientRect(rows_wnd_, &client);
            HDC mem = CreateCompatibleDC(dc);
            HBITMAP bmp = CreateCompatibleBitmap(dc, client.right, client.bottom);
            HGDIOBJ old = SelectObject(mem, bmp);
            if (rows_.size() == static_cast<size_t>(kPieceCount)) PaintRows(mem, 0, 0, true);
            BitBlt(dc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left, ps.rcPaint.bottom - ps.rcPaint.top, mem, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
            SelectObject(mem, old);
            DeleteObject(bmp);
            DeleteDC(mem);
            EndPaint(rows_wnd_, &ps);
            return 0;
        }
        case WM_PRINTCLIENT:
            if (rows_.size() == static_cast<size_t>(kPieceCount)) PaintRows(reinterpret_cast<HDC>(w), 0, 0, true);
            return 0;
        case WM_COMMAND: {
            const int id = LOWORD(w), code = HIWORD(w);
            if (code != BN_CLICKED) return 0;
            if (id >= IDC_V_ROWCHECK && id < IDC_V_ROWCHECK + kPieceCount) OnRowCheck(id - IDC_V_ROWCHECK);
            else if (id >= IDC_V_ROWACT && id < IDC_V_ROWACT + kPieceCount) OnRowAction(id - IDC_V_ROWACT);
            else if (id >= IDC_V_ROWUN && id < IDC_V_ROWUN + kPieceCount) OnRowUninstall(id - IDC_V_ROWUN);
            return 0;
        }
        case WM_CTLCOLORBTN: {
            // a check box sits on its card: the card's own colour, ticked (light blue) or not (white), enabled or not
            const int ctl = GetDlgCtrlID(reinterpret_cast<HWND>(l));
            if (ctl < IDC_V_ROWCHECK || ctl >= IDC_V_ROWCHECK + kPieceCount) return DefWindowProcW(rows_wnd_, m, w, l);       // a push button draws itself
            const int piece = ctl - IDC_V_ROWCHECK;
            const bool sel = rows_.size() == static_cast<size_t>(kPieceCount) && rows_[static_cast<size_t>(piece)].checked;
            SetBkColor(reinterpret_cast<HDC>(w), sel ? kCardSelected : kCard);
            return reinterpret_cast<LRESULT>(sel ? sel_brush_ : white_brush_);
        }
        case WM_CTLCOLORSTATIC:
            SetBkMode(reinterpret_cast<HDC>(w), TRANSPARENT);
            return reinterpret_cast<LRESULT>(GetStockObject(NULL_BRUSH));
        default:
            break;
    }
    return DefWindowProcW(rows_wnd_, m, w, l);
}

}  // namespace

InstallViewResult ShowInstallView(HINSTANCE instance, HWND owner, Model& model, const std::vector<Piece>& tick, bool explicit_tick) {
    View view(instance, owner, model, tick, explicit_tick);
    return view.Run();
}

}  // namespace gui
}  // namespace vrml
