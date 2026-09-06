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

// ------------------------------------------------------------------ settings
//
// The same file also carries `key=value` lines. One per line, no sections, no
// quoting, because the person editing it found this repository through a video and
// should not have to learn a format to turn the character off.
//
//     disable_char_layer=false
//     force_24_hour=false
//     clock_offset_y=0
//     experimental=false
//     follow_time_of_day=false
//
// A line is a setting if it contains '='; anything else is a scene. That is the
// whole grammar, and it means a scene name can never be mistaken for a setting or
// the other way round -- no scene name contains '=' and no key is a bare word.
struct Settings {
    // Draw the room and the wake, but never the foreground character. For somebody
    // who wants the classroom without anybody standing in front of it.
    bool  disableCharLayer = false;
    // Show a 24-hour clock regardless of the system's format. An override, not a
    // default: with this off the clock follows Windows, which is what almost
    // everyone should want. See localeclock.h.
    bool  force24Hour = false;
    // Added to the clock's vertical position, as a fraction of screen height.
    // Negative moves it up. Resolution-independent, so it survives a monitor change.
    float clockOffsetY = 0.0f;
    // Opt in to work that has never run on hardware this project owns: Windows
    // Hello coexistence and Surface-shaped screens. Off by default because the
    // honest state of both is "unmeasured", and the people who turn it on are the
    // ones who have volunteered to find out.
    bool  experimental = false;
    // Pick day scenes between 06:00 and 18:00 and night scenes outside it, instead
    // of choosing from everything active. This is the reference wallpaper's own
    // rule: `isday = !(spoilerChar && (hr < 6 || hr >= 18))`.
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
        if (key == "disable_char_layer")   s.disableCharLayer = ParseBool(val, s.disableCharLayer, &ok);
        else if (key == "force_24_hour")   s.force24Hour      = ParseBool(val, s.force24Hour, &ok);
        else if (key == "experimental")    s.experimental     = ParseBool(val, s.experimental, &ok);
        else if (key == "follow_time_of_day") s.followTimeOfDay = ParseBool(val, s.followTimeOfDay, &ok);
        else if (key == "no_lock_screen")  s.noLockScreen     = ParseBool(val, s.noLockScreen, &ok);
        else if (key == "clock_offset_y") {
            // strtof rather than atof: atof cannot report that it read nothing, so
            // `clock_offset_y=up` would silently become 0 and look like it worked.
            const char *b = val.c_str();
            char *end = nullptr;
            const float f = std::strtof(b, &end);
            if (end == b || *end != '\0') ok = false;
            // Clamped: this is a fraction of screen height, and a typo of 50 instead
            // of 0.50 would put the clock off the screen with no way to tell that
            // from the overlay having failed to draw.
            else s.clockOffsetY = f < -0.5f ? -0.5f : (f > 0.5f ? 0.5f : f);
        } else {
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

}  // namespace scenes
