// ==WindhawkMod==
// @id              taskbar-music-lounge-picker
// @name            Taskbar Music Lounge (Source Picker)
// @description     A native-style music ticker with media controls and a right-click playback source picker.
// @version         1.0.1
// @author          ciahciach
// @github          https://github.com/ciahciach
// @include         explorer.exe
// @compilerOptions -lole32 -ldwmapi -lgdi32 -luser32 -lwindowsapp -lshcore -lgdiplus -lshell32
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Taskbar Music Lounge (Source Picker)

Fork of [Taskbar Music Lounge](https://github.com/ramensoftware/windhawk-mods/blob/main/mods/taskbar-music-lounge.wh.cpp)
by [Hashah2311](https://github.com/Hashah2311), with one addition: **right-click the widget to pick the playback source.**

A media controller that uses Windows 11 native DWM styling for a seamless look.

## Added in this fork

* **Right-click playback source picker** - a rounded, themed popup listing every active media
  session with its app icon and a play/pause marker.
* **Pinned source** - the chosen source stays on the panel even while another app starts
  playing (e.g. Spotify stays visible while a YouTube video plays).
* **Commands follow the selection** - play/pause/next/previous are sent to the pinned session,
  not to whichever session Windows considers "current".
* **Automatic fallback** - if the pinned app disappears, the panel drops back to the most recent
  playback and re-attaches on its own when the app comes back.
* **`Automatic`** entry at the top of the popup restores the original behaviour.

The pinned source is stored per user in `HKCU\Software\TaskbarMusicLoungePicker` (`PinnedSource`),
so it survives Explorer and Windhawk restarts. Delete that value to reset.

## Features inherited from upstream

* **Universal Support:** Smart scanning detects active playback from any app.
* **Album Art:** Displays current track cover art.
* **Fullscreen Mode:** Hides automatically when running full-screen applications.
* **Native Look:** Windows 11 hardware-accelerated rounding and acrylic blur.
* **Idle Timeout:** Optional setting to fade out the widget when music is paused for X seconds.
* **Controls:** Play/Pause, Next, Previous.
* **Volume:** Scroll over widget to adjust volume.

## Requirements

* **Disable Widgets:** Taskbar Settings -> Widgets -> Off.
* **Windows 11:** Required for rounded corners.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- PanelWidth: 300
  $name: Panel Width
- PanelHeight: 48
  $name: Panel Height
- FontSize: 11
  $name: Font Size
- ButtonScale: "1.0"
  $name: Button Scale (1.0 = Normal, 2.0 = 4K)
- HideFullscreen: false
  $name: Hide when Fullscreen
- IdleTimeout: 0
  $name: Auto-hide when paused (Seconds). Set 0 to disable.
- OffsetX: 12
  $name: X Offset
- OffsetY: 0
  $name: Y Offset
- AutoTheme: true
  $name: Auto Theme
- TextColor: "0xFFFFFF"
  $name: Manual Text Color (Hex)
- BgOpacity: 0
  $name: Acrylic Tint Opacity (0-255). Keep 0 for pure glass.
- EnableSourcePicker: true
  $name: Right-click playback source picker
  $description: Right-click the widget to choose which media app the panel follows.
*/
// ==/WindhawkModSettings==

#include <windows.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <gdiplus.h>
#include <shcore.h>
#include <string>
#include <vector>
#include <map>
#include <utility>
#include <atomic>
#include <thread>
#include <mutex>
#include <cwctype>
#include <cstring>
#include <cstdio>

// WinRT
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>

using namespace Gdiplus;
using namespace std;
using namespace winrt;
using namespace Windows::Media::Control;
using namespace Windows::Storage::Streams;

// --- Constants ---
const WCHAR* FONT_NAME = L"Segoe UI Variable Display";
const WCHAR* MENU_CLASS_NAME = L"WindhawkMusicLounge_Menu_GSMTC";

// --- DWM API ---
typedef enum _WINDOWCOMPOSITIONATTRIB { WCA_ACCENT_POLICY = 19 } WINDOWCOMPOSITIONATTRIB;
typedef enum _ACCENT_STATE {
    ACCENT_DISABLED = 0,
    ACCENT_ENABLE_BLURBEHIND = 3,
    ACCENT_ENABLE_ACRYLICBLURBEHIND = 4,
    ACCENT_INVALID_STATE = 5
} ACCENT_STATE;
typedef struct _ACCENT_POLICY {
    ACCENT_STATE AccentState;
    DWORD AccentFlags;
    DWORD GradientColor;
    DWORD AnimationId;
} ACCENT_POLICY;
typedef struct _WINDOWCOMPOSITIONATTRIBDATA {
    WINDOWCOMPOSITIONATTRIB Attribute;
    PVOID Data;
    SIZE_T SizeOfData;
} WINDOWCOMPOSITIONATTRIBDATA;
typedef BOOL(WINAPI* pSetWindowCompositionAttribute)(HWND, WINDOWCOMPOSITIONATTRIBDATA*);

// --- Z-Band API ---
enum ZBID {
    ZBID_DEFAULT = 0,
    ZBID_DESKTOP = 1,
    ZBID_UIACCESS = 2,
    ZBID_IMMERSIVE_IHM = 3,
    ZBID_IMMERSIVE_NOTIFICATION = 4,
    ZBID_IMMERSIVE_APPCHROME = 5,
    ZBID_IMMERSIVE_MOGO = 6,
    ZBID_IMMERSIVE_EDGY = 7,
    ZBID_IMMERSIVE_INACTIVEMOBODY = 8,
    ZBID_IMMERSIVE_INACTIVEDOCK = 9,
    ZBID_IMMERSIVE_ACTIVEMOBODY = 10,
    ZBID_IMMERSIVE_ACTIVEDOCK = 11,
    ZBID_IMMERSIVE_BACKGROUND = 12,
    ZBID_IMMERSIVE_SEARCH = 13,
    ZBID_GENUINE_WINDOWS = 14,
    ZBID_IMMERSIVE_RESTRICTED = 15,
    ZBID_SYSTEM_TOOLS = 16,
    ZBID_LOCK = 17,
    ZBID_ABOVELOCK_UX = 18,
};

typedef HWND(WINAPI* pCreateWindowInBand)(
    DWORD dwExStyle,
    LPCWSTR lpClassName,
    LPCWSTR lpWindowName,
    DWORD dwStyle,
    int x,
    int y,
    int nWidth,
    int nHeight,
    HWND hWndParent,
    HMENU hMenu,
    HINSTANCE hInstance,
    LPVOID lpParam,
    DWORD dwBand
);

// --- Configurable State ---
struct ModSettings {
    int width = 300;
    int height = 48;
    int fontSize = 11;
    double buttonScale = 1.0;
    bool hideFullscreen = false;
    int idleTimeout = 0;
    int offsetX = 12;
    int offsetY = 0;
    bool autoTheme = true;
    DWORD manualTextColor = 0xFFFFFFFF;
    int bgOpacity = 0;
    bool enableSourcePicker = true;
} g_Settings;

// --- Global State ---
HWND g_hMediaWindow = NULL;
bool g_Running = true;
int g_HoverState = 0;
HWINEVENTHOOK g_TaskbarHook = nullptr;
UINT g_TaskbarCreatedMsg = RegisterWindowMessage(L"TaskbarCreated");
pCreateWindowInBand g_CreateWindowInBand = nullptr;

// Idle Tracking
int g_IdleSecondsCounter = 0;
bool g_IsHiddenByIdle = false;

// Data Model
struct MediaState {
    wstring title = L"Waiting for media...";
    wstring artist = L"";
    bool isPlaying = false;
    bool hasMedia = false;
    Bitmap* albumArt = nullptr;
    mutex lock;
} g_MediaState;

// Animation
int g_ScrollOffset = 0;
int g_TextWidth = 0;
bool g_IsScrolling = false;
int g_ScrollWait = 60;

// --- Fork: pinned playback source (persisted per user) ---
const WCHAR* PIN_REG_PATH = L"Software\\TaskbarMusicLoungePicker";
const WCHAR* PIN_REG_VALUE = L"PinnedSource";

mutex g_PinLock;
wstring g_PinnedSource;

void SavePinnedSourceToRegistry(const wstring& aumid) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, PIN_REG_PATH, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return;
    }
    if (aumid.empty()) {
        RegDeleteValueW(key, PIN_REG_VALUE);
    } else {
        RegSetValueExW(key, PIN_REG_VALUE, 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(aumid.c_str()),
                       (DWORD)((aumid.size() + 1) * sizeof(WCHAR)));
    }
    RegCloseKey(key);
}

