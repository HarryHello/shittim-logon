// shittim-logon / adapter / scenes.h
//
// Which scene the logon screen shows, and which character stands in front of it:
// the config format, the parser, and the table that maps a name to a set of Spine
// animation tracks.
//
// **The real table is not in this repository.** What is here is the mechanism with
// a placeholder table in it. The actual rows are a list of one particular game's
// animation names and how they combine, which is a catalogue of somebody else's
// assets rather than anything this project wrote, and it is left out for the same
// reason no skeleton or atlas byte is here. Everything that makes the mechanism
// worth reading -- the grammar, the parser, the defaults, the failure behaviour --
// is unchanged.
//
// Two things the shape of this file is trying to say, both learned the hard way:
//
//   * A scene is not arithmetic. It is tempting to write `night_N -> Idle_(N-1)`
//     and be done. That held until a companion character turned out to appear in
//     exactly two of nine scenes, as a specific pairing rather than a property of
//     the room -- and an earlier version that assumed one companion per room played
//     two of them at once, putting the same character on screen twice. So the table
//     is a table, and Resolve() is an exact match with no parsing in it.
//
//   * Nothing may assume an animation exists. spine-cpp's setAnimation *aborts the
//     process* on an unknown name. On a logon screen that abort lands somewhere
//     nobody can see it and nobody can dismiss it, so every play goes through a
//     lookup that refuses instead. Two exports of the same character were not even
//     the same runtime version here -- one was a 3.8 export that the 4.2 runtime
//     rejects outright -- and a build that assumed otherwise failed loudly, which
//     was the only mercy in it.
//
// The parser is deliberately free of Spine and of Windows: text in, entries out, so
// it can be tested with no assets, no VM and no logon screen.

#pragma once

