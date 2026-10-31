#include "game_controller.h"
#include "app_paths.h"
#include "game_input.h"
#include "log.h"
#include "screen_ocr.h"
#include "text_utils.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace {
    using Clock = std::chrono::steady_clock;

    bool readFile(const std::wstring& path, std::string& out) {
        std::ifstream f(path, std::ios::binary);
        if (!f.is_open()) return false;
        std::stringstream ss;
        ss << f.rdbuf();
        out = ss.str();
        if (TextUtils::startsWith(out, "\xEF\xBB\xBF")) out.erase(0, 3);
        return true;
    }

    bool writeFile(const std::wstring& path, const std::string& content) {
        const std::wstring tmp = path + L".tmp";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            if (!f.is_open()) return false;
            f << content;
            if (!f.good()) return false;
        }
        return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    }

    FILETIME fileTime(const std::wstring& path) {
        WIN32_FILE_ATTRIBUTE_DATA info{};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)) return FILETIME{};
        return info.ftLastWriteTime;
    }

    std::string nowHHMM() {
        SYSTEMTIME t;
        GetLocalTime(&t);
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%02d:%02d", t.wHour, t.wMinute);
        return buf;
    }

    std::string replaceAll(std::string s, const std::string& from, const std::string& to) {
        size_t pos = 0;
        while (!from.empty() && (pos = s.find(from, pos)) != std::string::npos) {
            s.replace(pos, from.size(), to);
            pos += to.size();
        }
        return s;
    }

    bool cancelled(const std::atomic<bool>* cancel) {
        return cancel && cancel->load();
    }

    // La pausa tra un passo e l'altro delle azioni lunghe (stepDelayMs del profilo),
    // interrompibile; in modalità prova niente attese. false = interrotto.
    bool stepPause(const GameProfile& p, const std::atomic<bool>* cancel) {
        if (p.dryRun) return !cancelled(cancel);
        for (int waited = 0; waited < p.stepDelayMs; waited += 50) {
            if (cancelled(cancel)) return false;
            Sleep(std::min(50, p.stepDelayMs - waited));
        }
        return !cancelled(cancel);
    }

    long long elapsedMs(Clock::time_point since) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - since).count();
    }

    std::vector<std::string> texts(const ScreenOcr::Result& r) {
        std::vector<std::string> out;
        for (const auto& h : r.hits) out.push_back(h.text);
        return out;
    }

    // Il pulsante cercato: per etichetta esatta (quasi), se no per parola chiave.
    int findButton(const ScreenOcr::Result& r, const std::vector<std::string>& labels, const std::vector<std::string>& keywords) {
        const std::vector<std::string> t = texts(r);
        for (const auto& label : labels) {
            int i = Cmd::bestText(t, label, 0.75);
            if (i >= 0) return i;
        }
        for (size_t i = 0; i < t.size(); ++i) {
            const std::string n = Cmd::normalize(t[i]);
            if (n.size() < 4 || n.find("non valid") != std::string::npos || n.find("invalid") != std::string::npos) continue;
            for (const auto& k : keywords) {
                if (n.find(k) != std::string::npos) return (int)i;
            }
        }
        return -1;
    }

    std::string firstLines(const ScreenOcr::Result& r, int max) {
        std::string out;
        int n = 0;
        for (const auto& h : r.hits) {
            if (!h.line || h.text.size() < 2) continue;
            if (n++ >= max) break;
            out += (out.empty() ? "" : " | ") + h.text;
        }
        return out;
    }

    const std::vector<std::string> kRouteKeywords = { "rotta", "percorso", "route", "destinazione", "destination" };
    const std::vector<std::string> kCancelKeywords = { "annulla", "cancel", "clear" };
}

GameController::GameController() {
    m_profile = GameProfile::defaults();
}

std::wstring GameController::profilePath() const {
    return AppPaths::getAppDataDir() + L"\\star_citizen.json";
}