void LoadPinnedSourceFromRegistry() {
    WCHAR buffer[512] = {};
    DWORD size = sizeof(buffer);
    wstring aumid;
    if (RegGetValueW(HKEY_CURRENT_USER, PIN_REG_PATH, PIN_REG_VALUE, RRF_RT_REG_SZ,
                     nullptr, buffer, &size) == ERROR_SUCCESS) {
        aumid = buffer;
    }
    lock_guard<mutex> guard(g_PinLock);
    g_PinnedSource = aumid;
}

wstring GetPinnedSource() {
    lock_guard<mutex> guard(g_PinLock);
    return g_PinnedSource;
}

void SetPinnedSource(const wstring& aumid) {
    {
        lock_guard<mutex> guard(g_PinLock);
        g_PinnedSource = aumid;
    }
    SavePinnedSourceToRegistry(aumid);
}

// --- Settings ---
void LoadSettings() {
    g_Settings.width = Wh_GetIntSetting(L"PanelWidth");
    g_Settings.height = Wh_GetIntSetting(L"PanelHeight");
    g_Settings.fontSize = Wh_GetIntSetting(L"FontSize");
    g_Settings.offsetX = Wh_GetIntSetting(L"OffsetX");
    g_Settings.offsetY = Wh_GetIntSetting(L"OffsetY");
    g_Settings.autoTheme = Wh_GetIntSetting(L"AutoTheme") != 0;

    PCWSTR scaleStr = Wh_GetStringSetting(L"ButtonScale");
    if (scaleStr) {
        g_Settings.buttonScale = _wtof(scaleStr);
        Wh_FreeStringSetting(scaleStr);
    } else {
        g_Settings.buttonScale = 1.0;
    }
    if (g_Settings.buttonScale < 0.5) g_Settings.buttonScale = 0.5;
    if (g_Settings.buttonScale > 4.0) g_Settings.buttonScale = 4.0;

    g_Settings.hideFullscreen = Wh_GetIntSetting(L"HideFullscreen") != 0;
    g_Settings.idleTimeout = Wh_GetIntSetting(L"IdleTimeout");

    PCWSTR textHex = Wh_GetStringSetting(L"TextColor");
    DWORD textRGB = 0xFFFFFF;
    if (textHex) {
        if (wcslen(textHex) > 0) textRGB = wcstoul(textHex, nullptr, 16);
        Wh_FreeStringSetting(textHex);
    }
    g_Settings.manualTextColor = 0xFF000000 | textRGB;

    g_Settings.bgOpacity = Wh_GetIntSetting(L"BgOpacity");
    if (g_Settings.bgOpacity < 0) g_Settings.bgOpacity = 0;
    if (g_Settings.bgOpacity > 255) g_Settings.bgOpacity = 255;

    g_Settings.enableSourcePicker = Wh_GetIntSetting(L"EnableSourcePicker") != 0;

    if (g_Settings.width < 100) g_Settings.width = 300;
    if (g_Settings.height < 24) g_Settings.height = 48;

    LoadPinnedSourceFromRegistry();
}

// --- WinRT / GSMTC ---
GlobalSystemMediaTransportControlsSessionManager g_SessionManager = nullptr;

// The session the panel currently shows; media commands are routed to it.
GlobalSystemMediaTransportControlsSession g_ActiveSession = nullptr;

Bitmap* StreamToBitmap(IRandomAccessStreamWithContentType const& stream) {
    if (!stream) return nullptr;
    IStream* nativeStream = nullptr;
    if (SUCCEEDED(CreateStreamOverRandomAccessStream(reinterpret_cast<IUnknown*>(winrt::get_abi(stream)), IID_PPV_ARGS(&nativeStream)))) {
        Bitmap* bmp = Bitmap::FromStream(nativeStream);
        nativeStream->Release();
        if (bmp && bmp->GetLastStatus() == Ok) return bmp;
        delete bmp;
    }
    return nullptr;
}

// --- Fork: session helpers ---
vector<GlobalSystemMediaTransportControlsSession> SnapshotSessions() {
    vector<GlobalSystemMediaTransportControlsSession> sessions;
    if (!g_SessionManager) return sessions;
    try {
        for (auto const& s : g_SessionManager.GetSessions()) {
            sessions.push_back(s);
        }
    } catch (...) {
    }
    return sessions;
}

wstring SessionAumid(GlobalSystemMediaTransportControlsSession const& session) {
    try {
        return wstring(session.SourceAppUserModelId().c_str());
    } catch (...) {
        return wstring();
    }
}

bool SessionIsPlaying(GlobalSystemMediaTransportControlsSession const& session) {
    try {
        auto info = session.GetPlaybackInfo();
        return info && info.PlaybackStatus() ==
                           GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;
    } catch (...) {
        return false;
    }
}

// Resolution order: pinned source (even when paused), then anything playing,
// then whatever Windows reports as the current session.
GlobalSystemMediaTransportControlsSession PickSession() {
    auto sessions = SnapshotSessions();
    if (sessions.empty()) return nullptr;

    wstring pinned = GetPinnedSource();
    if (!pinned.empty()) {
        for (auto const& s : sessions) {
            if (_wcsicmp(SessionAumid(s).c_str(), pinned.c_str()) == 0) {
                return s;
            }
        }
        // Pinned app is gone: fall through to the automatic behaviour.
    }

    for (auto const& s : sessions) {
        if (SessionIsPlaying(s)) return s;
    }

    try {
        return g_SessionManager.GetCurrentSession();
    } catch (...) {
        return nullptr;
    }
}

void UpdateMediaInfo() {
    try {
        if (!g_SessionManager) {
            g_SessionManager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
        }
        if (!g_SessionManager) return;

        auto session = PickSession();

        if (session) {
            auto props = session.TryGetMediaPropertiesAsync().get();
            auto info = session.GetPlaybackInfo();
            g_ActiveSession = session;

            lock_guard<mutex> guard(g_MediaState.lock);

            wstring newTitle = props ? wstring(props.Title().c_str()) : wstring();
            if (newTitle != g_MediaState.title || g_MediaState.albumArt == nullptr) {
                if (g_MediaState.albumArt) { delete g_MediaState.albumArt; g_MediaState.albumArt = nullptr; }
                if (props) {
                    auto thumbRef = props.Thumbnail();
                    if (thumbRef) {
                        auto stream = thumbRef.OpenReadAsync().get();
                        g_MediaState.albumArt = StreamToBitmap(stream);
                    }
                }
            }
            g_MediaState.title = newTitle;
            g_MediaState.artist = props ? wstring(props.Artist().c_str()) : wstring();
            g_MediaState.isPlaying = info && info.PlaybackStatus() ==
                                                  GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;
            g_MediaState.hasMedia = true;
        } else {
            g_ActiveSession = nullptr;
            lock_guard<mutex> guard(g_MediaState.lock);
            g_MediaState.hasMedia = false;
            g_MediaState.title = L"No Media";
            g_MediaState.artist = L"";
            if (g_MediaState.albumArt) { delete g_MediaState.albumArt; g_MediaState.albumArt = nullptr; }
        }
    } catch (...) {
        g_ActiveSession = nullptr;
        lock_guard<mutex> guard(g_MediaState.lock);
        g_MediaState.hasMedia = false;
    }
}

void SendMediaCommand(int cmd) {
    try {
        // Fork: commands go to the displayed/pinned session, not to the
        // system-wide "current" one.
        auto session = g_ActiveSession;
        if (!session) {
            if (!g_SessionManager) return;
            session = g_SessionManager.GetCurrentSession();
        }
        if (session) {
            if (cmd == 1) session.TrySkipPreviousAsync();
            else if (cmd == 2) session.TryTogglePlayPauseAsync();
            else if (cmd == 3) session.TrySkipNextAsync();
        }
    } catch (...) {}
}

// --- Visuals ---
bool IsSystemLightMode() {
    DWORD value = 0; DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"SystemUsesLightTheme", RRF_RT_DWORD, nullptr, &value, &size) == ERROR_SUCCESS) {
        return value != 0;
    }
    return false;
}

DWORD GetCurrentTextColor() {
    if (g_Settings.autoTheme) return IsSystemLightMode() ? 0xFF000000 : 0xFFFFFFFF;
    return g_Settings.manualTextColor;
}

DWORD GetAccentColorRGB() {
    DWORD value = 0; DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"ColorizationColor", RRF_RT_DWORD, nullptr, &value, &size) == ERROR_SUCCESS) {
        return value & 0x00FFFFFF;
    }
    return 0x0078D4;
}

