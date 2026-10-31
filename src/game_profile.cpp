#include "game_profile.h"
#include "text_utils.h"

#include <nlohmann/json.hpp>

#include <algorithm>

namespace {
    using json = nlohmann::json;

    GameCommand cmd(const std::string& intent, std::vector<std::string> phrases, std::map<std::string, std::string> args = {},
                    const std::string& say = {}) {
        GameCommand c;
        c.intent = intent;
        c.phrases = std::move(phrases);
        c.args = std::move(args);
        c.say = say;
        return c;
    }

    template <typename T>
    void read(const json& j, const char* key, T& out) {
        if (!j.is_object() || !j.contains(key)) return;
        try {
            out = j.at(key).get<T>();
        } catch (...) {
        }
    }

    void readRegion(const json& j, const char* key, std::array<double, 4>& out) {
        if (!j.is_object() || !j.contains(key) || !j[key].is_array() || j[key].size() != 4) return;
        for (size_t i = 0; i < 4; ++i) {
            if (j[key][i].is_number()) out[i] = j[key][i].get<double>();
        }
    }

    std::vector<GameCommand> readCommands(const json& arr) {
        std::vector<GameCommand> out;
        if (!arr.is_array()) return out;
        for (const auto& c : arr) {
            if (!c.is_object()) continue;
            GameCommand g;
            read(c, "intent", g.intent);
            read(c, "phrases", g.phrases);
            read(c, "say", g.say);
            if (c.contains("args") && c["args"].is_object()) {
                for (auto it = c["args"].begin(); it != c["args"].end(); ++it) {
                    g.args[it.key()] = it.value().is_string() ? it.value().get<std::string>() : it.value().dump();
                }
            }
            if (!g.intent.empty() && !g.phrases.empty()) out.push_back(g);
        }
        return out;
    }

    void readStarMap(const json& m, StarMapSettings& s) {
        if (!m.is_object()) return;
        read(m, "searchBoxX", s.searchBoxX);
        read(m, "searchBoxY", s.searchBoxY);
        read(m, "searchBoxWidth", s.searchBoxWidth);
        read(m, "searchBoxHeight", s.searchBoxHeight);
        readRegion(m, "resultsRegion", s.resultsRegion);
        readRegion(m, "setRouteRegion", s.setRouteRegion);
        readRegion(m, "cancelRouteRegion", s.cancelRouteRegion);
        read(m, "setRouteLabels", s.setRouteLabels);
        read(m, "cancelRouteLabels", s.cancelRouteLabels);
        read(m, "searchPlaceholders", s.searchPlaceholders);
        read(m, "mapOpenTimeoutMs", s.mapOpenTimeoutMs);
        read(m, "focusSettleMs", s.focusSettleMs);
        read(m, "typeCharDelayMs", s.typeCharDelayMs);
        read(m, "resultsTimeoutMs", s.resultsTimeoutMs);
        read(m, "clickSettleMs", s.clickSettleMs);
        read(m, "mouseMoveSteps", s.mouseMoveSteps);
        read(m, "mouseMoveStepDelayMs", s.mouseMoveStepDelayMs);
        read(m, "typingAttempts", s.typingAttempts);
        read(m, "smoothMouseMove", s.smoothMouseMove);
        read(m, "clearFieldFirst", s.clearFieldFirst);
        read(m, "closeMapAfterSetRoute", s.closeMapAfterSetRoute);
    }

    void readOcr(const json& o, OcrSettings& s) {
        if (!o.is_object()) return;
        read(o, "language", s.language);
        double up = s.upscale;
        if (o.contains("upscaleFactor")) read(o, "upscaleFactor", up); // vecchia Iris
        if (o.contains("upscale")) read(o, "upscale", up);
        s.upscale = std::clamp((int)up, 1, 4);
        read(o, "matchThreshold", s.matchThreshold);
        read(o, "saveDebugShots", s.saveDebugShots);
    }

    // Comandi uguali (stesso intento e stessi argomenti): per non duplicare.
    bool sameCommand(const GameCommand& a, const GameCommand& b) {
        return a.intent == b.intent && a.args == b.args;
    }
}