#include "subtitle.h"   // 1.3.0: Style/Rgba/Language live there now

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace scenes {

// ------------------------------------------------------------------ the config
//
// One scene per line. A leading '+' means active; everything else is inert and is
// there so somebody can see what they could turn on. Blank lines and lines starting
// with '#' are ignored.
//
//     night_1
//     night_2
//     +night_3
//     day_1
//     +day_2
//
// At each logon one active scene is chosen at random. Activating only night_* is
// how somebody who wants only the night scenes says so.
//
// The parser is deliberately free of spine and of Windows: it is pure text in,
// entries out, so render/bin/regression.exe can test it with no assets, no VM and
// no logon screen -- which is the only kind of test this project trusts to run
// before every change.

struct Entry {
    std::string name;
    bool        active = false;
};

// ------------------------------------------------------------------ 1.3.0 constants
//
// The character's supersampling: 2x render-down for crispness, 1x above the
// height where the cost stops being worth it (auto), forced either way.
enum class CharacterAA {
    Auto,
    Off,
    On,
};

inline constexpr int kAutoAaOffHeight = 2000;

inline int AutoCharacterSuper(int screenHeightPx) {
    return screenHeightPx >= kAutoAaOffHeight ? 1 : 2;
}

inline int CharacterSuper(CharacterAA aa, int screenHeightPx) {
    if (aa == CharacterAA::Off) return 1;
    if (aa == CharacterAA::On)  return 2;
    return AutoCharacterSuper(screenHeightPx);
}

// Clock/character offsets are fractions of the screen; clamped so a typo of 50
// cannot push them off-screen.
inline constexpr float kClockOffsetLimit = 0.5f;
inline constexpr float kCharacterOffsetLimit = 0.25f;

// Voice: how long the room stays quiet before an idle line may play.
inline constexpr int kVoiceIdleIntervalDefault = 30;
inline constexpr int kVoiceIdleIntervalMin     = 15;
inline constexpr int kVoiceIdleIntervalMax     = 10000;

// BGM: which track, and the two volumes around it.
inline constexpr const char *kBgmTrackFactory = "factory";
inline constexpr const char *kBgmTrackCustom  = "custom";
inline constexpr size_t kBgmTrackMaxLen = 32;

inline bool BgmTrackIsCustom(const std::string &id) { return id == kBgmTrackCustom; }

inline constexpr int kBgmVolumeDefault = 40;
inline constexpr int kBgmDuckDefault   = 35;

// Real-time cap. 0 means uncapped.
inline constexpr int kFrameRateLimitMin =  24;
inline constexpr int kFrameRateLimitMax = 480;

// ------------------------------------------------------------------ settings
//
// The same file also carries `key=value` lines. One per line, no sections, no
// quoting, because the person editing it found this repository through a video and
// should not have to learn a format to change what they see.
//
//     clock_over_character=false
//     force_24_hour=true
//     clock_offset_y=0
//     experimental=false
//     follow_time_of_day=true
//
// Shown with the values they actually default to, which is what a fresh file gets.
// This block said `false` for both of the last two long after they were flipped, and
// a stale comment about a default is worse than none: it is the first place anybody
// looks to answer "what does it do out of the box".
//
// A line is a setting if it contains '='; anything else is a scene. That is the
// whole grammar, and it means a scene name can never be mistaken for a setting or
// the other way round -- no scene name contains '=' and no key is a bare word.
struct Settings {
    // Draw the clock in front of the seated character instead of behind her.
    //
    // The clock lives *inside* the room: it is composited at the character's own
    // depth, with her silhouette subtracted from its alpha, so she stands in front
    // of the digits the way she would stand in front of anything else in the room.
    // That is the effect, and in three of the nine scenes it costs some of the time.
    // Turning this on skips the subtraction and nothing else -- same clock, same
    // position, same light -- so the time is always whole.
    //
    // Off by default: the occlusion is the composition working, the text stays
    // legible through it, and whether that trade is worth it is the owner's call
    // rather than this file's.
    //
    // This key used to be `disable_char_layer`, which switched off the entire
    // foreground layer. Nobody wanted a classroom with nobody in it; what people
    // wanted was to read the clock. The old key also had a bug that made it worse
    // than useless -- see the test card note in overlay/src/dcomp.h.
    bool  clockOverCharacter = false;
    // Show a 24-hour clock regardless of the system's format.
    //
    // **On by default, and that is a deliberate break from "follow Windows".** The
    // scene is the thing people saw in the video and asked for, and in it the clock
    // is 24-hour; a `PM` hanging off the end of it reads badly at that size and in
    // that typeface. Following the system format is the technically correct default
    // and the wrong one for what this is. Owner's call, 2026-09-07.
    //
    // Set it to false to follow Windows' own time format and language. See
    // localeclock.h.
    bool  force24Hour = true;
    // Added to the clock's position, as a fraction of screen size. Negative
    // moves it up/left. Resolution-independent, so it survives a monitor change.
    float clockOffsetX = 0.0f;
    float clockOffsetY = 0.0f;
    // Opt in to work that has never run on hardware this project owns: Windows
    // Hello coexistence and Surface-shaped screens. Off by default because the
    // honest state of both is "unmeasured", and the people who turn it on are the
    // ones who have volunteered to find out.
    bool  experimental = false;
    // Pick day scenes between 06:00 and 18:00 and night scenes outside it, instead
    // of choosing from everything active. This is the reference wallpaper's own
    // rule: day between 06:00 and 18:00, night outside it.
    //
    // On by default, which the other four settings are not. Every scene ships active,
    // and without this a machine unlocked at three in the morning has a four-in-nine
    // chance of a noon classroom over open water. The conservative default for the
    // others is off; here it is on.
    bool  followTimeOfDay = true;
    // Set NoLockScreen=1, so a lock goes straight to the credential screen.
    //
    // The seamless handover depends on it: without it Windows draws its own lock
    // screen, with its own clock, over our wallpaper first, and the person sees that
    // before they see any of this. It is a machine-wide policy under
    // HKLM\SOFTWARE\Policies\Microsoft\Windows\Personalization, so it is written
    // here rather than only by the installer -- turning it off in the config should
    // put the machine back without needing to uninstall.
    bool  noLockScreen = true;
    // Windows blurs the lock-screen picture behind the credential screen, and since
    // that picture is now the scene's own room (sceneconfig::SetLockScreenImage) the
    // blur is a real choice rather than an accident: it is the difference between the
    // room the overlay hands over and the room Windows takes on.
    //
    // **On by default, which is Windows' own behaviour.** Turning it off is a
    // machine-wide policy write, and the crisper handover is not worth making that
    // decision on somebody's behalf. Owner's call, 2026-09-07.
    bool  logonBlur = true;
    // Draw the drifting particles across the whole screen, and keep the whole screen.
    //
    // This is the shape of a trade that cannot be avoided, so it is offered as a
    // choice rather than decided here. Measured 2026-09-07: on the Winlogon desktop
    // there is no window arrangement that draws a pixel *and* lets a click through it.
    // A DirectComposition window swallows clicks over its whole rectangle whatever its
    // alpha; a layered window with per-pixel alpha does the same, measured against a
    // control in the same session; a window region passes clicks and removes the
    // drawing with them. Drawing and input are one thing here.
    //
    // So the screen has to be divided, and the only question is where:
    //
    //   on  (default)  The scene keeps the whole screen and the particles drift across
    //                  all of it. Windows' credential box and corner buttons are
    //                  reachable through cut-outs, but a menu that opens from one of
    //                  those buttons lands back on the scene's side and takes no
    //                  clicks -- so it has to be driven from the keyboard.
    //   off            The particles are switched off and the right half of the screen
    //                  is handed to Windows outright. Everything over there works,
    //                  menus included. **The character stays** -- she is left of centre
    //                  and loses nothing.
    //
    // Owner's call, 2026-09-07. Naming it after the effect rather than after the
    // region is deliberate: what somebody gives up is the full-screen effect, and that
    // is the sentence they should be reading when they decide.
    bool  fullscreenFx = true;
    // --- 1.3.0 settings ---

    // After unlocking, sign back into the last account automatically.
    bool  restartAutoSignIn = false;
    // Pat the character on the head and she reacts.
    bool  headpat = true;
    // Don't fade the account tiles during the unlock handover.
    bool  disableAccountFade = false;
    // Draw the dismiss (arrow) button on the credential screen.
    bool  dismissButton = false;
    // Which monitor the logon scene covers, when there is more than one.
    std::string logonMonitor = "primary";
    // The foreground character's supersampling; see CharacterSuper.
    CharacterAA characterAA = CharacterAA::Auto;
    // The foreground character's position, as a fraction of screen size.
    float characterOffsetX = 0.0f;
    float characterOffsetY = 0.0f;
    // Idle voicelines, with the subtitle panel that accompanies them.
    bool voice = false;
    int  voiceVolume = 50;
    std::string voiceDevice;          // empty = the system default
    bool voiceDeviceFallback = false;
    bool voiceSubtitle = true;
    bool voiceSubtitleScene = true;
    bool voiceSubtitleCharacter = true;
    subtitle::Style voiceSubtitleStyle;
    subtitle::Language voiceSubtitleLanguage = subtitle::Language::Chinese;
    int  voiceIdleInterval = kVoiceIdleIntervalDefault;
    // Background music: which track, how loud, and how far to duck it while a
    // voice line plays.
    bool bgm = false;
    int  bgmVolume = kBgmVolumeDefault;
    std::string bgmTrack = kBgmTrackFactory;
    int  bgmDuck = kBgmDuckDefault;
    // Real-time cap; 0 = uncapped.
    int  frameRateLimit = 0;
};

// Case-insensitive, so `False` and `FALSE` behave. Anything that is not a
// recognised false is true only if it is a recognised true -- an unreadable value
// leaves the default alone rather than silently meaning "on", because every one of
// these defaults is the conservative answer.
inline bool ParseBool(const std::string &v, bool fallback, bool *understood = nullptr) {
    std::string s;
    for (char c : v) s.push_back((char)std::tolower((unsigned char)c));
    if (understood) *understood = true;
    if (s == "true" || s == "1" || s == "yes" || s == "on") return true;
    if (s == "false" || s == "0" || s == "no" || s == "off") return false;
    if (understood) *understood = false;
    return fallback;
}

// --- 1.3.0 parsers ---

inline CharacterAA ParseCharacterAA(const std::string &v, CharacterAA fallback,
                                    bool *understood = nullptr) {
    std::string s;
    for (char c : v) s.push_back((char)std::tolower((unsigned char)c));
    if (understood) *understood = true;
    if (s == "auto") return CharacterAA::Auto;
    if (s == "on"  || s == "true"  || s == "yes" || s == "2") return CharacterAA::On;
    if (s == "off" || s == "false" || s == "no"  || s == "1" || s == "0")
        return CharacterAA::Off;
    if (understood) *understood = false;
    return fallback;
}

inline std::string ParseBgmTrack(const std::string &v, const std::string &fallback,
                                 bool *understood = nullptr) {
    std::string s;
    for (char c : v) s.push_back((char)std::tolower((unsigned char)c));
    if (understood) *understood = true;
    if (s == "vendor" || s == "default") return kBgmTrackFactory;
    if (s == "own" || s == "user") return kBgmTrackCustom;
    if (!s.empty() && s.size() <= kBgmTrackMaxLen) {
        bool clean = true;
        for (char c : s)
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == '-' || c == '_' || c == '.'))
                clean = false;
        if (clean && s != "." && s != "..") return s;
    }
    if (understood) *understood = false;
    return fallback;
}

