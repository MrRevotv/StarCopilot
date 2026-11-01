#include "hud_host.h"
#include "axis_host.h"
#include "axis_overlay.h"
#include "quick_bar.h"
#include "../game_log.h"
#include "../hotkey_listener.h"
#include "../mining_overlay.h"
#include "../ocr_overlay.h"
#include "../overlay.h"
#include "../refinery.h"
#include "../text_utils.h"

#include <algorithm>
#include <ctime>

namespace {
    HudHost::Context g;
    bool g_started = false;
    bool g_hiddenAll = false;
    bool g_axisWasVisible = false;
    unsigned g_ticks = 0;

    std::wstring w(const std::string& s) { return TextUtils::utf8ToWide(s); }

    std::wstring upper(std::wstring s) {
        for (auto& c : s) c = (wchar_t)towupper(c);
        return s;
    }

    // "21:40" (ora locale) da secondi UTC; "ieri" / "12/10" se non è oggi.
    std::wstring clock(long long epoch) {
        if (epoch <= 0) return L"";
        const time_t t = (time_t)epoch, now = time(nullptr);
        tm local{}, today{};
        localtime_s(&local, &t);
        localtime_s(&today, &now);
        wchar_t buf[16];
        if (local.tm_yday == today.tm_yday && local.tm_year == today.tm_year) swprintf_s(buf, L"%02d:%02d", local.tm_hour, local.tm_min);
        else swprintf_s(buf, L"%02d/%02d", local.tm_mday, local.tm_mon + 1);
        return buf;
    }

    std::wstring keyText(const std::vector<TalkTrigger>& keys) {
        return keys.empty() ? L"" : upper(w(HotkeyListener::describeTrigger(keys.front())));
    }

    bool axisOpen() {
        const AxisOverlay::Status st = AxisOverlay::status();
        return st.open && !st.obsOnly;
    }

    void updateBar() {
        const AppSettings s = g.engine->settings();
        const Assistant::Status st = g.engine->status();
        std::wstring status;
        int tone = 6;
        switch (st.state) {
        case Assistant::State::Starting: status = L"AVVIO"; tone = 4; break;
        case Assistant::State::Idle: status = L"PRONTA"; tone = 6; break;
        case Assistant::State::Listening: status = L"TI ASCOLTO"; tone = 1; break;
        case Assistant::State::Thinking: status = L"CI PENSO"; tone = 2; break;
        case Assistant::State::Speaking: status = L"PARLO"; tone = 6; break;
        case Assistant::State::Error: status = L"ATTENZIONE"; tone = 3; break;
        }
        const bool miningReady = s.miningEnabled && s.miningRegionW > 0 && s.miningRegionH > 0;
        const bool chatReady = s.ocrRegionW > 0 && s.ocrRegionH > 0;
        const bool log = s.gameLogEnabled;
        const bool recording = g.engine->miningLog().active();
        std::vector<QuickBar::Button> b = {
            { "mining", L"MINING", miningReady ? L"Leggi adesso la signature dello scanner" : L"Mining: scegli prima la zona da leggere (pagina Mining)", false,
              miningReady },
            { "record", L"REGISTRO",
              recording ? L"Registro acceso (" + std::to_wstring(g.engine->miningLog().entries().size()) + L" rilevamenti): clic per chiuderlo e salvarlo sul desktop"
                        : L"Registra le scansioni del mining: alla fine luogo, qualità e file sul desktop",
              recording, true },
            { "chat", L"TRADUZIONE", chatReady ? (s.ocrEnabled ? L"Traduzione della chat accesa: clic per spegnerla" : L"Accendi la traduzione della chat letta a schermo")
                                               : L"Traduzione: scegli prima la zona della chat (pagina Traduzione chat)",
              s.ocrEnabled, chatReady },
            { "axis", L"ASSI", axisOpen() ? L"Chiudi l'overlay dell'Axis Visualizer" : L"Apri l'overlay dell'Axis Visualizer", axisOpen(), true },
            { "orb", L"SFERA", s.overlayEnabled ? L"Nascondi la sfera di Iris" : L"Mostra la sfera di Iris", s.overlayEnabled, true },
            { "missions", L"MISSIONI", log ? L"Contratti e obiettivi in corso (dal Game.log)" : L"Missioni: attiva prima il diario di bordo nella finestra di Iris",
              QuickBar::panelOpen("missions"), log },
            { "refinery", L"RAFFINERIA", L"Gli ordini della raffineria con il conto alla rovescia", QuickBar::panelOpen("refinery"), true },
            { "log", L"DIARIO", log ? L"Zona, salute e ultimi eventi (dal Game.log)" : L"Diario: attiva prima il diario di bordo nella finestra di Iris",
              QuickBar::panelOpen("log"), log },
        };
        std::wstring hint = keyText(s.quickBarTriggers);
        if (!hint.empty()) hint += L"  BARRA";
        const std::wstring all = keyText(s.hideAllTriggers);
        if (!all.empty()) hint += (hint.empty() ? L"" : L"   ·   ") + all + L"  NASCONDI TUTTO";
        QuickBar::setButtons(b, hint, status, tone);
    }

