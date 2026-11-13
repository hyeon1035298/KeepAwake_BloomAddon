// ==WindhawkMod==
// @id              keep-awake
// @name            Coffee
// @description     A coffee cup next to the taskbar clock keeps the computer and the screen awake
// @version         0.1.5
// @author          Hyeon
// @include         explorer.exe
// @architecture    x86-64
// @compilerOptions -lcomctl32 -ldwmapi -lole32 -loleaut32 -lruntimeobject -lshell32 -lshlwapi -luuid
// ==/WindhawkMod==

// SPDX-License-Identifier: GPL-3.0-only
// Source code is published under The GNU General Public License v3.0.
// Third-party sources and licenses: THIRD_PARTY.md in the repository root.

// ==WindhawkModReadme==
/*
# Coffee

A coffee cup next to the taskbar clock. Right-clicking it keeps the computer awake, and
with it the screen if you like; right-clicking again lets it sleep. Clicking it opens a
small window to pick how long: until turned off, 30 minutes, 1 hour, 2 hours or a number
of minutes, and whether the screen stays on too. While on, the cup is filled with the
accent picked in Bloom.

The request is held by one thread of its own, so it ends with the mod. After Explorer
restarts, a running session carries on with the time it had left.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- awake:
  - display: display
    $name: 깨우기 방식
    $description: 켤 때 화면도 켜 둘지, 시스템만 깨어 있게 할지
    $options:
    - display: 화면도 켜 두기
    - system: 시스템만 깨우기
  - duration: always
    $name: 기본 시간
    $description: 커피 아이콘을 오른쪽 클릭해 켤 때 얼마나 켜 둘지
    $options:
    - always: 끌 때까지
    - m30: 30분
    - h1: 1시간
    - h2: 2시간
    - custom: 직접 입력
  - minutes: 45
    $name: 직접 입력 시간(분)
    $description: 기본 시간이 직접 입력일 때의 분. 창의 직접 입력 칸도 이 값에서 시작합니다
  $name: 커피
- pill:
  - theme: system
    $name: 테마
    $description: 위젯과 창의 라이트·다크. Bloom 애드온으로 쓰면 Bloom의 테마를 따릅니다
    $options:
    - system: Windows 작업표시줄 테마
    - dark: 다크
    - light: 라이트
  - glass: true
    $name: 유리 효과
    $description: 끄면 창이 블러 없이 테마 색으로 칠해집니다
  $name: 모양
*/
// ==/WindhawkModSettings==

#include <windows.h>

#include <dwmapi.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#undef GetCurrentTime

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Input.h>
#include <winrt/Windows.UI.Text.h>
#include <winrt/Windows.UI.ViewManagement.h>
#include <winrt/Windows.UI.Xaml.Automation.h>
#include <winrt/Windows.UI.Xaml.Controls.Primitives.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Input.h>
#include <winrt/Windows.UI.Xaml.Interop.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Shapes.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/base.h>

#include <windhawk_utils.h>

namespace wf = winrt::Windows::Foundation;
namespace wdj = winrt::Windows::Data::Json;
namespace wux = winrt::Windows::UI::Xaml;
namespace wuxc = winrt::Windows::UI::Xaml::Controls;
namespace wuxm = winrt::Windows::UI::Xaml::Media;
namespace wuxs = winrt::Windows::UI::Xaml::Shapes;
using winrt::box_value;

namespace {

// ============================================================================
// Basics: unloading, language, paths, files
// ============================================================================

std::atomic<bool> g_unloading;
std::atomic<int> g_pending;  // queued taskbar work still to run

bool g_korean = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_KOREAN;
PCWSTR T(PCWSTR ko, PCWSTR en) {
    return g_korean ? ko : en;
}

std::wstring Format(PCWSTR format, ...) {
    wchar_t buffer[512];
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(buffer, _TRUNCATE, format, args);
    va_end(args);
    return buffer;
}

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR path = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &path))) out = path;
    CoTaskMemFree(path);
    return out;
}

// The add-on's data folder, which Bloom also writes values.json into.
std::wstring DataDir() {
    return KnownFolder(FOLDERID_LocalAppData) + L"\\Bloom\\addons\\keep-awake";
}

HINSTANCE ModuleInstance() {
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&ModuleInstance, &module);
    return module;
}

std::optional<std::wstring> ReadText(const std::wstring& path, DWORD maxBytes = 64 * 1024) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return std::nullopt;
    LARGE_INTEGER size{};
    std::string bytes;
    bool ok = GetFileSizeEx(file, &size) && size.QuadPart <= maxBytes;
    if (ok) {
        bytes.resize(size_t(size.QuadPart));
        DWORD read = 0;
        ok = bytes.empty() || (ReadFile(file, bytes.data(), DWORD(bytes.size()), &read, nullptr) && read == bytes.size());
    }
    CloseHandle(file);
    if (!ok) return std::nullopt;
    int n = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), int(bytes.size()), nullptr, 0);
    std::wstring text(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes.data(), int(bytes.size()), text.data(), n);
    return text;
}

// Whole or not at all: a temporary file, then a rename over the old one.
bool WriteText(const std::wstring& path, const std::wstring& text) {
    int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
    std::string bytes(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), bytes.data(), n, nullptr, nullptr);
    std::wstring temp = path + L".tmp";
    HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(file, bytes.data(), DWORD(bytes.size()), &written, nullptr) && written == bytes.size();
    CloseHandle(file);
    return ok && MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

std::optional<wdj::JsonObject> ParseObject(const std::optional<std::wstring>& text) {
    wdj::JsonObject object{nullptr};
    if (!text || !wdj::JsonObject::TryParse(*text, object)) return std::nullopt;
    return object;
}

// Milliseconds since 1970 (UTC) on the wall clock.
int64_t NowUnixMs() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    int64_t t = (int64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    return (t - 116444736000000000LL) / 10000;
}

// When this boot began, by the wall clock; tells an Explorer restart from a new boot.
int64_t BootUnixMs() {
    return NowUnixMs() - int64_t(GetTickCount64());
}

// ============================================================================
// Settings, as Bloom sets them (values.json, read through the runtime)
// ============================================================================

constexpr int kMaxMinutes = 24 * 60;

struct Config {
    bool display = true;  // else the system only
    int minutes = 0;      // what a click starts with; 0: until turned off
    int custom = 45;      // the typed-in minutes
};
Config g_config;  // taskbar thread

Config ReadConfig() {
    Config c;
    PCWSTR display = Wh_GetStringSetting(L"awake.display");
    c.display = !display || wcscmp(display, L"system") != 0;
    Wh_FreeStringSetting(display);
    c.custom = std::clamp(Wh_GetIntSetting(L"awake.minutes"), 1, kMaxMinutes);
    PCWSTR duration = Wh_GetStringSetting(L"awake.duration");
    c.minutes = !duration                       ? 0
                : !wcscmp(duration, L"m30")     ? 30
                : !wcscmp(duration, L"h1")      ? 60
                : !wcscmp(duration, L"h2")      ? 120
                : !wcscmp(duration, L"custom")  ? c.custom
                                                : 0;
    Wh_FreeStringSetting(duration);
    return c;
}

// ============================================================================
// Look: light or dark and glass, as Bloom hands them to its add-ons
// ============================================================================

// Bloom's design tokens: zinc greys and one ink whose alpha makes every overlay. The
// accent is the one picked in Bloom (ReadAccent), rose until it is read.
struct Palette {
    uint32_t win, pop, btn, btnHover, fg, muted, faint, ink, accent;
    double tintOpacity, tintLuminosity;
};
Palette kDark{0xFF18181B, 0xFF27272A, 0xFF3F3F46, 0xFF52525B, 0xFFECEDEE, 0xFFA1A1AA,
              0x99ECEDEE, 0x00FFFFFF, 0xFFFF7A9E, 0.75, 0.85};
Palette kLight{0xFFF4F4F5, 0xFFFAFAFA, 0xFFFFFFFF, 0xFFF4F4F5, 0xFF11181C, 0xFF3F3F46,
               0xFF52525B, 0x00111111, 0xFFE04E78, 0.75, 0.9};

// ---- the accent ---------------------------------------------------------------
// Bloom keeps its accent in its settings.json ("accent": a preset, or "custom" with
// "accentCustom" #RRGGBB, the light theme's colour) and does not hand it to add-ons, so
// it is read from there, with Bloom's own pairs. Without Bloom's settings the Windows
// accent is used.

struct AccentPair {
    uint32_t dark, light;
    bool operator==(const AccentPair&) const = default;
};

// HSL, as Bloom lifts a typed colour for the dark theme.
void ToHsl(uint32_t argb, double& h, double& s, double& l) {
    double r = BYTE(argb >> 16) / 255.0, g = BYTE(argb >> 8) / 255.0, b = BYTE(argb) / 255.0;
    double hi = std::max({r, g, b}), lo = std::min({r, g, b}), d = hi - lo;
    l = (hi + lo) / 2;
    s = d == 0 ? 0 : d / (1 - std::fabs(2 * l - 1));
    h = d == 0 ? 0 : hi == r ? 60 * std::fmod((g - b) / d + 6, 6) : hi == g ? 60 * ((b - r) / d + 2) : 60 * ((r - g) / d + 4);
}

uint32_t FromHsl(double h, double s, double l) {
    double c = (1 - std::fabs(2 * l - 1)) * s, x = c * (1 - std::fabs(std::fmod(h / 60, 2) - 1)), m = l - c / 2;
    double r = 0, g = 0, b = 0;
    int k = int(h / 60) % 6;
    if (k == 0) r = c, g = x;
    else if (k == 1) r = x, g = c;
    else if (k == 2) g = c, b = x;
    else if (k == 3) g = x, b = c;
    else if (k == 4) r = x, b = c;
    else r = c, b = x;
    auto byte = [&](double v) { return uint32_t(std::lround(std::clamp(v + m, 0.0, 1.0) * 255)); };
    return 0xFF000000u | byte(r) << 16 | byte(g) << 8 | byte(b);
}