// strtof rather than atof: atof cannot report that it read nothing, so
// `clock_offset_y=up` would silently become 0 and look like it worked.
inline float ParseFrac(const std::string &v, float limit, float fallback,
                       bool *understood = nullptr) {
    const char *b = v.c_str();
    char *end = nullptr;
    const float f = std::strtof(b, &end);
    if (end == b || *end != '\0' || f != f) {
        if (understood) *understood = false;
        return fallback;
    }
    if (understood) *understood = true;
    return f < -limit ? -limit : (f > limit ? limit : f);
}

// `#rgb`, `#rgba`, `#rrggbb`, `#rrggbbaa` -- into a subtitle colour.
inline subtitle::Rgba ParseColor(const std::string &v, subtitle::Rgba fallback,
                                 bool *understood = nullptr) {
    std::string h;
    for (char c : v) {
        if (c == '#' && h.empty()) continue;
        h.push_back((char)std::tolower((unsigned char)c));
    }
    const size_t n = h.size();
    if (n != 3 && n != 4 && n != 6 && n != 8) {
        if (understood) *understood = false;
        return fallback;
    }
    int nib[8] = { 0 };
    for (size_t i = 0; i < n; i++) {
        const char c = h[i];
        if (c >= '0' && c <= '9')      nib[i] = c - '0';
        else if (c >= 'a' && c <= 'f') nib[i] = 10 + (c - 'a');
        else {
            if (understood) *understood = false;
            return fallback;
        }
    }
    int comp[4] = { 0, 0, 0, 255 };
    if (n <= 4) {
        for (size_t i = 0; i < n; i++) comp[i] = nib[i] * 17;
    } else {
        for (size_t i = 0; i < n / 2; i++) comp[i] = nib[i * 2] * 16 + nib[i * 2 + 1];
    }
    if (understood) *understood = true;
    subtitle::Rgba out;
    out.r = (float)comp[0] / 255.0f;
    out.g = (float)comp[1] / 255.0f;
    out.b = (float)comp[2] / 255.0f;
    out.a = (float)comp[3] / 255.0f;
    return out;
}