void UpdateAppearance(HWND hwnd) {
    DWM_WINDOW_CORNER_PREFERENCE preference = DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &preference, sizeof(preference));

    HMODULE hUser = GetModuleHandle(L"user32.dll");
    if (hUser) {
        auto SetComp = (pSetWindowCompositionAttribute)GetProcAddress(hUser, "SetWindowCompositionAttribute");
        if (SetComp) {
            DWORD tint = 0;
            if (g_Settings.autoTheme) {
                tint = IsSystemLightMode() ? 0x40FFFFFF : 0x40000000;
            } else {
                tint = (g_Settings.bgOpacity << 24) | (0xFFFFFF);
            }
            ACCENT_POLICY policy = { ACCENT_ENABLE_ACRYLICBLURBEHIND, 0, tint, 0 };
            WINDOWCOMPOSITIONATTRIBDATA data = { WCA_ACCENT_POLICY, &policy, sizeof(ACCENT_POLICY) };
            SetComp(hwnd, &data);
        }
    }
}

void AddRoundedRect(GraphicsPath& path, int x, int y, int w, int h, int r) {
    int d = r * 2;
    if (d > h) d = h;
    if (d > w) d = w;
    if (d <= 0) {
        path.AddRectangle(Rect(x, y, w, h));
        path.CloseFigure();
        return;
    }
    path.AddArc(x, y, d, d, 180, 90);
    path.AddArc(x + w - d, y, d, d, 270, 90);
    path.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    path.AddArc(x, y + h - d, d, d, 90, 90);
    path.CloseFigure();
}

void DrawMediaPanel(HDC hdc, int width, int height) {
    Graphics graphics(hdc);
    graphics.SetSmoothingMode(SmoothingModeAntiAlias);
    graphics.SetTextRenderingHint(TextRenderingHintAntiAlias);
    graphics.Clear(Color(0, 0, 0, 0));

    Color mainColor{GetCurrentTextColor()};

    MediaState state;
    {
        lock_guard<mutex> guard(g_MediaState.lock);
        state.title = g_MediaState.title;
        state.artist = g_MediaState.artist;
        state.albumArt = g_MediaState.albumArt ? g_MediaState.albumArt->Clone() : nullptr;
        state.hasMedia = g_MediaState.hasMedia;
        state.isPlaying = g_MediaState.isPlaying;
    }

    // 1. Album Art (Rounded)
    int artSize = height - 12;
    int artX = 6;
    int artY = 6;

    GraphicsPath path;
    AddRoundedRect(path, artX, artY, artSize, artSize, 8);

    if (state.albumArt) {
        graphics.SetClip(&path);
        graphics.DrawImage(state.albumArt, artX, artY, artSize, artSize);
        graphics.ResetClip();
        delete state.albumArt;
    } else {
        SolidBrush placeBrush{Color(40, 128, 128, 128)};
        graphics.FillPath(&placeBrush, &path);
    }

    // 2. Controls (Scaled)
    double scale = g_Settings.buttonScale;
    int startControlX = artX + artSize + (int)(12 * scale);
    int controlY = height / 2;

    SolidBrush iconBrush{mainColor};
    SolidBrush hoverBrush{Color(255, mainColor.GetRed(), mainColor.GetGreen(), mainColor.GetBlue())};
    SolidBrush activeBg{Color(40, mainColor.GetRed(), mainColor.GetGreen(), mainColor.GetBlue())};

    float circleR = 12.0f * (float)scale;
    float iconW = 8.0f * (float)scale;
    float iconH = 12.0f * (float)scale;
    float gap = 28.0f * (float)scale;

    // Prev
    float pX = (float)startControlX;
    if (g_HoverState == 1) graphics.FillEllipse(&activeBg, pX - circleR, (float)controlY - circleR, circleR*2, circleR*2);
    PointF prevPts[3] = { PointF(pX + iconW, (float)controlY - (iconH/2)), PointF(pX + iconW, (float)controlY + (iconH/2)), PointF(pX, (float)controlY) };
    graphics.FillPolygon(g_HoverState == 1 ? &hoverBrush : &iconBrush, prevPts, 3);
    graphics.FillRectangle(g_HoverState == 1 ? &hoverBrush : &iconBrush, pX, (float)controlY - (iconH/2), 2.0f * (float)scale, iconH);

    // Play/Pause
    float plX = pX + gap;
    if (g_HoverState == 2) graphics.FillEllipse(&activeBg, plX - circleR, (float)controlY - circleR, circleR*2, circleR*2);
    if (state.isPlaying) {
        float barW = 3.0f * (float)scale;
        float barH = 14.0f * (float)scale;
        graphics.FillRectangle(g_HoverState == 2 ? &hoverBrush : &iconBrush, plX - (barW + 1), (float)controlY - (barH/2), barW, barH);
        graphics.FillRectangle(g_HoverState == 2 ? &hoverBrush : &iconBrush, plX + 1, (float)controlY - (barH/2), barW, barH);
    } else {
        float playW = 10.0f * (float)scale;
        float playH = 16.0f * (float)scale;
        PointF playPts[3] = { PointF(plX - (playW/2), (float)controlY - (playH/2)), PointF(plX - (playW/2), (float)controlY + (playH/2)), PointF(plX + (playW/2), (float)controlY) };
        graphics.FillPolygon(g_HoverState == 2 ? &hoverBrush : &iconBrush, playPts, 3);
    }

    // Next
    float nX = plX + gap;
    if (g_HoverState == 3) graphics.FillEllipse(&activeBg, nX - circleR, (float)controlY - circleR, circleR*2, circleR*2);
    PointF nextPts[3] = { PointF(nX - iconW, (float)controlY - (iconH/2)), PointF(nX - iconW, (float)controlY + (iconH/2)), PointF(nX, (float)controlY) };
    graphics.FillPolygon(g_HoverState == 3 ? &hoverBrush : &iconBrush, nextPts, 3);
    graphics.FillRectangle(g_HoverState == 3 ? &hoverBrush : &iconBrush, nX, (float)controlY - (iconH/2), 2.0f * (float)scale, iconH);

    // 3. Text
    int textX = (int)(nX + (20 * scale));
    int textMaxW = width - textX - 10;

    wstring fullText = state.title;
    if (!state.artist.empty()) fullText += L" • " + state.artist;

    FontFamily fontFamily(FONT_NAME, nullptr);
    Font font(&fontFamily, (REAL)g_Settings.fontSize, FontStyleBold, UnitPixel);
    SolidBrush textBrush{mainColor};

    RectF layoutRect(0, 0, 2000, 100);
    RectF boundRect;
    graphics.MeasureString(fullText.c_str(), -1, &font, layoutRect, &boundRect);
    g_TextWidth = (int)boundRect.Width;

    Region textClip(Rect(textX, 0, textMaxW, height));
    graphics.SetClip(&textClip);

    float textY = (height - boundRect.Height) / 2.0f;

    if (g_TextWidth > textMaxW) {
        g_IsScrolling = true;
        float drawX = (float)(textX - g_ScrollOffset);
        graphics.DrawString(fullText.c_str(), -1, &font, PointF(drawX, textY), &textBrush);
        if (drawX + g_TextWidth < width) {
             graphics.DrawString(fullText.c_str(), -1, &font, PointF(drawX + g_TextWidth + 40, textY), &textBrush);
        }
    } else {
        g_IsScrolling = false;
        g_ScrollOffset = 0;
        graphics.DrawString(fullText.c_str(), -1, &font, PointF((float)textX, textY), &textBrush);
    }
}

////////////////////////////////////////////////////////////////////////////////
// Fork: playback source picker
////////////////////////////////////////////////////////////////////////////////

// --- App name / icon resolution (shell:AppsFolder) ---
struct SourceVisual {
    wstring name;
    Bitmap* icon = nullptr;
};

map<wstring, SourceVisual> g_VisualCache;

// Keys are lower-case AUMIDs; values are the names shown in the picker.
const struct { const WCHAR* aumid; const WCHAR* name; } kKnownApps[] = {
    { L"spotify.exe", L"Spotify" },
    { L"spotify", L"Spotify" },
    { L"spotifyab.spotifymusic_zpdnekdrzrea0!spotify", L"Spotify" },
    { L"chrome", L"Chrome" },
    { L"chrome.exe", L"Chrome" },
    { L"msedge.exe", L"Edge" },
    { L"microsoft.microsoftedge_stable__8wekyb3d8bbwe!msedge", L"Edge" },
    { L"firefox.exe", L"Firefox" },
    { L"brave.exe", L"Brave" },
    { L"opera.exe", L"Opera" },
    { L"vivaldi.exe", L"Vivaldi" },
    { L"vlc.exe", L"VLC" },
    { L"aimp.exe", L"AIMP" },
    { L"wmplayer.exe", L"Windows Media Player" },
    { L"foobar2000.exe", L"foobar2000" },
    { L"musicbee.exe", L"MusicBee" },
    { L"tidal.exe", L"TIDAL" },
    { L"deezer.exe", L"Deezer" },
    { L"music.ui.exe", L"Media Player" },
    { L"microsoft.zunemusic_8wekyb3d8bbwe!microsoft.zunemusic", L"Media Player" },
    { L"microsoft.zunevideo_8wekyb3d8bbwe!microsoft.zunevideo", L"Movies & TV" },
};