std::string GameCommand::id() const {
    auto it = args.find("key");
    if (it != args.end()) return intent + ":" + it->second;
    auto k = args.find("keys");
    if (k != args.end()) return intent + ":" + k->second;
    return intent;
}

std::string relaxedJson(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    bool inString = false, escape = false;
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (inString) {
            out += c;
            if (escape) escape = false;
            else if (c == '\\') escape = true;
            else if (c == '"') inString = false;
            continue;
        }
        if (c == '"') {
            inString = true;
            out += c;
            continue;
        }
        if (c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
            while (i < text.size() && text[i] != '\n') ++i;
            out += '\n';
            continue;
        }
        if (c == '/' && i + 1 < text.size() && text[i + 1] == '*') {
            size_t end = text.find("*/", i + 2);
            i = end == std::string::npos ? text.size() : end + 1;
            continue;
        }
        out += c;
    }
    // Virgole prima di } o ]
    std::string clean;
    clean.reserve(out.size());
    inString = false;
    escape = false;
    for (size_t i = 0; i < out.size(); ++i) {
        char c = out[i];
        if (inString) {
            clean += c;
            if (escape) escape = false;
            else if (c == '\\') escape = true;
            else if (c == '"') inString = false;
            continue;
        }
        if (c == '"') inString = true;
        if (c == ',') {
            size_t j = i + 1;
            while (j < out.size() && (out[j] == ' ' || out[j] == '\t' || out[j] == '\r' || out[j] == '\n')) ++j;
            if (j < out.size() && (out[j] == '}' || out[j] == ']')) continue;
        }
        clean += c;
    }
    return clean;
}