inline float ParseScale(const std::string &v, float lo, float hi, float fallback,
                        bool *understood = nullptr) {
    const char *b = v.c_str();
    char *end = nullptr;
    const float f = std::strtof(b, &end);
    if (end == b || *end != '\0' || f != f) {
        if (understood) *understood = false;
        return fallback;
    }
    if (understood) *understood = true;
    return f < lo ? lo : (f > hi ? hi : f);
}

inline subtitle::Language ParseLanguage(const std::string &v, subtitle::Language fallback,
                                        bool *understood = nullptr) {
    std::string s;
    for (char c : v) s.push_back((char)std::tolower((unsigned char)c));
    if (understood) *understood = true;
    if (s.empty() || s == "zh" || s == "zh-cn" || s == "cn" || s == "chinese" ||
        s == "\xe4\xb8\xad\xe6\x96\x87")
        return subtitle::Language::Chinese;
    if (s == "ja" || s == "ja-jp" || s == "jp" || s == "japanese" ||
        s == "\xe6\x97\xa5\xe6\x96\x87" ||
        s == "\xe6\x97\xa5\xe8\xaf\xad" ||
        s == "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e")
        return subtitle::Language::Japanese;
    if (understood) *understood = false;
    return fallback;
}

inline int ParseCount(const std::string &v, int lo, int hi, int fallback,
                      bool *understood = nullptr) {
    const char *b = v.c_str();
    char *end = nullptr;
    const long n = std::strtol(b, &end, 10);
    if (end == b || *end != '\0') {
        if (understood) *understood = false;
        return fallback;
    }
    if (understood) *understood = true;
    return n < lo ? lo : (n > hi ? hi : (int)n);
}

