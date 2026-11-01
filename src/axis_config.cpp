#include "axis_config.h"
#include "app_paths.h"
#include "log.h"
#include "text_utils.h"

#include <windows.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>
#include <sstream>

using json = nlohmann::json;

namespace Axis {

namespace {
    // Lettura tollerante: il campo può mancare o avere il tipo sbagliato (file scritto a mano).
    template <typename T>
    T get(const json& j, const char* key, T fallback) {
        auto it = j.find(key);
        if (it == j.end() || it->is_null()) return fallback;
        try {
            return it->get<T>();
        } catch (...) {
            return fallback;
        }
    }

    std::string kindName(Kind k) { return k == Kind::Barre ? "Barre" : k == Kind::Pulsanti ? "Pulsanti" : "Assi"; }

    Kind parseKind(const json& j) {
        auto it = j.find("Kind");
        if (it == j.end()) return Kind::Assi;
        if (it->is_number_integer()) {
            const int v = it->get<int>();
            return v == 1 ? Kind::Barre : v == 2 ? Kind::Pulsanti : Kind::Assi;
        }
        const std::string s = it->is_string() ? TextUtils::toLower(it->get<std::string>()) : "";
        return s == "barre" ? Kind::Barre : s == "pulsanti" ? Kind::Pulsanti : Kind::Assi;
    }

    std::string parseAlign(const json& j) {
        auto it = j.find("Align");
        if (it != j.end() && it->is_number_integer()) {
            const int v = it->get<int>();
            return v == 1 ? "Centro" : v == 2 ? "Destra" : "Sinistra";
        }
        const std::string s = it != j.end() && it->is_string() ? TextUtils::toLower(it->get<std::string>()) : "";
        return s == "centro" ? "Centro" : s == "destra" ? "Destra" : "Sinistra";
    }

    std::map<int, std::string> parseLabels(const json& j, const char* key) {
        std::map<int, std::string> out;
        auto it = j.find(key);
        if (it == j.end() || !it->is_object()) return out;
        for (auto& [k, v] : it->items()) {
            if (!v.is_string()) continue;
            const std::string text = TextUtils::trim(v.get<std::string>());
            if (!text.empty()) out[std::atoi(k.c_str())] = text;
        }
        return out;
    }

    json labelsJson(const std::map<int, std::string>& labels) {
        json o = json::object();
        for (const auto& [k, v] : labels) o[std::to_string(k)] = v;
        return o;
    }

    std::vector<int> parseInts(const json& j, const char* key) {
        std::vector<int> out;
        auto it = j.find(key);
        if (it == j.end() || !it->is_array()) return out;
        for (const auto& v : *it) {
            if (v.is_number_integer()) out.push_back(v.get<int>());
        }
        return out;
    }

    std::optional<double> parseOptional(const json& j, const char* key) {
        auto it = j.find(key);
        if (it == j.end() || !it->is_number()) return std::nullopt;
        return it->get<double>();
    }

    std::string validColor(const std::string& c, const char* fallback) {
        if (c.size() == 7 && c[0] == '#' && std::all_of(c.begin() + 1, c.end(), [](char ch) { return std::isxdigit((unsigned char)ch) != 0; })) return c;
        if (c.size() == 9 && c[0] == '#') return "#" + c.substr(3); // #AARRGGBB di WPF: il colore senza alfa
        return fallback;
    }

    Pane parsePane(const json& j) {
        Pane p;
        p.kind = parseKind(j);
        p.deviceId = get<std::string>(j, "DeviceId", "");
        p.label = get<std::string>(j, "Label", "");
        p.align = parseAlign(j);
        p.size = get<int>(j, "Size", 200);
        p.group = get<int>(j, "Group", 0);
        p.locked = get<bool>(j, "Locked", false);
        p.left = parseOptional(j, "OverlayLeft");
        p.top = parseOptional(j, "OverlayTop");
        p.axisX = get<int>(j, "AxisX", 0);
        p.axisY = get<int>(j, "AxisY", 1);
        p.axisZ = get<int>(j, "AxisZ", 2);
        p.invertX = get<bool>(j, "InvertX", false);
        p.invertY = get<bool>(j, "InvertY", false);
        p.invertZ = get<bool>(j, "InvertZ", false);
        p.trailSeconds = get<double>(j, "TrailSeconds", 0.35);
        p.showValues = get<bool>(j, "ShowValues", false);
        p.barAxes = parseInts(j, "BarAxes");
        p.buttons = parseInts(j, "Buttons");
        p.buttonColumns = get<int>(j, "ButtonColumns", 0);
        p.showHat = get<bool>(j, "ShowHat", true);
        p.axisLabels = parseLabels(j, "AxisLabels");
        p.buttonLabels = parseLabels(j, "ButtonLabels");
        p.dotColor = validColor(get<std::string>(j, "DotColor", "#2AA198"), "#2AA198");
        p.barColor = validColor(get<std::string>(j, "BarColor", "#00E5FF"), "#00E5FF");
        return p;
    }

