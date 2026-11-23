#include "ship_toggle.h"
#include "command_matcher.h"
#include "text_utils.h"

#include <algorithm>
#include <set>
#include <sstream>

namespace ShipToggle {

    namespace {
        using ScTelemetry::Deploy;
        using ScTelemetry::ShipState;

        // Le frasi di ogni interruttore, con genere e numero giusti.
        struct Words {
            const char* name;                       // "armi"
            const char* on;                         // "accese"
            const char* off;                        // "spente"
            const char* alreadyOn;                  // "Le armi sono già accese."
            const char* alreadyOff;
            const char* briefAlreadyOn;             // "Già accese."
            const char* briefAlreadyOff;
            const char* nowOn;                      // "Armi accese."
            const char* nowOff;
            const char* briefNowOn;                 // nelle risposte brevi (una o due parole)
            const char* briefNowOff;
            const char* stillOn;                    // "le armi risultano ancora accese"
            const char* stillOff;
        };

        const Words& words(Field f) {
            static const Words none{ "", "", "", "", "", "", "", "", "", "", "", "", "" };
            static const Words lights{ "luci", "accese", "spente", "Le luci sono già accese.", "Le luci sono già spente.", "Già accese.",
                                       "Già spente.", "Luci accese.", "Luci spente.", "Luci accese.", "Luci spente.",
                                       "le luci risultano ancora accese", "le luci risultano ancora spente" };
            static const Words weapons{ "armi", "accese", "spente", "Le armi sono già accese.", "Le armi sono già spente.", "Già accese.",
                                        "Già spente.", "Armi accese.", "Armi spente.", "Armi accese.", "Armi spente.",
                                        "le armi risultano ancora accese", "le armi risultano ancora spente" };
            static const Words shields{ "scudi", "alzati", "abbassati", "Gli scudi sono già alzati.", "Gli scudi sono già abbassati.",
                                        "Già alzati.", "Già abbassati.", "Scudi alzati.", "Scudi abbassati.", "Scudi alzati.",
                                        "Scudi abbassati.", "gli scudi risultano ancora alzati", "gli scudi risultano ancora abbassati" };
            static const Words engines{ "motori", "accesi", "spenti", "I motori sono già accesi.", "I motori sono già spenti.", "Già accesi.",
                                        "Già spenti.", "Motori accesi.", "Motori spenti.", "Motori accesi.", "Motori spenti.",
                                        "i motori risultano ancora accesi", "i motori risultano ancora spenti" };
            static const Words power{ "alimentazione", "accesa", "spenta", "L'alimentazione è già accesa.", "L'alimentazione è già spenta.",
                                      "Già accesa.", "Già spenta.", "Alimentazione accesa.", "Alimentazione spenta.", "Alimentazione accesa.",
                                      "Alimentazione spenta.", "l'alimentazione risulta ancora accesa", "l'alimentazione risulta ancora spenta" };
            static const Words gear{ "carrello", "abbassato", "alzato", "Il carrello è già abbassato.", "Il carrello è già alzato.",
                                     "Già abbassato.", "Già alzato.", "Carrello abbassato.", "Carrello alzato.", "Carrello giù.", "Carrello su.",
                                     "il carrello risulta ancora abbassato", "il carrello risulta ancora alzato" };
            static const Words decoupled{ "modalità disaccoppiata", "attiva", "spenta", "Sei già in modalità disaccoppiata.",
                                          "Sei già in modalità accoppiata.", "Già disaccoppiato.", "Già accoppiato.", "Modalità disaccoppiata.",
                                          "Modalità accoppiata.", "Disaccoppiato.", "Accoppiato.", "risulti ancora in modalità disaccoppiata",
                                          "risulti ancora in modalità accoppiata" };
            static const Words cruise{ "cruise control", "acceso", "spento", "Il cruise control è già acceso.", "Il cruise control è già spento.",
                                       "Già acceso.", "Già spento.", "Cruise control acceso.", "Cruise control spento.", "Cruise acceso.",
                                       "Cruise spento.", "il cruise control risulta ancora acceso", "il cruise control risulta ancora spento" };
            switch (f) {
            case Field::Lights: return lights;
            case Field::Weapons: return weapons;
            case Field::Shields: return shields;
            case Field::Engines: return engines;
            case Field::Power: return power;
            case Field::Gear: return gear;
            case Field::Decoupled: return decoupled;
            case Field::Cruise: return cruise;
            case Field::None: break;
            }
            return none;
        }