inline int ParseFrameRateLimit(const std::string &v, int fallback,
                               bool *understood = nullptr) {
    const int n = ParseCount(v, 0, kFrameRateLimitMax, fallback, understood);
    if (n <= 0) return 0;
    return n < kFrameRateLimitMin ? kFrameRateLimitMin : n;
}

// Trims ASCII space and tab from both ends. Not locale-aware on purpose: these are
// scene names, and a scene name with a non-breaking space in it is a typo either way.
inline std::string trim(const std::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) b--;
    return s.substr(a, b - a);
}

// Reads the `key=value` lines. Unknown keys are collected rather than dropped so the
// caller can name them in the log: a misspelled setting that silently does nothing
// is indistinguishable from a setting that does not work.
inline Settings ParseSettings(const std::string &text,
                              std::vector<std::string> *unknown = nullptr,
                              std::vector<std::string> *badValue = nullptr) {
    Settings s;
    std::string line;
    for (size_t i = 0; i <= text.size(); i++) {
        if (i != text.size() && text[i] != '\n') { line.push_back(text[i]); continue; }
        const std::string t = trim(line);
        line.clear();
        if (t.empty() || t[0] == '#') continue;
        const size_t eq = t.find('=');
        if (eq == std::string::npos) continue;          // a scene, not a setting

        std::string key = trim(t.substr(0, eq));
        const std::string val = trim(t.substr(eq + 1));
        for (char &c : key) c = (char)std::tolower((unsigned char)c);

        bool ok = true;
        if (key == "clock_over_character") s.clockOverCharacter = ParseBool(val, s.clockOverCharacter, &ok);
        else if (key == "force_24_hour")   s.force24Hour      = ParseBool(val, s.force24Hour, &ok);
        else if (key == "experimental")    s.experimental     = ParseBool(val, s.experimental, &ok);
        else if (key == "follow_time_of_day") s.followTimeOfDay = ParseBool(val, s.followTimeOfDay, &ok);
        else if (key == "no_lock_screen")  s.noLockScreen     = ParseBool(val, s.noLockScreen, &ok);
        else if (key == "logon_blur")      s.logonBlur        = ParseBool(val, s.logonBlur, &ok);
        else if (key == "restart_auto_sign_in")
            s.restartAutoSignIn = ParseBool(val, s.restartAutoSignIn, &ok);
        else if (key == "fullscreen_fx")   s.fullscreenFx     = ParseBool(val, s.fullscreenFx, &ok);
        else if (key == "headpat")         s.headpat          = ParseBool(val, s.headpat, &ok);
        else if (key == "disable_account_fade")
            s.disableAccountFade = ParseBool(val, s.disableAccountFade, &ok);
        else if (key == "dismiss_button")
            s.dismissButton = ParseBool(val, s.dismissButton, &ok);

        else if (key == "account_switch")  continue;   // legacy key, accepted and ignored

        else if (key == "logon_monitor")   s.logonMonitor = val.empty() ? "primary" : val;
        else if (key == "clock_offset_x")
            s.clockOffsetX = ParseFrac(val, kClockOffsetLimit, s.clockOffsetX, &ok);
        else if (key == "clock_offset_y")
            s.clockOffsetY = ParseFrac(val, kClockOffsetLimit, s.clockOffsetY, &ok);

        else if (key == "character_antialias")
            s.characterAA = ParseCharacterAA(val, s.characterAA, &ok);
        else if (key == "character_offset_x")
            s.characterOffsetX = ParseFrac(val, kCharacterOffsetLimit, s.characterOffsetX, &ok);
        else if (key == "character_offset_y")
            s.characterOffsetY = ParseFrac(val, kCharacterOffsetLimit, s.characterOffsetY, &ok);

        else if (key == "voice")          s.voice         = ParseBool(val, s.voice, &ok);
        else if (key == "voice_subtitle") s.voiceSubtitle = ParseBool(val, s.voiceSubtitle, &ok);
        else if (key == "voice_subtitle_scene")
            s.voiceSubtitleScene = ParseBool(val, s.voiceSubtitleScene, &ok);
        else if (key == "voice_subtitle_character")
            s.voiceSubtitleCharacter = ParseBool(val, s.voiceSubtitleCharacter, &ok);

        else if (key == "voice_subtitle_background")
            s.voiceSubtitleStyle.background = ParseColor(val, s.voiceSubtitleStyle.background, &ok);
        else if (key == "voice_subtitle_border")
            s.voiceSubtitleStyle.border = ParseColor(val, s.voiceSubtitleStyle.border, &ok);
        else if (key == "voice_subtitle_text")
            s.voiceSubtitleStyle.text = ParseColor(val, s.voiceSubtitleStyle.text, &ok);
        else if (key == "voice_subtitle_border_width")
            s.voiceSubtitleStyle.borderWidth =
                (float)ParseCount(val, 0, 4, (int)s.voiceSubtitleStyle.borderWidth, &ok);

        else if (key == "voice_subtitle_font") s.voiceSubtitleStyle.font = val;
        else if (key == "voice_subtitle_corner")
            s.voiceSubtitleStyle.corner =
                (float)ParseCount(val, 0, 16, (int)s.voiceSubtitleStyle.corner, &ok);

        else if (key == "voice_subtitle_scale")
            s.voiceSubtitleStyle.scale = ParseScale(val, subtitle::kScaleMin, subtitle::kScaleMax,
                                                    s.voiceSubtitleStyle.scale, &ok);

        else if (key == "voice_subtitle_language")
            s.voiceSubtitleLanguage = ParseLanguage(val, s.voiceSubtitleLanguage, &ok);
        else if (key == "voice_volume")
            s.voiceVolume = ParseCount(val, 0, 100, s.voiceVolume, &ok);

        else if (key == "voice_device")
            s.voiceDevice = (val == "auto") ? std::string() : val;
        else if (key == "voice_device_fallback")
            s.voiceDeviceFallback = ParseBool(val, s.voiceDeviceFallback, &ok);
        else if (key == "voice_idle_interval")
            s.voiceIdleInterval = ParseCount(val, kVoiceIdleIntervalMin, kVoiceIdleIntervalMax,
                                             s.voiceIdleInterval, &ok);
        else if (key == "bgm") s.bgm = ParseBool(val, s.bgm, &ok);
        else if (key == "bgm_volume")
            s.bgmVolume = ParseCount(val, 0, 100, s.bgmVolume, &ok);
        else if (key == "bgm_track")
            s.bgmTrack = ParseBgmTrack(val, s.bgmTrack, &ok);
        else if (key == "bgm_duck")
            s.bgmDuck = ParseCount(val, 0, 100, s.bgmDuck, &ok);
        else if (key == "frame_rate_limit")
            s.frameRateLimit = ParseFrameRateLimit(val, s.frameRateLimit, &ok);
        else {
            if (unknown) unknown->push_back(key);
            continue;
        }
        if (!ok && badValue) badValue->push_back(key + "=" + val);
    }
    return s;
}