std::optional<AccentPair> BloomAccent(const std::wstring& key, const std::wstring& custom) {
    struct Preset {
        PCWSTR key;
        AccentPair pair;
    };
    static constexpr Preset kPresets[] = {
        {L"rose", {0xFFFF7A9E, 0xFFE04E78}},      {L"lavender", {0xFFB49CFF, 0xFF7B5BDB}},
        {L"sky", {0xFF72B6FF, 0xFF2F72D6}},       {L"mint", {0xFF5ED3AE, 0xFF168F6C}},
        {L"tangerine", {0xFFFF9B5E, 0xFFD9652A}}, {L"lemon", {0xFFEDC94F, 0xFFA87C00}},
        {L"gray", {0xFFE4E4E4, 0xFF2E2E2E}},
    };
    if (key == L"custom") {
        std::wstring t = custom;
        if (!t.empty() && t[0] == L'#') t.erase(0, 1);
        if (t.size() != 6 || t.find_first_not_of(L"0123456789abcdefABCDEF") != std::wstring::npos) return std::nullopt;
        uint32_t light = 0xFF000000u | uint32_t(wcstoul(t.c_str(), nullptr, 16));
        double h, s, l;
        ToHsl(light, h, s, l);
        return AccentPair{FromHsl(h, s, std::max(l, std::min(0.86, l + 0.18))), light};
    }
    for (auto& p : kPresets)
        if (key == p.key) return p.pair;
    return std::nullopt;
}

std::wstring BloomSettingsPath() {
    return KnownFolder(FOLDERID_RoamingAppData) + L"\\Bloom\\settings.json";
}

AccentPair WindowsAccent() {
    try {
        winrt::Windows::UI::ViewManagement::UISettings ui;
        auto argb = [](winrt::Windows::UI::Color c) { return uint32_t(c.A) << 24 | c.R << 16 | c.G << 8 | c.B; };
        return {argb(ui.GetColorValue(winrt::Windows::UI::ViewManagement::UIColorType::AccentLight2)),
                argb(ui.GetColorValue(winrt::Windows::UI::ViewManagement::UIColorType::AccentDark1))};
    } catch (...) {
        return {0xFFFF7A9E, 0xFFE04E78};
    }
}

AccentPair ReadAccent() {
    if (auto text = ReadText(BloomSettingsPath(), 4 * 1024 * 1024)) {
        std::wstring s = *text;
        if (!s.empty() && s[0] == 0xFEFF) s.erase(0, 1);
        wdj::JsonObject o{nullptr};
        if (wdj::JsonObject::TryParse(s, o)) {
            auto key = std::wstring(o.GetNamedString(L"accent", L"rose"));
            auto custom = std::wstring(o.GetNamedString(L"accentCustom", L""));
            return BloomAccent(key, custom).value_or(AccentPair{0xFFFF7A9E, 0xFFE04E78});
        }
    }
    return WindowsAccent();
}

// Puts the accent into both palettes; true when it changed.
bool ApplyAccent(const AccentPair& a) {
    if (kDark.accent == a.dark && kLight.accent == a.light) return false;
    kDark.accent = a.dark;
    kLight.accent = a.light;
    return true;
}

// Bloom's settings file is looked at again only when its time changed.
FILETIME g_bloomSettingsTime{};
bool RefreshAccent() {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    FILETIME t{};
    if (GetFileAttributesExW(BloomSettingsPath().c_str(), GetFileExInfoStandard, &data)) t = data.ftLastWriteTime;
    if (!CompareFileTime(&t, &g_bloomSettingsTime) && (t.dwLowDateTime || t.dwHighDateTime)) return false;
    g_bloomSettingsTime = t;
    return ApplyAccent(ReadAccent());
}

struct Look {
    bool known = false;  // from Bloom; else the taskbar's own theme
    bool dark = true;
    bool glass = true;
};
Look g_look;

// The add-on's own settings (pill.theme, pill.glass), which Bloom sets to its theme.
// With "system" the widget follows the taskbar it sits on.
Look ReadLook() {
    Look look;
    PCWSTR theme = Wh_GetStringSetting(L"pill.theme");
    if (theme && (!wcscmp(theme, L"dark") || !wcscmp(theme, L"light"))) {
        look.known = true;
        look.dark = !wcscmp(theme, L"dark");
    }
    Wh_FreeStringSetting(theme);
    look.glass = Wh_GetIntSetting(L"pill.glass") != 0;
    return look;
}

const Palette& PaletteFor(const wux::FrameworkElement& onTaskbar) {
    bool dark = g_look.known ? g_look.dark : !onTaskbar || onTaskbar.ActualTheme() != wux::ElementTheme::Light;
    return dark ? kDark : kLight;
}

wuxm::SolidColorBrush Brush(uint32_t argb) {
    return wuxm::SolidColorBrush(winrt::Windows::UI::Color{BYTE(argb >> 24), BYTE(argb >> 16), BYTE(argb >> 8), BYTE(argb)});
}

// A Thickness boxed by XAML itself. box_value boxes this XAML struct in this DLL, and a
// resource dictionary that keeps the box would release it after the DLL is unloaded.
wf::IInspectable XamlBox(wux::Thickness t) {
    wuxc::Border b;
    b.Padding(t);
    return b.GetValue(wuxc::Border::PaddingProperty());
}

uint32_t WithAlpha(uint32_t argb, BYTE alpha) {
    return (argb & 0x00FFFFFF) | (uint32_t(alpha) << 24);
}

// The ink at a percent of alpha: the w5, w8, w10, w15 and w25 overlays.
uint32_t Ink(const Palette& c, int percent) {
    return WithAlpha(c.ink, BYTE(percent * 255 / 100));
}

wuxm::Brush GlassBrush(const Palette& c) {
    if (!g_look.glass) return Brush(c.pop);
    auto color = Brush(c.pop).Color();
    wuxm::AcrylicBrush glass;
    glass.BackgroundSource(wuxm::AcrylicBackgroundSource::HostBackdrop);
    glass.TintColor(color);
    glass.TintOpacity(c.tintOpacity);
    glass.TintLuminosityOpacity(c.tintLuminosity);
    glass.FallbackColor(color);
    return glass;
}

// ============================================================================
// Taskbar windows and their XAML (CTaskBand::GetTaskbarHost)
// ============================================================================

void* CTaskBand_ITaskListWndSite_vftable;
void* CSecondaryTaskBand_ITaskListWndSite_vftable;
using GetTaskbarHost_t = void*(WINAPI*)(void* pThis, void** result);
GetTaskbarHost_t CTaskBand_GetTaskbarHost_Original;
GetTaskbarHost_t CSecondaryTaskBand_GetTaskbarHost_Original;
void* TaskbarHost_FrameHeight_Original;
using Decref_t = void(WINAPI*)(void* pThis);
Decref_t std__Ref_count_base__Decref_Original;