        std::vector<std::string> wordsOf(const std::string& normalized) {
            std::vector<std::string> out;
            std::istringstream in(normalized);
            for (std::string w; in >> w;) out.push_back(w);
            return out;
        }

        bool has(const std::vector<std::string>& w, const std::set<std::string>& set) {
            return std::any_of(w.begin(), w.end(), [&](const std::string& x) { return set.count(x) > 0; });
        }

        bool startsAny(const std::vector<std::string>& w, std::initializer_list<const char*> stems) {
            for (const auto& x : w) {
                for (const char* s : stems) {
                    if (TextUtils::startsWith(x, s)) return true;
                }
            }
            return false;
        }

        // Il carrello va al contrario degli scudi: "abbassa il carrello" = fuori (On), "alza / tira su il carrello" = dentro.
        int gearDirection(const std::string& normalized) {
            static const std::set<std::string> down = { "abbassa", "abbassare", "abbassalo", "abbassato", "giu", "estrai", "estrarre",
                                                        "estrailo", "estratto", "fuori", "cala", "calare", "apri", "aprire", "down", "deploy" };
            static const std::set<std::string> up = { "alza", "alzare", "alzalo", "alzato", "su", "ritrai", "ritrarre", "ritira", "ritirare",
                                                      "ritiralo", "retrai", "rientra", "rientrare", "ritratto", "dentro", "chiudi", "chiudere",
                                                      "up", "retract" };
            const auto w = wordsOf(normalized);
            const bool d = has(w, down), u = has(w, up);
            return d == u ? 0 : d ? 1 : -1;
        }

        // Disaccoppiato (On) o accoppiato: l'oggetto della frase ("disaccoppiato", "coupled") e il verbo
        // ("disattiva la modalità coupled" = disaccoppiato).
        int decoupledDirection(const std::string& normalized) {
            const auto w = wordsOf(normalized);
            int object = 0;
            if (startsAny(w, { "disaccoppia", "decoupl" })) object = 1;
            else if (startsAny(w, { "accoppia", "coupl" })) object = -1;
            if (object == 0) return 0;
            return Cmd::polarity(normalized) < 0 ? -object : object;
        }

        // Luci, armi, scudi, motori, alimentazione, cruise: il verso dei verbi (Cmd::polarity) più qualche
        // parola che dice lo stato ("armi in linea", "luci spente", "scudi su").
        int plainDirection(Field f, const std::string& normalized) {
            static const std::set<std::string> onWords = { "accese", "acceso", "accesi", "accesa", "attive", "attivi", "attivo", "alzati" };
            static const std::set<std::string> offWords = { "spente", "spento", "spenti", "spenta", "abbassati", "disattivati", "disattivate" };
            const auto w = wordsOf(normalized);
            const int p = Cmd::polarity(normalized);
            bool on = p > 0 || has(w, onWords) || normalized.find("in linea") != std::string::npos;
            bool off = p < 0 || has(w, offWords);
            if (f == Field::Shields) {
                on = on || has(w, { "su" });
                off = off || has(w, { "giu" });
            }
            return on == off ? 0 : on ? 1 : -1;
        }

        int directionOf(Field f, const std::string& text) {
            const std::string n = Cmd::normalize(text);
            if (n.empty()) return 0;
            switch (f) {
            case Field::Gear: return gearDirection(n);
            case Field::Decoupled: return decoupledDirection(n);
            case Field::None: return 0;
            default: return plainDirection(f, n);
            }
        }