void GameController::load() {
    const std::wstring path = profilePath();
    std::string text;
    if (readFile(path, text)) {
        GameProfile p;
        std::string error;
        std::lock_guard<std::mutex> lock(m_mutex);
        if (GameProfile::parse(text, p, error)) {
            m_profile = p;
            m_profileError.clear();
            Log::info("Profilo di gioco: " + std::to_string(p.commands.size()) + " comandi, " + std::to_string(p.keys.size()) + " tasti");
        } else {
            m_profileError = error;
            Log::error("Profilo di gioco non valido (" + error + "): resta quello di prima");
        }
        m_profileTime = fileTime(path);
        m_statusTime = {};
        return;
    }
    // Primo avvio: si importa la vecchia Iris (C#), se c'è.
    GameProfile p = GameProfile::defaults();
    const std::wstring oldDir = AppPaths::getAppDataDir() + L"\\config";
    std::string settings, dictionary, error;
    const bool hasSettings = readFile(oldDir + L"\\settings.json", settings);
    const bool hasDictionary = readFile(oldDir + L"\\dictionary.json", dictionary);
    if ((hasSettings || hasDictionary) && GameProfile::importOldIris(settings, dictionary, p, error)) {
        Log::info("Profilo di gioco importato dalla vecchia Iris (" + std::to_string(p.commands.size()) + " comandi)");
    } else {
        Log::info("Profilo di gioco predefinito di Star Citizen");
    }
    saveProfile(p);
}

void GameController::reloadIfChanged() {
    const FILETIME t = fileTime(profilePath());
    bool changed;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        changed = CompareFileTime(&t, &m_profileTime) != 0;
    }
    if (changed) {
        Log::info("Profilo di gioco modificato: lo rileggo");
        load();
    }
}

GameProfile GameController::profile() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_profile;
}

std::string GameController::profileError() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_profileError;
}

bool GameController::saveProfile(const GameProfile& profile) {
    AppPaths::ensureDirectoriesExist();
    const std::wstring path = profilePath();
    std::string header =
        "// Profilo di gioco di Iris. Puoi modificarlo a mano (anche con commenti //): Iris lo rilegge quando lo salvi.\n"
        "// keys: nome logico -> tasto (\"N\", \"LAlt+N\", \"F2\"). commands: frasi -> azione. aliases: nome nel gioco -> come lo pronunci.\n";
    const bool ok = writeFile(path, header + profile.toJson() + "\n");
    std::lock_guard<std::mutex> lock(m_mutex);
    if (ok) {
        m_profile = profile;
        m_profileError.clear();
        m_profileTime = fileTime(path);
        m_statusTime = {};
    } else {
        Log::warn("Profilo di gioco non salvato");
    }
    return ok;
}

GameController::Status GameController::status() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_statusTime != Clock::time_point{} && Clock::now() - m_statusTime < std::chrono::seconds(1)) return m_status;
    Status s;
    HWND w = GameInput::findGameWindow(m_profile.processName, m_profile.windowTitle);
    s.running = w != nullptr;
    s.foreground = GameInput::isForeground(w);
    s.elevationProblem = w && GameInput::windowElevated(w) && !GameInput::selfElevated();
    if (!s.running) s.text = m_profile.name + " non è aperto.";
    else if (s.elevationProblem)
        s.text = m_profile.name + " è avviato come amministratore e Iris no: Windows scarterebbe i tasti. Chiudi Iris e riaprila con "
                 "tasto destro > Esegui come amministratore.";
    else s.text = m_profile.name + " è aperto e pronto: quando serve (comandi, calibrazione, Leggi) lo porto io in primo piano.";
    if (m_profile.dryRun) s.text += " Modalità prova: non premo tasti.";
    m_status = s;
    m_window = w;
    m_statusTime = Clock::now();
    return s;
}

bool GameController::gameInForeground() {
    const GameProfile p = profile();
    return GameInput::isForeground(GameInput::findGameWindow(p.processName, p.windowTitle));
}

Cmd::Match GameController::match(const std::string& text) {
    return Cmd::match(profile(), text);
}

GameController::Result GameController::execute(const Cmd::Match& match, const std::function<void(const std::string&)>& say,
                                               const std::atomic<bool>* cancel) {
    if (!match.found) return { false, "", "Non è un comando di gioco.", "" };
    if (match.command.intent == "repeat") {
        Cmd::Match last;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_hasLast) return { false, "Non ho niente da ripetere.", "Niente da ripetere.", "Niente da ripetere." };
            last = m_last;
        }
        return run(last.command, last.slots, say, cancel);
    }
    Result r = run(match.command, match.slots, say, cancel);
    if (r.ok && match.command.intent != "status" && match.command.intent != "toggle_dry_run") {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_last = match;
        m_hasLast = true;
    }
    return r;
}