inline std::vector<Entry> ParseConfig(const std::string &text) {
    std::vector<Entry> out;
    std::string line;
    // Hand-rolled rather than std::getline on a stringstream, so that a file with
    // CRLF, LF, or a missing final newline all parse the same. A config edited in
    // Notepad on the machine it runs on will be CRLF; one edited here will not.
    for (size_t i = 0; i <= text.size(); i++) {
        if (i == text.size() || text[i] == '\n') {
            const std::string t = trim(line);
            line.clear();
            if (t.empty() || t[0] == '#') continue;
            // A line with '=' is a setting; ParseSettings has it. Skipped here so a
            // key never enters the scene pool -- otherwise `experimental=true` would
            // be reported as an unknown scene every boot.
            if (t.find('=') != std::string::npos) continue;
            Entry e;
            if (t[0] == '+') {
                e.active = true;
                e.name = trim(t.substr(1));
            } else {
                e.name = t;
            }
            // '+' on its own is not a scene. Dropped rather than added as an empty
            // name, so a stray keystroke cannot become an entry that never matches
            // and quietly reduces the pool.
            if (!e.name.empty()) out.push_back(e);
            continue;
        }
        line.push_back(text[i]);
    }
    return out;
}

// ------------------------------------------------------------------ the scenes

enum class TimeOfDay { Night, Day };