        std::string joinNames(const std::vector<std::string>& names) {
            std::string out;
            for (size_t i = 0; i < names.size(); ++i) out += (i == 0 ? "" : i + 1 == names.size() ? " e " : ", ") + names[i];
            return out;
        }

        // Quello che conta per dire "la nave si è assestata": tutti gli interruttori che si leggono.
        std::string signature(const ShipState& s) {
            std::string out;
            for (Field f : { Field::Lights, Field::Weapons, Field::Shields, Field::Engines, Field::Power, Field::Gear, Field::Decoupled, Field::Cruise }) {
                out += (char)('0' + (int)read(f, s));
            }
            return out;
        }

        // Dopo l'alimentazione: aspetta che cambi (fino a verifyMs), poi settleMs di frame uguali (al
        // massimo settleMaxMs in tutto). false = interrotto.
        bool settle(Env& env, Value before) {
            int waited = 0, stable = 0;
            bool changed = false;
            std::string last;
            while (waited < env.settleMaxMs) {
                if (!env.wait(env.pollMs)) return false;
                waited += env.pollMs;
                const auto st = env.state();
                if (!st) return true; // telemetria persa: si va avanti come sempre
                if (!changed) {
                    const Value v = read(Field::Power, *st);
                    if (v != before && v != Value::Unknown) {
                        changed = true;
                        last = signature(*st);
                    } else if (waited >= env.verifyMs) {
                        return true; // non è cambiata: lo dice il controllo alla fine
                    }
                    continue;
                }
                const std::string sig = signature(*st);
                if (sig == last) {
                    stable += env.pollMs;
                    if (stable >= env.settleMs) return true;
                } else {
                    last = sig;
                    stable = 0;
                }
            }
            return true;
        }
    }

    Field fieldFor(const std::string& logicalKey) {
        const std::string k = TextUtils::toLower(TextUtils::trim(logicalKey));
        if (k == "lights") return Field::Lights;
        if (k == "weapons") return Field::Weapons;
        if (k == "shields") return Field::Shields;
        if (k == "engines") return Field::Engines;
        if (k == "power") return Field::Power;
        if (k == "landing_gear") return Field::Gear;
        if (k == "decoupled") return Field::Decoupled;
        if (k == "cruise") return Field::Cruise;
        return Field::None;
    }

    Value read(Field field, const ShipState& s) {
        auto onOff = [](bool on) { return on ? Value::On : Value::Off; };
        switch (field) {
        case Field::Lights: return s.hasVehicle ? onOff(s.lightsOn) : Value::Unknown;
        case Field::Weapons: return s.hasPower ? onOff(s.weapons) : Value::Unknown;
        case Field::Shields: return s.hasPower ? onOff(s.shieldGenerators) : Value::Unknown;
        case Field::Engines: return s.hasPower ? onOff(s.engines) : Value::Unknown;
        case Field::Power: return s.hasPower ? onOff(s.powerPlants) : Value::Unknown;
        case Field::Gear:
            if (!s.hasFlightState) return Value::Unknown;
            switch (s.gear) {
            case Deploy::Deployed: return Value::On;
            case Deploy::Retracted: return Value::Off;
            case Deploy::Deploying: return Value::TurningOn;
            case Deploy::Retracting: return Value::TurningOff;
            default: return Value::Unknown;
            }
        // Solo se la nave ce l'ha (enabled): se no il valore non vuol dire niente.
        case Field::Decoupled: return s.hasFlightSettings && s.coupled.enabled ? onOff(!s.coupled.active) : Value::Unknown;
        case Field::Cruise: return s.hasFlightSettings && s.cruise.enabled ? onOff(s.cruise.active) : Value::Unknown;
        case Field::None: break;
        }
        return Value::Unknown;
    }

    int wanted(Field field, const std::string& heard, const std::string& phrase) {
        const int h = directionOf(field, heard);
        return h != 0 ? h : directionOf(field, phrase);
    }