GameController::Result GameController::executeById(const std::string& id, const std::string& destination,
                                                   const std::function<void(const std::string&)>& say, const std::atomic<bool>* cancel) {
    const GameProfile p = profile();
    Cmd::Match m;
    for (const auto& c : p.commands) {
        if (c.id() == id) {
            m.command = c;
            m.found = true;
            break;
        }
    }
    if (!m.found && TextUtils::startsWith(id, "press:")) {
        // Un tasto del profilo senza frasi: si preme lo stesso.
        m.command.intent = "press";
        m.command.args["key"] = id.substr(6);
        m.found = true;
    }
    if (!m.found) return { false, "Non conosco il comando " + id + ".", "Comando sconosciuto: " + id, "Comando sconosciuto." };
    if (m.command.intent == "set_route") {
        const std::string canonical = Cmd::resolveAlias(p, "destination", destination);
        m.slots["destination"] = canonical.empty() ? TextUtils::trim(destination) : canonical;
    }
    m.score = 1.0;
    return execute(m, say, cancel);
}

bool GameController::prepareGame(const GameProfile& p, HWND& window, std::string& why, std::string& brief) {
    window = GameInput::findGameWindow(p.processName, p.windowTitle);
    if (!window) {
        why = p.name + " non è aperto.";
        brief = "Gioco chiuso.";
        return false;
    }
    if (GameInput::windowElevated(window) && !GameInput::selfElevated()) {
        why = p.name + " è avviato come amministratore e Iris no: Windows scarterebbe i tasti. Riapri Iris con tasto destro, Esegui come "
                       "amministratore.";
        brief = "Serve amministratore.";
        return false;
    }
    if (p.dryRun) return true; // in prova non serve il primo piano
    if (!GameInput::focus(window)) {
        why = "Non riesco a portare " + p.name + " in primo piano.";
        brief = "Gioco non raggiungibile.";
        return false;
    }
    return true;
}

void GameController::pressLogical(const GameProfile& p, const std::string& logical, std::string& note) {
    auto it = p.keys.find(logical);
    std::string combo = it != p.keys.end() ? it->second : std::string();
    // Nel profilo si può anche scrivere il tasto direttamente ("press" con key = "N").
    if (combo.empty() && !GameInput::parseCombo(logical).empty()) combo = logical;
    if (combo.empty()) {
        note = "nessun tasto per \"" + logical + "\" nel profilo";
        return;
    }
    GameInput::pressCombo(combo, p.keyHoldMs, p.keyGapMs, p.dryRun);
    note = combo;
}