GameProfile GameProfile::defaults() {
    GameProfile p;
    // Tasti predefiniti di Star Citizen (controlla che siano i tuoi: Opzioni > Comandi).
    p.keys = {
        { "map", "F2" }, { "mobiglas", "F1" }, { "confirm", "Enter" }, { "cancel", "Escape" }, { "search_focus", "" },
        { "landing_gear", "N" }, { "lights", "L" }, { "vtol", "K" }, { "quantum_spool", "B" }, { "quantum_engage", "B" },
        { "scan_mode", "V" }, { "ping", "Tab" }, { "power", "U" }, { "engines", "I" }, { "shields", "O" }, { "weapons", "P" },
        { "flight_ready", "R" }, { "request_landing", "LAlt+N" }, { "decoupled", "C" }, { "cruise", "LAlt+C" },
        { "missiles", "G" }, { "decoy", "H" }, { "noise", "J" }, { "exit_seat", "Y" },
    };
    p.commands = {
        cmd("set_route", { "imposta rotta per {destination}", "imposta la rotta per {destination}", "rotta per {destination}",
                           "rotta verso {destination}", "portami a {destination}", "andiamo a {destination}", "destinazione {destination}",
                           "imposta destinazione {destination}", "traccia la rotta per {destination}", "set route to {destination}" },
            {}, "Calcolo la rotta per {destination}."),
        cmd("cancel_route", { "annulla rotta", "annulla la rotta", "cancella rotta", "cancella la rotta", "togli la rotta", "elimina la rotta" },
            {}, "Annullo la rotta."),
        cmd("open_map", { "apri la mappa", "apri mappa", "mappa stellare", "starmap" }),
        cmd("close_map", { "chiudi la mappa", "chiudi mappa", "togli la mappa" }),
        cmd("press", { "carrello", "carrello di atterraggio", "abbassa il carrello", "alza il carrello", "tira su il carrello", "gear" },
            { { "key", "landing_gear" } }, "Carrello."),
        cmd("press", { "luci", "accendi le luci", "spegni le luci", "fari" }, { { "key", "lights" } }, "Luci."),
        cmd("press", { "vtol", "modalita vtol", "vettoriale" }, { { "key", "vtol" } }, "VTOL."),
        cmd("press", { "modalita scansione", "scansione", "attiva lo scanner" }, { { "key", "scan_mode" } }),
        cmd("press", { "ping", "manda un ping", "pinga", "scansiona intorno" }, { { "key", "ping" } }),
        cmd("press", { "alimentazione", "accendi la nave", "spegni la nave" }, { { "key", "power" } }),
        cmd("press", { "scudi", "alza gli scudi", "abbassa gli scudi" }, { { "key", "shields" } }, "Scudi."),
        cmd("press", { "motori", "accendi i motori", "spegni i motori" }, { { "key", "engines" } }, "Motori."),
        cmd("press", { "armi", "armi in linea", "arma i cannoni", "disarma" }, { { "key", "weapons" } }, "Armi."),
        cmd("press", { "pronti al volo", "avvia i sistemi", "prepara al volo", "flight ready" }, { { "key", "flight_ready" } }, "Sistemi pronti."),
        cmd("press", { "richiedi atterraggio", "chiedi di atterrare", "permesso di atterraggio", "chiama la torre" },
            { { "key", "request_landing" } }, "Richiedo l'atterraggio."),
        cmd("press", { "modalita disaccoppiata", "disaccoppiato", "decoupled" }, { { "key", "decoupled" } }),
        cmd("press", { "cruise control", "velocita di crociera", "crociera" }, { { "key", "cruise" } }),
        cmd("press", { "missili", "arma i missili", "modalita missili" }, { { "key", "missiles" } }, "Missili."),
        cmd("press", { "flare", "lancia i flare", "contromisure", "decoy" }, { { "key", "decoy" } }),
        cmd("press", { "chaff", "disturbo", "lancia il disturbo" }, { { "key", "noise" } }),
        cmd("hold", { "alzati", "esci dal sedile", "lascia il sedile" }, { { "key", "exit_seat" }, { "ms", "1200" } }),
        cmd("sequence", { "prepara la nave", "accendi tutto", "check pre volo", "procedura di decollo" },
            { { "keys", "power,engines,shields,weapons" } }, "Accendo tutto."),
        cmd("quantum_travel", { "quantum", "salto quantico", "vai in quantum", "attiva il quantum", "avvia il salto" }, {}, "Salto quantico."),
        cmd("repeat", { "ancora", "di nuovo", "ripeti", "un'altra volta", "rifallo" }),
        cmd("toggle_dry_run", { "modalita test", "attiva la simulazione", "disattiva la simulazione", "dry run" }),
        cmd("status", { "stato", "come stiamo", "rapporto", "status" }),
    };
    // Il valore a sinistra va scritto come appare nel gioco (è quello che Iris
    // digita nella ricerca della mappa); a destra come lo pronunci.
    p.aliases["destination"] = {
        { "Stazione Orbitale Seraphim", { "seraphim", "serafim", "stazione seraphim", "serafino", "sera a fino", "sera a film", "seraphim station" } },
        { "Port Olisar", { "porto olisar", "olisar", "porto oliza" } },
        { "Everus Harbor", { "everus arbor", "everus", "everus harbour" } },
        { "Baijini Point", { "baijini", "baigini point", "bajini point" } },
        { "Port Tressler", { "porto tressler", "tressler" } },
        { "Grim HEX", { "grim hex", "grimex", "grim ex" } },
        { "Lorville", { "lorvil", "lorvill" } },
        { "Area18", { "area diciotto", "area 18", "area eighteen" } },
        { "New Babbage", { "nuova babbage", "niu babbage" } },
        { "Orison", { "orizon", "orisson" } },
        { "Crusader", { "cruseider" } },
        { "microTech", { "micro tech", "maicrotech" } },
        { "ArcCorp", { "arc corp", "arcorp" } },
        { "Hurston", { "erston", "urston" } },
        { "Yela", { "iela" } },
        { "Daymar", { "deimar", "damar" } },
        { "Cellin", { "selin" } },
        { "Aberdeen", { "aberdin" } },
        { "Arial", { "ariel" } },
        { "Magda", {} },
        { "Ita", {} },
        { "Clio", { "klio" } },
        { "Euterpe", { "iuterpe" } },
        { "Calliope", { "kalliope" } },
        { "Lyria", { "liria" } },
        { "Wala", { "uala" } },
        { "Kareah", { "karea", "carea" } },
        { "Jumptown", { "jump town" } },
        { "Pyro", { "piro" } },
        { "Stanton", { "stenton" } },
        { "Nyx", { "nix" } },
        { "Levski", { "levsky" } },
        { "Ruin Station", { "ruin" } },
        { "Checkmate Station", { "checkmate" } },
        { "Cru-L1", { "crul1", "cru l1", "cruelle 1", "crusader l1" } },
    };
    p.corrections = {
        { "a nulla", "annulla" }, { "cruzader", "crusader" }, { "crusade", "crusader" }, { "arcorp", "arccorp" },
        { "olisa", "olisar" }, { "quantico", "quantum" }, { "quantam", "quantum" }, { "pinaga", "pinga" },
        { "quantum and mode", "quantum" }, { "sera a film", "seraphim" }, { "serafin", "seraphim" },
    };
    return p;
}