struct Resolved {
    bool        ok = false;
    TimeOfDay   when = TimeOfDay::Night;
    // The name it resolved from, carried back rather than remembered separately by
    // every caller. The scene chosen for the next logon has to be written to a file
    // and turned into a still's filename, and both want the name, not the parts.
    std::string name;
    std::string room;        // room skeleton, without extension
    std::string character;   // foreground skeleton, without extension
    int         idle = 0;    // N in Idle_%02d on track 1
    // N in Idle_%02d on track 4, or 0 for nobody. Seven of the nine scenes have
    // nobody: the companion is a specific pairing, not a fixture of the room.
    int         companion = 0;
    // How bright this room settles. Both renderers must apply it; see Entryv::gain.
    float       gain = 1.0f;
};

// The scene table.
//
// **Placeholder rows.** See the note at the top of this file: the real ones name a
// particular game's animations and are not published. The shape is the point --
// each row is a complete, independently valid combination, written out rather than
// computed, because the combinations are not derivable from the names.
//
// `companion` being 0 for most rows is not padding. In the work this came from,
// seven of nine scenes had nobody else in the room and the two that did were
// specific pairings. Treating the companion as a property of the room instead is
// exactly the assumption that put two copies of one character on screen.
struct Entryv {
    const char *name;
    TimeOfDay   when;
    const char *room;        // room skeleton, without extension
    const char *character;   // foreground skeleton, without extension
    int         idle;        // N in Idle_%02d on track 1
    int         companion;   // N in Idle_%02d on track 4; 0 for nobody
    // How bright this room settles once the screen is ours. 1.0 is the artwork's own
    // light. A property of the scene rather than a flag on either renderer, because
    // BOTH have to apply it and apply it identically: one renderer bakes the picture
    // the OS shows, the other draws the live room over it, and the two register to
    // zero pixels. A gain applied in one and not the other is a visible step in
    // brightness at the handover.
    float       gain;
};

inline const std::vector<Entryv> &Table() {
    static const std::vector<Entryv> t = {
        { "night_1", TimeOfDay::Night, "room_night", "character_a", 0,  0, 1.00f },
        { "night_2", TimeOfDay::Night, "room_night", "character_a", 1,  0, 1.00f },
        { "night_3", TimeOfDay::Night, "room_night", "character_a", 1, 11, 1.00f },
        { "day_1",   TimeOfDay::Day,   "room_day",   "character_b", 0, 11, 0.95f },
        { "day_2",   TimeOfDay::Day,   "room_day",   "character_b", 1,  0, 0.95f },
    };
    return t;
}

// Exact match against the table. Not parsed into a prefix and a number any more:
// the mapping stopped being arithmetic the moment the companion became a dimension,
// and a parser that still looked like arithmetic would invite the next person to
// "simplify" it back into one.
inline Resolved Resolve(const std::string &name) {
    Resolved r;
    for (const Entryv &e : Table()) {
        if (name != e.name) continue;
        r.ok = true;
        r.name = e.name;
        r.when = e.when;
        r.room = e.room;
        r.character = e.character;
        r.idle = e.idle;
        r.companion = e.companion;
        r.gain = e.gain;
        return r;
    }
    return r;
}

// Every scene this build knows about, in table order. Used to seed the config on
// first run so that somebody opening it sees the whole menu rather than having to
// be told the names.
inline std::vector<std::string> AllNames() {
    std::vector<std::string> out;
    for (const Entryv &e : Table()) out.push_back(e.name);
    return out;
}

// The active entries that also resolve, in file order. Unknown names are dropped by
// the caller with a log line -- returning them would push the "is this real" test
// into the frame loop, where a wrong answer is a crash on the logon screen.
inline std::vector<Resolved> Active(const std::vector<Entry> &entries,
                                    std::vector<std::string> *rejected = nullptr) {
    std::vector<Resolved> out;
    for (const Entry &e : entries) {
        if (!e.active) continue;
        Resolved r = Resolve(e.name);
        if (r.ok) out.push_back(r);
        else if (rejected) rejected->push_back(e.name);
    }
    return out;
}