GameController::Result GameController::run(const GameCommand& command, const std::map<std::string, std::string>& slots,
                                           const std::function<void(const std::string&)>& say, const std::atomic<bool>* cancel) {
    GameProfile p = profile();
    auto slotValue = [&](const char* name) {
        auto it = slots.find(name);
        return it == slots.end() ? std::string() : it->second;
    };
    const std::string destination = slotValue("destination");
    const std::string sayText = replaceAll(command.say, "{destination}", destination);
    const std::string& intent = command.intent;
    auto arg = [&](const char* name) {
        auto it = command.args.find(name);
        return it == command.args.end() ? std::string() : it->second;
    };

    if (intent == "toggle_dry_run") {
        p.dryRun = !p.dryRun;
        saveProfile(p);
        const std::string msg = p.dryRun ? "Modalità prova attivata: non premo tasti." : "Modalità prova disattivata: ora premo i tasti davvero.";
        addAction(msg);
        return { true, msg, msg, p.dryRun ? "Prova attiva." : "Prova spenta." };
    }
    if (intent == "status") {
        const Status st = status();
        return { true, st.text, st.text, !st.running ? "Gioco chiuso." : st.elevationProblem ? "Serve amministratore." : "Tutto pronto." };
    }

    // Un messaggio in chat in corso finisce prima: i tasti non si mescolano.
    std::lock_guard<std::recursive_mutex> keys(GameInput::inputLock());
    HWND window = nullptr;
    std::string why, whyBrief;
    if (!prepareGame(p, window, why, whyBrief)) {
        addAction("Non eseguito: " + why);
        return { false, why, why, whyBrief };
    }

    const std::string prefix = p.dryRun ? "[prova] " : "";
    std::string note;
    Result r;
    if (intent == "press") {
        pressLogical(p, arg("key"), note);
        r.ok = note.find("nessun tasto") == std::string::npos;
        r.shown = prefix + (r.ok ? arg("key") + " (" + note + ")" : note);
        r.spoken = r.ok ? sayText : "Non ho un tasto per " + arg("key") + " nel profilo.";
        if (!r.ok) r.brief = "Tasto mancante.";
    } else if (intent == "hold") {
        const std::string logical = arg("key");
        auto it = p.keys.find(logical);
        const std::string combo = it != p.keys.end() && !it->second.empty() ? it->second : logical;
        int ms = 800;
        try { if (!arg("ms").empty()) ms = std::stoi(arg("ms")); } catch (...) {}
        r.ok = !GameInput::parseCombo(combo).empty();
        if (r.ok) GameInput::holdCombo(combo, ms, p.keyGapMs, p.dryRun);
        r.shown = prefix + logical + " tenuto " + std::to_string(ms) + " ms (" + combo + ")";
        r.spoken = r.ok ? sayText : "Non ho un tasto per " + logical + ".";
        if (!r.ok) r.brief = "Tasto mancante.";
    } else if (intent == "sequence") {
        std::string keys = arg("keys"), done;
        r.ok = true;
        size_t start = 0;
        while (start <= keys.size()) {
            if (cancelled(cancel)) return { false, "", "Sequenza interrotta." };
            size_t comma = keys.find(',', start);
            const std::string step = TextUtils::trim(keys.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
            if (!step.empty()) {
                if (!done.empty() && !stepPause(p, cancel)) return { false, "", "Sequenza interrotta." };
                pressLogical(p, step, note);
                if (note.find("nessun tasto") != std::string::npos) r.ok = false;
                done += (done.empty() ? "" : ", ") + step;
            }
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
        r.shown = prefix + "sequenza: " + done;
        r.spoken = sayText;
    } else if (intent == "quantum_travel") {
        pressLogical(p, "quantum_spool", note);
        if (!p.dryRun) Sleep(500);
        if (!stepPause(p, cancel)) return { false, "", "Salto quantico interrotto." };
        auto it = p.keys.find("quantum_engage");
        GameInput::holdCombo(it != p.keys.end() && !it->second.empty() ? it->second : "B", 900, p.keyGapMs, p.dryRun);
        r.ok = true;
        r.shown = prefix + "salto quantico";
        r.spoken = sayText;
    } else if (intent == "open_map" || intent == "close_map") {
        pressLogical(p, "map", note);
        r.ok = note.find("nessun tasto") == std::string::npos;
        r.shown = prefix + (intent == "open_map" ? "mappa aperta" : "mappa chiusa") + " (" + note + ")";
        r.spoken = sayText;
    } else if (intent == "set_route") {
        if (destination.empty()) return { false, "Non ho capito la destinazione.", "Destinazione mancante.", "Destinazione?" };
        if (say && !sayText.empty()) say(sayText);
        r = setRoute(p, destination, cancel);
    } else if (intent == "cancel_route") {
        if (say && !sayText.empty()) say(sayText);
        r = cancelRoute(p, cancel);
    } else {
        r = { false, "Non so ancora fare " + intent + ".", "Azione sconosciuta: " + intent, "Non so farlo." };
    }
    addAction(r.shown);
    return r;
}

RECT GameController::zoneRect(const RECT& game, const GameProfile& p, Zone zone) {
    const double gw = game.right - game.left, gh = game.bottom - game.top;
    const auto& m = p.starMap;
    std::array<double, 4> rel{};
    switch (zone) {
    case Zone::SearchBox: rel = { m.searchBoxX - m.searchBoxWidth / 2, m.searchBoxY - m.searchBoxHeight / 2, m.searchBoxWidth, m.searchBoxHeight }; break;
    case Zone::Results: rel = m.resultsRegion; break;
    case Zone::SetRoute: rel = m.setRouteRegion; break;
    case Zone::CancelRoute: rel = m.cancelRouteRegion; break;
    }
    rel[0] = std::clamp(rel[0], 0.0, 1.0);
    rel[1] = std::clamp(rel[1], 0.0, 1.0);
    return RECT{ game.left + (LONG)(gw * rel[0]), game.top + (LONG)(gh * rel[1]), game.left + (LONG)(gw * std::min(1.0, rel[0] + rel[2])),
                 game.top + (LONG)(gh * std::min(1.0, rel[1] + rel[3])) };
}

GameController::Result GameController::setRoute(const GameProfile& p, const std::string& destination, const std::atomic<bool>* cancel) {
    const auto& m = p.starMap;
    const auto& ocr = p.ocr;
    HWND window = GameInput::findGameWindow(p.processName, p.windowTitle);
    RECT game;
    if (!GameInput::clientRect(window, game)) return { false, p.name + " non è aperto.", "Gioco non trovato.", "Gioco chiuso." };
    std::string note;
    auto fail = [&](const std::string& msg, bool closeMap) {
        if (closeMap) pressLogical(p, "cancel", note);
        Log::warn("Rotta: " + msg);
        return Result{ false, msg, "Rotta per " + destination + " non impostata: " + msg, "Rotta non riuscita." };
    };

    // 1) La mappa: si aspetta che la casella di ricerca compaia (non un tempo fisso).
    pressLogical(p, "map", note);
    bool mapOpen = p.dryRun;
    for (auto t0 = Clock::now(); !mapOpen && elapsedMs(t0) < m.mapOpenTimeoutMs;) {
        if (cancelled(cancel)) return fail("Interrotto.", true);
        Sleep(250);
        const ScreenOcr::Result r = ScreenOcr::read(zoneRect(game, p, Zone::SearchBox), ocr, "");
        const std::string txt = Cmd::normalize(ScreenOcr::linesText(r));
        // Il segnaposto ("CERCA LOCALITÀ") o del testo già scritto: la casella c'è.
        if (txt.size() >= 3) mapOpen = true;
        for (const auto& ph : m.searchPlaceholders) {
            if (txt.find(Cmd::normalize(ph)) != std::string::npos) mapOpen = true;
        }
    }
    if (!mapOpen) Log::warn("Rotta: non vedo la casella di ricerca della mappa, provo lo stesso");
    if (!stepPause(p, cancel)) return fail("Interrotto.", true);

    // 2) Il nome nella ricerca, verificato con l'OCR: se il campo resta vuoto si riclicca e si riscrive.
    const POINT box{ game.left + (LONG)((game.right - game.left) * m.searchBoxX), game.top + (LONG)((game.bottom - game.top) * m.searchBoxY) };
    bool typed = p.dryRun;
    std::string seen;
    const int attempts = std::max(1, m.typingAttempts);
    for (int attempt = 1; attempt <= attempts && !typed; ++attempt) {
        if (cancelled(cancel)) return fail("Interrotto.", true);
        GameInput::click(box.x, box.y, m.smoothMouseMove, m.mouseMoveSteps, m.mouseMoveStepDelayMs, m.focusSettleMs, p.dryRun);
        if (attempt > 1) GameInput::click(box.x, box.y, false, 1, 0, m.focusSettleMs, p.dryRun); // secondo clic: il campo a volte non prende il fuoco
        if (!stepPause(p, cancel)) return fail("Interrotto.", true);
        if (m.clearFieldFirst) {
            GameInput::pressCombo("LCtrl+A", p.keyHoldMs, 60, p.dryRun);
            GameInput::pressCombo("Delete", p.keyHoldMs, 60, p.dryRun);
        }
        // All'ultimo tentativo si cambia metodo: caratteri Unicode invece degli scancode.
        if (attempt < attempts || attempts == 1) GameInput::typeScanCodes(destination, m.typeCharDelayMs, p.dryRun);
        else GameInput::typeUnicode(destination, 25, p.dryRun);
        Sleep(400);
        const ScreenOcr::Result r = ScreenOcr::read(zoneRect(game, p, Zone::SearchBox), ocr, "mappa-ricerca");
        seen = ScreenOcr::linesText(r);
        const std::string sn = Cmd::normalize(seen), dn = Cmd::normalize(destination);
        typed = !dn.empty() && (Cmd::similarity(sn, dn) > 0.45 || sn.find(dn.substr(0, std::min<size_t>(4, dn.size()))) != std::string::npos);
        if (!typed) Log::warn("Rotta, tentativo " + std::to_string(attempt) + ": nel campo leggo \"" + TextUtils::collapseSpaces(seen) + "\"");
    }
    if (!typed) {
        return fail("il campo di ricerca della mappa è rimasto vuoto, il gioco non ha ricevuto i tasti. Prova a ricalibrare la casella di "
                    "ricerca nella pagina Star Citizen.",
                    true);
    }
    if (p.dryRun) return { true, "Prova: avrei impostato la rotta per " + destination + ".", "[prova] rotta per " + destination, "Rotta di prova." };
    if (!stepPause(p, cancel)) return fail("Interrotto.", true);

    // 3) I risultati: si aspetta che compaiano, poi si clicca quello giusto.
    ScreenOcr::Result results;
    int best = -1;
    for (auto t0 = Clock::now(); best < 0 && elapsedMs(t0) < std::max(500, m.resultsTimeoutMs);) {
        if (cancelled(cancel)) return fail("Interrotto.", true);
        results = ScreenOcr::read(zoneRect(game, p, Zone::Results), ocr, "mappa-risultati");
        best = Cmd::bestText(texts(results), destination, ocr.matchThreshold);
        if (best < 0) Sleep(300);
    }
    if (best < 0) {
        const std::string readText = firstLines(results, 6);
        return fail("non trovo " + destination + " tra i risultati della mappa" + (readText.empty() ? "." : " (leggo: " + readText + ")."), true);
    }
    Log::info("Rotta: risultato scelto \"" + results.hits[best].text + "\"");
    const POINT hit = results.hits[best].center();
    GameInput::click(hit.x, hit.y, m.smoothMouseMove, m.mouseMoveSteps, m.mouseMoveStepDelayMs, m.clickSettleMs, false);

    // 4) Il pulsante "Imposta rotta".
    if (!stepPause(p, cancel)) return fail("Interrotto.", true);
    const ScreenOcr::Result buttons = ScreenOcr::read(zoneRect(game, p, Zone::SetRoute), ocr, "mappa-imposta");
    for (const auto& h : buttons.hits) {
        const std::string n = Cmd::normalize(h.text);
        if (n.find("non valid") != std::string::npos || n.find("invalid") != std::string::npos) {
            return fail("il gioco dice destinazione non valida: il clic sul risultato non l'ha selezionata. Ricalibra la zona dei risultati.", true);
        }
    }
    const int button = findButton(buttons, m.setRouteLabels, kRouteKeywords);
    if (button < 0) return fail("ho selezionato " + destination + " ma non trovo il pulsante Imposta rotta: confermalo tu.", false);
    const POINT bp = buttons.hits[button].center();
    GameInput::click(bp.x, bp.y, m.smoothMouseMove, m.mouseMoveSteps, m.mouseMoveStepDelayMs, 400, false);

    // 5) Verifica: se compare "Annulla", la rotta è attiva.
    if (!stepPause(p, cancel)) return fail("Interrotto.", false);
    const ScreenOcr::Result check = ScreenOcr::read(zoneRect(game, p, Zone::CancelRoute), ocr, "mappa-verifica");
    if (findButton(check, m.cancelRouteLabels, kCancelKeywords) >= 0) {
        if (m.closeMapAfterSetRoute && stepPause(p, cancel)) pressLogical(p, "cancel", note);
        const std::string msg = "Rotta impostata su " + destination + ".";
        Log::info("Rotta: " + msg);
        return { true, msg, msg, "Rotta impostata." };
    }
    const std::string msg = "Ho premuto Imposta rotta ma non vedo comparire Annulla: controlla in gioco che la rotta per " + destination + " sia attiva.";
    Log::warn("Rotta: " + msg);
    return { false, msg, msg, "Rotta da controllare." };
}

GameController::Result GameController::cancelRoute(const GameProfile& p, const std::atomic<bool>* cancel) {
    const auto& m = p.starMap;
    HWND window = GameInput::findGameWindow(p.processName, p.windowTitle);
    RECT game;
    if (!GameInput::clientRect(window, game)) return { false, p.name + " non è aperto.", "Gioco non trovato.", "Gioco chiuso." };
    std::string note;
    pressLogical(p, "map", note);
    if (p.dryRun) return { true, "Prova: avrei annullato la rotta.", "[prova] rotta annullata", "Annullo di prova." };
    ScreenOcr::Result r;
    int button = -1;
    for (auto t0 = Clock::now(); button < 0 && elapsedMs(t0) < m.mapOpenTimeoutMs;) {
        if (cancelled(cancel)) break;
        Sleep(300);
        r = ScreenOcr::read(zoneRect(game, p, Zone::CancelRoute), p.ocr, "mappa-annulla");
        button = findButton(r, m.cancelRouteLabels, kCancelKeywords);
    }
    if (button < 0) {
        pressLogical(p, "cancel", note);
        return { false, "Non vedo una rotta attiva da annullare.", "Nessuna rotta da annullare.", "Nessuna rotta." };
    }
    if (!stepPause(p, cancel)) {
        pressLogical(p, "cancel", note);
        return { false, "", "Annullamento della rotta interrotto." };
    }
    const POINT c = r.hits[button].center();
    GameInput::click(c.x, c.y, m.smoothMouseMove, m.mouseMoveSteps, m.mouseMoveStepDelayMs, 300, false);
    stepPause(p, nullptr); // la chiusura della mappa va fatta comunque
    pressLogical(p, "cancel", note);
    return { true, "Rotta annullata.", "Rotta annullata." };
}

void GameController::setDryRun(bool on) {
    GameProfile p = profile();
    if (p.dryRun == on) return;
    p.dryRun = on;
    saveProfile(p);
}

std::string GameController::zoneName(Zone zone) {
    switch (zone) {
    case Zone::SearchBox: return "casella di ricerca";
    case Zone::Results: return "zona dei risultati";
    case Zone::SetRoute: return "pulsante Imposta rotta";
    case Zone::CancelRoute: return "pulsante Annulla";
    }
    return "";
}

bool GameController::bringToFront(std::string& error) {
    const GameProfile p = profile();
    HWND window = GameInput::findGameWindow(p.processName, p.windowTitle);
    if (!window) {
        error = p.name + " non è aperto. Avvialo in finestra senza bordi.";
        return false;
    }
    if (!GameInput::focus(window)) {
        error = "non riesco a portare " + p.name + " in primo piano: cliccaci sopra tu.";
        return false;
    }
    return true;
}

std::string GameController::readZone(Zone zone) {
    const GameProfile p = profile();
    HWND window = GameInput::findGameWindow(p.processName, p.windowTitle);
    RECT game;
    if (!GameInput::clientRect(window, game)) return p.name + " non è aperto. Avvialo in finestra senza bordi.";
    // La cattura prende lo schermo così com'è: se sopra c'è Iris, leggerebbe Iris.
    std::string warning;
    if (!GameInput::isForeground(window)) {
        if (GameInput::focus(window)) Sleep(500); // il tempo di ridisegnarsi sopra le altre finestre
        else warning = "Attenzione: non sono riuscita a portare il gioco in primo piano, potrei aver letto un'altra finestra.\r\n";
    }
    GameInput::clientRect(window, game);
    const RECT r = zoneRect(game, p, zone);
    OcrSettings o = p.ocr;
    o.saveDebugShots = true;
    const ScreenOcr::Result res = ScreenOcr::read(r, o, "prova");
    std::string out = "Finestra del gioco " + std::to_string(game.right - game.left) + "x" + std::to_string(game.bottom - game.top) + ", " +
                      zoneName(zone) + " " + std::to_string(r.right - r.left) + "x" + std::to_string(r.bottom - r.top) + " pixel.\r\n" + warning;
    const std::string lines = ScreenOcr::linesText(res);
    if (!res.error.empty()) out += "Errore: " + res.error + "\r\n";
    if (lines.empty()) {
        out += "Nessun testo riconosciuto. Cause tipiche: gioco in schermo intero esclusivo (la cattura esce nera), HDR attivo, zona "
               "sbagliata, lingua OCR non installata in Windows.";
    } else {
        out += "Testo letto: " + replaceAll(lines, "\n", " | ");
    }
    if (!res.savedImage.empty()) out += "\r\nImmagine di controllo: " + res.savedImage;
    return out;
}

bool GameController::cursorRelative(double& rx, double& ry, std::string& error) {
    const GameProfile p = profile();
    HWND window = GameInput::findGameWindow(p.processName, p.windowTitle);
    RECT game;
    if (!GameInput::clientRect(window, game)) {
        error = p.name + " non è aperto.";
        return false;
    }
    const POINT c = GameInput::cursorPosition();
    rx = (c.x - game.left) / (double)(game.right - game.left);
    ry = (c.y - game.top) / (double)(game.bottom - game.top);
    if (rx < 0 || ry < 0 || rx > 1 || ry > 1) {
        error = "il mouse non è sopra la finestra del gioco";
        return false;
    }
    return true;
}

bool GameController::setZone(Zone zone, double x1, double y1, double x2, double y2) {
    GameProfile p = profile();
    auto& m = p.starMap;
    const std::array<double, 4> rect{ std::min(x1, x2), std::min(y1, y2), std::abs(x2 - x1), std::abs(y2 - y1) };
    switch (zone) {
    case Zone::SearchBox:
        m.searchBoxX = x1;
        m.searchBoxY = y1;
        break;
    case Zone::Results: m.resultsRegion = rect; break;
    case Zone::SetRoute: m.setRouteRegion = rect; break;
    case Zone::CancelRoute: m.cancelRouteRegion = rect; break;
    }
    if (zone != Zone::SearchBox && (rect[2] < 0.01 || rect[3] < 0.005)) return false; // rettangolo vuoto: i due angoli coincidono
    Log::info("Calibrazione: " + zoneName(zone) + " salvata");
    return saveProfile(p);
}

std::string GameController::toolDefinition() {
    const GameProfile p = profile();
    using json = nlohmann::json;
    json ids = json::array();
    std::string list;
    for (const auto& c : p.commands) {
        if (c.intent == "repeat" || c.intent == "toggle_dry_run") continue;
        const std::string id = c.id();
        if (std::find(ids.begin(), ids.end(), id) != ids.end()) continue;
        ids.push_back(id);
        list += (list.empty() ? "" : "; ") + id + " = " + (c.phrases.empty() ? id : c.phrases.front());
    }
    json f = { { "type", "function" },
               { "function",
                 { { "name", "game_command" },
                   { "description", "Run a command in " + p.name + " (the game is open): ship keys, set or cancel the route on the star map. "
                                    "Call it once per action. Commands: " + list },
                   { "parameters",
                     { { "type", "object" },
                       { "properties",
                         { { "command", { { "type", "string" }, { "enum", ids } } },
                           { "destination", { { "type", "string" }, { "description", "only for set_route: the place, as said by the user" } } } } },
                       { "required", { "command" } } } } } } };
    return f.dump();
}

void GameController::addAction(const std::string& text) {
    if (text.empty()) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_actions.push_back(nowHHMM() + "  " + text);
    while (m_actions.size() > 60) m_actions.pop_front();
}

std::vector<std::string> GameController::recentActions() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return std::vector<std::string>(m_actions.rbegin(), m_actions.rend());
}

std::vector<std::string> GameController::unrecognized() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return std::vector<std::string>(m_unrecognized.rbegin(), m_unrecognized.rend());
}

void GameController::noteUnrecognized(const std::string& text, const Cmd::Match& nearMiss) {
    char score[16];
    std::snprintf(score, sizeof(score), "%.2f", nearMiss.score);
    const std::string line = nowHHMM() + "  \"" + text + "\"" +
                             (nearMiss.command.intent.empty() ? std::string() : "  (il più vicino: \"" + nearMiss.phrase + "\", " + score + ")");
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_unrecognized.push_back(line);
        while (m_unrecognized.size() > 60) m_unrecognized.pop_front();
    }
    std::ofstream f(AppPaths::getAppDataDir() + L"\\frasi_non_capite.txt", std::ios::binary | std::ios::app);
    if (f.is_open()) f << line << "\r\n";
}