wstring ToLowerWstring(wstring value) {
    for (auto& c : value) c = (WCHAR)towlower(c);
    return value;
}

wstring PrettifyAumid(const wstring& aumid) {
    wstring s = aumid;
    size_t bang = s.find(L'!');
    if (bang != wstring::npos) s = s.substr(0, bang);
    size_t underscore = s.find(L'_');
    if (underscore != wstring::npos && underscore > 0) s = s.substr(0, underscore);

    if (s.size() > 4 && _wcsicmp(s.c_str() + s.size() - 4, L".exe") == 0) {
        s = s.substr(0, s.size() - 4);
    } else {
        size_t dot = s.rfind(L'.');
        if (dot != wstring::npos && dot + 1 < s.size()) s = s.substr(dot + 1);
    }

    wstring pretty;
    for (size_t i = 0; i < s.size(); i++) {
        WCHAR c = s[i];
        if (i > 0 && iswupper(c) && !iswupper(s[i - 1]) && s[i - 1] != L' ') pretty += L' ';
        pretty += c;
    }
    if (!pretty.empty()) pretty[0] = (WCHAR)towupper(pretty[0]);
    return pretty.empty() ? aumid : pretty;
}

// Converts a shell 32bpp bitmap into a GDI+ bitmap that keeps its alpha.
Bitmap* BitmapFromShellHBitmap(HBITMAP hbmp) {
    if (!hbmp) return nullptr;

    BITMAP bm = {};
    if (!GetObject(hbmp, sizeof(bm), &bm) || bm.bmWidth <= 0 || bm.bmHeight <= 0) return nullptr;

    int w = bm.bmWidth;
    int h = bm.bmHeight;

    if (bm.bmBitsPixel != 32) {
        Bitmap* fallback = Bitmap::FromHBITMAP(hbmp, nullptr);
        if (fallback && fallback->GetLastStatus() == Ok) return fallback;
        delete fallback;
        return nullptr;
    }

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    vector<BYTE> pixels((size_t)w * (size_t)h * 4, 0);
    HDC screenDC = GetDC(nullptr);
    int lines = GetDIBits(screenDC, hbmp, 0, (UINT)h, pixels.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, screenDC);
    if (lines == 0) return nullptr;

    bool hasAlpha = false;
    for (size_t i = 3; i < pixels.size(); i += 4) {
        if (pixels[i] != 0) { hasAlpha = true; break; }
    }
    if (!hasAlpha) {
        Bitmap* fallback = Bitmap::FromHBITMAP(hbmp, nullptr);
        if (fallback && fallback->GetLastStatus() == Ok) return fallback;
        delete fallback;
        return nullptr;
    }

    for (size_t i = 0; i < pixels.size(); i += 4) {
        BYTE alpha = pixels[i + 3];
        if (alpha == 255) continue;
        pixels[i]     = (BYTE)((pixels[i]     * alpha) / 255);
        pixels[i + 1] = (BYTE)((pixels[i + 1] * alpha) / 255);
        pixels[i + 2] = (BYTE)((pixels[i + 2] * alpha) / 255);
    }

    Bitmap* bmp = new Bitmap(w, h, PixelFormat32bppPARGB);
    if (!bmp) return nullptr;
    if (bmp->GetLastStatus() != Ok) { delete bmp; return nullptr; }

    Rect rect(0, 0, w, h);
    BitmapData bd = {};
    if (bmp->LockBits(&rect, ImageLockModeWrite, PixelFormat32bppPARGB, &bd) != Ok) {
        delete bmp;
        return nullptr;
    }
    for (int y = 0; y < h; y++) {
        ptrdiff_t rowOffset = (bd.Stride >= 0)
                                  ? (ptrdiff_t)y * bd.Stride
                                  : (ptrdiff_t)(h - 1 - y) * (-(ptrdiff_t)bd.Stride);
        memcpy((BYTE*)bd.Scan0 + rowOffset, pixels.data() + (size_t)y * (size_t)w * 4, (size_t)w * 4);
    }
    bmp->UnlockBits(&bd);
    return bmp;
}

Bitmap* ResolveSourceIcon(const wstring& aumid) {
    if (aumid.empty()) return nullptr;

    wstring path = L"shell:AppsFolder\\" + aumid;
    IShellItemImageFactory* factory = nullptr;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&factory))) || !factory) {
        return nullptr;
    }

    SIZE requested = { 32, 32 };
    HBITMAP hbmp = nullptr;
    Bitmap* result = nullptr;
    if (SUCCEEDED(factory->GetImage(requested, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &hbmp)) && hbmp) {
        result = BitmapFromShellHBitmap(hbmp);
        DeleteObject(hbmp);
    }
    factory->Release();
    return result;
}

wstring ResolveSourceName(const wstring& aumid) {
    if (aumid.empty()) return L"Unknown app";

    wstring lowered = ToLowerWstring(aumid);
    for (auto const& known : kKnownApps) {
        if (lowered == known.aumid) return known.name;
    }

    wstring name;
    wstring path = L"shell:AppsFolder\\" + aumid;
    IShellItem* item = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item))) && item) {
        LPWSTR display = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &display)) && display) {
            name = display;
            CoTaskMemFree(display);
        }
        item->Release();
    }

    if (name.empty() || _wcsicmp(name.c_str(), aumid.c_str()) == 0) {
        name = PrettifyAumid(aumid);
    }
    return name;
}

SourceVisual* GetSourceVisual(const wstring& aumid) {
    auto it = g_VisualCache.find(aumid);
    if (it != g_VisualCache.end()) return &it->second;

    SourceVisual visual;
    visual.name = ResolveSourceName(aumid);
    visual.icon = ResolveSourceIcon(aumid);
    auto inserted = g_VisualCache.emplace(aumid, visual);
    return &inserted.first->second;
}

void ClearSourceVisualCache() {
    for (auto& entry : g_VisualCache) {
        delete entry.second.icon;
        entry.second.icon = nullptr;
    }
    g_VisualCache.clear();
}

// --- Picker popup state ---
#define IDT_MENU_ARM 1010

struct MenuRow {
    enum Kind { kHeader, kItem, kSeparator, kInfo } kind = kItem;
    wstring label;
    wstring aumid;
    bool playing = false;
    bool checked = false;
    bool clickable = false;
    Bitmap* icon = nullptr;
    int y = 0;
    int h = 0;
};

vector<MenuRow> g_MenuRows;
int g_MenuHover = -1;
bool g_MenuDone = false;
bool g_MenuArmed = false;
bool g_MenuChosen = false;
bool g_MenuOpen = false;
wstring g_MenuChoice;
HWND g_hMenuWindow = NULL;

int g_MenuPadX = 8;
int g_MenuPadY = 6;
int g_MenuIconSize = 20;
int g_MenuGap = 8;
int g_MenuCheckW = 18;
int g_MenuStatusW = 14;

void DrawCheckMark(Graphics& graphics, float x, float y, float size, Color color) {
    Pen pen(color, 2.0f);
    pen.SetLineCap(LineCapRound, LineCapRound, DashCapRound);
    PointF pts[3] = {
        PointF(x, y + size * 0.55f),
        PointF(x + size * 0.35f, y + size * 0.92f),
        PointF(x + size, y + size * 0.08f),
    };
    graphics.DrawLines(&pen, pts, 3);
}

void DrawPlaybackGlyph(Graphics& graphics, float centerX, float centerY, bool playing, Color color) {
    SolidBrush brush(color);
    if (playing) {
        PointF pts[3] = {
            PointF(centerX - 4.0f, centerY - 5.0f),
            PointF(centerX - 4.0f, centerY + 5.0f),
            PointF(centerX + 5.0f, centerY),
        };
        graphics.FillPolygon(&brush, pts, 3);
    } else {
        graphics.FillRectangle(&brush, centerX - 3.5f, centerY - 5.0f, 2.5f, 10.0f);
        graphics.FillRectangle(&brush, centerX + 1.0f, centerY - 5.0f, 2.5f, 10.0f);
    }
}

