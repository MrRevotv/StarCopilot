#include "game_log.h"
#include "app_paths.h"
#include "log.h"
#include "text_utils.h"

#include <windows.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>

namespace GameLog {

namespace {
    using json = nlohmann::json;

    std::string lower(const std::string& s) { return TextUtils::toLower(s); }

    bool sameName(const std::string& a, const std::string& b) { return !a.empty() && !b.empty() && lower(a) == lower(b); }

    // Il testo tra apici dopo "key" ("Actor 'Nome'"), da pos in poi.
    std::string quotedAfter(const std::string& line, const std::string& key, size_t* pos = nullptr) {
        const size_t from = pos ? *pos : 0;
        const size_t k = line.find(key, from);
        if (k == std::string::npos) return {};
        const size_t a = line.find('\'', k + key.size());
        if (a == std::string::npos) return {};
        const size_t b = line.find('\'', a + 1);
        if (b == std::string::npos) return {};
        if (pos) *pos = b + 1;
        return line.substr(a + 1, b - a - 1);
    }

    // Il testo tra parentesi quadre dopo "key" ("Handle[Nome]").
    std::string bracketAfter(const std::string& line, const std::string& key) {
        const size_t k = line.find(key);
        if (k == std::string::npos) return {};
        const size_t a = k + key.size();
        const size_t b = line.find(']', a);
        if (b == std::string::npos) return {};
        return line.substr(a, b - a);
    }

    std::string guid(const std::string& line, const std::string& key) {
        const std::string g = TextUtils::trim(bracketAfter(line, key));
        if (g.empty() || g.find_first_not_of("0-") == std::string::npos) return {}; // tutti zeri = nessuno
        return g;
    }

    std::string capitalize(std::string s) {
        if (!s.empty() && s[0] >= 'a' && s[0] <= 'z') s[0] = (char)(s[0] - 'a' + 'A');
        return s;
    }

    std::string lowerFirst(std::string s) {
        if (!s.empty() && s[0] >= 'A' && s[0] <= 'Z' && (s.size() < 2 || !(s[1] >= 'A' && s[1] <= 'Z'))) s[0] = (char)(s[0] - 'A' + 'a');
        return s;
    }

    // "StantonMagnus" -> "Stanton Magnus"; "Area18" resta com'è.
    std::string splitCamel(const std::string& s) {
        std::string out;
        for (size_t i = 0; i < s.size(); ++i) {
            const char c = s[i];
            if (i > 0 && c >= 'A' && c <= 'Z' && s[i - 1] >= 'a' && s[i - 1] <= 'z') out += ' ';
            out += c;
        }
        return out;
    }

    struct Rule {
        const char* prefix; // minuscolo
        Kind kind;
        const char* action;
        Level level;
    };