bool GameProfile::parse(const std::string& text, GameProfile& out, std::string& error) {
    json j = json::parse(relaxedJson(text), nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        error = "JSON non valido (controlla virgole e parentesi)";
        return false;
    }
    GameProfile p = defaults();
    read(j, "name", p.name);
    read(j, "processName", p.processName);
    read(j, "windowTitle", p.windowTitle);
    read(j, "dryRun", p.dryRun);
    read(j, "keyHoldMs", p.keyHoldMs);
    read(j, "keyGapMs", p.keyGapMs);
    read(j, "stepDelayMs", p.stepDelayMs);
    p.stepDelayMs = std::clamp(p.stepDelayMs, 0, 10000);
    read(j, "matchThreshold", p.matchThreshold);
    if (j.contains("keys")) {
        p.keys.clear();
        read(j, "keys", p.keys);
    }
    if (j.contains("corrections")) {
        p.corrections.clear();
        read(j, "corrections", p.corrections);
    }
    if (j.contains("commands")) p.commands = readCommands(j["commands"]);
    if (j.contains("aliases")) {
        p.aliases.clear();
        read(j, "aliases", p.aliases);
    }
    if (j.contains("starMap")) readStarMap(j["starMap"], p.starMap);
    if (j.contains("ocr")) readOcr(j["ocr"], p.ocr);
    out = p;
    return true;
}

std::string GameProfile::toJson() const {
    json j;
    j["name"] = name;
    j["processName"] = processName;
    j["windowTitle"] = windowTitle;
    j["dryRun"] = dryRun;
    j["keyHoldMs"] = keyHoldMs;
    j["keyGapMs"] = keyGapMs;
    j["stepDelayMs"] = stepDelayMs;
    j["matchThreshold"] = matchThreshold;
    j["keys"] = keys;
    j["corrections"] = corrections;
    json cmds = json::array();
    for (const auto& c : commands) {
        json o;
        o["intent"] = c.intent;
        if (!c.args.empty()) o["args"] = c.args;
        o["phrases"] = c.phrases;
        if (!c.say.empty()) o["say"] = c.say;
        cmds.push_back(o);
    }
    j["commands"] = cmds;
    j["aliases"] = aliases;
    json m;
    m["searchBoxX"] = starMap.searchBoxX;
    m["searchBoxY"] = starMap.searchBoxY;
    m["searchBoxWidth"] = starMap.searchBoxWidth;
    m["searchBoxHeight"] = starMap.searchBoxHeight;
    m["resultsRegion"] = starMap.resultsRegion;
    m["setRouteRegion"] = starMap.setRouteRegion;
    m["cancelRouteRegion"] = starMap.cancelRouteRegion;
    m["setRouteLabels"] = starMap.setRouteLabels;
    m["cancelRouteLabels"] = starMap.cancelRouteLabels;
    m["searchPlaceholders"] = starMap.searchPlaceholders;
    m["mapOpenTimeoutMs"] = starMap.mapOpenTimeoutMs;
    m["focusSettleMs"] = starMap.focusSettleMs;
    m["typeCharDelayMs"] = starMap.typeCharDelayMs;
    m["resultsTimeoutMs"] = starMap.resultsTimeoutMs;
    m["clickSettleMs"] = starMap.clickSettleMs;
    m["mouseMoveSteps"] = starMap.mouseMoveSteps;
    m["mouseMoveStepDelayMs"] = starMap.mouseMoveStepDelayMs;
    m["typingAttempts"] = starMap.typingAttempts;
    m["smoothMouseMove"] = starMap.smoothMouseMove;
    m["clearFieldFirst"] = starMap.clearFieldFirst;
    m["closeMapAfterSetRoute"] = starMap.closeMapAfterSetRoute;
    j["starMap"] = m;
    json o;
    o["language"] = ocr.language;
    o["upscale"] = ocr.upscale;
    o["matchThreshold"] = ocr.matchThreshold;
    o["saveDebugShots"] = ocr.saveDebugShots;
    j["ocr"] = o;
    return j.dump(2);
}