    int wantedFromState(Field field, const std::string& state) {
        const std::string s = Cmd::normalize(state);
        if (s.empty() || field == Field::None) return 0;
        if (field == Field::Gear) {
            if (s == "down" || s == "deployed" || s == "deploy" || s == "lower" || s == "extended") return 1;
            if (s == "up" || s == "retracted" || s == "retract" || s == "raise") return -1;
            if (s == "on" || s == "off" || s == "true" || s == "false") return 0; // "on" = giù o su? meglio non indovinare
            return directionOf(field, s);
        }
        if (field == Field::Decoupled) {
            if (s == "decoupled") return 1;
            if (s == "coupled") return -1;
        }
        if (s == "on" || s == "true" || s == "1" || s == "up" || s == "enable" || s == "enabled" || s == "raise") return 1;
        if (s == "off" || s == "false" || s == "0" || s == "down" || s == "disable" || s == "disabled" || s == "lower") return -1;
        return directionOf(field, s);
    }

    int commandDirection(Field field, const std::vector<std::string>& phrases) {
        int found = 0;
        for (const auto& p : phrases) {
            const int d = directionOf(field, p);
            if (d == 0) continue;
            if (found != 0 && d != found) return 0;
            found = d;
        }
        return found;
    }

    std::vector<std::string> clauses(const std::string& request) {
        std::vector<std::string> out;
        std::string piece;
        auto flush = [&]() {
            // Dentro un pezzo: "e", "poi", "quindi" separano due azioni ("alza il carrello e accendi le luci").
            std::string current;
            for (const auto& w : wordsOf(Cmd::normalize(piece))) {
                if (w == "e" || w == "ed" || w == "poi" || w == "quindi" || w == "and" || w == "then") {
                    if (!current.empty()) out.push_back(current);
                    current.clear();
                } else {
                    current += (current.empty() ? "" : " ") + w;
                }
            }
            if (!current.empty()) out.push_back(current);
            piece.clear();
        };
        for (char c : request) {
            if (c == ',' || c == ';' || c == '.' || c == '!' || c == '?' || c == ':' || c == '\n') flush();
            else piece += c;
        }
        flush();
        return out;
    }

    std::string clauseAbout(Field field, const std::vector<std::string>& parts) {
        static const std::vector<std::pair<Field, std::vector<const char*>>> objects = {
            { Field::Lights, { "luc", "fari", "faro", "light" } },
            { Field::Weapons, { "armi", "arma", "disarm", "cannon", "weapon" } },
            { Field::Shields, { "scud", "shield" } },
            { Field::Engines, { "motor", "engine", "propulsor" } },
            { Field::Power, { "alimentazion", "energia", "corrente", "nave", "power" } },
            { Field::Gear, { "carrell", "gear" } },
            { Field::Decoupled, { "accoppia", "disaccoppia", "coupl", "decoupl" } },
            { Field::Cruise, { "cruise", "crociera" } },
        };
        for (const auto& [f, stems] : objects) {
            if (f != field) continue;
            for (const auto& part : parts) {
                for (const auto& w : wordsOf(part)) {
                    for (const char* stem : stems) {
                        if (TextUtils::startsWith(w, stem)) return part;
                    }
                }
            }
        }
        return {};
    }

    std::string name(Field field) { return words(field).name; }

    std::string valueText(Field field, Value value) {
        const Words& w = words(field);
        switch (value) {
        case Value::On: return w.on;
        case Value::Off: return w.off;
        case Value::TurningOn: return "in discesa";
        case Value::TurningOff: return "in salita";
        case Value::Unknown: break;
        }
        return "non si sa";
    }