    // Le notifiche dell'HUD (italiano e inglese). L'ordine conta: prima le più lunghe.
    const Rule kRules[] = {
        { "contratto non accettato", Kind::Contract, "refused", Level::Warn },
        { "contract not accepted", Kind::Contract, "refused", Level::Warn },
        { "contratto accettato", Kind::Contract, "accepted", Level::Good },
        { "contract accepted", Kind::Contract, "accepted", Level::Good },
        { "contratto completato", Kind::Contract, "completed", Level::Good },
        { "contract completed", Kind::Contract, "completed", Level::Good },
        { "contract complete", Kind::Contract, "completed", Level::Good },
        { "contratto fallito", Kind::Contract, "failed", Level::Bad },
        { "contract failed", Kind::Contract, "failed", Level::Bad },
        { "contratto abbandonato", Kind::Contract, "abandoned", Level::Warn },
        { "contract abandoned", Kind::Contract, "abandoned", Level::Warn },
        { "contratto annullato", Kind::Contract, "abandoned", Level::Warn },
        { "contract withdrawn", Kind::Contract, "abandoned", Level::Warn },
        { "contratto scaduto", Kind::Contract, "abandoned", Level::Warn },
        { "contract expired", Kind::Contract, "abandoned", Level::Warn },
        { "contratto condiviso", Kind::Contract, "shared", Level::Info },
        { "contract shared", Kind::Contract, "shared", Level::Info },
        { "contratto disponibile", Kind::Contract, "available", Level::Info },
        { "contract available", Kind::Contract, "available", Level::Info },
        { "nuovo obiettivo", Kind::Objective, "new", Level::Info },
        { "new objective", Kind::Objective, "new", Level::Info },
        { "obiettivo completato", Kind::Objective, "completed", Level::Good },
        { "objective completed", Kind::Objective, "completed", Level::Good },
        { "objective complete", Kind::Objective, "completed", Level::Good },
        { "obiettivo fallito", Kind::Objective, "failed", Level::Bad },
        { "objective failed", Kind::Objective, "failed", Level::Bad },
        { "obiettivo revocato", Kind::Objective, "withdrawn", Level::Info },
        { "objective withdrawn", Kind::Objective, "withdrawn", Level::Info },
        { "sei in una zona di armistizio", Kind::Zone, "armistice_in", Level::Info },
        { "entering armistice zone", Kind::Zone, "armistice_in", Level::Info },
        { "stai lasciando la zona di armistizio", Kind::Zone, "armistice_out", Level::Warn },
        { "leaving armistice zone", Kind::Zone, "armistice_out", Level::Warn },
        { "entrato in spazio monitorato", Kind::Zone, "monitored_in", Level::Info },
        { "entered monitored space", Kind::Zone, "monitored_in", Level::Info },
        { "uscito da spazio monitorato", Kind::Zone, "monitored_out", Level::Warn },
        { "exited monitored space", Kind::Zone, "monitored_out", Level::Warn },
        { "left monitored space", Kind::Zone, "monitored_out", Level::Warn },
        { "spazio monitorato disattivato", Kind::Zone, "monitored_down", Level::Warn },
        { "monitored space down", Kind::Zone, "monitored_down", Level::Warn },
        { "monitored space disabled", Kind::Zone, "monitored_down", Level::Warn },
        { "spazio monitorato ripristinato", Kind::Zone, "monitored_up", Level::Info },
        { "monitored space restored", Kind::Zone, "monitored_up", Level::Info },
        { "ingresso in una propriet\xC3\xA0 privata", Kind::Zone, "private_in", Level::Warn },
        { "entering private property", Kind::Zone, "private_in", Level::Warn },
        { "propriet\xC3\xA0 privata lasciata", Kind::Zone, "private_out", Level::Info },
        { "leaving private property", Kind::Zone, "private_out", Level::Info },
        { "voce del diario aggiunta", Kind::Journal, "journal", Level::Info },
        { "journal entry added", Kind::Journal, "journal", Level::Info },
        { "richiesta hangar completata", Kind::Hangar, "hangar", Level::Info },
        { "hangar request completed", Kind::Hangar, "hangar", Level::Info },
        { "hangar request complete", Kind::Hangar, "hangar", Level::Info },
        { "received blueprint", Kind::Blueprint, "blueprint", Level::Good },
        { "blueprint ricevuto", Kind::Blueprint, "blueprint", Level::Good },
        { "progetto ricevuto", Kind::Blueprint, "blueprint", Level::Good },
        { "sei entrato in ", Kind::Jurisdiction, "enter", Level::Info },
    };

    // Notifiche che non dicono niente di utile.
    const char* const kNoise[] = { "suggeriment", "esci dal letto", "download in corso", "calibrazione del viaggio quantum", "hint:", "tip:",
                                   "quantum travel calibration", "exit bed" };

    // Il resto della notifica dopo il prefisso: "Contratto accettato: Nome: " -> "Nome".
    std::string rest(const std::string& text, size_t prefixLength) {
        std::string r = text.substr(std::min(prefixLength, text.size()));
        size_t a = r.find_first_not_of(" :-");
        r = a == std::string::npos ? std::string() : r.substr(a);
        while (!r.empty() && (r.back() == ' ' || r.back() == ':' || r.back() == '.')) r.pop_back();
        return r;
    }

    const char* contractLabel(const std::string& action) {
        if (action == "accepted") return "Contratto accettato";
        if (action == "completed") return "Contratto completato";
        if (action == "failed") return "Contratto fallito";
        if (action == "abandoned") return "Contratto chiuso";
        if (action == "shared") return "Contratto condiviso";
        if (action == "available") return "Contratto disponibile";
        return "Contratto non accettato";
    }

    const char* objectiveLabel(const std::string& action) {
        if (action == "new") return "Nuovo obiettivo";
        if (action == "completed") return "Obiettivo completato";
        if (action == "failed") return "Obiettivo fallito";
        return "Obiettivo revocato";
    }

    const char* zoneText(const std::string& action) {
        if (action == "armistice_in") return "Zona di armistizio: combattimento proibito";
        if (action == "armistice_out") return "Fuori dalla zona di armistizio: cautela";
        if (action == "monitored_in") return "Entrato in spazio monitorato";
        if (action == "monitored_out") return "Uscito dallo spazio monitorato";
        if (action == "monitored_down") return "Spazio monitorato disattivato";
        if (action == "monitored_up") return "Spazio monitorato ripristinato";
        if (action == "private_in") return "Ingresso in una proprietà privata";
        return "Proprietà privata lasciata";
    }