void DrawSourceMenu(HDC hdc, int width, int height) {
    Graphics graphics(hdc);
    graphics.SetSmoothingMode(SmoothingModeAntiAlias);
    graphics.SetTextRenderingHint(TextRenderingHintAntiAlias);
    graphics.Clear(Color(0, 0, 0, 0));

    // Card contrast follows the text color, so a manual Text Color keeps the
    // menu readable even when it disagrees with the system theme.
    DWORD textRGB = GetCurrentTextColor() & 0x00FFFFFF;
    int luminance = (((textRGB >> 16) & 0xFF) + ((textRGB >> 8) & 0xFF) + (textRGB & 0xFF)) / 3;
    bool light = luminance < 128;
    Color mainColor{GetCurrentTextColor()};
    Color mutedColor = light ? Color(170, 0, 0, 0) : Color(170, 255, 255, 255);
    DWORD accentRGB = GetAccentColorRGB();
    Color accentColor(255, (BYTE)((accentRGB >> 16) & 0xFF), (BYTE)((accentRGB >> 8) & 0xFF), (BYTE)(accentRGB & 0xFF));

    // Card: near-opaque so the panel stays readable with and without acrylic.
    GraphicsPath card;
    AddRoundedRect(card, 0, 0, width, height, 8);
    SolidBrush cardBrush{light ? Color(214, 249, 249, 249) : Color(218, 26, 26, 28)};
    graphics.FillPath(&cardBrush, &card);

    FontFamily fontFamily(FONT_NAME, nullptr);
    Font itemFont(&fontFamily, (REAL)g_Settings.fontSize, FontStyleRegular, UnitPixel);
    int headerSize = g_Settings.fontSize - 2;
    if (headerSize < 9) headerSize = 9;
    Font headerFont(&fontFamily, (REAL)headerSize, FontStyleRegular, UnitPixel);

    SolidBrush mainBrush{mainColor};
    SolidBrush mutedBrush{mutedColor};

    for (size_t i = 0; i < g_MenuRows.size(); i++) {
        MenuRow& row = g_MenuRows[i];
        int index = (int)i;

        if (row.kind == MenuRow::kSeparator) {
            Pen separator{light ? Color(46, 0, 0, 0) : Color(46, 255, 255, 255), 1.0f};
            graphics.DrawLine(&separator, (REAL)g_MenuPadX, (REAL)(row.y + row.h / 2),
                              (REAL)(width - g_MenuPadX), (REAL)(row.y + row.h / 2));
            continue;
        }

        bool hovered = row.clickable && index == g_MenuHover;
        if (hovered) {
            GraphicsPath highlight;
            AddRoundedRect(highlight, g_MenuPadX - 2, row.y, width - (g_MenuPadX - 2) * 2, row.h, 5);
            SolidBrush highlightBrush{light ? Color(26, 0, 0, 0) : Color(32, 255, 255, 255)};
            graphics.FillPath(&highlightBrush, &highlight);
        }

        if (row.kind == MenuRow::kHeader) {
            StringFormat headerFormat;
            headerFormat.SetFormatFlags(StringFormatFlagsNoWrap);
            headerFormat.SetTrimming(StringTrimmingEllipsisCharacter);
            RectF headerRect((REAL)(g_MenuPadX + 4), (REAL)row.y, (REAL)(width - g_MenuPadX * 2), (REAL)row.h);
            graphics.DrawString(row.label.c_str(), -1, &headerFont, headerRect, &headerFormat, &mutedBrush);
            continue;
        }

        if (row.kind == MenuRow::kInfo) {
            StringFormat infoFormat;
            infoFormat.SetFormatFlags(StringFormatFlagsNoWrap);
            RectF infoRect((REAL)(g_MenuPadX + 4), (REAL)row.y, (REAL)(width - g_MenuPadX * 2), (REAL)row.h);
            graphics.DrawString(row.label.c_str(), -1, &itemFont, infoRect, &infoFormat, &mutedBrush);
            continue;
        }

        // Item row: check | icon | label | playback state
        if (row.checked) {
            float checkSize = 11.0f;
            float checkX = (float)(g_MenuPadX + 3);
            float checkY = (float)row.y + ((float)row.h - checkSize) / 2.0f - 1.0f;
            DrawCheckMark(graphics, checkX, checkY, checkSize, accentColor);
        }

        int iconX = g_MenuPadX + g_MenuCheckW;
        if (row.icon) {
            int iconY = row.y + (row.h - g_MenuIconSize) / 2;
            graphics.SetInterpolationMode(InterpolationModeHighQualityBicubic);
            graphics.DrawImage(row.icon, Rect(iconX, iconY, g_MenuIconSize, g_MenuIconSize));
        }

        int textX = iconX + g_MenuIconSize + g_MenuGap;
        int textW = width - textX - g_MenuStatusW - g_MenuPadX - 4;
        if (textW < 20) textW = 20;

        StringFormat textFormat;
        textFormat.SetFormatFlags(StringFormatFlagsNoWrap);
        textFormat.SetTrimming(StringTrimmingEllipsisCharacter);
        textFormat.SetLineAlignment(StringAlignmentCenter);
        RectF textRect((REAL)textX, (REAL)row.y, (REAL)textW, (REAL)row.h);
        graphics.DrawString(row.label.c_str(), -1, &itemFont, textRect, &textFormat, &mainBrush);

        if (row.playing) {
            float glyphCenterX = (float)(width - g_MenuPadX - g_MenuStatusW / 2 - 2);
            float glyphCenterY = (float)row.y + (float)row.h / 2.0f;
            DrawPlaybackGlyph(graphics, glyphCenterX, glyphCenterY, true, accentColor);
        }
    }
}

int HitTestMenuRow(POINT pt) {
    for (size_t i = 0; i < g_MenuRows.size(); i++) {
        MenuRow const& row = g_MenuRows[i];
        if (pt.y >= row.y && pt.y < row.y + row.h && row.clickable) {
            return (int)i;
        }
    }
    return -1;
}

LRESULT CALLBACK SourceMenuWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            UpdateAppearance(hwnd);
            return 0;

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            HDC memDC = CreateCompatibleDC(hdc);
            HBITMAP memBitmap = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
            HBITMAP oldBitmap = (HBITMAP)SelectObject(memDC, memBitmap);

            DrawSourceMenu(memDC, rc.right, rc.bottom);

            BitBlt(hdc, 0, 0, rc.right, rc.bottom, memDC, 0, 0, SRCCOPY);
            SelectObject(memDC, oldBitmap); DeleteObject(memBitmap); DeleteDC(memDC);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_MOUSEMOVE: {
            POINT pt = { (int)(short)LOWORD(lParam), (int)(short)HIWORD(lParam) };
            int index = HitTestMenuRow(pt);
            if (index != g_MenuHover) {
                g_MenuHover = index;
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }

        case WM_LBUTTONUP: {
            POINT pt = { (int)(short)LOWORD(lParam), (int)(short)HIWORD(lParam) };
            int index = HitTestMenuRow(pt);
            if (index >= 0) {
                g_MenuChosen = true;
                g_MenuChoice = g_MenuRows[index].aumid;
            }
            g_MenuDone = true;
            return 0;
        }

        case WM_RBUTTONUP:
            g_MenuDone = true;
            return 0;

        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                g_MenuDone = true;
                return 0;
            }
            break;

        case WM_ACTIVATE:
            if (LOWORD(wParam) == WA_INACTIVE && g_MenuArmed && !g_MenuDone) {
                g_MenuDone = true;
            }
            return 0;

        case WM_TIMER:
            if (wParam == IDT_MENU_ARM) {
                g_MenuArmed = true;
                KillTimer(hwnd, IDT_MENU_ARM);
                return 0;
            }
            break;

        case WM_SETTINGCHANGE:
            UpdateAppearance(hwnd);
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;

        default:
            break;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