    void updateMissions() {
        const AppSettings s = g.engine->settings();
        std::vector<QuickBar::Row> rows;
        if (!s.gameLogEnabled) {
            rows.push_back({ L"Diario di bordo spento.", L"", 4 });
            rows.push_back({ L"Si attiva nella finestra di Iris, a tuo rischio.", L"", 4 });
        } else {
            const GameLog::State st = g.engine->gameLog().state();
            rows.push_back({ L"IN CORSO (" + std::to_wstring(st.contracts.size()) + L")", L"", 5 });
            if (st.contracts.empty()) rows.push_back({ L"Nessun contratto in corso.", L"", 4 });
            for (const auto& c : st.contracts) rows.push_back({ w(c.name), clock(c.since), 0 });
            if (!st.objectives.empty()) {
                rows.push_back({ L"OBIETTIVI", L"", 5 });
                for (const auto& o : st.objectives) rows.push_back({ w(o.text), L"", 6 });
            }
            rows.push_back({ L"SESSIONE", L"", 5 });
            rows.push_back({ L"Completati " + std::to_wstring(st.completed) + L"   ·   Falliti " + std::to_wstring(st.failed), L"",
                             st.failed > 0 ? 2 : st.completed > 0 ? 1 : 4 });
        }
        QuickBar::setPanel("missions", L"MISSIONI", rows);
    }

    void updateRefinery() {
        const long long now = (long long)time(nullptr);
        std::vector<QuickBar::Row> rows;
        const auto jobs = g.engine->refinery().jobs();
        if (jobs.empty()) {
            rows.push_back({ L"Nessun ordine in lavorazione.", L"", 4 });
            rows.push_back({ L"Aggiungili dalla pagina Mining o a voce:", L"", 4 });
            rows.push_back({ L"«Iris, raffineria tra 3 ore e 20»", L"", 4 });
        }
        for (const auto& j : jobs) {
            std::wstring name = w(j.place.empty() ? "Ordine" : j.place);
            if (!j.note.empty()) name += L" · " + w(j.note);
            const double total = (double)std::max(1LL, j.end - j.start);
            const bool done = j.done(now);
            rows.push_back({ name, done ? L"PRONTO" : w(Refinery::formatRemaining(j.end - now)), done ? 1 : 0, std::clamp((now - j.start) / total, 0.0, 1.0) });
        }
        QuickBar::setPanel("refinery", L"RAFFINERIA", rows);
    }