    std::string effectName(const std::string& effect) {
        static const std::pair<const char*, const char*> kNames[] = {
            { "dehydrated", "disidratato" }, { "starved", "affamato" }, { "hungry", "fame" }, { "thirsty", "sete" },
            { "drunk", "ubriaco" }, { "bleeding", "emorragia" }, { "concussion", "commozione cerebrale" },
            { "hypothermia", "ipotermia" }, { "hyperthermia", "ipertermia" }, { "suffocating", "soffocamento" },
            { "overdose", "overdose" }, { "intoxicated", "intossicato" }, { "stunned", "stordito" },
        };
        const std::string l = lower(TextUtils::trim(effect));
        for (const auto& [en, it] : kNames) {
            if (l == en) return it;
        }
        return splitCamel(TextUtils::trim(effect));
    }

    // "Rilevata ferita lieve - Braccio sinistro - Trattamento di livello 3 richiesto"
    // -> "Ferita lieve · braccio sinistro (trattamento di livello 3)".
    Event injury(const std::string& text, bool severe) {
        std::vector<std::string> parts;
        size_t start = 0;
        while (true) {
            const size_t dash = text.find(" - ", start);
            parts.push_back(TextUtils::trim(text.substr(start, dash == std::string::npos ? std::string::npos : dash - start)));
            if (dash == std::string::npos) break;
            start = dash + 3;
        }
        std::string what = parts[0];
        for (const char* p : { "Rilevata ", "Rilevato ", "rilevata ", "rilevato " }) {
            if (TextUtils::startsWith(what, p)) what = what.substr(strlen(p));
        }
        const std::string l = lower(what);
        if (l.find("injury detected") != std::string::npos) what = TextUtils::trim(what.substr(0, l.find("injury detected"))) + " injury";
        Event e;
        e.kind = Kind::Health;
        e.action = "injury";
        e.level = severe || lower(what).find("grave") != std::string::npos || lower(what).find("severe") != std::string::npos ? Level::Bad : Level::Warn;
        e.subject = capitalize(what);
        e.detail = parts.size() > 1 ? lowerFirst(parts[1]) : "";
        e.text = e.subject + (e.detail.empty() ? "" : " · " + e.detail);
        if (parts.size() > 2) e.text += " (" + lowerFirst(rest(parts[2], 0)) + ")";
        return e;
    }

    std::optional<Event> notification(const std::string& line) {
        const std::string key = "Added notification \"";
        const size_t at = line.find(key);
        if (at == std::string::npos || line.find("<SHUDEvent_OnNotification>") == std::string::npos) return std::nullopt;
        const size_t start = at + key.size();
        size_t end = line.find("\" [", start);
        if (end == std::string::npos) end = line.find('"', start);
        if (end == std::string::npos) return std::nullopt;
        std::string text = line.substr(start, end - start);
        while (!text.empty() && (text.back() == ' ' || text.back() == ':')) text.pop_back();
        text = TextUtils::trim(text);
        if (text.empty()) return std::nullopt;
        const std::string l = lower(text);
        for (const char* n : kNoise) {
            if (l.find(n) != std::string::npos) return std::nullopt;
        }

        Event e;
        e.missionId = guid(line, "MissionId: [");
        e.objectiveId = guid(line, "ObjectiveId: [");

        // Ferite e soccorsi
        if (TextUtils::startsWith(l, "rilevata ferita") || TextUtils::startsWith(l, "rilevato infortunio") || l.find("injury detected") != std::string::npos) {
            Event i = injury(text, TextUtils::startsWith(l, "rilevato infortunio"));
            i.missionId = e.missionId;
            return i;
        }
        if (l.find("servizi di emergenza") != std::string::npos || l.find("emergency services") != std::string::npos) {
            e.kind = Kind::Health;
            e.action = "emergency";
            e.level = Level::Bad;
            e.text = "Soccorsi in arrivo";
            return e;
        }
        // Un compagno in quantum verso di te
        for (const char* qt : { " ha iniziato a qt verso", " has started qt", " has started quantum travel", " is quantum traveling to" }) {
            const size_t p = l.find(qt);
            if (p != std::string::npos && p > 0) {
                e.kind = Kind::Party;
                e.action = "qt";
                e.subject = TextUtils::trim(text.substr(0, p));
                e.text = e.subject + " è in quantum verso di te";
                return e;
            }
        }
        // Giurisdizione in inglese: "Entered UEE Jurisdiction"
        if (TextUtils::startsWith(l, "entered ") && l.size() > 21 && l.compare(l.size() - 13, 13, " jurisdiction") == 0) {
            e.kind = Kind::Jurisdiction;
            e.action = "enter";
            e.subject = TextUtils::trim(text.substr(8, text.size() - 8 - 13));
            e.text = "Giurisdizione: " + e.subject;
            return e;
        }
        for (const Rule& r : kRules) {
            if (!TextUtils::startsWith(l, r.prefix)) continue;
            e.kind = r.kind;
            e.action = r.action;
            e.level = r.level;
            e.subject = rest(text, strlen(r.prefix));
            switch (r.kind) {
            case Kind::Contract: e.text = std::string(contractLabel(e.action)) + (e.subject.empty() ? "" : ": " + e.subject); break;
            case Kind::Objective: e.text = std::string(objectiveLabel(e.action)) + (e.subject.empty() ? "" : ": " + e.subject); break;
            case Kind::Zone: e.subject.clear(); e.text = zoneText(e.action); break;
            case Kind::Jurisdiction: e.text = "Giurisdizione: " + e.subject; break;
            case Kind::Journal: e.text = "Diario: " + e.subject; break;
            case Kind::Hangar: e.subject.clear(); e.text = "Hangar pronto"; break;
            case Kind::Blueprint: e.text = "Blueprint ricevuto: " + e.subject; break;
            default: e.text = text; break;
            }
            return e;
        }
        e.kind = Kind::Other;
        e.action = "notice";
        e.subject = text;
        e.text = text;
        return e;
    }
} // namespace

long long lineEpoch(const std::string& line) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    if (line.size() < 21 || line[0] != '<' || sscanf_s(line.c_str(), "<%4d-%2d-%2dT%2d:%2d:%2d", &y, &mo, &d, &h, &mi, &s) != 6) return 0;
    std::tm t{};
    t.tm_year = y - 1900;
    t.tm_mon = mo - 1;
    t.tm_mday = d;
    t.tm_hour = h;
    t.tm_min = mi;
    t.tm_sec = s;
    const time_t v = _mkgmtime(&t);
    return v < 0 ? 0 : (long long)v;
}