bool HookTaskbarDll() {
    HMODULE module = LoadLibraryExW(L"taskbar.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) return false;
    WindhawkUtils::SYMBOL_HOOK hooks[] = {
        {{LR"(const CTaskBand::`vftable'{for `ITaskListWndSite'})"}, &CTaskBand_ITaskListWndSite_vftable},
        {{LR"(const CSecondaryTaskBand::`vftable'{for `ITaskListWndSite'})"}, &CSecondaryTaskBand_ITaskListWndSite_vftable},
        {{LR"(public: virtual class std::shared_ptr<class TaskbarHost> __cdecl CTaskBand::GetTaskbarHost(void)const )"},
         &CTaskBand_GetTaskbarHost_Original},
        {{LR"(public: virtual class std::shared_ptr<class TaskbarHost> __cdecl CSecondaryTaskBand::GetTaskbarHost(void)const )"},
         &CSecondaryTaskBand_GetTaskbarHost_Original},
        {{LR"(public: int __cdecl TaskbarHost::FrameHeight(void)const )"}, &TaskbarHost_FrameHeight_Original},
        {{LR"(public: void __cdecl std::_Ref_count_base::_Decref(void))"}, &std__Ref_count_base__Decref_Original},
    };
    return WindhawkUtils::HookSymbols(module, hooks, ARRAYSIZE(hooks));
}

bool IsSecondary(HWND hwnd) {
    wchar_t cls[32];
    return GetClassNameW(hwnd, cls, ARRAYSIZE(cls)) && _wcsicmp(cls, L"Shell_SecondaryTrayWnd") == 0;
}

std::vector<HWND> Taskbars() {
    std::vector<HWND> list;
    EnumWindows(
        [](HWND hwnd, LPARAM param) -> BOOL {
            wchar_t cls[32];
            DWORD pid = 0;
            if (GetWindowThreadProcessId(hwnd, &pid) && pid == GetCurrentProcessId() &&
                GetClassNameW(hwnd, cls, ARRAYSIZE(cls)) &&
                (_wcsicmp(cls, L"Shell_TrayWnd") == 0 || _wcsicmp(cls, L"Shell_SecondaryTrayWnd") == 0))
                ((std::vector<HWND>*)param)->push_back(hwnd);
            return TRUE;
        },
        (LPARAM)&list);
    std::ranges::stable_partition(list, [](HWND h) { return !IsSecondary(h); });
    return list;
}

HWND PrimaryTaskbar() {
    auto list = Taskbars();
    return list.empty() || IsSecondary(list[0]) ? nullptr : list[0];
}

// Taskbar thread.
wux::FrameworkElement TaskbarElement(HWND hwnd) {
    bool secondary = IsSecondary(hwnd);
    void* vftable = secondary ? CSecondaryTaskBand_ITaskListWndSite_vftable : CTaskBand_ITaskListWndSite_vftable;
    auto getHost = secondary ? CSecondaryTaskBand_GetTaskbarHost_Original : CTaskBand_GetTaskbarHost_Original;
    if (!vftable || !getHost || !TaskbarHost_FrameHeight_Original || !std__Ref_count_base__Decref_Original)
        return nullptr;
    HWND band = secondary ? FindWindowExW(hwnd, nullptr, L"WorkerW", nullptr) : (HWND)GetPropW(hwnd, L"TaskbandHWND");
    void* site = band ? (void*)GetWindowLongPtrW(band, 0) : nullptr;
    for (int i = 0; site && *(void**)site != vftable; i++) {
        if (i == 20) return nullptr;
        site = (void**)site + 1;
    }
    if (!site) return nullptr;
    void* host[2]{};  // std::shared_ptr<TaskbarHost>
    getHost(site, host);
    if (!host[0]) {
        if (host[1]) std__Ref_count_base__Decref_Original(host[1]);
        return nullptr;
    }
    // 48:83EC 28 | sub rsp,28 ; 48:83C1 48 | add rcx,48 <- the element's offset in TaskbarHost
    size_t offset = 0x10;
    const BYTE* b = (const BYTE*)TaskbarHost_FrameHeight_Original;
    if (b[0] == 0x48 && b[1] == 0x83 && b[2] == 0xEC && b[4] == 0x48 && b[5] == 0x83 && b[6] == 0xC1 && b[7] <= 0x7F)
        offset = b[7];
    wux::FrameworkElement element{nullptr};
    winrt::copy_from_abi(element, *(void**)((BYTE*)host[0] + offset));
    std__Ref_count_base__Decref_Original(host[1]);
    return element;
}

wux::FrameworkElement Parent(const wux::FrameworkElement& e) {
    return wuxm::VisualTreeHelper::GetParent(e).try_as<wux::FrameworkElement>();
}

template <class F>
wux::FrameworkElement FindDescendant(const wux::FrameworkElement& e, F&& visit, int depth) {
    if (!e || depth <= 0) return nullptr;
    int n = wuxm::VisualTreeHelper::GetChildrenCount(e);
    for (int i = 0; i < n; i++) {
        auto child = wuxm::VisualTreeHelper::GetChild(e, i).try_as<wux::FrameworkElement>();
        if (!child) continue;
        if (visit(child)) return child;
        if (auto found = FindDescendant(child, visit, depth - 1)) return found;
    }
    return nullptr;
}

// The tray's horizontal panel: icons, then the clock (NotificationCenterButton).
wuxc::StackPanel TrayPanel(HWND hwnd) {
    auto element = TaskbarElement(hwnd);
    auto root = element ? element.XamlRoot() : nullptr;
    auto content = root ? root.Content().try_as<wux::FrameworkElement>() : nullptr;
    auto frame = FindDescendant(
        content, [](auto& e) { return winrt::get_class_name(e) == L"SystemTray.SystemTrayFrame"; }, 8);
    auto grid = FindDescendant(frame, [](auto& e) { return e.Name() == L"SystemTrayFrameGrid"; }, 2);
    return grid ? grid.try_as<wuxc::StackPanel>() : nullptr;
}

// Other Bloom add-ons find this widget by name. The row reads coffee, brightness, RAM,
// clock: the brightness widget goes right before the memory-clean one, which goes right
// before the clock, so this one goes before the first of the three it finds.
constexpr PCWSTR kWidgetName = L"BloomAddon.keep-awake";
constexpr PCWSTR kBrightnessWidgetName = L"BloomAddon.brightness";
constexpr PCWSTR kMemoryWidgetName = L"BloomAddon.memory-clean";

uint32_t InsertIndex(const wuxc::StackPanel& tray) {
    auto children = tray.Children();
    uint32_t n = children.Size();
    for (uint32_t i = 0; i < n; i++) {
        auto child = children.GetAt(i).try_as<wux::FrameworkElement>();
        if (!child) continue;
        auto name = child.Name();
        if (name == kBrightnessWidgetName || name == kMemoryWidgetName || name == L"NotificationCenterButton" ||
            FindDescendant(child, [](auto& e) { return e.Name() == L"NotificationCenterButton"; }, 3))
            return i;
    }
    return n;
}

bool TaskbarOnTop(HWND taskbar) {
    RECT rc{};
    MONITORINFO mi{sizeof(mi)};
    if (!GetWindowRect(taskbar, &rc) || !GetMonitorInfoW(MonitorFromWindow(taskbar, MONITOR_DEFAULTTONEAREST), &mi))
        return false;
    return (rc.top + rc.bottom) / 2 < (mi.rcMonitor.top + mi.rcMonitor.bottom) / 2;
}

// ---- running work on the taskbar thread ---------------------------------------

UINT RunMsg() {
    static UINT msg = RegisterWindowMessageW(L"KeepAwake_Run");
    return msg;
}

// An auto-hiding taskbar stays out while the window is open on it, as it does for
// Windows' own tray windows: the tray's hide check (timer 2, as TrayUI::_SetAutoHideTimer
// sets it) skips its ticks then. While the taskbar is in front Windows keeps it out by
// itself, so those ticks go on.
constexpr UINT_PTR kTrayHideTimer = 2;
HWND g_holdTaskbar;  // taskbar thread

LRESULT CALLBACK TaskbarSubclass(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, DWORD_PTR) {
    if (msg == WM_TIMER && wParam == kTrayHideTimer && hwnd == g_holdTaskbar && GetForegroundWindow() != hwnd)
        return 0;
    if (msg == RunMsg() && lParam) {
        try {
            (*(std::function<void()>*)lParam)();
        } catch (...) {
            Wh_Log(L"taskbar work failed: %08X", winrt::to_hresult());
        }
        return 0;
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

HWND g_subclassed;

// Holds `taskbar` out (null lets go). Called on the taskbar thread.
void HoldTaskbar(HWND taskbar) {
    if (g_holdTaskbar == taskbar) return;
    if (g_holdTaskbar && g_holdTaskbar != g_subclassed)
        WindhawkUtils::RemoveWindowSubclassFromAnyThread(g_holdTaskbar, TaskbarSubclass);
    g_holdTaskbar = nullptr;
    if (taskbar && (taskbar == g_subclassed ||
                    WindhawkUtils::SetWindowSubclassFromAnyThread(taskbar, TaskbarSubclass, 0)))
        g_holdTaskbar = taskbar;
}

// Runs `work` on the window's thread and waits for it.
bool RunOn(HWND hwnd, std::function<void()> work) {
    if (!hwnd) return false;
    if (g_subclassed != hwnd) {
        if (g_subclassed) WindhawkUtils::RemoveWindowSubclassFromAnyThread(g_subclassed, TaskbarSubclass);
        g_subclassed = nullptr;
        if (!WindhawkUtils::SetWindowSubclassFromAnyThread(hwnd, TaskbarSubclass, 0)) return false;
        g_subclassed = hwnd;
    }
    SendMessageW(hwnd, RunMsg(), 0, (LPARAM)&work);
    return true;
}

winrt::Windows::System::DispatcherQueue g_queue{nullptr};  // the taskbar thread's, set there

// Runs `work` on the taskbar thread later, from any thread.
bool Post(std::function<void()> work) {
    auto queue = g_queue;
    if (g_unloading || !queue) return false;
    g_pending++;
    bool queued = false;
    try {
        queued = queue.TryEnqueue([work = std::move(work)]() mutable {
            // What the work holds is let go of before the count drops, so Wh_ModUninit never
            // returns while a destructor in this DLL is still to run on the taskbar thread.
            if (auto run = std::move(work); !g_unloading) {
                try {
                    run();
                } catch (...) {
                    Wh_Log(L"posted work failed: %08X", winrt::to_hresult());
                }
            }
            g_pending--;
        });
    } catch (...) {
    }
    if (!queued) g_pending--;
    return queued;
}

// ============================================================================
// The awake thread: the only thread that calls SetThreadExecutionState
// ============================================================================
// ES_CONTINUOUS holds only while the calling thread lives, so one thread of the mod's own
// holds it and lets go when told to, when its time is up, or when the mod unloads. The
// taskbar thread owns the session; this thread only follows what it was last told.

struct AwakeOrder {
    bool on = false;
    bool display = false;
    uint64_t deadline = 0;  // GetTickCount64 time to stop at; 0: until told
    uint64_t session = 0;   // which session this is, so a late "time is up" can be told apart
    bool quit = false;
};

std::mutex g_awakeLock;
AwakeOrder g_order;  // under g_awakeLock
HANDLE g_awakeEvent, g_awakeThread;

void OnTimeUp(uint64_t session);  // taskbar thread

DWORD WINAPI AwakeMain(void*) {
    EXECUTION_STATE held = 0;
    for (;;) {
        AwakeOrder order;
        {
            std::lock_guard lock(g_awakeLock);
            order = g_order;
        }
        if (order.quit) break;
        uint64_t now = GetTickCount64();
        bool timeUp = order.on && order.deadline && now >= order.deadline;
        EXECUTION_STATE want =
            order.on && !timeUp ? ES_SYSTEM_REQUIRED | (order.display ? ES_DISPLAY_REQUIRED : 0) : 0;
        if (want != held) {
            if (SetThreadExecutionState(ES_CONTINUOUS | want)) held = want;
            else Wh_Log(L"execution state %08X refused", want);
        }
        if (timeUp) {
            {
                std::lock_guard lock(g_awakeLock);
                if (g_order.session == order.session) g_order.on = false;
            }
            uint64_t session = order.session;
            Post([session] { OnTimeUp(session); });
            continue;
        }
        // Waits do not run on while the computer sleeps, so a long wait is cut into pieces
        // and the deadline read again from the tick count, which does.
        DWORD wait = want && order.deadline ? DWORD(std::min<uint64_t>(order.deadline - now, 60000)) : INFINITE;
        WaitForSingleObject(g_awakeEvent, wait);
    }
    if (held) SetThreadExecutionState(ES_CONTINUOUS);
    return 0;
}

void Order(bool on, bool display, uint64_t deadline, uint64_t session) {
    {
        std::lock_guard lock(g_awakeLock);
        g_order.on = on;
        g_order.display = display;
        g_order.deadline = deadline;
        g_order.session = session;
    }
    if (g_awakeEvent) SetEvent(g_awakeEvent);
}

void StartAwakeThread() {
    if (g_awakeThread) return;
    g_awakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (g_awakeEvent) g_awakeThread = CreateThread(nullptr, 0, AwakeMain, nullptr, 0, nullptr);
    if (!g_awakeThread) Wh_Log(L"awake thread not started: %u", GetLastError());
}

void StopAwakeThread() {
    if (g_awakeThread) {
        {
            std::lock_guard lock(g_awakeLock);
            g_order.quit = true;
        }
        SetEvent(g_awakeEvent);
        if (WaitForSingleObject(g_awakeThread, 3000) == WAIT_TIMEOUT) Wh_Log(L"awake thread still running");
        CloseHandle(g_awakeThread);
        g_awakeThread = nullptr;
    }
    if (g_awakeEvent) {
        CloseHandle(g_awakeEvent);
        g_awakeEvent = nullptr;
    }
}

// ============================================================================
// The session: on or off, how long, and whether the screen stays on (taskbar thread)
// ============================================================================

struct Session {
    bool on = false;
    bool display = true;
    int minutes = 0;          // what was picked; 0: until turned off
    uint64_t deadline = 0;    // GetTickCount64 time it ends; 0: until turned off
    int64_t untilUnixMs = 0;  // the same by the wall clock, for the state file
    uint64_t id = 0;
};
Session g_s;

void PaintWidgets(bool force = false);
void RefreshPanel();
void UpdateTicker();

std::wstring StatePath() {
    return DataDir() + L"\\awake.json";
}

// awake.json lets a session outlive an Explorer restart; it is written on every change.
void SaveState() {
    wdj::JsonObject obj;
    obj.SetNamedValue(L"v", wdj::JsonValue::CreateNumberValue(1));
    obj.SetNamedValue(L"on", wdj::JsonValue::CreateBooleanValue(g_s.on));
    obj.SetNamedValue(L"display", wdj::JsonValue::CreateBooleanValue(g_s.display));
    obj.SetNamedValue(L"minutes", wdj::JsonValue::CreateNumberValue(g_s.minutes));
    obj.SetNamedValue(L"until", wdj::JsonValue::CreateNumberValue(double(g_s.on ? g_s.untilUnixMs : 0)));
    obj.SetNamedValue(L"boot", wdj::JsonValue::CreateNumberValue(double(BootUnixMs())));
    SHCreateDirectoryExW(nullptr, DataDir().c_str(), nullptr);
    if (!WriteText(StatePath(), std::wstring(obj.Stringify()))) Wh_Log(L"state not written: %u", GetLastError());
}

void Apply() {
    Order(g_s.on, g_s.display, g_s.on ? g_s.deadline : 0, g_s.id);
    SaveState();
    PaintWidgets(true);
    RefreshPanel();
    UpdateTicker();
}

void TurnOn(int minutes, bool display) {
    minutes = std::clamp(minutes, 0, kMaxMinutes);
    g_s.on = true;
    g_s.display = display;
    g_s.minutes = minutes;
    g_s.deadline = minutes ? GetTickCount64() + uint64_t(minutes) * 60000 : 0;
    g_s.untilUnixMs = minutes ? NowUnixMs() + int64_t(minutes) * 60000 : 0;
    g_s.id++;
    Wh_Log(L"on: %d min, display %d", minutes, display);
    Apply();
}

void TurnOff() {
    if (!g_s.on) return;
    g_s.on = false;
    g_s.display = g_config.display;
    g_s.id++;
    Wh_Log(L"off");
    Apply();
}

void Toggle() {
    if (g_s.on) TurnOff();
    else TurnOn(g_config.minutes, g_config.display);
}

void SetDisplay(bool display) {
    if (g_s.display == display) return;
    g_s.display = display;
    Apply();
}

void OnTimeUp(uint64_t session) {
    if (g_s.on && g_s.id == session) {
        Wh_Log(L"time is up");
        TurnOff();
    }
}

// Milliseconds left; -1 when on until turned off, 0 when off.
int64_t Left() {
    if (!g_s.on) return 0;
    if (!g_s.deadline) return -1;
    uint64_t now = GetTickCount64();
    return now >= g_s.deadline ? 0 : int64_t(g_s.deadline - now);
}

// A session running when Explorer went away carries on for the time it had left, in the
// same boot only.
void RestoreState() {
    auto o = ParseObject(ReadText(StatePath()));
    if (!o || !o->GetNamedBoolean(L"on", false)) return;
    int64_t boot = int64_t(o->GetNamedNumber(L"boot", 0));
    if (std::llabs(boot - BootUnixMs()) > 120000) return;
    int64_t until = int64_t(o->GetNamedNumber(L"until", 0));
    int64_t now = NowUnixMs();
    if (until && until <= now + 1000) return;
    g_s.on = true;
    g_s.display = o->GetNamedBoolean(L"display", g_config.display);
    g_s.minutes = std::clamp(int(o->GetNamedNumber(L"minutes", 0)), 0, kMaxMinutes);
    g_s.untilUnixMs = until;
    g_s.deadline = until ? GetTickCount64() + uint64_t(until - now) : 0;
    g_s.id++;
    Wh_Log(L"restored, %lld s left", until ? (until - now) / 1000 : -1LL);
}

// "1시간 23분", "45분", "1분 미만".
std::wstring Duration(int64_t ms) {
    int64_t minutes = (ms + 59999) / 60000;
    if (ms < 60000) return T(L"1분 미만", L"under a minute");
    int64_t h = minutes / 60, m = minutes % 60;
    if (!h) return Format(T(L"%lld분", L"%lld min"), m);
    if (!m) return Format(T(L"%lld시간", L"%lld h"), h);
    return Format(T(L"%lld시간 %lld분", L"%lld h %lld min"), h, m);
}

// "1:23:45", for the window while it is open.
std::wstring Clock(int64_t ms) {
    int64_t s = (ms + 999) / 1000;
    return s >= 3600 ? Format(L"%lld:%02lld:%02lld", s / 3600, s / 60 % 60, s % 60)
                     : Format(L"%lld:%02lld", s / 60, s % 60);
}

std::wstring ModeName(bool display) {
    return display ? T(L"화면도 켜 둠", L"screen kept on") : T(L"시스템만 깨움", L"system only");
}

// ============================================================================
// The widget: a coffee cup, filled while on
// ============================================================================

// The neighbours' line (1.3) and body height (0.95 to 14.25 of 18), with the box only as wide
// as the cup's ink plus their 0.5 edge, so the space between the drawings matches theirs.
constexpr double kW = 20, kH = 18, kStroke = 1.3;
constexpr PCWSTR kFont = L"Segoe UI, Malgun Gothic";  // Bloom's own pair

winrt::Windows::UI::Text::FontWeight Weight(int weight) {
    return {uint16_t(weight)};
}

// A path from figures written as start point and segments.
struct PathBuilder {
    wuxm::PathGeometry geometry;
    wuxm::PathFigure figure{nullptr};
    PathBuilder& Move(double x, double y, bool closed = false) {
        figure = wuxm::PathFigure();
        figure.StartPoint({float(x), float(y)});
        figure.IsClosed(closed);
        figure.IsFilled(closed);
        geometry.Figures().Append(figure);
        return *this;
    }
    PathBuilder& Line(double x, double y) {
        wuxm::LineSegment s;
        s.Point({float(x), float(y)});
        figure.Segments().Append(s);
        return *this;
    }
    PathBuilder& Arc(double x, double y, double r) {  // clockwise, the short way
        wuxm::ArcSegment s;
        s.Point({float(x), float(y)});
        s.Size({float(r), float(r)});
        s.SweepDirection(wuxm::SweepDirection::Clockwise);
        figure.Segments().Append(s);
        return *this;
    }
    PathBuilder& Curve(double x1, double y1, double x2, double y2, double x, double y) {
        wuxm::BezierSegment s;
        s.Point1({float(x1), float(y1)});
        s.Point2({float(x2), float(y2)});
        s.Point3({float(x), float(y)});
        figure.Segments().Append(s);
        return *this;
    }
    wuxs::Path Path(double stroke) {
        wuxs::Path p;
        p.Data(geometry);
        p.StrokeThickness(stroke);
        p.StrokeStartLineCap(wuxm::PenLineCap::Round);
        p.StrokeEndLineCap(wuxm::PenLineCap::Round);
        p.StrokeLineJoin(wuxm::PenLineJoin::Round);
        return p;
    }
};

// One drawing of the cup: a body, a handle on the right and a saucer, outlined like the
// neighbours' screen and RAM module; while on, the cup is filled with the accent.
struct CupIcon {
    wuxc::Canvas canvas{nullptr};
    wuxs::Path body{nullptr}, handle{nullptr};
    wuxs::Rectangle saucer{nullptr};

    void Build() {
        canvas = wuxc::Canvas();
        canvas.Width(kW);
        canvas.Height(kH);
        body = PathBuilder().Move(1.15, 0.95, true).Line(15.85, 0.95).Line(15.85, 10.05).Arc(11.65, 14.25, 4.2)
                   .Line(5.35, 14.25).Arc(1.15, 10.05, 4.2).Path(kStroke);
        handle = PathBuilder().Move(15.85, 3.3).Arc(15.85, 9.2, 2.95).Path(kStroke);
        saucer = wuxs::Rectangle();
        saucer.Width(16);
        saucer.Height(kStroke);
        saucer.RadiusX(kStroke / 2);
        saucer.RadiusY(kStroke / 2);
        wuxc::Canvas::SetLeft(saucer, 0.5);
        wuxc::Canvas::SetTop(saucer, kH - 1.45 - kStroke / 2);
        for (wux::UIElement e : {wux::UIElement(handle), wux::UIElement(body), wux::UIElement(saucer)})
            canvas.Children().Append(e);
    }

    // Off: the outline only. On: the same outline with the cup filled in the accent.
    void Paint(bool on, const Palette& c) {
        auto ink = Brush(c.fg);
        body.Stroke(ink);
        body.Fill(on ? Brush(c.accent) : Brush(0));
        handle.Stroke(ink);
        saucer.Fill(ink);
    }
};

struct Widget {
    HWND taskbar = nullptr;
    wuxc::Grid root{nullptr};
    CupIcon icon;
    int shown = -1;  // the on state last painted
    std::wstring tip;
    const Palette* palette = nullptr;
    bool hover = false, fitted = false;
    // XAML may keep the root after it leaves the tray; its handlers live in this DLL.
    wux::UIElement::PointerEntered_revoker entered;
    wux::UIElement::PointerExited_revoker exited;
    wux::UIElement::Tapped_revoker tapped;
    wux::UIElement::ContextRequested_revoker context;
    wux::UIElement::RightTapped_revoker rightTapped;
};
std::vector<std::unique_ptr<Widget>> g_widgets;  // taskbar thread
void TogglePanel(Widget& w);
void ToggleFromMenu();

// ---- fitting the taskbar ----------------------------------------------------------
// How the widget and its window sit on the taskbar, kept the same as the brightness and
// memory-clean add-ons: the widget's height and hover, and where the window opens.

// Height and corners of the tray's own icons, so the hover box lines up with theirs.
bool FitToTray(Widget& w, const wuxc::StackPanel& tray) {
    auto icon = FindDescendant(
        tray,
        [](auto& e) {
            auto cls = winrt::get_class_name(e);
            return (cls == L"SystemTray.ChevronIconView" || cls == L"SystemTray.NotifyIconView") &&
                   e.ActualHeight() > 0;
        },
        8);
    if (!icon) return false;
    auto grid = FindDescendant(icon, [](auto& e) { return e.Name() == L"ContainerGrid"; }, 2).try_as<wuxc::Grid>();
    double height = grid && grid.ActualHeight() > 0 ? grid.ActualHeight() : icon.ActualHeight();
    wux::CornerRadius radius = grid ? grid.CornerRadius() : wux::CornerRadius{};
    if (radius.TopLeft <= 0)
        if (auto control = icon.try_as<wuxc::Control>()) radius = control.CornerRadius();
    if (height < 16 || height > 64) return false;
    w.root.Height(height);
    if (radius.TopLeft > 0) w.root.CornerRadius(radius);
    return true;
}

// The tray icons' hover: Windows' subtle fill, by the taskbar's own theme.
uint32_t TrayHover(const wux::FrameworkElement& e) {
    return e.ActualTheme() == wux::ElementTheme::Light ? 0x09000000 : 0x0FFFFFFF;
}

// Opens the window over the widget, or under it when the taskbar is at the top, kGap clear
// of the taskbar's edge rather than of the widget in its middle: the widget's distance to
// that edge is measured in the taskbar's XAML root. Sideways it is centred on the widget but
// kept kGap inside the monitor, as Windows' tray windows are near the screen's corner.
void ShowFlyout(Widget& w, const wuxc::Flyout& flyout) {
    constexpr double kGap = 8;
    bool below = TaskbarOnTop(w.taskbar);
    double toEdge = 0, shift = 0;
    try {
        if (auto root = w.root.XamlRoot()) {
            auto box = w.root.TransformToVisual(nullptr).TransformBounds(
                {0, 0, float(w.root.ActualWidth()), float(w.root.ActualHeight())});
            toEdge = std::max(0.0, below ? root.Size().Height - (box.Y + box.Height) : double(box.Y));
            RECT bar{};
            MONITORINFO mi{sizeof(mi)};
            double scale = root.RasterizationScale();
            auto content = flyout.Content().try_as<wux::FrameworkElement>();
            if (content && scale > 0 && GetWindowRect(w.taskbar, &bar) &&
                GetMonitorInfoW(MonitorFromWindow(w.taskbar, MONITOR_DEFAULTTONEAREST), &mi)) {
                double center = bar.left / scale + box.X + box.Width / 2, half = content.Width() / 2;
                double left = mi.rcMonitor.left / scale + kGap, right = mi.rcMonitor.right / scale - kGap;
                if (center + half > right) shift = right - (center + half);
                if (center + shift - half < left) shift = left - (center - half);
            }
        }
    } catch (...) {
    }
    wuxc::Primitives::FlyoutShowOptions options;
    options.Placement(below ? wuxc::Primitives::FlyoutPlacementMode::Bottom : wuxc::Primitives::FlyoutPlacementMode::Top);
    options.Position(wf::IReference<wf::Point>(wf::Point{
        float(w.root.ActualWidth() / 2 + shift),
        float(below ? w.root.ActualHeight() + toEdge + kGap : -(toEdge + kGap))}));
    flyout.ShowAt(w.root, options);
}

std::wstring Tip() {
    std::wstring head = std::wstring(T(L"커피", L"Coffee")) + L": ";
    if (!g_s.on)
        return head + T(L"꺼짐\n클릭: 시간과 방식 · 오른쪽 클릭: 켜기",
                        L"off\nClick: time and mode · Right-click: turn on");
    int64_t left = Left();
    std::wstring when = left < 0 ? std::wstring(T(L"끌 때까지", L"until turned off"))
                                 : Format(T(L"%s 남음", L"%s left"), Duration(left).c_str());
    return head + ModeName(g_s.display) + L", " + when + L"\n" +
           T(L"클릭: 시간과 방식 · 오른쪽 클릭: 끄기", L"Click: time and mode · Right-click: turn off");
}

void PaintWidget(Widget& w, bool force) {
    const Palette& c = PaletteFor(w.root);
    if (force || w.shown != int(g_s.on) || w.palette != &c) {
        w.shown = int(g_s.on);
        w.palette = &c;
        w.icon.Paint(g_s.on, c);
        w.root.Background(Brush(w.hover ? TrayHover(w.root) : 0));
    }
    std::wstring tip = Tip();
    if (tip != w.tip) {
        w.tip = tip;
        wuxc::ToolTipService::SetToolTip(w.root, box_value(tip));
        wux::Automation::AutomationProperties::SetName(w.root, tip);
    }
}

void PaintWidgets(bool force) {
    for (auto& w : g_widgets) PaintWidget(*w, force);
}

std::unique_ptr<Widget> BuildWidget(HWND taskbar) {
    auto w = std::make_unique<Widget>();
    w->taskbar = taskbar;
    w->root = wuxc::Grid();
    w->root.Name(kWidgetName);
    w->root.Height(32);
    w->root.Padding({5, 0, 5, 0});  // the memory-clean widget's sides
    w->root.Margin({2, 0, 2, 0});
    w->root.CornerRadius({4, 4, 4, 4});
    w->root.VerticalAlignment(wux::VerticalAlignment::Center);
    w->root.Background(Brush(0));
    w->icon.Build();
    w->icon.canvas.VerticalAlignment(wux::VerticalAlignment::Center);
    w->root.Children().Append(w->icon.canvas);
    Widget* raw = w.get();
    w->entered = w->root.PointerEntered(winrt::auto_revoke, [raw](auto&&, auto&&) {
        raw->hover = true;
        PaintWidget(*raw, true);
    });
    w->exited = w->root.PointerExited(winrt::auto_revoke, [raw](auto&&, auto&&) {
        raw->hover = false;
        PaintWidget(*raw, true);
    });
    w->tapped = w->root.Tapped(winrt::auto_revoke, [raw](auto&&, wux::Input::TappedRoutedEventArgs const& args) {
        args.Handled(true);
        TogglePanel(*raw);
    });
    // A right click turns it on or off at once, instead of the taskbar's own menu.
    w->context = w->root.ContextRequested(winrt::auto_revoke, [](auto&&, wux::Input::ContextRequestedEventArgs const& args) {
        args.Handled(true);
        ToggleFromMenu();
    });
    w->rightTapped = w->root.RightTapped(winrt::auto_revoke, [](auto&&, wux::Input::RightTappedRoutedEventArgs const& args) {
        args.Handled(true);
        ToggleFromMenu();
    });
    return w;
}

void RemoveWidget(Widget& w) {
    if (!w.root) return;
    if (auto panel = Parent(w.root).try_as<wuxc::Panel>()) {
        uint32_t index;
        if (panel.Children().IndexOf(w.root, index)) panel.Children().RemoveAt(index);
    }
}

// For a widget going for good: its handlers first, then off the tray.
void DropWidget(Widget& w) {
    w.entered.revoke();
    w.exited.revoke();
    w.tapped.revoke();
    w.context.revoke();
    w.rightTapped.revoke();
    RemoveWidget(w);
}

// Every taskbar gets one; ones whose tray was rebuilt go in again.
void PlaceWidgets() {
    auto taskbars = Taskbars();
    std::erase_if(g_widgets, [&](auto& w) {
        bool gone = std::ranges::find(taskbars, w->taskbar) == taskbars.end();
        if (gone) DropWidget(*w);
        return gone;
    });
    for (HWND hwnd : taskbars) {
        auto it = std::ranges::find_if(g_widgets, [&](auto& w) { return w->taskbar == hwnd; });
        if (it != g_widgets.end() && Parent((*it)->root) && (*it)->root.IsLoaded()) {
            if (!(*it)->fitted)
                if (auto tray = Parent((*it)->root).try_as<wuxc::StackPanel>()) (*it)->fitted = FitToTray(**it, tray);
            continue;
        }
        auto tray = TrayPanel(hwnd);
        if (!tray) continue;
        if (it == g_widgets.end()) {
            g_widgets.push_back(BuildWidget(hwnd));
            it = g_widgets.end() - 1;
        } else {
            RemoveWidget(**it);
        }
        // A widget of an earlier load of this mod would be a stranger now.
        for (uint32_t i = tray.Children().Size(); i-- > 0;)
            if (auto e = tray.Children().GetAt(i).try_as<wux::FrameworkElement>(); e && e.Name() == kWidgetName)
                tray.Children().RemoveAt(i);
        tray.Children().InsertAt(InsertIndex(tray), (*it)->root);
        (*it)->fitted = FitToTray(**it, tray);
        PaintWidget(**it, true);
        Wh_Log(L"widget placed on %p", hwnd);
    }
}

// ============================================================================
// Parts in Bloom's design: buttons, segments, switch, number box, panels
// ============================================================================

const Palette* g_c = &kDark;  // the palette the open window is drawn with

wuxc::TextBlock Text(const std::wstring& s, double size, uint32_t color, int weight = 400) {
    wuxc::TextBlock t;
    t.Text(s);
    t.FontFamily(wuxm::FontFamily(kFont));
    t.FontSize(size);
    t.Foreground(Brush(color));
    if (weight != 400) t.FontWeight(Weight(weight));
    t.TextTrimming(wux::TextTrimming::CharacterEllipsis);
    t.VerticalAlignment(wux::VerticalAlignment::Center);
    return t;
}

wuxc::TextBlock Wrapped(const std::wstring& s, double size, uint32_t color) {
    auto t = Text(s, size, color);
    t.TextWrapping(wux::TextWrapping::Wrap);
    t.TextTrimming(wux::TextTrimming::None);
    return t;
}

wuxc::Border Box(double radius, uint32_t background) {
    wuxc::Border b;
    b.CornerRadius({radius, radius, radius, radius});
    b.Background(Brush(background));
    return b;
}

wuxc::StackPanel HStack(double spacing) {
    wuxc::StackPanel s;
    s.Orientation(wuxc::Orientation::Horizontal);
    s.Spacing(spacing);
    return s;
}

wuxc::StackPanel VStack(double spacing) {
    wuxc::StackPanel s;
    s.Spacing(spacing);
    return s;
}

// Columns: > 0 pixels, 0 auto, < 0 star.
wuxc::Grid Columns(std::initializer_list<double> widths, double spacing) {
    wuxc::Grid g;
    g.ColumnSpacing(spacing);
    for (double w : widths) {
        wuxc::ColumnDefinition col;
        col.Width(w > 0 ? wux::GridLength{w, wux::GridUnitType::Pixel}
                  : w == 0 ? wux::GridLengthHelper::Auto()
                           : wux::GridLength{-w, wux::GridUnitType::Star});
        g.ColumnDefinitions().Append(col);
    }
    return g;
}

template <class E>
E InColumn(E e, int column) {
    wuxc::Grid::SetColumn(e, column);
    return e;
}

// Handlers of the window's elements, revoked when the page is rebuilt or the window
// closes: a handler holding its own element would otherwise keep it alive for good.
std::vector<std::shared_ptr<void>> g_keep;

template <class R>
void Keep(R&& revoker) {
    g_keep.push_back(std::make_shared<std::decay_t<R>>(std::forward<R>(revoker)));
}

// Drops the page's handlers once the event that asked for it has finished.
void ReleaseHandlers() {
    if (g_keep.empty()) return;
    auto old = std::make_shared<std::vector<std::shared_ptr<void>>>(std::move(g_keep));
    g_keep.clear();
    if (!Post([old] { old->clear(); })) old->clear();
}

template <class E, class F>
void OnClick(const E& e, F&& action) {
    Keep(e.Tapped(winrt::auto_revoke, [action = std::forward<F>(action)](auto&&, wux::Input::TappedRoutedEventArgs const& args) {
        args.Handled(true);
        action();
    }));
}

template <class E, class In, class Out>
void OnHover(const E& e, In&& enter, Out&& leave) {
    Keep(e.PointerEntered(winrt::auto_revoke, [enter = std::forward<In>(enter)](auto&&, auto&&) { enter(); }));
    Keep(e.PointerExited(winrt::auto_revoke, [leave = std::forward<Out>(leave)](auto&&, auto&&) { leave(); }));
}

// ---- buttons ------------------------------------------------------------------------

// A solid button, height 28, radius 8.
wuxc::Border MakeButton(const std::wstring& label, std::function<void()> action) {
    const Palette& c = *g_c;
    auto root = Box(8, c.btn);
    root.Height(28);
    root.Padding({12, 0, 12, 0});
    if (&c == &kLight) {
        root.BorderThickness({1, 1, 1, 1});
        root.BorderBrush(Brush(Ink(c, 10)));
    }
    auto text = Text(label, 12, c.fg, 500);
    text.HorizontalAlignment(wux::HorizontalAlignment::Center);
    root.Child(text);
    OnHover(
        root, [root, cp = &c] { root.Background(Brush(cp->btnHover)); },
        [root, cp = &c] { root.Background(Brush(cp->btn)); });
    OnClick(root, std::move(action));
    return root;
}

// ---- segments -------------------------------------------------------------------------

struct Segment {
    struct State {
        std::vector<wuxc::Border> items;
        std::vector<wuxc::TextBlock> labels;
        int on = 0;
    };
    std::shared_ptr<State> s;
    wuxc::Border root{nullptr};

    void Select(int index) {
        const Palette& c = *g_c;
        s->on = index;
        for (size_t i = 0; i < s->items.size(); i++) {
            bool sel = int(i) == index;
            s->items[i].Background(Brush(sel ? Ink(c, 15) : 0));
            s->labels[i].Foreground(Brush(sel ? c.fg : c.faint));
        }
    }
};

// w5 track, radius 8, 2 inside; items radius 6, 12 px 500, sharing the width evenly.
// `on` < 0 selects none.
Segment MakeSegment(const std::vector<std::wstring>& names, int on, std::function<void(int)> picked) {
    const Palette& c = *g_c;
    Segment seg;
    seg.s = std::make_shared<Segment::State>();
    seg.root = Box(8, Ink(c, 5));
    seg.root.Padding({2, 2, 2, 2});
    wuxc::Grid row;
    row.ColumnSpacing(2);
    int i = 0;
    for (auto& name : names) {
        wuxc::ColumnDefinition col;
        col.Width({1, wux::GridUnitType::Star});
        row.ColumnDefinitions().Append(col);
        auto item = Box(6, 0);
        item.Height(26);
        item.Padding({4, 0, 4, 0});
        auto label = Text(name, 12, c.faint, 500);
        label.HorizontalAlignment(wux::HorizontalAlignment::Center);
        item.Child(label);
        row.Children().Append(InColumn(item, i));
        seg.s->items.push_back(item);
        seg.s->labels.push_back(label);
        auto state = seg.s;
        OnHover(
            item, [label, cp = &c] { label.Foreground(Brush(cp->fg)); },
            [label, state, i, cp = &c] { label.Foreground(Brush(state->on == i ? cp->fg : cp->faint)); });
        OnClick(item, [seg, i, picked] {
            auto copy = seg;
            copy.Select(i);
            picked(i);
        });
        i++;
    }
    seg.root.Child(row);
    seg.Select(on);
    return seg;
}

// ---- the switch: 40 x 22, lit line inside and a lit knob when on ------------------

wuxc::Border MakeSwitch(bool on, std::function<void(bool)> changed) {
    const Palette& c = *g_c;
    auto track = Box(11, 0);
    track.Width(40);
    track.Height(22);
    track.BorderThickness({1, 1, 1, 1});
    wuxc::Canvas canvas;
    wuxs::Rectangle glowLine, line;
    for (auto* r : {&glowLine, &line}) {
        r->Fill(Brush(r == &line ? c.accent : WithAlpha(c.accent, 0x40)));
        r->RadiusX(1);
        r->RadiusY(1);
        canvas.Children().Append(*r);
    }
    line.Height(2);
    wuxc::Canvas::SetLeft(line, 6);
    wuxc::Canvas::SetTop(line, 9);
    glowLine.Height(6);
    wuxc::Canvas::SetLeft(glowLine, 4);
    wuxc::Canvas::SetTop(glowLine, 7);
    wuxs::Ellipse halo, knob;
    halo.Width(22);
    halo.Height(22);
    halo.Fill(Brush(WithAlpha(c.accent, 0x30)));
    halo.IsHitTestVisible(false);
    knob.Width(14);
    knob.Height(14);
    knob.StrokeThickness(1.5);
    wuxc::Canvas::SetTop(halo, -1);
    wuxc::Canvas::SetTop(knob, 3);
    canvas.Children().Append(halo);
    canvas.Children().Append(knob);
    track.Child(canvas);
    auto state = std::make_shared<bool>(on);
    auto paint = [track, line, glowLine, halo, knob, state, cp = &c] {
        bool v = *state;
        track.BorderBrush(Brush(Ink(*cp, v ? 15 : 25)));
        line.Width(v ? 18 : 0);
        glowLine.Width(v ? 22 : 0);
        halo.Visibility(v ? wux::Visibility::Visible : wux::Visibility::Collapsed);
        wuxc::Canvas::SetLeft(halo, v ? 17 : -1);
        wuxc::Canvas::SetLeft(knob, v ? 21 : 3);
        knob.Fill(Brush(v ? cp->accent : 0));
        knob.Stroke(Brush(v ? cp->accent : cp->faint));
    };
    paint();
    track.Background(Brush(0));
    OnClick(track, [state, paint, changed] {
        *state = !*state;
        paint();
        changed(*state);
    });
    return track;
}

// ---- the number box: press to type, the wheel over it steps by 1 --------------------------

void StyleTextBox(const wuxc::TextBox& box, const Palette& c) {
    box.FontFamily(wuxm::FontFamily(kFont));
    box.FontSize(12);
    box.Padding({0, 0, 0, 0});
    box.MinWidth(0);
    box.MinHeight(0);
    box.BorderThickness({0, 0, 0, 0});
    box.VerticalAlignment(wux::VerticalAlignment::Center);
    box.IsSpellCheckEnabled(false);
    auto res = box.Resources();
    for (auto [key, argb] : std::initializer_list<std::pair<PCWSTR, uint32_t>>{
             {L"TextControlBackground", 0}, {L"TextControlBackgroundPointerOver", 0},
             {L"TextControlBackgroundFocused", 0}, {L"TextControlBorderBrush", 0},
             {L"TextControlBorderBrushPointerOver", 0}, {L"TextControlBorderBrushFocused", 0},
             {L"TextControlForeground", c.fg}, {L"TextControlForegroundPointerOver", c.fg},
             {L"TextControlForegroundFocused", c.fg}, {L"TextControlSelectionHighlightColor", c.accent}})
        res.Insert(box_value(key), Brush(argb));
    res.Insert(box_value(L"TextControlThemeMinHeight"), box_value(0.0));
    res.Insert(box_value(L"TextControlThemeMinWidth"), box_value(0.0));
    res.Insert(box_value(L"TextControlThemePadding"), XamlBox(wux::Thickness{0, 0, 0, 0}));
    res.Insert(box_value(L"TextControlBorderThemeThickness"), XamlBox(wux::Thickness{0, 0, 0, 0}));
}

wux::Input::InputScope NumberScope() {
    wux::Input::InputScope scope;
    wux::Input::InputScopeName name;
    name.NameValue(wux::Input::InputScopeNameValue::Number);
    scope.Names().Append(name);
    return scope;
}

// Reads the number typed into `box`, or `fallback`.
int TypedNumber(const wuxc::TextBox& box, int fallback) {
    std::wstring text(box.Text());
    wchar_t* end = nullptr;
    long v = wcstol(text.c_str(), &end, 10);
    return end && end != text.c_str() ? int(v) : fallback;
}

// w8 field, height 28, radius 8: the number, then its unit in the faint colour. Enter in
// the field runs `enter` after the number is taken.
wuxc::Border NumberBox(int value, int lo, int hi, PCWSTR unit, std::function<void(int)> changed,
                       std::function<void()> enter) {
    const Palette& c = *g_c;
    auto field = Box(8, Ink(c, 8));
    field.Height(28);
    field.BorderThickness({1, 1, 1, 1});
    field.BorderBrush(Brush(0));
    field.Padding({10, 0, 10, 0});
    auto row = HStack(3);
    row.VerticalAlignment(wux::VerticalAlignment::Center);
    row.HorizontalAlignment(wux::HorizontalAlignment::Center);
    wuxc::TextBox box;
    StyleTextBox(box, c);
    int digits = hi >= 1000 ? 4 : hi >= 100 ? 3 : 2;
    box.Width(7.0 * digits);
    box.TextAlignment(wux::TextAlignment::Right);
    box.InputScope(NumberScope());
    box.Text(std::to_wstring(value));
    row.Children().Append(box);
    if (unit && *unit) row.Children().Append(Text(unit, 12, c.faint));
    field.Child(row);
    auto current = std::make_shared<int>(value);
    auto set = [box, current, lo, hi, changed](int v) {
        v = std::clamp(v, lo, hi);
        box.Text(std::to_wstring(v));
        if (v == *current) return;
        *current = v;
        changed(v);
    };
    auto commit = [box, current, set] { set(TypedNumber(box, *current)); };
    OnHover(
        field, [field, cp = &c] { field.BorderBrush(Brush(Ink(*cp, 25))); }, [field] { field.BorderBrush(Brush(0)); });
    OnClick(field, [box] {
        box.Focus(wux::FocusState::Programmatic);
        box.SelectAll();
    });
    Keep(box.GotFocus(winrt::auto_revoke, [box](auto&&, auto&&) { box.SelectAll(); }));
    Keep(box.LostFocus(winrt::auto_revoke, [commit](auto&&, auto&&) { commit(); }));
    Keep(box.KeyDown(winrt::auto_revoke, [commit, enter](auto&&, wux::Input::KeyRoutedEventArgs const& args) {
        if (args.Key() == winrt::Windows::System::VirtualKey::Enter) {
            args.Handled(true);
            commit();
            if (enter) enter();
        }
    }));
    Keep(field.PointerWheelChanged(winrt::auto_revoke, [field, current, set](auto&&, wux::Input::PointerRoutedEventArgs const& args) {
        args.Handled(true);
        int delta = args.GetCurrentPoint(field).Properties().MouseWheelDelta();
        if (delta) set(*current + (delta > 0 ? 1 : -1));
    }));
    return field;
}

wuxc::Grid SectionHead(const std::wstring& title) {
    const Palette& c = *g_c;
    auto g = Columns({-1}, 8);
    g.Margin({2, 14, 2, 6});
    g.Children().Append(Text(title, 12, c.muted, 600));
    return g;
}

// ============================================================================
// The one-flyout rule, on a message-only window of the taskbar thread
// ============================================================================

// Every Bloom add-on's flyout window carries this title; its class is the add-on's own.
constexpr PCWSTR kFlyoutTitle = L"Bloom.Addon.Flyout";
constexpr PCWSTR kFlyoutClass = L"Bloom.Addon.keep-awake.Flyout";

HWND g_flyoutWnd;  // taskbar thread

UINT CloseFlyoutMsg() {
    static UINT msg = RegisterWindowMessageW(L"Bloom.Addon.CloseFlyout");
    return msg;
}

void ClosePanel();

LRESULT CALLBACK FlyoutWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == CloseFlyoutMsg()) {
        if (!g_unloading) ClosePanel();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void CreateFlyoutWindow() {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = FlyoutWndProc;
    wc.hInstance = ModuleInstance();
    wc.lpszClassName = kFlyoutClass;
    RegisterClassExW(&wc);
    g_flyoutWnd = CreateWindowExW(0, kFlyoutClass, kFlyoutTitle, 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                  wc.hInstance, nullptr);
    if (!g_flyoutWnd) Wh_Log(L"flyout window not created: %u", GetLastError());
}

void DestroyFlyoutWindow() {
    if (g_flyoutWnd) {
        DestroyWindow(g_flyoutWnd);
        g_flyoutWnd = nullptr;
    }
    UnregisterClassW(kFlyoutClass, ModuleInstance());
}

// Before this add-on's window opens, every other add-on's closes.
void CloseOtherFlyouts() {
    for (HWND w = nullptr; (w = FindWindowExW(HWND_MESSAGE, w, nullptr, kFlyoutTitle));)
        if (w != g_flyoutWnd) PostMessageW(w, CloseFlyoutMsg(), 0, 0);
}

// ============================================================================
// The window: on or off, the time left, how long and how
// ============================================================================

// The choices in the window, as minutes; 0 runs until turned off, -1 is typed in.
constexpr int kChoices[] = {0, 30, 60, 120, -1};

struct PanelState {
    HWND taskbar = nullptr;
    const Palette* c = &kDark;
    wuxc::Flyout flyout{nullptr};
    wuxc::Border bubble{nullptr}, pageHost{nullptr};
    wuxc::TextBlock status{nullptr}, big{nullptr};
    bool custom = false;  // the typed-in field is shown
    int typed = 45;
    // Revoked before Hide: Closed comes after it, by when an unload may have freed the handler.
    wuxc::Primitives::FlyoutBase::Opened_revoker opened;
    wuxc::Primitives::FlyoutBase::Closed_revoker closed;
};
std::unique_ptr<PanelState> g_panel;
uint64_t g_panelClosedAt = 0, g_menuAt = 0;

void BuildPage();

// Which choice the running session is: its minutes if they are one of the chips, else
// "typed in"; none while off.
int ChoiceIndex() {
    if (!g_s.on) return g_panel && g_panel->custom ? 4 : -1;
    if (g_panel && g_panel->custom) return 4;
    for (int i = 0; i < 4; i++)
        if (kChoices[i] == g_s.minutes) return i;
    return 4;
}

std::wstring StatusLine() {
    return g_s.on ? ModeName(g_s.display) : std::wstring(T(L"꺼짐", L"Off"));
}

std::wstring BigLine() {
    if (!g_s.on) return T(L"잠들 수 있음", L"Free to sleep");
    int64_t left = Left();
    return left < 0 ? std::wstring(T(L"끌 때까지 깨어 있음", L"Awake until turned off"))
                    : Format(T(L"%s 남음", L"%s left"), Clock(left).c_str());
}

// The time left changes every second; the rest only when something was picked.
void RefreshTime() {
    if (!g_panel || !g_panel->big) return;
    g_panel->big.Text(BigLine());
    g_panel->status.Text(StatusLine());
}

void RefreshPanel() {
    if (g_panel) BuildPage();
}

void BuildPage() {
    auto& p = *g_panel;
    g_c = p.c;
    const Palette& c = *p.c;
    ReleaseHandlers();
    auto page = VStack(0);

    // Head: the cup, the name and state, and the switch.
    auto head = Columns({0, -1, 0}, 10);
    head.Height(36);
    head.Margin({4, 0, 2, 0});
    CupIcon cup;
    cup.Build();
    cup.Paint(g_s.on, c);
    cup.canvas.VerticalAlignment(wux::VerticalAlignment::Center);
    head.Children().Append(cup.canvas);
    auto names = VStack(0);
    names.VerticalAlignment(wux::VerticalAlignment::Center);
    names.Children().Append(Text(T(L"커피", L"Coffee"), 13, c.fg, 600));
    p.status = Text(StatusLine(), 12, c.faint);
    names.Children().Append(p.status);
    head.Children().Append(InColumn(names, 1));
    auto sw = MakeSwitch(g_s.on, [](bool on) {
        if (!g_panel) return;
        if (on) TurnOn(g_panel->custom ? g_panel->typed : g_config.minutes, g_s.display);
        else TurnOff();
    });
    sw.VerticalAlignment(wux::VerticalAlignment::Center);
    head.Children().Append(InColumn(sw, 2));
    page.Children().Append(head);

    // The time left, large.
    auto bigBox = Box(12, Ink(c, 5));
    bigBox.Margin({0, 8, 0, 0});
    bigBox.Padding({14, 12, 14, 12});
    p.big = Text(BigLine(), 20, g_s.on ? c.fg : c.muted, 600);
    bigBox.Child(p.big);
    page.Children().Append(bigBox);

    // How long: picking one starts it, or starts it again from now.
    page.Children().Append(SectionHead(T(L"시간", L"How long")));
    auto times = MakeSegment({T(L"계속", L"Always"), T(L"30분", L"30 min"), T(L"1시간", L"1 h"),
                              T(L"2시간", L"2 h"), T(L"직접", L"Custom")},
                             ChoiceIndex(), [](int i) {
                                 if (!g_panel) return;
                                 g_panel->custom = kChoices[i] < 0;
                                 if (g_panel->custom) BuildPage();
                                 else TurnOn(kChoices[i], g_s.display);
                             });
    page.Children().Append(times.root);
    if (p.custom) {
        auto row = Columns({-1, 0, 0}, 8);
        row.Margin({2, 8, 0, 0});
        row.Children().Append(Text(T(L"몇 분 동안", L"For how many minutes"), 12, c.muted));
        auto start = [] {
            if (g_panel) TurnOn(g_panel->typed, g_s.display);
        };
        row.Children().Append(InColumn(NumberBox(p.typed, 1, kMaxMinutes, T(L"분", L"min"),
                                                 [](int v) {
                                                     if (g_panel) g_panel->typed = v;
                                                 },
                                                 start),
                                       1));
        row.Children().Append(InColumn(MakeButton(T(L"시작", L"Start"), start), 2));
        page.Children().Append(row);
    }

    // How: the screen too, or the system only. Applies to the running session; the next
    // one starts from the setting in Bloom again.
    page.Children().Append(SectionHead(T(L"깨우기 방식", L"Keep awake")));
    auto mode = MakeSegment({T(L"화면도 켜 두기", L"Screen too"), T(L"시스템만", L"System only")},
                            g_s.display ? 0 : 1, [](int i) { SetDisplay(i == 0); });
    page.Children().Append(mode.root);
    page.Children().Append([&] {
        auto t = Wrapped(g_s.display ? T(L"화면이 꺼지거나 잠기지 않고, PC도 잠들지 않습니다.",
                                         L"The screen neither turns off nor locks, and the PC does not sleep.")
                                     : T(L"PC는 잠들지 않지만 화면은 평소처럼 꺼질 수 있습니다.",
                                         L"The PC does not sleep, but the screen may turn off as usual."),
                         12, c.faint);
        t.Margin({2, 8, 2, 2});
        return t;
    }());

    p.pageHost.Child(page);
}

// A popup window draws the desktop behind it only once DWM lets it.
void AllowHostBackdrop() {
    EnumThreadWindows(
        GetCurrentThreadId(),
        [](HWND window, LPARAM) -> BOOL {
            wchar_t name[32];
            if (GetClassNameW(window, name, ARRAYSIZE(name)) && wcscmp(name, L"Xaml_WindowedPopupClass") == 0) {
                BOOL allow = TRUE;
                DwmSetWindowAttribute(window, 17 /* DWMWA_USE_HOSTBACKDROPBRUSH */, &allow, sizeof(allow));
            }
            return TRUE;
        },
        0);
}

// The flyout's own presenter shows nothing: the bubble inside draws the glass.
wux::Style BarePresenter() {
    wux::Style style{winrt::xaml_typename<wuxc::FlyoutPresenter>()};
    auto setters = style.Setters();
    auto set = [&](const wux::DependencyProperty& property, const wf::IInspectable& value) {
        setters.Append(wux::Setter(property, value));
    };
    set(wuxc::Control::PaddingProperty(), box_value(wux::Thickness{}));
    set(wuxc::Control::BackgroundProperty(), Brush(0));
    set(wuxc::Control::BorderThicknessProperty(), box_value(wux::Thickness{}));
    set(wuxc::Control::CornerRadiusProperty(), box_value(wux::CornerRadius{}));
    set(wuxc::FlyoutPresenter::IsDefaultShadowEnabledProperty(), box_value(false));
    set(wux::FrameworkElement::MinWidthProperty(), box_value(0.0));
    set(wux::FrameworkElement::MinHeightProperty(), box_value(0.0));
    set(wux::FrameworkElement::MaxWidthProperty(), box_value(10000.0));
    set(wux::FrameworkElement::MaxHeightProperty(), box_value(10000.0));
    return style;
}

void ClosePanel() {
    if (!g_panel) return;
    auto panel = std::move(g_panel);
    g_panelClosedAt = GetTickCount64();
    HoldTaskbar(nullptr);
    ReleaseHandlers();
    UpdateTicker();
    panel->opened.revoke();
    panel->closed.revoke();
    try {
        if (panel->flyout) panel->flyout.Hide();
    } catch (...) {
    }
}

void TogglePanel(Widget& w) {
    if (g_panel) {
        ClosePanel();
        return;
    }
    if (GetTickCount64() - g_panelClosedAt < 300) return;  // the press that just closed it
    CloseOtherFlyouts();
    HoldTaskbar(w.taskbar);
    g_panel = std::make_unique<PanelState>();
    auto& p = *g_panel;
    p.taskbar = w.taskbar;
    p.c = &PaletteFor(w.root);
    p.typed = g_s.on && g_s.minutes && std::ranges::find(kChoices, g_s.minutes) == std::end(kChoices) ? g_s.minutes
                                                                                                      : g_config.custom;
    p.custom = g_s.on && g_s.minutes && std::ranges::find(kChoices, g_s.minutes) == std::end(kChoices);
    g_c = p.c;
    const Palette& c = *p.c;

    // Width 348 as the other add-ons' windows, padding 10, radius 12, a w8 edge.
    p.bubble = wuxc::Border();
    p.bubble.Width(348);
    p.bubble.Padding({10, 10, 10, 12});
    p.bubble.CornerRadius({12, 12, 12, 12});
    p.bubble.Background(GlassBrush(c));
    p.bubble.BorderThickness({1, 1, 1, 1});
    p.bubble.BorderBrush(Brush(Ink(c, 8)));
    p.bubble.RequestedTheme(&c == &kLight ? wux::ElementTheme::Light : wux::ElementTheme::Dark);
    p.pageHost = wuxc::Border();
    p.bubble.Child(p.pageHost);

    p.flyout = wuxc::Flyout();
    p.flyout.FlyoutPresenterStyle(BarePresenter());
    p.flyout.Content(p.bubble);
    p.flyout.ShouldConstrainToRootBounds(false);
    p.flyout.ShowMode(wuxc::Primitives::FlyoutShowMode::Standard);
    p.opened = p.flyout.Opened(winrt::auto_revoke, [](auto&&, auto&&) { AllowHostBackdrop(); });
    p.closed = p.flyout.Closed(winrt::auto_revoke, [](wf::IInspectable const& sender, auto&&) {
        if (g_panel && g_panel->flyout == sender.try_as<wuxc::Flyout>()) {
            g_panel.reset();
            g_panelClosedAt = GetTickCount64();
            HoldTaskbar(nullptr);
            ReleaseHandlers();
            UpdateTicker();
        }
    });
    BuildPage();
    ShowFlyout(w, p.flyout);
    UpdateTicker();
}

// A right click raises both ContextRequested and RightTapped; the second is the same press.
void ToggleFromMenu() {
    uint64_t now = GetTickCount64();
    if (now - g_menuAt < 300) return;
    g_menuAt = now;
    Toggle();
}

// ============================================================================
// The ticker, the tick that keeps the widget in place, start and stop
// ============================================================================

// Once a second while on: the time left in the tooltip and the open window.
winrt::Windows::System::DispatcherQueueTimer g_ticker{nullptr};
winrt::Windows::System::DispatcherQueueTimer g_timer{nullptr};
winrt::Windows::System::DispatcherQueueTimer::Tick_revoker g_tickerTick, g_timerTick;

void UpdateTicker() {
    if (!g_ticker) return;
    if (g_s.on && g_s.deadline) g_ticker.Start();
    else g_ticker.Stop();
}

void Tick() {
    if (g_unloading) return;
    if (RefreshAccent()) {
        PaintWidgets(true);
        RefreshPanel();
    }
    PlaceWidgets();
}

void Start() {
    g_queue = winrt::Windows::System::DispatcherQueue::GetForCurrentThread();
    if (!g_queue) {
        Wh_Log(L"no dispatcher queue on the taskbar thread");
        return;
    }
    g_look = ReadLook();
    RefreshAccent();
    g_config = ReadConfig();
    g_s.display = g_config.display;
    CreateFlyoutWindow();
    RestoreState();
    g_ticker = g_queue.CreateTimer();
    g_ticker.Interval(std::chrono::seconds(1));
    g_tickerTick = g_ticker.Tick(winrt::auto_revoke, [](auto&&, auto&&) {
        if (g_unloading) return;
        PaintWidgets();
        RefreshTime();
    });
    if (g_s.on) Apply();
    PlaceWidgets();
    g_timer = g_queue.CreateTimer();
    g_timer.Interval(std::chrono::seconds(5));
    g_timerTick = g_timer.Tick(winrt::auto_revoke, [](auto&&, auto&&) { Tick(); });
    g_timer.Start();
}

// A clean unload (the add-on turned off, Bloom gone, an update) ends the session for good:
// the add-on turned on again later starts asleep. An Explorer that crashes or restarts never
// gets here, so its session is picked up by the next Explorer.
void Stop() {
    for (auto* tick : {&g_timerTick, &g_tickerTick}) tick->revoke();
    for (auto* timer : {&g_timer, &g_ticker}) {
        if (*timer) {
            timer->Stop();
            *timer = nullptr;
        }
    }
    ClosePanel();
    if (g_s.on) {
        g_s.on = false;
        g_s.id++;
        Order(false, false, 0, g_s.id);
        SaveState();
    }
    for (auto& w : g_widgets) DropWidget(*w);
    g_widgets.clear();
    DestroyFlyoutWindow();
    g_queue = nullptr;
}

}  // namespace

BOOL Wh_ModInit() {
    Wh_Log(L">");
    if (!HookTaskbarDll()) {
        Wh_Log(L"taskbar symbols missing");
        return FALSE;
    }
    return TRUE;
}

void Wh_ModAfterInit() {
    Wh_Log(L">");
    StartAwakeThread();
    RunOn(PrimaryTaskbar(), Start);
}

void Wh_ModBeforeUninit() {
    Wh_Log(L">");
    g_unloading = true;
    RunOn(PrimaryTaskbar(), Stop);
    if (g_subclassed) {
        WindhawkUtils::RemoveWindowSubclassFromAnyThread(g_subclassed, TaskbarSubclass);
        g_subclassed = nullptr;
    }
}

void Wh_ModUninit() {
    Wh_Log(L">");
    StopAwakeThread();
    for (int i = 0; g_pending > 0 && i < 100; i++) Sleep(10);
}

BOOL Wh_ModSettingsChanged(BOOL* bReload) {
    *bReload = FALSE;
    Look look = ReadLook();
    Config config = ReadConfig();
    RunOn(PrimaryTaskbar(), [look, config] {
        bool display = config.display != g_config.display;
        g_look = look;
        g_config = config;
        ClosePanel();  // the window opens again in the new look
        // A new way of keeping awake applies to the running session too; its time stays.
        if (display && g_s.on) SetDisplay(config.display);
        else if (!g_s.on) g_s.display = config.display;
        PaintWidgets(true);
    });
    return TRUE;
}