// The filename a scene's still is written to and read back by.
//
// Here, in the one header both sides share, because three things have to agree on it
// or the picture visibly breaks: lab\Bake-SceneStills.ps1 writes it, SetWallpaperFor
// points Windows' lock screen at it, and the overlay covers the transition with it.
// They were three separate format strings and the third was never updated when
// per-scene stills arrived, so the cover stayed at the generic `room-WxH.png` --
// which is byte-identical to night_3's still and differs from every *day* still by a
// mean of 80 per channel. Every day scene was covered by the night classroom for the
// length of the transition.
//
// The PowerShell baker still holds a fourth copy; a .ps1 and a header cannot share a
// constant. It is the one that is checked by eye.
inline std::string StillName(const std::string &scene, int width, int height) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "room-%s-%dx%d.png", scene.c_str(), width, height);
    return buf;
}

// The pre-per-scene name, kept as a fallback for a machine whose stills were baked
// before the scene table existed.
inline std::string StillName(int width, int height) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "room-%dx%d.png", width, height);
    return buf;
}

// ------------------------------------------------------- one still, any screen
//
// What size to bake a still at, given the screen it will be shown on.
//
// **The answer is always 16:9**, and that is the whole point. The world rect above
// is authored 2880x1620 -- exactly 16:9 -- so a still baked at 16:9 crops nothing.
// Uniform-covering an uncropped still onto any screen then produces *exactly* the
// same world rectangle that roomViewport(W,H) returns for that screen, at every
// resolution and every aspect. The proof is two lines: with Sw = 2880m, Sh = 1620m,
//
//     k = max(Dw/Sw, Dh/Sh) = (1/m) * max(Dw/2880, Dh/1620) = s/m
//     visible world = (Dw/k)*(2880/Sw) x (Dh/k)*(1620/Sh) = Dw/s x Dh/s
//
// which is roomViewport's own answer. regression.exe checks it rather than trusting
// the algebra.
//
// So the still stopped being a per-screen artefact. It used to be baked at the
// screen's exact size because the cover was drawn stretched to fill, which is only
// correct when the sizes already match -- and that made a display change (a laptop
// docked to a monitor, a Surface rotated) leave the machine with no usable still at
// all. Now the size below is chosen for *sharpness only*: the smallest 16:9 image
// that still covers this screen without being upscaled. Getting it wrong costs
// resampling, never geometry.
//
// A 1920x1080 panel bakes 1920x1080, a 4K panel bakes 3840x2160, and two 1080p
// monitors side by side (3840x1080) bake 3840x2160 -- because covering a 32:9 strip
// from a 16:9 source is driven by the width.
inline void StillSize(int screenW, int screenH, int *outW, int *outH) {
    if (screenW < 1) screenW = 1;
    if (screenH < 1) screenH = 1;
    // One integer multiplier of 16x9, not a scale applied to each axis separately.
    //
    // The difference is the whole correctness of this: rounding the two axes on
    // their own gave 1366x770 for a 1366x768 laptop, which is 1.774 and not 16:9,
    // and a bake that is not exactly the world's aspect crops something -- so the
    // second cover cannot restore it and the identity above stops holding. Measured
    // as a 6 px error at 1366x768 and 1.5 px at 3440x1440 before this was integer.
    const int m = (int)std::ceil((std::max)(screenW / 16.0, screenH / 9.0));
    *outW = 16 * (m < 1 ? 1 : m);
    *outH = 9  * (m < 1 ? 1 : m);
}

// The part of a source image that a uniform cover of it onto dstW x dstH shows.
//
// Centred on both axes. Shared rather than written twice: the overlay draws the
// cover with it, and regression.exe proves the identity above with it, so the two
// cannot drift apart -- which is exactly how the still filename ended up spelled
// three different ways once before.
inline void CoverSrc(int srcW, int srcH, int dstW, int dstH,
                     double *x, double *y, double *w, double *h) {
    if (srcW < 1 || srcH < 1 || dstW < 1 || dstH < 1) {
        *x = *y = 0.0; *w = srcW; *h = srcH;
        return;
    }
    // Which axis fills. The other one is the one that gets cropped.
    if ((double)dstW / dstH > (double)srcW / srcH) {
        *w = srcW;
        *h = (double)srcW * dstH / dstW;
    } else {
        *h = srcH;
        *w = (double)srcH * dstW / dstH;
    }
    *x = (srcW - *w) * 0.5;
    *y = (srcH - *h) * 0.5;
}

}  // namespace scenes