std::string shipName(const std::string& code) {
    std::string c = code;
    // Via il numero dell'istanza in fondo ("_749027674818").
    const size_t us = c.find_last_of('_');
    if (us != std::string::npos && us + 1 < c.size() && c.find_first_not_of("0123456789", us + 1) == std::string::npos) c = c.substr(0, us);
    static const std::pair<const char*, const char*> kMakers[] = {
        { "AEGS", "Aegis" }, { "ANVL", "Anvil" }, { "ARGO", "Argo" }, { "BANU", "Banu" }, { "CNOU", "C.O." }, { "CRUS", "Crusader" },
        { "DRAK", "Drake" }, { "ESPR", "Esperia" }, { "GAMA", "Gatac" }, { "GRIN", "Greycat" }, { "KRIG", "Kruger" }, { "MISC", "MISC" },
        { "MRAI", "Mirai" }, { "ORIG", "Origin" }, { "RSI", "RSI" }, { "TMBL", "Tumbril" }, { "VNCL", "Vanduul" }, { "XIAN", "Aopoa" },
        { "XNAA", "Aopoa" }, { "AOPO", "Aopoa" }, { "BEHR", "Behring" },
    };
    std::string out;
    size_t start = 0;
    bool first = true;
    while (start <= c.size()) {
        const size_t u = c.find('_', start);
        std::string part = c.substr(start, u == std::string::npos ? std::string::npos : u - start);
        if (first) {
            for (const auto& [k, v] : kMakers) {
                if (part == k) part = v;
            }
        }
        if (!part.empty()) out += (out.empty() ? "" : " ") + part;
        first = false;
        if (u == std::string::npos) break;
        start = u + 1;
    }
    return out.empty() ? code : out;
}

std::string locationName(const std::string& code) {
    static const std::pair<const char*, const char*> kKnown[] = {
        { "Stanton1_Lorville", "Lorville" }, { "Stanton2_Orison", "Orison" }, { "Stanton3_Area18", "Area18" },
        { "Stanton4_NewBabbage", "New Babbage" }, { "RR_HUR_LEO", "Everus Harbor" }, { "RR_CRU_LEO", "Seraphim Station" },
        { "RR_ARC_LEO", "Baijini Point" }, { "RR_MIC_LEO", "Port Tressler" }, { "TheCollectorAsteroid_Stanton4", "Wikelo (asteroide del Collezionista)" },
    };
    for (const auto& [k, v] : kKnown) {
        if (code == k) return v;
    }
    // Stazioni di Lagrange: "RR_CRU_L5" -> "CRU-L5"; portali: "RR_JP_StantonMagnus" -> "Portale Stanton–Magnus".
    if (TextUtils::startsWith(code, "RR_JP_")) {
        std::string s = splitCamel(code.substr(6));
        const size_t sp = s.find(' ');
        if (sp != std::string::npos) s = s.substr(0, sp) + "\xE2\x80\x93" + s.substr(sp + 1);
        return "Portale " + s;
    }
    if (TextUtils::startsWith(code, "RR_") && code.size() > 6) {
        std::string s = code.substr(3);
        std::replace(s.begin(), s.end(), '_', '-');
        return s;
    }
    std::string s = code;
    std::replace(s.begin(), s.end(), '_', ' ');
    return splitCamel(s);
}