void ShowSourceMenu(HWND owner) {
    if (g_MenuOpen || !owner) return;
    g_MenuOpen = true;

    if (!g_SessionManager) {
        try {
            g_SessionManager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
        } catch (...) {
        }
    }

    g_MenuRows.clear();
    g_MenuChoice.clear();
    g_MenuChosen = false;
    g_MenuHover = -1;
    g_MenuDone = false;
    g_MenuArmed = false;

    {
        MenuRow header;
        header.kind = MenuRow::kHeader;
        header.label = L"Playback source";
        g_MenuRows.push_back(header);

        MenuRow automatic;
        automatic.kind = MenuRow::kItem;
        automatic.label = L"Automatic";
        automatic.clickable = true;
        automatic.checked = GetPinnedSource().empty();
        g_MenuRows.push_back(automatic);
    }

    vector<pair<wstring, bool>> sources;
    for (auto const& session : SnapshotSessions()) {
        wstring aumid = SessionAumid(session);
        if (aumid.empty()) continue;

        bool duplicate = false;
        for (auto const& existing : sources) {
            if (_wcsicmp(existing.first.c_str(), aumid.c_str()) == 0) { duplicate = true; break; }
        }
        if (duplicate) continue;

        sources.push_back(make_pair(aumid, SessionIsPlaying(session)));
    }

    if (sources.empty()) {
        MenuRow empty;
        empty.kind = MenuRow::kInfo;
        empty.label = L"No media sessions";
        g_MenuRows.push_back(empty);
    } else {
        MenuRow separator;
        separator.kind = MenuRow::kSeparator;
        g_MenuRows.push_back(separator);

        wstring pinned = GetPinnedSource();
        for (auto const& source : sources) {
            MenuRow row;
            row.kind = MenuRow::kItem;
            row.clickable = true;
            row.aumid = source.first;
            row.playing = source.second;
            row.checked = !pinned.empty() && _wcsicmp(pinned.c_str(), source.first.c_str()) == 0;
            SourceVisual* visual = GetSourceVisual(source.first);
            row.label = visual->name;
            row.icon = visual->icon;
            g_MenuRows.push_back(row);
        }
    }

    // Layout
    int itemH = 10 + (int)(g_Settings.fontSize * 2.0f);
    if (itemH < 26) itemH = 26;
    int headerH = 6 + (int)(g_Settings.fontSize * 1.5f);
    if (headerH < 20) headerH = 20;
    int separatorH = 9;

    for (auto& row : g_MenuRows) {
        row.h = (row.kind == MenuRow::kHeader) ? headerH
              : (row.kind == MenuRow::kSeparator) ? separatorH
              : itemH;
    }

    // Measure the widest label
    int widest = 0;
    HDC screenDC = GetDC(nullptr);
    {
        Graphics graphics(screenDC);
        graphics.SetTextRenderingHint(TextRenderingHintAntiAlias);
        FontFamily fontFamily(FONT_NAME, nullptr);
        Font itemFont(&fontFamily, (REAL)g_Settings.fontSize, FontStyleRegular, UnitPixel);
        Font headerFont(&fontFamily, (REAL)(g_Settings.fontSize - 2 > 8 ? g_Settings.fontSize - 2 : 9), FontStyleRegular, UnitPixel);
        for (auto& row : g_MenuRows) {
            if (row.kind == MenuRow::kSeparator || row.label.empty()) continue;
            Font* font = (row.kind == MenuRow::kHeader) ? &headerFont : &itemFont;
            RectF layoutRect(0, 0, 2000, 100);
            RectF boundRect;
            graphics.MeasureString(row.label.c_str(), -1, font, layoutRect, &boundRect);
            int width = (int)boundRect.Width + 6;
            if (row.kind == MenuRow::kHeader) width += 8;
            if (width > widest) widest = width;
        }
    }
    ReleaseDC(nullptr, screenDC);

    int menuWidth = g_MenuPadX * 2 + g_MenuCheckW + g_MenuIconSize + g_MenuGap + widest + g_MenuGap + g_MenuStatusW;
    int minWidth = 180;
    int maxWidth = 420;
    if (menuWidth < minWidth) menuWidth = minWidth;
    if (menuWidth > maxWidth) menuWidth = maxWidth;

    int menuHeight = g_MenuPadY * 2;
    int cursorY = g_MenuPadY;
    for (auto& row : g_MenuRows) {
        row.y = cursorY;
        cursorY += row.h;
    }
    menuHeight += cursorY - g_MenuPadY;

    // Position: above the panel when the taskbar sits at the bottom.
    RECT panelRect = {};
    GetWindowRect(owner, &panelRect);
    MONITORINFO monitorInfo = { sizeof(MONITORINFO) };
    HMONITOR monitor = MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST);
    GetMonitorInfo(monitor, &monitorInfo);

    RECT work = monitorInfo.rcWork;
    int x = panelRect.left;
    bool panelInTopHalf = (panelRect.top < work.top + (work.bottom - work.top) / 2);
    int y = panelInTopHalf ? (panelRect.bottom + 6) : (panelRect.top - menuHeight - 6);

    if (x + menuWidth > work.right) x = work.right - menuWidth - 4;
    if (x < work.left) x = work.left + 4;
    if (y + menuHeight > work.bottom) y = work.bottom - menuHeight - 4;
    if (y < work.top) y = work.top + 4;

    HINSTANCE instance = GetModuleHandle(NULL);
    g_hMenuWindow = nullptr;
    if (g_CreateWindowInBand) {
        g_hMenuWindow = g_CreateWindowInBand(
            WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
            MENU_CLASS_NAME, L"MusicLoungeSourcePicker",
            WS_POPUP,
            x, y, menuWidth, menuHeight,
            NULL, NULL, instance, NULL,
            ZBID_IMMERSIVE_NOTIFICATION);
    }
    if (!g_hMenuWindow) {
        g_hMenuWindow = CreateWindowEx(
            WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
            MENU_CLASS_NAME, L"MusicLoungeSourcePicker",
            WS_POPUP,
            x, y, menuWidth, menuHeight,
            NULL, NULL, instance, NULL);
    }

    if (!g_hMenuWindow) {
        Wh_Log(L"Failed to create source picker window");
        g_MenuRows.clear();
        g_MenuOpen = false;
        return;
    }

    SetLayeredWindowAttributes(g_hMenuWindow, 0, 255, LWA_ALPHA);
    SetWindowPos(g_hMenuWindow, HWND_TOPMOST, x, y, menuWidth, menuHeight,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    ShowWindow(g_hMenuWindow, SW_SHOWNOACTIVATE);
    SetForegroundWindow(g_hMenuWindow);
    SetFocus(g_hMenuWindow);
    SetCapture(g_hMenuWindow);
    SetTimer(g_hMenuWindow, IDT_MENU_ARM, 80, nullptr);
    InvalidateRect(g_hMenuWindow, NULL, TRUE);

    MSG msg;
    while (!g_MenuDone) {
        BOOL result = GetMessage(&msg, NULL, 0, 0);
        if (result <= 0) break;
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (g_hMenuWindow) {
        HWND menuWindow = g_hMenuWindow;
        g_hMenuWindow = NULL;
        ReleaseCapture();
        DestroyWindow(menuWindow);
    }

    g_MenuRows.clear();
    g_MenuHover = -1;
    g_MenuOpen = false;

    if (g_MenuChosen) {
        SetPinnedSource(g_MenuChoice);
        UpdateMediaInfo();
        InvalidateRect(owner, NULL, FALSE);
        Wh_Log(L"Playback source set to: %s", g_MenuChoice.empty() ? L"Automatic" : g_MenuChoice.c_str());
    }
}

// --- Event Hook ---
bool IsTaskbarWindow(HWND hwnd) {
    WCHAR cls[64];
    if (!hwnd) return false;
    GetClassNameW(hwnd, cls, ARRAYSIZE(cls));
    return wcscmp(cls, L"Shell_TrayWnd") == 0;
}

void CALLBACK TaskbarEventProc(
    HWINEVENTHOOK,
    DWORD event,
    HWND hwnd,
    LONG, LONG,
    DWORD, DWORD
) {
    if (!IsTaskbarWindow(hwnd) || !g_hMediaWindow) return;
    PostMessage(g_hMediaWindow, WM_APP + 10, 0, 0);
}

// Register Event Hook scoped to Taskbar Thread
void RegisterTaskbarHook(HWND hwnd)
{
    HWND hTaskbar = FindWindow(L"Shell_TrayWnd", nullptr);
    if (hTaskbar) {
        DWORD pid = 0;
        DWORD tid = GetWindowThreadProcessId(hTaskbar, &pid);
        if (tid != 0) {
            g_TaskbarHook = SetWinEventHook(
                EVENT_OBJECT_LOCATIONCHANGE,
                EVENT_OBJECT_LOCATIONCHANGE,
                nullptr,
                TaskbarEventProc,
                pid, tid,
                WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS
            );
        }
    }
    PostMessage(hwnd, WM_APP + 10, 0, 0);
}

// --- Window Procedure ---
#define IDT_POLL_MEDIA 1001
#define IDT_ANIMATION  1002
#define APP_WM_CLOSE   WM_APP

LRESULT CALLBACK MediaWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            UpdateAppearance(hwnd);
            SetTimer(hwnd, IDT_POLL_MEDIA, 1000, NULL);
            RegisterTaskbarHook(hwnd);
            return 0;

        case WM_ERASEBKGND:
            return 1;

        case WM_CLOSE:
            return 0;

        case APP_WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            if (g_TaskbarHook) {
                UnhookWinEvent(g_TaskbarHook);
                g_TaskbarHook = nullptr;
            }
            g_SessionManager = nullptr;
            g_ActiveSession = nullptr;
            PostQuitMessage(0);
            return 0;

        case WM_SETTINGCHANGE:
            UpdateAppearance(hwnd);
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;

        case WM_TIMER:
            if (wParam == IDT_POLL_MEDIA) {
                UpdateMediaInfo();

                bool shouldHide = false;

                // 1. Check Fullscreen
                if (g_Settings.hideFullscreen) {
                    QUERY_USER_NOTIFICATION_STATE state;
                    if (SUCCEEDED(SHQueryUserNotificationState(&state))) {
                        if (state == QUNS_BUSY || state == QUNS_RUNNING_D3D_FULL_SCREEN || state == QUNS_PRESENTATION_MODE) {
                            shouldHide = true;
                        }
                    }
                }

                // 2. Check Idle Timeout
                bool isPlaying = false;
                {
                    lock_guard<mutex> guard(g_MediaState.lock);
                    isPlaying = g_MediaState.isPlaying;
                }

                if (g_Settings.idleTimeout > 0) {
                    if (isPlaying) {
                        g_IdleSecondsCounter = 0;
                        g_IsHiddenByIdle = false;
                    } else {
                        g_IdleSecondsCounter++;
                        if (g_IdleSecondsCounter >= g_Settings.idleTimeout) {
                            g_IsHiddenByIdle = true;
                        }
                    }
                } else {
                    g_IsHiddenByIdle = false;
                }

                if (g_IsHiddenByIdle) shouldHide = true;

                // 3. Final Visibility Check
                if (shouldHide && IsWindowVisible(hwnd)) {
                    ShowWindow(hwnd, SW_HIDE);
                } else if (!shouldHide && !IsWindowVisible(hwnd)) {
                    // Only restore if Taskbar is also visible
                    HWND hTaskbar = FindWindow(L"Shell_TrayWnd", nullptr);
                    if (hTaskbar && IsWindowVisible(hTaskbar)) {
                        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
                    }
                }

                InvalidateRect(hwnd, NULL, FALSE);
            }
            else if (wParam == IDT_ANIMATION) {
                if (g_IsScrolling) {
                    if (g_ScrollWait > 0) {
                        g_ScrollWait--;
                    } else {
                        g_ScrollOffset++;
                        if (g_ScrollOffset > g_TextWidth + 40) {
                            g_ScrollOffset = 0;
                            g_ScrollWait = 60;
                        }
                        InvalidateRect(hwnd, NULL, FALSE);
                    }
                } else {
                    KillTimer(hwnd, IDT_ANIMATION);
                }
            }
            return 0;

        case WM_APP + 10: {
            HWND hTaskbar = FindWindow(TEXT("Shell_TrayWnd"), nullptr);
            if (!hTaskbar) break;

            // Merged Logic: Check visibility first
            if (!IsWindowVisible(hTaskbar)) {
                if (IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_HIDE);
                return 0;
            }

            // Restore visibility if we aren't hidden by fullscreen or Idle modes
            // (The Timer loop handles fullscreen/idle hiding, this handles Taskbar hiding)
            if (!g_IsHiddenByIdle && !IsWindowVisible(hwnd)) {
                // Double check fullscreen mode isn't forcing hide
                bool gameModeHide = false;
                if (g_Settings.hideFullscreen) {
                     QUERY_USER_NOTIFICATION_STATE state;
                     if (SUCCEEDED(SHQueryUserNotificationState(&state))) {
                        if (state == QUNS_BUSY || state == QUNS_RUNNING_D3D_FULL_SCREEN || state == QUNS_PRESENTATION_MODE) gameModeHide = true;
                     }
                }
                if (!gameModeHide) ShowWindow(hwnd, SW_SHOWNOACTIVATE);
            }

            RECT rc;
            GetWindowRect(hTaskbar, &rc);

            int x = rc.left + g_Settings.offsetX;
            int taskbarHeight = rc.bottom - rc.top;
            int y = rc.top + (taskbarHeight / 2) -
            (g_Settings.height / 2) + g_Settings.offsetY;

            RECT myRc; GetWindowRect(hwnd, &myRc);
            if (myRc.left != x || myRc.top != y ||
                (myRc.right - myRc.left) != g_Settings.width ||
                (myRc.bottom - myRc.top) != g_Settings.height) {
                    SetWindowPos(
                        hwnd,
                        HWND_TOPMOST,
                        x, y,
                        g_Settings.width,
                        g_Settings.height,
                        SWP_NOACTIVATE
                    );
            }
            return 0;
        }

        case WM_MOUSEMOVE: {
            int x = LOWORD(lParam);
            int y = HIWORD(lParam);
            int artSize = g_Settings.height - 12;
            double scale = g_Settings.buttonScale;

            // Re-calculate hit targets based on scale
            int startControlX = 6 + artSize + (int)(12 * scale);
            float gap = 28.0f * (float)scale;
            float pX = (float)startControlX;
            float plX = pX + gap;
            float nX = plX + gap;
            float radius = 12.0f * (float)scale;

            int newState = 0;
            if (y > 10 && y < g_Settings.height - 10) {
                if (x >= pX - radius && x <= pX + radius) newState = 1;
                else if (x >= plX - radius && x <= plX + radius) newState = 2;
                else if (x >= nX - radius && x <= nX + radius) newState = 3;
            }

            if (newState != g_HoverState) {
                g_HoverState = newState;
                InvalidateRect(hwnd, NULL, FALSE);
            }

            TRACKMOUSEEVENT tme = { sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            return 0;
        }
        case WM_MOUSELEAVE:
            g_HoverState = 0;
            InvalidateRect(hwnd, NULL, FALSE);
            break;
        case WM_LBUTTONUP:
            if (g_HoverState > 0) SendMediaCommand(g_HoverState);
            return 0;
        case WM_RBUTTONDOWN:
            // Swallow so the right click doesn't fall through to the taskbar.
            if (g_Settings.enableSourcePicker) return 0;
            break;
        case WM_RBUTTONUP:
            if (g_Settings.enableSourcePicker) {
                g_HoverState = 0;
                ShowSourceMenu(hwnd);
                return 0;
            }
            break;
        case WM_MOUSEWHEEL: {
            short zDelta = GET_WHEEL_DELTA_WPARAM(wParam);
            keybd_event(zDelta > 0 ? VK_VOLUME_UP : VK_VOLUME_DOWN, 0, 0, 0);
            keybd_event(zDelta > 0 ? VK_VOLUME_UP : VK_VOLUME_DOWN, 0, KEYEVENTF_KEYUP, 0);
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc; GetClientRect(hwnd, &rc);
            HDC memDC = CreateCompatibleDC(hdc);
            HBITMAP memBitmap = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
            HBITMAP oldBitmap = (HBITMAP)SelectObject(memDC, memBitmap);

            DrawMediaPanel(memDC, rc.right, rc.bottom);

            if (g_IsScrolling) SetTimer(hwnd, IDT_ANIMATION, 16, NULL);

            BitBlt(hdc, 0, 0, rc.right, rc.bottom, memDC, 0, 0, SRCCOPY);
            SelectObject(memDC, oldBitmap); DeleteObject(memBitmap); DeleteDC(memDC);
            EndPaint(hwnd, &ps);
            return 0;
        }
        default:
            if (msg == g_TaskbarCreatedMsg) {
                if (g_TaskbarHook) {
                    UnhookWinEvent(g_TaskbarHook);
                    g_TaskbarHook = nullptr;
                }
                RegisterTaskbarHook(hwnd);
                return 0;
            }
            break;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// --- Main Thread ---
void MediaThread() {
    winrt::init_apartment();

    GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken;
    GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);

    WNDCLASS wc = {0};
    wc.lpfnWndProc = MediaWndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = TEXT("WindhawkMusicLounge_GSMTC");
    wc.hCursor = LoadCursor(NULL, IDC_HAND);
    RegisterClass(&wc);

    WNDCLASS menuClass = {0};
    menuClass.lpfnWndProc = SourceMenuWndProc;
    menuClass.hInstance = wc.hInstance;
    menuClass.lpszClassName = MENU_CLASS_NAME;
    menuClass.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClass(&menuClass);

    // Try to use CreateWindowInBand
    HMODULE hUser32 = GetModuleHandle(L"user32.dll");
    if (hUser32) {
        g_CreateWindowInBand = (pCreateWindowInBand)GetProcAddress(hUser32, "CreateWindowInBand");
    }

    if (g_CreateWindowInBand) {
        g_hMediaWindow = g_CreateWindowInBand(
            WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
            wc.lpszClassName, TEXT("MusicLounge"),
            WS_POPUP | WS_VISIBLE,
            0, 0, g_Settings.width, g_Settings.height,
            NULL, NULL, wc.hInstance, NULL,
            ZBID_IMMERSIVE_NOTIFICATION
        );
        if (g_hMediaWindow) {
            Wh_Log(L"Created window in ZBID_IMMERSIVE_NOTIFICATION band");
        }
    }

    if (!g_hMediaWindow) {
        Wh_Log(L"Falling back to CreateWindowEx");
        g_hMediaWindow = CreateWindowEx(
            WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
            wc.lpszClassName, TEXT("MusicLounge"),
            WS_POPUP | WS_VISIBLE,
            0, 0, g_Settings.width, g_Settings.height,
            NULL, NULL, wc.hInstance, NULL
        );
    }

    SetLayeredWindowAttributes(g_hMediaWindow, 0, 255, LWA_ALPHA);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    ClearSourceVisualCache();
    UnregisterClass(menuClass.lpszClassName, menuClass.hInstance);
    UnregisterClass(wc.lpszClassName, wc.hInstance);
    GdiplusShutdown(gdiplusToken);
    winrt::uninit_apartment();
}

std::thread* g_pMediaThread = nullptr;

// --- CALLBACKS ---
BOOL WhTool_ModInit() {
    SetCurrentProcessExplicitAppUserModelID(L"taskbar-music-lounge-picker");
    LoadSettings();
    g_Running = true;
    g_pMediaThread = new std::thread(MediaThread);
    return TRUE;
}

void WhTool_ModUninit() {
    g_Running = false;
    if (g_hMediaWindow) SendMessage(g_hMediaWindow, APP_WM_CLOSE, 0, 0);
    if (g_pMediaThread) {
        if (g_pMediaThread->joinable()) g_pMediaThread->join();
        delete g_pMediaThread;
        g_pMediaThread = nullptr;
    }
}

void WhTool_ModSettingsChanged() {
    LoadSettings();
    if (g_hMediaWindow) {
         PostMessage(g_hMediaWindow, WM_TIMER, IDT_POLL_MEDIA, 0);
         PostMessage(g_hMediaWindow, WM_SETTINGCHANGE, 0, 0);
         // Reposition/resize the window to pick up new size and offsets. This
         // path is normally only triggered by taskbar movement, so without it
         // changes to Panel Width/Height/Offset wouldn't apply until the mod is
         // reloaded.
         PostMessage(g_hMediaWindow, WM_APP + 10, 0, 0);
    }
}

////////////////////////////////////////////////////////////////////////////////
// Windhawk tool mod implementation for mods which don't need to inject to other
// processes or hook other functions. Context:
// https://github.com/ramensoftware/windhawk-mods/pull/1916
//
// The mod will load and run in a dedicated windhawk.exe process.
//
// Paste the code below as part of the mod code, and use these callbacks:
// * WhTool_ModInit
// * WhTool_ModSettingsChanged
// * WhTool_ModUninit
//
// Currently, other callbacks are not supported.

bool g_isToolModProcessLauncher;
HANDLE g_toolModProcessMutex;

void WINAPI EntryPoint_Hook() {
    Wh_Log(L">");
    ExitThread(0);
}

BOOL Wh_ModInit() {
    bool isService = false;
    bool isToolModProcess = false;
    bool isCurrentToolModProcess = false;
    int argc;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLine(), &argc);
    if (!argv) {
        Wh_Log(L"CommandLineToArgvW failed");
        return FALSE;
    }

    for (int i = 1; i < argc; i++) {
        if (wcscmp(argv[i], L"-service") == 0) {
            isService = true;
            break;
        }
    }

    for (int i = 1; i < argc - 1; i++) {
        if (wcscmp(argv[i], L"-tool-mod") == 0) {
            isToolModProcess = true;
            if (wcscmp(argv[i + 1], WH_MOD_ID) == 0) {
                isCurrentToolModProcess = true;
            }
            break;
        }
    }

    LocalFree(argv);

    if (isService) {
        return FALSE;
    }

    if (isCurrentToolModProcess) {
        g_toolModProcessMutex =
            CreateMutex(nullptr, TRUE, L"windhawk-tool-mod_" WH_MOD_ID);
        if (!g_toolModProcessMutex) {
            Wh_Log(L"CreateMutex failed");
            ExitProcess(1);
        }

        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            Wh_Log(L"Tool mod already running (%s)", WH_MOD_ID);
            ExitProcess(1);
        }

        if (!WhTool_ModInit()) {
            ExitProcess(1);
        }

        IMAGE_DOS_HEADER* dosHeader =
            (IMAGE_DOS_HEADER*)GetModuleHandle(nullptr);
        IMAGE_NT_HEADERS* ntHeaders =
            (IMAGE_NT_HEADERS*)((BYTE*)dosHeader + dosHeader->e_lfanew);

        DWORD entryPointRVA = ntHeaders->OptionalHeader.AddressOfEntryPoint;
        void* entryPoint = (BYTE*)dosHeader + entryPointRVA;

        Wh_SetFunctionHook(entryPoint, (void*)EntryPoint_Hook, nullptr);
        return TRUE;
    }

    if (isToolModProcess) {
        return FALSE;
    }

    g_isToolModProcessLauncher = true;
    return TRUE;
}