    Plan plan(Field field, int want, Value now) {
        Plan p;
        if (field == Field::None || now == Value::Unknown) return p; // come sempre: si preme e basta
        const Words& w = words(field);
        if (want > 0) {
            if (now == Value::On) {
                p.press = false;
                p.spoken = w.alreadyOn;
                p.brief = w.briefAlreadyOn;
            } else if (now == Value::TurningOn) {
                p.press = false;
                p.spoken = "Il carrello sta già scendendo.";
                p.brief = "Sta già scendendo.";
            } else {
                p.target = Value::On;
            }
        } else if (want < 0) {
            if (now == Value::Off) {
                p.press = false;
                p.spoken = w.alreadyOff;
                p.brief = w.briefAlreadyOff;
            } else if (now == Value::TurningOff) {
                p.press = false;
                p.spoken = "Il carrello sta già salendo.";
                p.brief = "Sta già salendo.";
            } else {
                p.target = Value::Off;
            }
        } else {
            // Senza verso: si preme come sempre, e deve cambiare (il carrello in movimento torna indietro).
            p.target = (now == Value::On || now == Value::TurningOn) ? Value::Off : Value::On;
        }
        return p;
    }

    bool reached(Value target, Value now) {
        if (target == Value::On) return now == Value::On || now == Value::TurningOn;
        if (target == Value::Off) return now == Value::Off || now == Value::TurningOff;
        return true;
    }

    std::string doneText(Field field, Value now) {
        const Words& w = words(field);
        switch (now) {
        case Value::On: return w.nowOn;
        case Value::Off: return w.nowOff;
        case Value::TurningOn: return "Carrello in discesa.";
        case Value::TurningOff: return "Carrello in salita.";
        case Value::Unknown: break;
        }
        return "";
    }

    std::string doneBrief(Field field, Value now) {
        const Words& w = words(field);
        switch (now) {
        case Value::On:
        case Value::TurningOn: return w.briefNowOn;
        case Value::Off:
        case Value::TurningOff: return w.briefNowOff;
        case Value::Unknown: break;
        }
        return "";
    }

    std::string stillText(Field field, Value now) {
        const Words& w = words(field);
        switch (now) {
        case Value::On: return w.stillOn;
        case Value::Off: return w.stillOff;
        case Value::TurningOn: return "il carrello sta ancora scendendo";
        case Value::TurningOff: return "il carrello sta ancora salendo";
        case Value::Unknown: break;
        }
        return std::string("non si legge più com'è (") + w.name + ")";
    }

    std::string notChangedText(Field field, Value now, const std::string& combo) {
        return "Ho premuto " + combo + ", ma " + stillText(field, now) + ".";
    }

    Outcome afterPress(Field field, Value target, const std::string& combo, Env& env) {
        Outcome o;
        if (field == Field::None || target == Value::Unknown || env.dryRun) return o;
        Value last = Value::Unknown;
        for (int waited = 0;; waited += env.pollMs) {
            const auto st = env.state();
            if (!st) return o; // telemetria persa: come sempre
            last = read(field, *st);
            if (last == Value::Unknown) return o;
            if (reached(target, last)) {
                o.used = true;
                o.spoken = doneText(field, last);
                o.brief = doneBrief(field, last);
                o.shown = "telemetria: " + name(field) + " " + valueText(field, last);
                return o;
            }
            if (waited >= env.verifyMs) break;
            if (!env.wait(env.pollMs)) return o; // interrotto: niente verdetto
        }
        o.used = true;
        o.ok = false;
        o.spoken = notChangedText(field, last, combo);
        o.brief = "Non è cambiato.";
        o.shown = "telemetria: non è cambiato, " + stillText(field, last);
        return o;
    }