std::optional<Event> parseLine(const std::string& rawLine, const std::string& handle) {
    std::string line = rawLine;
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
    if (line.size() < 30) return std::nullopt;
    std::optional<Event> out;

    if (line.find("Added notification \"") != std::string::npos) {
        out = notification(line);
    } else if (line.find("User Login Success") != std::string::npos) {
        const std::string h = bracketAfter(line, "Handle[");
        if (!h.empty()) {
            Event e;
            e.kind = Kind::Login;
            e.action = "login";
            e.subject = h;
            e.text = "Accesso al gioco come " + h;
            out = e;
        }
    } else if (line.find("releasing control token for '") != std::string::npos) {
        const std::string code = quotedAfter(line, "releasing control token for");
        if (!code.empty()) {
            Event e;
            e.kind = Kind::Ship;
            e.action = "left";
            e.subject = shipName(code);
            e.text = "Comandi lasciati: " + e.subject;
            out = e;
        }
    } else if (line.find("requested inventory for Location[") != std::string::npos) {
        const std::string who = bracketAfter(line, "Player[");
        const std::string code = bracketAfter(line, "Location[");
        if (!code.empty() && (handle.empty() || who.empty() || sameName(who, handle))) {
            Event e;
            e.kind = Kind::Location;
            e.action = "inventory";
            e.subject = locationName(code);
            e.text = "Posizione: " + e.subject;
            out = e;
        }
    } else if (line.find("of a status effect!") != std::string::npos) {
        // "Logged a start of a status effect! nickname: Nome, status effect: Dehydrated"
        const size_t n = line.find("nickname: ");
        const size_t c = n == std::string::npos ? n : line.find(',', n);
        const size_t f = line.find("status effect: ");
        if (n != std::string::npos && c != std::string::npos && f != std::string::npos) {
            const std::string who = TextUtils::trim(line.substr(n + 10, c - n - 10));
            std::string effect = line.substr(f + 15);
            const size_t endWord = effect.find_first_of("[\r\n");
            effect = TextUtils::trim(effect.substr(0, endWord));
            if (sameName(who, handle) && !effect.empty()) {
                const bool ended = line.find("Logged an end") != std::string::npos || line.find("Logged a end") != std::string::npos;
                Event e;
                e.kind = Kind::Health;
                e.action = ended ? "effect_end" : "effect";
                e.subject = effectName(effect);
                e.level = ended ? Level::Info : Level::Warn;
                e.text = ended ? "Passato: " + e.subject : "Stato: " + e.subject;
                out = e;
            }
        }
    } else if (line.find("<[ActorState] Dead>") != std::string::npos) {
        const std::string who = quotedAfter(line, "Actor ");
        if (sameName(who, handle)) {
            Event e;
            e.kind = Kind::Health;
            e.action = "death";
            e.level = Level::Bad;
            e.text = "Sei morto";
            out = e;
        }
    } else if (line.find("<Actor Death>") != std::string::npos) {
        // "CActor::Kill: 'Vittima' [id] in zone 'z' killed by 'Killer' [id] using 'Arma' [Class ...]"
        size_t pos = 0;
        const std::string victim = quotedAfter(line, "CActor::Kill:", &pos);
        const std::string killer = quotedAfter(line, "killed by", &pos);
        const std::string weapon = quotedAfter(line, "using", &pos);
        if (!victim.empty() && !handle.empty()) {
            Event e;
            e.kind = Kind::Combat;
            if (sameName(victim, handle)) {
                e.action = "killed";
                e.level = Level::Bad;
                e.subject = killer;
                e.text = sameName(killer, handle) ? "Sei morto" : "Ucciso da " + killer + (weapon.empty() ? "" : " (" + weapon + ")");
                out = e;
            } else if (sameName(killer, handle)) {
                e.action = "kill";
                e.level = Level::Good;
                e.subject = victim;
                e.text = "Hai eliminato " + victim;
                out = e;
            }
        }
    } else if (line.find("<Vehicle Destruction>") != std::string::npos) {
        size_t pos = 0;
        const std::string vehicle = quotedAfter(line, "Vehicle", &pos);
        const std::string driver = quotedAfter(line, "driven by", &pos);
        int from = -1, to = -1;
        const size_t lv = line.find("destroy level ");
        if (lv != std::string::npos) sscanf_s(line.c_str() + lv, "destroy level %d to %d", &from, &to);
        const std::string attacker = quotedAfter(line, "caused by", &pos);
        if (!vehicle.empty() && to >= 1 && !handle.empty() && (sameName(attacker, handle) || sameName(driver, handle))) {
            Event e;
            e.kind = Kind::Combat;
            e.action = to >= 2 ? "destroyed" : "disabled";
            e.subject = shipName(vehicle);
            if (sameName(driver, handle) && !sameName(attacker, handle)) {
                e.level = Level::Bad;
                e.text = std::string(to >= 2 ? "Nave distrutta" : "Nave disabilitata") + (attacker.empty() ? "" : " da " + attacker);
            } else {
                e.level = Level::Good;
                e.text = std::string(to >= 2 ? "Distrutta: " : "Disabilitata: ") + e.subject;
            }
            out = e;
        }
    }
    if (out) out->epoch = lineEpoch(line);
    return out;
}