    json paneJson(const Pane& p) {
        json j;
        j["Kind"] = kindName(p.kind);
        j["DeviceId"] = p.deviceId;
        j["Label"] = p.label;
        j["Align"] = p.align;
        j["Size"] = p.size;
        j["Group"] = p.group;
        j["Locked"] = p.locked;
        if (p.left) j["OverlayLeft"] = *p.left;
        if (p.top) j["OverlayTop"] = *p.top;
        j["AxisX"] = p.axisX;
        j["AxisY"] = p.axisY;
        j["AxisZ"] = p.axisZ;
        j["InvertX"] = p.invertX;
        j["InvertY"] = p.invertY;
        j["InvertZ"] = p.invertZ;
        j["TrailSeconds"] = p.trailSeconds;
        j["ShowValues"] = p.showValues;
        j["BarAxes"] = p.barAxes;
        j["Buttons"] = p.buttons;
        j["ButtonColumns"] = p.buttonColumns;
        j["ShowHat"] = p.showHat;
        j["AxisLabels"] = labelsJson(p.axisLabels);
        j["ButtonLabels"] = labelsJson(p.buttonLabels);
        j["DotColor"] = p.dotColor;
        j["BarColor"] = p.barColor;
        return j;
    }

    bool readFile(const std::wstring& path, std::string& out) {
        std::ifstream f(path, std::ios::binary);
        if (!f) return false;
        std::stringstream ss;
        ss << f.rdbuf();
        out = ss.str();
        if (out.size() >= 3 && (unsigned char)out[0] == 0xEF && (unsigned char)out[1] == 0xBB && (unsigned char)out[2] == 0xBF) out.erase(0, 3);
        return true;
    }
}

std::string Pane::displayName() const {
    std::string name = TextUtils::trim(label).empty() ? kindName(kind) : label;
    std::string out;
    for (char c : name) out += (char)std::toupper((unsigned char)c);
    return out;
}

Profile& Settings::active() {
    if (profiles.empty()) profiles.push_back(Profile{});
    return profiles[std::clamp(activeProfile, 0, (int)profiles.size() - 1)];
}

const Profile& Settings::active() const {
    static const Profile empty;
    if (profiles.empty()) return empty;
    return profiles[std::clamp(activeProfile, 0, (int)profiles.size() - 1)];
}

void Settings::sanitize() {
    if (profiles.empty()) profiles.push_back(Profile{});
    activeProfile = std::clamp(activeProfile, 0, (int)profiles.size() - 1);
    serverPort = std::clamp(serverPort, 1024, 65535);
    opacity = std::clamp(opacity, 0.2, 1.0);
    dotRadius = std::clamp(dotRadius, 3, 40);
    textSize = std::clamp(textSize, 8, 24);
    smoothing = std::clamp(smoothing, 0.0, 1.0);
    mouseRange = std::clamp(mouseRange, 50, 2000);
    columns = std::clamp(columns, 0, 10);
    gap = std::clamp(gap, 0, 40);
    backgroundColor = validColor(backgroundColor, "#000000");
    accentColor = validColor(accentColor, "#2AA198");
    for (auto& profile : profiles) {
        if (profile.panes.size() > 10) profile.panes.resize(10); // come Axis Visualizer: dieci moduli per profilo
        for (auto& p : profile.panes) {
            p.size = std::clamp(p.size, 80, 600);
            p.group = std::clamp(p.group, 0, 9);
            p.trailSeconds = std::clamp(p.trailSeconds, 0.0, 2.0);
            p.buttonColumns = std::clamp(p.buttonColumns, 0, 16);
        }
        // Gli eseguibili una volta sola, senza cartella.
        std::vector<std::string> triggers;
        std::set<std::string> seen;
        for (const auto& t : profile.triggers) {
            std::string name = TextUtils::trim(t);
            const size_t slash = name.find_last_of("\\/");
            if (slash != std::string::npos) name = name.substr(slash + 1);
            if (!name.empty() && seen.insert(processName(name)).second) triggers.push_back(name);
        }
        profile.triggers = triggers;
    }
}

Settings parse(const std::string& text) {
    Settings s;
    const json j = json::parse(text, nullptr, false);
    if (!j.is_object()) {
        s.sanitize();
        return s;
    }
    s.serverEnabled = get<bool>(j, "ServerEnabled", true);
    s.serverPort = get<int>(j, "ServerPort", 8787);
    s.activeProfile = get<int>(j, "ActiveProfile", 0);
    s.autoOverlayWithGame = get<bool>(j, "AutoOverlayWithGame", true);
    s.backgroundColor = get<std::string>(j, "BackgroundColor", "#000000");
    s.accentColor = get<std::string>(j, "AccentColor", "#2AA198");
    s.opacity = get<double>(j, "Opacity", 0.75);
    s.dotRadius = get<int>(j, "DotRadius", 12);
    s.textSize = get<int>(j, "TextSize", 11);
    s.boldText = get<bool>(j, "BoldText", false);
    s.smoothing = get<double>(j, "Smoothing", 0.25);
    s.mouseRange = get<int>(j, "MouseRange", 300);
    s.showGrid = get<bool>(j, "ShowGrid", true);
    s.showFrames = get<bool>(j, "ShowFrames", true);
    s.showTitles = get<bool>(j, "ShowTitles", true);
    s.columns = get<int>(j, "Columns", 0);
    s.gap = get<int>(j, "Gap", 8);
    s.locked = get<bool>(j, "Locked", false);
    if (auto k = j.find("Keyboard"); k != j.end() && k->is_object()) {
        s.keyboard.up = get<int>(*k, "Up", 0x20);
        s.keyboard.down = get<int>(*k, "Down", 0x11);
        s.keyboard.left = get<int>(*k, "Left", 0x41);
        s.keyboard.right = get<int>(*k, "Right", 0x44);
        s.keyboard.barPlus = get<int>(*k, "BarPlus", 0x57);
        s.keyboard.barMinus = get<int>(*k, "BarMinus", 0x53);
        s.keyboard.barTravelSeconds = get<double>(*k, "BarTravelSeconds", 2.0);
    }
    if (auto ps = j.find("Profiles"); ps != j.end() && ps->is_array()) {
        for (const auto& pj : *ps) {
            if (!pj.is_object()) continue;
            Profile p;
            p.name = get<std::string>(pj, "Name", "PREDEFINITO");
            if (auto panes = pj.find("Panes"); panes != pj.end() && panes->is_array()) {
                for (const auto& x : *panes) {
                    if (x.is_object()) p.panes.push_back(parsePane(x));
                }
            }
            if (auto t = pj.find("Triggers"); t != pj.end() && t->is_array()) {
                for (const auto& x : *t) {
                    if (x.is_string()) p.triggers.push_back(x.get<std::string>());
                }
            }
            s.profiles.push_back(std::move(p));
        }
    } else if (auto panes = j.find("Panes"); panes != j.end() && panes->is_array()) {
        // Formato vecchio di Axis Visualizer: i moduli senza profili.
        Profile p;
        for (const auto& x : *panes) {
            if (x.is_object()) p.panes.push_back(parsePane(x));
        }
        s.profiles.push_back(std::move(p));
    }
    s.sanitize();
    return s;
}

std::string serialize(const Settings& s) {
    json j;
    j["ServerEnabled"] = s.serverEnabled;
    j["ServerPort"] = s.serverPort;
    json profiles = json::array();
    for (const auto& p : s.profiles) {
        json panes = json::array();
        for (const auto& pane : p.panes) panes.push_back(paneJson(pane));
        profiles.push_back({ { "Name", p.name }, { "Panes", panes }, { "Triggers", p.triggers } });
    }
    j["Profiles"] = profiles;
    j["ActiveProfile"] = s.activeProfile;
    j["AutoOverlayWithGame"] = s.autoOverlayWithGame;
    j["BackgroundColor"] = s.backgroundColor;
    j["AccentColor"] = s.accentColor;
    j["Opacity"] = s.opacity;
    j["DotRadius"] = s.dotRadius;
    j["TextSize"] = s.textSize;
    j["BoldText"] = s.boldText;
    j["Smoothing"] = s.smoothing;
    j["MouseRange"] = s.mouseRange;
    j["ShowGrid"] = s.showGrid;
    j["ShowFrames"] = s.showFrames;
    j["ShowTitles"] = s.showTitles;
    j["Columns"] = s.columns;
    j["Gap"] = s.gap;
    j["Locked"] = s.locked;
    j["Keyboard"] = { { "Up", s.keyboard.up },
                      { "Down", s.keyboard.down },
                      { "Left", s.keyboard.left },
                      { "Right", s.keyboard.right },
                      { "BarPlus", s.keyboard.barPlus },
                      { "BarMinus", s.keyboard.barMinus },
                      { "BarTravelSeconds", s.keyboard.barTravelSeconds } };
    return j.dump(2, ' ', false, json::error_handler_t::replace);
}

std::wstring path() { return AppPaths::getAppDataDir() + L"\\axis.json"; }

std::wstring axisVisualizerSettingsPath() {
    wchar_t buf[MAX_PATH] = {};
    const DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    return (n > 0 && n < MAX_PATH ? std::wstring(buf) : std::wstring()) + L"\\AxisVisualizer\\settings.json";
}

bool exists() { return GetFileAttributesW(path().c_str()) != INVALID_FILE_ATTRIBUTES; }

Settings load() {
    std::string text;
    if (readFile(path(), text)) return parse(text);
    Settings s;
    s.sanitize();
    return s;
}

bool save(const Settings& settings) {
    // Prima in un file a parte, poi al posto di quello vero: un'interruzione non lo lascia a metà.
    const std::wstring target = path(), temp = target + L".tmp";
    {
        std::ofstream f(temp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << serialize(settings);
        if (!f) return false;
    }
    return MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

bool importFromAxisVisualizer(Settings& out, std::string& message) {
    std::string text;
    if (!readFile(axisVisualizerSettingsPath(), text)) {
        message = "Axis Visualizer non trovato su questo PC (manca %APPDATA%\\AxisVisualizer\\settings.json).";
        return false;
    }
    const json j = json::parse(text, nullptr, false);
    if (!j.is_object()) {
        message = "Le impostazioni di Axis Visualizer non si leggono.";
        return false;
    }
    out = parse(text);
    // La porta di Axis Visualizer resta la sua: se è ancora aperto, Iris non deve litigarci.
    if (!j.contains("ServerPort") || out.serverPort == 8777) out.serverPort = 8787;
    size_t panes = 0;
    for (const auto& p : out.profiles) panes += p.panes.size();
    message = "Importati " + std::to_string(out.profiles.size()) + (out.profiles.size() == 1 ? " profilo" : " profili") + " e " +
              std::to_string(panes) + " moduli da Axis Visualizer.";
    Log::info("Axis: " + message);
    return true;
}

std::vector<WindowGroup> windowGroups(const Profile& profile) {
    std::vector<WindowGroup> groups;
    std::map<int, size_t> byNumber;
    for (int i = 0; i < (int)profile.panes.size(); ++i) {
        const Pane& p = profile.panes[i];
        if (!p.enabled()) continue;
        if (p.group <= 0) {
            groups.push_back({ "m" + std::to_string(i + 1), p.displayName(), { i } });
            continue;
        }
        auto it = byNumber.find(p.group);
        if (it == byNumber.end()) {
            byNumber[p.group] = groups.size();
            groups.push_back({ "g" + std::to_string(p.group), "", { i } });
        } else {
            groups[it->second].panes.push_back(i);
        }
    }
    for (auto& g : groups) {
        if (g.key[0] != 'g') continue;
        std::string names;
        for (int i : g.panes) names += (names.empty() ? "" : " + ") + profile.panes[i].displayName();
        g.name = "GRUPPO " + g.key.substr(1) + "  ·  " + names;
    }
    return groups;
}

std::string processName(const std::string& executable) {
    std::string name = TextUtils::trim(executable);
    while (!name.empty() && name.front() == '"') name.erase(0, 1);
    while (!name.empty() && name.back() == '"') name.pop_back();
    const size_t slash = name.find_last_of("\\/");
    if (slash != std::string::npos) name = name.substr(slash + 1);
    name = TextUtils::toLower(name);
    if (name.size() > 4 && name.compare(name.size() - 4, 4, ".exe") == 0) name.resize(name.size() - 4);
    return name;
}

} // namespace Axis