    void updateLog() {
        const AppSettings s = g.engine->settings();
        std::vector<QuickBar::Row> rows;
        if (!s.gameLogEnabled) {
            rows.push_back({ L"Diario di bordo spento.", L"", 4 });
            rows.push_back({ L"Si attiva nella finestra di Iris, a tuo rischio.", L"", 4 });
            QuickBar::setPanel("log", L"DIARIO DI BORDO", rows);
            return;
        }
        const GameLog::State st = g.engine->gameLog().state();
        const GameLog::Watcher::Status ws = g.engine->gameLog().status();
        rows.push_back({ L"STATO", L"", 5 });
        if (!ws.found) rows.push_back({ L"Game.log non trovato: il gioco non è partito?", L"", 2 });
        if (!st.jurisdiction.empty() || !st.location.empty()) {
            rows.push_back({ w(st.location.empty() ? st.jurisdiction : st.location), w(st.location.empty() ? "" : st.jurisdiction), 0 });
        }
        if (st.armistice >= 0) rows.push_back({ st.armistice ? L"Zona di armistizio" : L"Fuori dall'armistizio", L"", st.armistice ? 1 : 2 });
        if (st.monitored >= 0) rows.push_back({ st.monitored ? L"Spazio monitorato" : L"Spazio non monitorato", L"", st.monitored ? 1 : 2 });
        const long long now = (long long)time(nullptr);
        for (const auto& m : st.health) {
            if (m.epoch && now - m.epoch > 30 * 60) continue;
            rows.push_back({ w(m.text), clock(m.epoch), 2 });
        }
        if (!st.lastShip.empty()) rows.push_back({ L"Ultima nave: " + w(st.lastShip), L"", 0 });
        rows.push_back({ L"ULTIMI EVENTI", L"", 5 });
        int shown = 0;
        for (auto it = st.recent.rbegin(); it != st.recent.rend() && shown < 8; ++it) {
            if (it->kind == GameLog::Kind::Contract && it->action == "shared") continue; // le condivisioni del gruppo sono troppe
            const int tone = it->level == GameLog::Level::Good ? 1 : it->level == GameLog::Level::Warn ? 2 : it->level == GameLog::Level::Bad ? 3 : 0;
            rows.push_back({ w(it->text), clock(it->epoch), tone });
            ++shown;
        }
        if (shown == 0) rows.push_back({ ws.catchingUp ? L"Leggo la sessione di gioco..." : L"Ancora niente.", L"", 4 });
        QuickBar::setPanel("log", L"DIARIO DI BORDO", rows);
    }

    void updatePanels() {
        updateMissions();
        updateRefinery();
        updateLog();
    }

    void setPanelOpen(const std::string& id, bool open) {
        g.engine->updateSettings([&](AppSettings& s) {
            if (id == "missions") s.panelMissionsOpen = open;
            if (id == "refinery") s.panelRefineryOpen = open;
            if (id == "log") s.panelLogOpen = open;
        });
    }

    void openPanel(const std::string& id) {
        const AppSettings s = g.engine->settings();
        if (id == "missions") QuickBar::openPanel(id, s.panelMissionsX, s.panelMissionsY);
        if (id == "refinery") QuickBar::openPanel(id, s.panelRefineryX, s.panelRefineryY);
        if (id == "log") QuickBar::openPanel(id, s.panelLogX, s.panelLogY);
    }

    void clicked(const std::string& id) {
        Assistant& e = *g.engine;
        if (id == "settings") {
            if (g.showMain) g.showMain();
        } else if (id == "record") {
            if (!e.miningLog().active()) {
                e.miningLog().start((long long)time(nullptr));
                e.miningLogChanged();
                e.chime(true);
            } else if (g.askLogClose) {
                g.askLogClose();
            }
        } else if (id == "mining") {
            e.mining().scan("barra");
        } else if (id == "chat") {
            e.updateSettings([](AppSettings& s) { s.ocrEnabled = !s.ocrEnabled; });
            e.chime(e.settings().ocrEnabled);
        } else if (id == "orb") {
            e.updateSettings([](AppSettings& s) { s.overlayEnabled = !s.overlayEnabled; });
        } else if (id == "axis") {
            if (axisOpen()) {
                AxisOverlay::close();
            } else if (g.axis) {
                g.axis->overlayFromGame = false;
                AxisOverlay::open(g.axis->settings, false);
            }
        } else if (id == "missions" || id == "refinery" || id == "log") {
            const bool open = !QuickBar::panelOpen(id);
            if (open) {
                updatePanels();
                openPanel(id);
            } else {
                QuickBar::closePanel(id);
            }
            setPanelOpen(id, open);
        }
        updateBar();
    }
} // namespace