bool apply(State& s, const Event& e, size_t maxRecent) {
    bool record = true;
    switch (e.kind) {
    case Kind::Login:
        s.handle = e.subject;
        break;
    case Kind::Contract: {
        auto same = [&](const Contract& c) { return e.missionId.empty() ? c.name == e.subject : c.id == e.missionId; };
        if (e.action == "accepted") {
            if (std::none_of(s.contracts.begin(), s.contracts.end(), same)) s.contracts.push_back({ e.missionId, e.subject, e.epoch });
            ++s.accepted;
        } else if (e.action == "completed" || e.action == "failed" || e.action == "abandoned") {
            s.contracts.erase(std::remove_if(s.contracts.begin(), s.contracts.end(), same), s.contracts.end());
            if (!e.missionId.empty()) {
                s.objectives.erase(std::remove_if(s.objectives.begin(), s.objectives.end(), [&](const Objective& o) { return o.missionId == e.missionId; }),
                                   s.objectives.end());
            }
            if (e.action == "completed") ++s.completed;
            if (e.action == "failed") ++s.failed;
        }
        break;
    }
    case Kind::Objective: {
        auto same = [&](const Objective& o) { return e.objectiveId.empty() ? o.text == e.subject : o.id == e.objectiveId; };
        if (e.action == "new") {
            if (std::none_of(s.objectives.begin(), s.objectives.end(), same)) s.objectives.push_back({ e.objectiveId, e.missionId, e.subject, e.epoch });
        } else {
            s.objectives.erase(std::remove_if(s.objectives.begin(), s.objectives.end(), same), s.objectives.end());
        }
        break;
    }
    case Kind::Zone:
        if (e.action == "armistice_in") s.armistice = 1;
        else if (e.action == "armistice_out") s.armistice = 0;
        else if (e.action == "monitored_in" || e.action == "monitored_up") s.monitored = 1;
        else if (e.action == "monitored_out" || e.action == "monitored_down") s.monitored = 0;
        else if (e.action == "private_in") s.privateProperty = 1;
        else if (e.action == "private_out") s.privateProperty = 0;
        break;
    case Kind::Jurisdiction:
        s.jurisdiction = e.subject;
        break;
    case Kind::Location:
        if (s.location == e.subject) record = false; // stessa località: niente di nuovo
        s.location = e.subject;
        break;
    case Kind::Ship:
        s.lastShip = e.subject;
        break;
    case Kind::Health:
        if (e.action == "effect_end") {
            s.health.erase(std::remove_if(s.health.begin(), s.health.end(), [&](const Mark& m) { return m.text == "Stato: " + e.subject; }), s.health.end());
        } else {
            s.health.push_back({ e.text, e.epoch });
            if (s.health.size() > 6) s.health.erase(s.health.begin());
        }
        break;
    default:
        break;
    }
    if (!record) return false;
    // La stessa notifica ripetuta subito (il gioco a volte la manda due volte): una sola.
    if (!s.recent.empty()) {
        const Event& last = s.recent.back();
        if (last.kind == e.kind && last.action == e.action && last.subject == e.subject && last.text == e.text && std::llabs(last.epoch - e.epoch) <= 3) {
            return false;
        }
    }
    s.recent.push_back(e);
    while (s.recent.size() > maxRecent) s.recent.pop_front();
    return true;
}

// --- Dove sta il file ---------------------------------------------------------------------------