    std::vector<std::string> splitSteps(const std::string& keys) {
        std::vector<std::string> out;
        size_t start = 0;
        while (start <= keys.size()) {
            const size_t comma = keys.find(',', start);
            const std::string step = TextUtils::trim(keys.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
            if (!step.empty()) out.push_back(step);
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
        return out;
    }

    bool nothingToPress(const std::vector<std::string>& steps, const std::function<int(Field)>& wantFor, const ShipState& state) {
        if (steps.empty()) return false;
        for (const auto& step : steps) {
            const Field f = fieldFor(step);
            if (f == Field::None) return false;
            if (plan(f, wantFor(f), read(f, state)).press) return false;
        }
        return true;
    }

    Outcome sequence(const std::vector<std::string>& steps, const std::function<int(Field)>& wantFor, const std::string& sayText, Env& env) {
        Outcome o;
        o.used = true;
        struct Pressed {
            std::string logical, combo;
            Field field;
            Value target;
            bool ok;
        };
        std::vector<Pressed> pressed;
        std::vector<std::string> skippedNames, skippedKeys;
        bool justSettled = false;
        for (const auto& step : steps) {
            if (env.cancelled && env.cancelled()) {
                o.interrupted = true;
                return o;
            }
            const Field f = fieldFor(step);
            const int want = f == Field::None ? 0 : wantFor(f);
            const auto st = f == Field::None ? std::nullopt : env.state();
            const Value now = st ? read(f, *st) : Value::Unknown;
            const Plan pl = plan(f, want, now);
            if (!pl.press) {
                skippedNames.push_back(name(f));
                skippedKeys.push_back(step);
                continue;
            }
            // La pausa tra un tasto e l'altro, come sempre (dopo l'alimentazione si è già aspettato).
            if (!pressed.empty() && !justSettled && !env.pause()) {
                o.interrupted = true;
                return o;
            }
            justSettled = false;
            bool ok = true;
            const std::string combo = env.press(step, ok);
            if (!ok) o.ok = false; // tasto mancante nel profilo: come prima
            pressed.push_back({ step, combo, f, ok ? pl.target : Value::Unknown, ok });
            // Riaccesa (o spenta) l'alimentazione, i sottosistemi possono cambiare da soli: prima di
            // guardare i passi dopo si aspetta che la nave si assesti.
            if (f == Field::Power && ok && now != Value::Unknown && !env.dryRun) {
                if (!settle(env, now)) {
                    o.interrupted = true;
                    return o;
                }
                justSettled = true;
            }
        }

        // Alla fine: tutto quello premuto è cambiato davvero?
        std::vector<std::string> problems;
        const bool anyTarget = std::any_of(pressed.begin(), pressed.end(), [](const Pressed& p) { return p.target != Value::Unknown; });
        if (!env.dryRun && anyTarget) {
            for (int waited = 0;; waited += env.pollMs) {
                const auto st = env.state();
                if (!st) break; // telemetria persa: non si sa, come sempre
                std::vector<std::string> missing;
                for (const auto& p : pressed) {
                    if (p.target == Value::Unknown) continue;
                    const Value v = read(p.field, *st);
                    if (v != Value::Unknown && !reached(p.target, v)) missing.push_back(stillText(p.field, v));
                }
                if (missing.empty()) break;
                if (waited >= env.verifyMs) {
                    problems = missing;
                    break;
                }
                if (!env.wait(env.pollMs)) break;
            }
        }

        // Le risposte.
        std::string keys;
        std::vector<std::string> combos;
        for (const auto& p : pressed) {
            keys += (keys.empty() ? "" : ", ") + p.logical + (p.combo.empty() ? "" : " (" + p.combo + ")");
            if (p.ok) combos.push_back(p.combo);
        }
        o.shown = "sequenza: " + (pressed.empty() ? std::string("niente da premere") : keys);
        if (!skippedKeys.empty()) {
            std::string list;
            for (const auto& k : skippedKeys) list += (list.empty() ? "" : ", ") + k;
            o.shown += "; già a posto (telemetria): " + list;
        }
        if (!problems.empty()) {
            o.ok = false;
            o.spoken = "Ho premuto " + joinNames(combos) + ", ma " + joinNames(problems) + ".";
            o.brief = "Non è cambiato.";
            o.shown += "; non è cambiato (telemetria): " + joinNames(problems);
        } else if (pressed.empty()) {
            o.spoken = "È già tutto a posto: " + joinNames(skippedNames) + ".";
            o.brief = "Già a posto.";
        } else if (!skippedNames.empty()) {
            o.spoken = (sayText.empty() ? std::string("Fatto.") : sayText) + " Già a posto: " + joinNames(skippedNames) + ".";
            o.brief = "Fatto.";
        } else {
            o.spoken = sayText;
        }
        return o;
    }

} // namespace ShipToggle