void Wh_ModAfterInit() {
    if (!g_isToolModProcessLauncher) {
        return;
    }

    WCHAR currentProcessPath[MAX_PATH];
    switch (GetModuleFileName(nullptr, currentProcessPath,
                              ARRAYSIZE(currentProcessPath))) {
        case 0:
        case ARRAYSIZE(currentProcessPath):
            Wh_Log(L"GetModuleFileName failed");
            return;
    }

    WCHAR
    commandLine[MAX_PATH + 2 +
                (sizeof(L" -tool-mod \"" WH_MOD_ID "\"") / sizeof(WCHAR)) - 1];
    swprintf_s(commandLine, L"\"%s\" -tool-mod \"%s\"", currentProcessPath,
               WH_MOD_ID);

    HMODULE kernelModule = GetModuleHandle(L"kernelbase.dll");
    if (!kernelModule) {
        kernelModule = GetModuleHandle(L"kernel32.dll");
        if (!kernelModule) {
            Wh_Log(L"No kernelbase.dll/kernel32.dll");
            return;
        }
    }

    using CreateProcessInternalW_t = BOOL(WINAPI*)(
        HANDLE hUserToken, LPCWSTR lpApplicationName, LPWSTR lpCommandLine,
        LPSECURITY_ATTRIBUTES lpProcessAttributes,
        LPSECURITY_ATTRIBUTES lpThreadAttributes, WINBOOL bInheritHandles,
        DWORD dwCreationFlags, LPVOID lpEnvironment, LPCWSTR lpCurrentDirectory,
        LPSTARTUPINFOW lpStartupInfo,
        LPPROCESS_INFORMATION lpProcessInformation,
        PHANDLE hRestrictedUserToken);
    CreateProcessInternalW_t pCreateProcessInternalW =
        (CreateProcessInternalW_t)GetProcAddress(kernelModule,
                                                 "CreateProcessInternalW");
    if (!pCreateProcessInternalW) {
        Wh_Log(L"No CreateProcessInternalW");
        return;
    }

    STARTUPINFO si{
        .cb = sizeof(STARTUPINFO),
        .dwFlags = STARTF_FORCEOFFFEEDBACK,
    };
    PROCESS_INFORMATION pi;
    if (!pCreateProcessInternalW(nullptr, currentProcessPath, commandLine,
                                 nullptr, nullptr, FALSE, NORMAL_PRIORITY_CLASS,
                                 nullptr, nullptr, &si, &pi, nullptr)) {
        Wh_Log(L"CreateProcess failed");
        return;
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
}

void Wh_ModSettingsChanged() {
    if (g_isToolModProcessLauncher) {
        return;
    }

    WhTool_ModSettingsChanged();
}

void Wh_ModUninit() {
    if (g_isToolModProcessLauncher) {
        return;
    }

    WhTool_ModUninit();
    ExitProcess(0);
}