std::wstring discover() {
    std::vector<std::wstring> roots;
    wchar_t buf[MAX_PATH];
    for (const wchar_t* var : { L"ProgramFiles", L"ProgramW6432", L"ProgramFiles(x86)" }) {
        const DWORD n = GetEnvironmentVariableW(var, buf, MAX_PATH);
        if (n > 0 && n < MAX_PATH) roots.push_back(std::wstring(buf) + L"\\Roberts Space Industries\\StarCitizen");
    }
    // Ogni disco fisso (mai quelli di rete o le schede: un controllo lì può bloccarsi per secondi).
    const DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        const std::wstring drive = std::wstring(1, (wchar_t)(L'A' + i)) + L":\\";
        if (GetDriveTypeW(drive.c_str()) != DRIVE_FIXED) continue;
        for (const wchar_t* sub : { L"Roberts Space Industries\\StarCitizen", L"Program Files\\Roberts Space Industries\\StarCitizen", L"StarCitizen",
                                    L"Games\\StarCitizen", L"Games\\Roberts Space Industries\\StarCitizen" }) {
            roots.push_back(drive + sub);
        }
    }
    std::wstring best;
    ULONGLONG bestTime = 0;
    for (const auto& root : roots) {
        for (const wchar_t* branch : { L"LIVE", L"PTU", L"EPTU", L"HOTFIX", L"TECH-PREVIEW" }) {
            const std::wstring path = root + L"\\" + branch + L"\\Game.log";
            WIN32_FILE_ATTRIBUTE_DATA a{};
            if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a) || (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            const ULONGLONG t = ((ULONGLONG)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime;
            if (t > bestTime) {
                bestTime = t;
                best = path;
            }
        }
    }
    return best;
}

// --- Lettura continua -----------------------------------------------------------------------------

void Watcher::start(const std::wstring& pathOverride, Callback onEvent, Changed onChanged) {
    stop();
    m_override = pathOverride;
    m_onEvent = std::move(onEvent);
    m_onChanged = std::move(onChanged);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_status = Status{};
        m_state = State{};
    }
    m_fileIndexHigh = m_fileIndexLow = 0;
    loadContracts();
    m_stop = false;
    m_running = true;
    m_thread = std::thread([this]() { loop(); });
}

void Watcher::stop() {
    m_stop = true;
    if (m_thread.joinable()) m_thread.join();
    m_running = false;
    closeFile();
}

Watcher::Status Watcher::status() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_status;
}

State Watcher::state() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_state;
}

void Watcher::forgetContract(const std::string& id) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto& c = m_state.contracts;
        c.erase(std::remove_if(c.begin(), c.end(), [&](const Contract& x) { return x.id == id || (x.id.empty() && x.name == id); }), c.end());
    }
    saveContracts();
    if (m_onChanged) m_onChanged();
}

void Watcher::clearRecent() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_state.recent.clear();
    }
    if (m_onChanged) m_onChanged();
}

namespace {
    std::wstring contractsPath() { return AppPaths::getAppDataDir() + L"\\missioni.json"; }
    constexpr long long kContractMemorySeconds = 3 * 24 * 3600; // un contratto dimenticato si toglie da solo dopo 3 giorni
}

void Watcher::loadContracts() {
    std::ifstream f(contractsPath(), std::ios::binary);
    if (!f) return;
    std::stringstream ss;
    ss << f.rdbuf();
    const json j = json::parse(ss.str(), nullptr, false);
    if (!j.is_array()) return;
    const long long now = (long long)std::time(nullptr);
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& c : j) {
        if (!c.is_object()) continue;
        Contract x{ c.value("id", std::string()), c.value("name", std::string()), c.value("since", 0LL) };
        if (x.name.empty() || (x.since > 0 && now - x.since > kContractMemorySeconds)) continue;
        m_state.contracts.push_back(x);
    }
}

void Watcher::saveContracts() {
    json j = json::array();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& c : m_state.contracts) j.push_back({ { "id", c.id }, { "name", c.name }, { "since", c.since } });
    }
    const std::wstring path = contractsPath(), tmp = path + L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return;
        f << j.dump(1);
    }
    MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

bool Watcher::openFile(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION info{};
    LARGE_INTEGER size{};
    if (!GetFileInformationByHandle(h, &info) || !GetFileSizeEx(h, &size)) {
        CloseHandle(h);
        return false;
    }
    const bool newSession = m_fileIndexHigh != 0 || m_fileIndexLow != 0; // c'era già un file: il gioco è ripartito
    m_file = h;
    m_fileIndexHigh = info.nFileIndexHigh;
    m_fileIndexLow = info.nFileIndexLow;
    m_offset = 0;
    m_catchUpEnd = size.QuadPart;
    m_partial.clear();
    m_live = m_catchUpEnd == 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_status.path = path;
        m_status.found = true;
        m_status.catchingUp = !m_live;
        m_status.bytes = 0;
        m_status.lines = 0;
        // Una sessione di gioco nuova: obiettivi, zone e salute di prima non valgono più (i contratti sì).
        m_state.objectives.clear();
        m_state.armistice = m_state.monitored = m_state.privateProperty = -1;
        m_state.jurisdiction.clear();
        m_state.location.clear();
        m_state.health.clear();
        m_state.accepted = m_state.completed = m_state.failed = 0;
        if (newSession) {
            Event e;
            e.kind = Kind::Other;
            e.action = "session";
            e.text = "Nuova sessione di gioco";
            e.epoch = (long long)std::time(nullptr);
            m_state.recent.push_back(e);
        }
    }
    Log::info("Diario di bordo: leggo " + TextUtils::wideToUtf8(path) + " (" + std::to_string(m_catchUpEnd / 1024) + " KB già scritti)");
    return true;
}