namespace HudHost {

void start(HINSTANCE instance, Context context) {
    g = std::move(context);
    QuickBar::Callbacks cb;
    cb.clicked = clicked;
    cb.moved = [](const std::string& id, int x, int y) {
        g.engine->updateSettings([&](AppSettings& s) {
            if (id == "missions") s.panelMissionsX = x, s.panelMissionsY = y;
            if (id == "refinery") s.panelRefineryX = x, s.panelRefineryY = y;
            if (id == "log") s.panelLogX = x, s.panelLogY = y;
        });
    };
    cb.closed = [](const std::string& id) {
        setPanelOpen(id, false);
        updateBar();
    };
    QuickBar::create(instance, cb);
    g_started = true;
    updateBar();
    updatePanels();
    // I pannelli che avevi lasciato aperti.
    const AppSettings s = g.engine->settings();
    if (s.panelMissionsOpen && s.gameLogEnabled) openPanel("missions");
    if (s.panelRefineryOpen) openPanel("refinery");
    if (s.panelLogOpen && s.gameLogEnabled) openPanel("log");
}

void stop() {
    if (!g_started) return;
    g_started = false;
    QuickBar::destroy();
}

void onEngineEvent(Assistant::Event e) {
    if (!g_started) return;
    switch (e) {
    case Assistant::Event::QuickBar: toggleBar(); break;
    case Assistant::Event::HideAll: toggleHideAll(); break;
    case Assistant::Event::GameLog:
        updateMissions();
        updateLog();
        updateBar();
        break;
    case Assistant::Event::Refinery: updateRefinery(); break;
    case Assistant::Event::MiningLog: updateBar(); break;
    case Assistant::Event::Settings: {
        const AppSettings s = g.engine->settings();
        // Diario spento: i suoi pannelli non servono.
        if (!s.gameLogEnabled) {
            QuickBar::closePanel("missions");
            QuickBar::closePanel("log");
        }
        updateBar();
        updateMissions();
        updateLog();
        break;
    }
    case Assistant::Event::Status: updateBar(); break;
    default: break;
    }
}

void onTimer() {
    if (!g_started) return;
    if (++g_ticks % 30 == 0) { // ogni secondo: il conto alla rovescia
        g.engine->tickRefinery();
        if (QuickBar::panelOpen("refinery")) updateRefinery();
        if (QuickBar::panelOpen("log") && g_ticks % 300 == 0) updateLog(); // le ferite vecchie spariscono
    }
}

void refresh() {
    if (!g_started) return;
    updateBar();
    updatePanels();
}

void toggleBar() {
    if (!g_started) return;
    updateBar();
    QuickBar::toggle();
}

void toggleHideAll() {
    if (!g_started) return;
    if (!g_hiddenAll) {
        g_hiddenAll = true;
        QuickBar::setHiddenAll(true);
        Overlay::setVisible(false);
        MiningOverlay::hide();
        MiningOverlay::setBorder(RECT{}, false);
        OcrOverlay::hide();
        const AxisOverlay::Status st = AxisOverlay::status();
        g_axisWasVisible = st.open && st.visible && !st.obsOnly;
        if (g_axisWasVisible) AxisOverlay::toggleVisible();
    } else {
        g_hiddenAll = false;
        QuickBar::setHiddenAll(false);
        if (g.applyOverlays) g.applyOverlays();
        if (g_axisWasVisible && AxisOverlay::status().open && !AxisOverlay::status().visible) AxisOverlay::toggleVisible();
        g_axisWasVisible = false;
    }
}

bool hiddenAll() { return g_hiddenAll; }

} // namespace HudHost