bool GameProfile::importOldIris(const std::string& settingsText, const std::string& dictionaryText, GameProfile& out, std::string& error) {
    GameProfile p = defaults();
    bool any = false;

    json s = json::parse(relaxedJson(settingsText), nullptr, false);
    if (!s.is_discarded() && s.is_object()) {
        any = true;
        read(s, "dryRun", p.dryRun);
        if (s.contains("game") && s["game"].is_object()) {
            const json& g = s["game"];
            read(g, "processName", p.processName);
            read(g, "windowTitle", p.windowTitle);
            read(g, "keyHoldMs", p.keyHoldMs);
            read(g, "keyGapMs", p.keyGapMs);
            std::map<std::string, std::string> oldKeys;
            read(g, "keys", oldKeys);
            for (const auto& [k, v] : oldKeys) p.keys[k] = v; // i tuoi tasti hanno la precedenza
            if (g.contains("starMap")) readStarMap(g["starMap"], p.starMap);
        }
        if (s.contains("ocr")) readOcr(s["ocr"], p.ocr);
    }

    json d = json::parse(relaxedJson(dictionaryText), nullptr, false);
    if (!d.is_discarded() && d.is_object()) {
        any = true;
        read(d, "matchThreshold", p.matchThreshold);
        std::map<std::string, std::string> oldCorrections;
        read(d, "corrections", oldCorrections);
        for (const auto& [k, v] : oldCorrections) p.corrections[k] = v;
        // Comandi: i tuoi, più quelli nuovi che non avevi.
        std::vector<GameCommand> old = readCommands(d.contains("commands") ? d["commands"] : json());
        if (!old.empty()) {
            std::vector<GameCommand> merged = old;
            for (const auto& def : p.commands) {
                auto it = std::find_if(merged.begin(), merged.end(), [&](const GameCommand& c) { return sameCommand(c, def); });
                if (it == merged.end()) {
                    merged.push_back(def);
                } else {
                    // Frasi nuove aggiunte alle tue (senza doppioni).
                    for (const auto& ph : def.phrases) {
                        if (std::find(it->phrases.begin(), it->phrases.end(), ph) == it->phrases.end()) it->phrases.push_back(ph);
                    }
                    if (it->say.empty()) it->say = def.say;
                }
            }
            p.commands = merged;
        }
        std::map<std::string, std::map<std::string, std::vector<std::string>>> oldAliases;
        read(d, "aliases", oldAliases);
        for (const auto& [slot, table] : oldAliases) {
            for (const auto& [canonical, variants] : table) {
                auto& dst = p.aliases[slot][canonical];
                for (const auto& v : variants) {
                    if (std::find(dst.begin(), dst.end(), v) == dst.end()) dst.push_back(v);
                }
            }
        }
    }
    if (!any) {
        error = "file della vecchia Iris non leggibili";
        return false;
    }
    out = p;
    return true;
}