void Watcher::closeFile() {
    if (m_file) CloseHandle((HANDLE)m_file);
    m_file = nullptr;
}

void Watcher::handleLine(const std::string& line) {
    std::string handle;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_status.lines;
        handle = m_state.handle;
    }
    const std::optional<Event> e = parseLine(line, handle);
    if (!e) return;
    bool changed;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_status.events;
        if (e->epoch) m_status.lastEpoch = e->epoch;
        const size_t contractsBefore = m_state.contracts.size();
        changed = apply(m_state, *e);
        if (e->kind == Kind::Contract && m_state.contracts.size() != contractsBefore) m_contractsDirty = true;
    }
    if (m_onEvent) m_onEvent(*e, m_live);
    if (m_live && changed && m_onChanged) m_onChanged();
}

void Watcher::readNew() {
    HANDLE h = (HANDLE)m_file;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size)) return;
    if (size.QuadPart < m_offset) { // accorciato: si ricomincia da capo
        m_offset = 0;
        m_partial.clear();
    }
    char buf[64 * 1024];
    while (!m_stop && m_offset < size.QuadPart) {
        LARGE_INTEGER at;
        at.QuadPart = m_offset;
        SetFilePointerEx(h, at, nullptr, FILE_BEGIN);
        DWORD got = 0;
        if (!ReadFile(h, buf, sizeof(buf), &got, nullptr) || got == 0) break;
        m_offset += got;
        size_t start = 0;
        for (DWORD i = 0; i < got; ++i) {
            if (buf[i] != '\n') continue;
            m_partial.append(buf + start, i - start);
            handleLine(m_partial);
            m_partial.clear();
            start = i + 1;
        }
        m_partial.append(buf + start, got - start);
        if (m_partial.size() > 1 << 20) m_partial.clear(); // una "riga" da un mega non è una riga
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_status.bytes = m_offset;
        }
    }
    if (!m_live && m_offset >= m_catchUpEnd) {
        m_live = true;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_status.catchingUp = false;
        }
        m_contractsDirty = true;
        if (m_onChanged) m_onChanged();
    }
}

void Watcher::loop() {
    using Clock = std::chrono::steady_clock;
    auto lastLook = Clock::now() - std::chrono::hours(1);
    auto lastCheck = Clock::now();
    std::wstring path;
    while (!m_stop) {
        const auto now = Clock::now();
        if (!m_file) {
            if (now - lastLook >= std::chrono::seconds(8)) {
                lastLook = now;
                path = m_override.empty() ? discover() : m_override;
                const bool opened = !path.empty() && openFile(path);
                if (!opened) {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_status.found = false;
                    m_status.path = path;
                }
                if (m_onChanged) m_onChanged();
            }
        } else {
            readNew();
            if (now - lastCheck >= std::chrono::seconds(2)) {
                lastCheck = now;
                // Il gioco, ripartendo, sposta il vecchio Game.log in logbackups e ne crea uno nuovo:
                // il nostro handle punta ancora al vecchio. Si confronta l'identità del file.
                HANDLE probe = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                                           nullptr);
                bool replaced = false;
                if (probe != INVALID_HANDLE_VALUE) {
                    BY_HANDLE_FILE_INFORMATION info{};
                    if (GetFileInformationByHandle(probe, &info)) replaced = info.nFileIndexHigh != m_fileIndexHigh || info.nFileIndexLow != m_fileIndexLow;
                    CloseHandle(probe);
                }
                // Senza percorso scelto a mano: se un altro ramo (PTU...) è più recente, si passa a quello.
                if (!replaced && m_override.empty() && now - lastLook >= std::chrono::seconds(30)) {
                    lastLook = now;
                    const std::wstring newest = discover();
                    if (!newest.empty() && _wcsicmp(newest.c_str(), path.c_str()) != 0) {
                        path = newest;
                        replaced = true;
                    }
                }
                if (replaced) {
                    readNew(); // le ultime righe del vecchio
                    closeFile();
                    if (!openFile(path)) {
                        std::lock_guard<std::mutex> lock(m_mutex);
                        m_status.found = false;
                    }
                    if (m_onChanged) m_onChanged();
                }
            }
        }
        if (m_contractsDirty && m_live) {
            m_contractsDirty = false;
            saveContracts();
        }
        for (int i = 0; i < 5 && !m_stop; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(80));
    }
}

} // namespace GameLog
