#include "web_bridge.h"
#include "axis_host.h"
#include "axis_overlay.h"
#include "axis_panes.h"
#include "../axis_input.h"
#include "../autostart.h"
#include "../game_log.h"
#include "../mining_log.h"
#include "../refinery.h"
#include "hud_host.h"
#include "../app_paths.h"
#include "../assistant.h"
#include "../audio_capture.h"
#include "../game_input.h"
#include "../glossary.h"
#include "../hotkey_listener.h"
#include "../joystick_input.h"
#include "../languages.h"
#include "../log.h"
#include "../mining_decoder.h"
#include "../mining_overlay.h"
#include "../ocr_engine.h"
#include "../ocr_overlay.h"
#include "../overlay.h"
#include "../ppocr.h"
#include "../region_selector.h"
#include "../text_utils.h"
#include "../tts_engine.h"
#include "../updater.h"
#include "../voice_effect.h"

#include <commdlg.h>
#include <tlhelp32.h>
#include <dwmapi.h>
#include <shellapi.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <map>
#include <set>

using json = nlohmann::json;

namespace {
    std::wstring w(const std::string& s) { return TextUtils::utf8ToWide(s); }
    std::string u8(const std::wstring& s) { return TextUtils::wideToUtf8(s); }

    std::string dump(const json& j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); }

    void openFile(HWND owner, const std::wstring& path, bool notepad) {
        if (notepad) {
            const std::wstring args = L"\"" + path + L"\"";
            ShellExecuteW(owner, L"open", L"notepad.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
        } else {
            ShellExecuteW(owner, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
    }

    void copyToClipboard(HWND owner, const std::wstring& text) {
        if (!OpenClipboard(owner)) return;
        EmptyClipboard();
        HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
        if (mem) {
            memcpy(GlobalLock(mem), text.c_str(), (text.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(mem);
            if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
        }
        CloseClipboard();
    }

    const char* stateId(Assistant::State s) {
        switch (s) {
        case Assistant::State::Starting: return "starting";
        case Assistant::State::Idle: return "idle";
        case Assistant::State::Listening: return "listening";
        case Assistant::State::Thinking: return "thinking";
        case Assistant::State::Speaking: return "speaking";
        case Assistant::State::Error: return "error";
        }
        return "idle";
    }

    const char* entryKind(Assistant::Entry::Kind k) {
        switch (k) {
        case Assistant::Entry::Kind::User: return "user";
        case Assistant::Entry::Kind::Iris: return "iris";
        case Assistant::Entry::Kind::Tool: return "tool";
        case Assistant::Entry::Kind::Info: return "info";
        }
        return "info";
    }

    const char* updatePhase(UpdateManager::Phase p) {
        switch (p) {
        case UpdateManager::Phase::Idle: return "idle";
        case UpdateManager::Phase::Checking: return "checking";
        case UpdateManager::Phase::UpToDate: return "uptodate";
        case UpdateManager::Phase::Downloading: return "downloading";
        case UpdateManager::Phase::Ready: return "ready";
        case UpdateManager::Phase::Error: return "error";
        case UpdateManager::Phase::Disabled: return "disabled";
        }
        return "idle";
    }

    // Un tasto per la pagina: come si chiama, se il joystick non c'è, se vale anche per l'altro gruppo.
    json describeKeys(const std::vector<TalkTrigger>& keys, const std::vector<TalkTrigger>* others) {
        json out = json::array();
        const auto connected = JoystickInput::devices();
        for (const auto& t : keys) {
            const bool missing = t.type == TalkTrigger::Type::Joystick &&
                                 std::none_of(connected.begin(), connected.end(), [&](const auto& d) { return d.id == t.joystickId; });
            const bool shared = others && std::find(others->begin(), others->end(), t) != others->end();
            out.push_back({ { "label", HotkeyListener::describeTrigger(t) }, { "missing", missing }, { "shared", shared } });
        }
        return out;
    }

    // I tasti a pressione singola: 0-3 il mining, 4 la barra rapida, 5 nascondi tutto.
    std::vector<TalkTrigger>& miningKeys(AppSettings& s, int row) {
        switch (row) {
        case 0: return s.miningScanTriggers;
        case 1: return s.miningManualTriggers;
        case 2: return s.miningBorderTriggers;
        case 4: return s.quickBarTriggers;
        case 5: return s.hideAllTriggers;
        default: return s.miningToggleTriggers;
        }
    }

    std::string describeReading(const MiningScanner::Reading& r) {
        if (!r.valid) return "Nessuna lettura ancora.";
        std::string text = r.time + " (" + r.source + "): ";
        if (r.decoded.hasValue) text += Mining::summary(r.decoded);
        else text += r.error.empty() ? std::string("nessun numero") : r.error;
        if (!r.decoded.note.empty()) text += " — " + r.decoded.note;
        if (!r.ocrText.empty() && r.source != "voce" && r.source != "manuale") {
            text += "   [" + (r.engine.empty() ? std::string("OCR") : r.engine);
            if (r.confidence >= 0) text += " " + std::to_string((int)std::lround(r.confidence * 100)) + "%";
            text += ": \"" + r.ocrText + "\"]";
        }
        if (r.durationMs > 0) text += "  " + std::to_string(r.durationMs) + " ms";
        return text;
    }

    const char* readingKind(const MiningScanner::Reading& r) {
        if (!r.valid) return "none";
        if (r.decoded.matches.empty()) return "bad";
        return r.decoded.corrected ? "warn" : "ok";
    }

    bool waitF8(const std::atomic<bool>& cancel) {
        while (GetAsyncKeyState(VK_F8) & 0x8000) {
            if (cancel) return false;
            Sleep(20);
        }
        for (int i = 0; i < 1500; ++i) {
            if (cancel) return false;
            if (GetAsyncKeyState(VK_F8) & 0x8000) return true;
            Sleep(20);
        }
        return false;
    }

    std::string str(const json& m, const char* key) {
        auto it = m.find(key);
        return it != m.end() && it->is_string() ? TextUtils::trim(it->get<std::string>()) : std::string();
    }

    int integer(const json& m, const char* key, int fallback = 0) {
        auto it = m.find(key);
        if (it == m.end()) return fallback;
        if (it->is_number()) return (int)std::lround(it->get<double>());
        if (it->is_string()) return std::atoi(it->get<std::string>().c_str());
        return fallback;
    }
}

WebBridge::WebBridge(Assistant& engine, UpdateManager& updates, AxisHost& axis, HWND window, Send send)
    : m_engine(engine), m_updates(updates), m_window(window), m_send(std::move(send)), m_axis(axis) {}

WebBridge::~WebBridge() {
    stopWork();
    if (m_gameKeyThread.joinable()) {
        m_gameKeyCancel = true;
        m_gameKeyThread.join();
    }
    if (m_captureThread.joinable()) {
        m_captureCancel = true;
        m_captureThread.join();
    }
    if (!m_captureTarget.empty()) m_engine.setTalkPaused(false);
}

// --- Verso la pagina ----------------------------------------------------------------

void WebBridge::send(const char* type, const std::string& valueJson) {
    if (!m_ready || !m_send) return;
    m_send(std::string("{\"t\":\"") + type + "\",\"v\":" + valueJson + "}");
}

void WebBridge::postJson(const std::string& text) {
    auto* copy = new std::string(text);
    if (!PostMessageW(m_window, WM_APP_BRIDGE_SEND, 0, (LPARAM)copy)) delete copy;
}

void WebBridge::toast(const std::string& text, bool ok) { send("toast", dump({ { "text", text }, { "ok", ok } })); }

void WebBridge::sendAll() {
    sendLists();
    sendSettings();
    sendStatus();
    sendConversation();
    sendChatHistory();
    sendChatNotice();
    sendFiltered();
    sendMining();
    sendMiningData();
    sendGame();
    sendUpdate();
    sendOcrInfo();
    sendAxis();
    sendGameLog();
    sendRefinery();
    sendGameCommands(true);
    sendMiningLog();
}

void WebBridge::sendSettings() {
    const AppSettings s = m_engine.settings();
    json j;
    j["userName"] = s.userName;
    // Ascolto
    j["wakeWordEnabled"] = s.wakeWordEnabled;
    j["triggers"] = describeKeys(s.triggers, &s.chatTriggers);
    j["micName"] = s.deviceNames.empty() ? "" : s.deviceNames.front();
    j["micGain"] = s.micGain;
    j["vadThreshold"] = s.vadThreshold;
    j["endSilenceMs"] = s.endSilenceMs;
    j["followUpSeconds"] = s.followUpSeconds;
    j["replyStyle"] = s.replyStyle;
    // Voce
    j["speechEnabled"] = s.speechEnabled;
    j["voiceEngine"] = s.voiceEngine;
    j["voiceName"] = s.voiceName;
    j["voiceEffect"] = s.voiceEffect;
    j["outputDeviceName"] = s.outputDeviceName;
    j["speechRate"] = s.speechRate;
    // Cervello, sfera, aggiornamenti
    j["gpuMemoryLimitMb"] = s.gpuMemoryLimitMb;
    j["gameEnabled"] = s.gameEnabled;
    j["overlayEnabled"] = s.overlayEnabled;
    j["overlayOpacity"] = s.overlayOpacity;
    j["overlaySize"] = s.overlaySize;
    j["autoUpdate"] = s.autoUpdate;
    j["memoryCount"] = m_engine.memory().size();
    // Chat di gioco
    j["chatEnabled"] = s.chatEnabled;
    j["chatTriggers"] = describeKeys(s.chatTriggers, &s.triggers);
    j["sourceLanguageIndex"] = s.sourceLanguageIndex;
    j["targetLanguageIndex"] = s.targetLanguageIndex;
    j["autoSend"] = s.autoSend;
    j["gamingTone"] = s.gamingTone;
    j["silenceThreshold"] = s.silenceThreshold;
    j["chatOpenEnabled"] = s.chatOpenEnabled;
    j["chatOpenKey"] = HotkeyListener::keyName(s.chatOpenKeyVk);
    j["chatSendKey"] = HotkeyListener::keyName(s.chatSendKeyVk);
    j["chatOpenDelayMs"] = s.chatOpenDelayMs;
    j["typingDelayMs"] = s.typingDelayMs;
    j["unicodeFallback"] = s.unicodeFallback;
    // Traduzione della chat letta
    j["ocrEnabled"] = s.ocrEnabled;
    j["ocrTargetLanguageIndex"] = s.ocrTargetLanguageIndex;
    j["chatFilterTag"] = s.chatFilterTag;
    j["ocrRegion"] = { { "x", s.ocrRegionX }, { "y", s.ocrRegionY }, { "w", s.ocrRegionW }, { "h", s.ocrRegionH }, { "angle", s.ocrRegionAngle } };
    j["ocrOverlayOpacity"] = s.ocrOverlayOpacity;
    j["ocrMaxMessages"] = s.ocrMaxMessages;
    // Mining
    j["miningEnabled"] = s.miningEnabled;
    AppSettings copy = s;
    json keys = json::array();
    for (int i = 0; i < 4; ++i) keys.push_back(describeKeys(miningKeys(copy, i), nullptr));
    j["miningKeys"] = keys;
    j["miningRegion"] = { { "x", s.miningRegionX }, { "y", s.miningRegionY }, { "w", s.miningRegionW }, { "h", s.miningRegionH } };
    j["miningOverlayOpacity"] = s.miningOverlayOpacity;
    j["miningOverlayTextOpacity"] = s.miningOverlayTextOpacity;
    j["miningOverlaySeconds"] = s.miningOverlaySeconds;
    j["miningOverlayBasic"] = s.miningOverlayBasic;
    j["miningShowBorder"] = s.miningShowBorder;
    j["miningAuto"] = s.miningAuto;
    j["miningSaveImages"] = s.miningSaveImages;
    j["miningSpeak"] = s.miningSpeak;
    j["miningLogCsv"] = s.miningLogCsv;
    // Avvio e area di notifica (l'avvio con Windows è nel registro)
    j["greetOnStart"] = s.greetOnStart;
    j["closeToTray"] = s.closeToTray;
    std::wstring autostartCommand;
    j["autostart"] = Autostart::enabled(&autostartCommand);
    j["autostartElsewhere"] = j["autostart"].get<bool>() && !Autostart::pointsHere();
    // Barra rapida
    j["quickBarEnabled"] = s.quickBarEnabled;
    j["barKeys"] = json::array({ describeKeys(s.quickBarTriggers, nullptr), describeKeys(s.hideAllTriggers, nullptr) });
    // Diario di bordo e raffineria
    j["gameLogEnabled"] = s.gameLogEnabled;
    j["gameLogPath"] = s.gameLogPath;
    j["gameLogSpeakContracts"] = s.gameLogSpeakContracts;
    j["gameLogSpeakHealth"] = s.gameLogSpeakHealth;
    j["gameLogSpeakZones"] = s.gameLogSpeakZones;
    j["gameLogSpeakParty"] = s.gameLogSpeakParty;
    j["refinerySpeak"] = s.refinerySpeak;
    send("settings", dump(j));
}

void WebBridge::sendStatus() {
    const Assistant::Status st = m_engine.status();
    json j = { { "state", stateId(st.state) },
               { "text", st.text },
               { "canRetry", st.canRetry },
               { "heard", m_engine.lastHeard() },
               { "gpu", m_engine.gpuSummary() },
               { "ready", m_engine.isReady() } };
    send("status", dump(j));
}

void WebBridge::sendConversation() {
    json list = json::array();
    for (const auto& e : m_engine.conversation()) {
        list.push_back({ { "kind", entryKind(e.kind) }, { "time", e.time }, { "text", e.text }, { "pending", e.pending } });
    }
    send("conversation", dump(list));
}

void WebBridge::sendLists() {
    json j;
    json languages = json::array();
    for (const auto& l : supportedLanguages()) languages.push_back(u8(l.displayName));
    j["languages"] = languages;
    // Periferiche: quelle collegate, più quella scelta anche se ora non c'è.
    const AppSettings s = m_engine.settings();
    json mics = json::array();
    std::vector<std::string> present;
    for (const auto& d : AudioCapture::listCaptureDevices()) present.push_back(d.name);
    for (const auto& name : present) mics.push_back({ { "name", name }, { "missing", false } });
    if (!s.deviceNames.empty() && std::find(present.begin(), present.end(), s.deviceNames.front()) == present.end()) {
        mics.push_back({ { "name", s.deviceNames.front() }, { "missing", true } });
    }
    j["mics"] = mics;
    json outputs = json::array();
    const auto outs = TtsEngine::outputDevices();
    for (const auto& name : outs) outputs.push_back({ { "name", name }, { "missing", false } });
    if (!s.outputDeviceName.empty() && std::find(outs.begin(), outs.end(), s.outputDeviceName) == outs.end()) {
        outputs.push_back({ { "name", s.outputDeviceName }, { "missing", true } });
    }
    j["outputs"] = outputs;
    j["voices"] = TtsEngine::voices();
    json effects = json::array();
    for (const auto& e : VoiceEffect::presets()) effects.push_back({ { "id", e.id }, { "label", e.label } });
    j["effects"] = effects;
    j["version"] = Updater::currentVersion();
    send("lists", dump(j));
}

void WebBridge::sendChatHistory() {
    json list = json::array();
    const auto h = m_engine.chat().history();
    for (auto it = h.rbegin(); it != h.rend(); ++it) {
        list.push_back({ { "time", it->time }, { "heard", it->heard }, { "written", it->written }, { "typed", it->typed }, { "note", it->note } });
    }
    send("chatHistory", dump(list));
}

void WebBridge::sendChatNotice() {
    GameChat& chat = m_engine.chat();
    json j = { { "text", "" }, { "kind", "dim" } };
    if (chat.busy()) {
        const std::string b = chat.busyText();
        j = { { "text", "In corso: " + (b.empty() ? std::string("in coda...") : b) }, { "kind", "busy" } };
    } else {
        const GameChat::Notice n = chat.notice();
        if (!n.text.empty()) j = { { "text", n.text }, { "kind", n.error ? "bad" : "ok" } };
    }
    const std::string text = dump(j);
    if (text == m_lastNotice) return;
    m_lastNotice = text;
    send("chatNotice", text);
}

void WebBridge::sendFiltered() { send("filtered", dump(m_engine.chat().filteredMessages())); }

void WebBridge::sendMining() {
    MiningScanner& mining = m_engine.mining();
    const MiningScanner::Reading last = mining.lastReading();
    json history = json::array();
    for (const auto& r : mining.history()) history.push_back({ { "text", describeReading(r) }, { "kind", readingKind(r) } });
    // Con che cosa legge: PP-OCR (quello di Citizen Assistant) o l'OCR di Windows.
    std::string ocr, ocrKind = "warn";
    if (PpOcr::ready()) {
        ocr = "Legge con PP-OCR (il riconoscitore di Citizen Assistant), l'OCR di Windows resta di riserva.";
        ocrKind = "ok";
    } else if (PpOcr::filesPresent()) {
        std::string error;
        ocr = PpOcr::load(error) ? "Legge con PP-OCR (il riconoscitore di Citizen Assistant)." : "PP-OCR non si carica (" + error + "): legge con l'OCR di Windows.";
        if (PpOcr::ready()) ocrKind = "ok";
    } else {
        ocr = "Legge con l'OCR di Windows: PP-OCR (24 MB) si scarica da solo dopo l'avvio.";
    }
    send("mining", dump({ { "last", describeReading(last) }, { "kind", readingKind(last) }, { "history", history }, { "ocr", ocr }, { "ocrKind", ocrKind } }));
}

void WebBridge::sendMiningData() {
    MiningScanner& mining = m_engine.mining();
    json unknowns = json::array();
    for (const auto& u : mining.unknowns()) {
        unknowns.push_back({ { "value", u.value }, { "label", Mining::formatValue(u.value) }, { "count", u.count }, { "time", u.time } });
    }
    send("miningData", dump({ { "table", mining.materialsText() }, { "unknowns", unknowns } }));
}

void WebBridge::sendGame() {
    GameController& game = m_engine.game();
    const GameController::Status st = game.status();
    json j = { { "text", st.text },
               { "running", st.running },
               { "elevationProblem", st.elevationProblem },
               { "dryRun", game.profile().dryRun },
               { "profileError", game.profileError() },
               { "actions", game.recentActions() },
               { "unrecognized", game.unrecognized() } };
    const std::string text = dump(j);
    if (text == m_lastGame) return;
    m_lastGame = text;
    send("game", text);
}

void WebBridge::sendUpdate() {
    const UpdateManager::State u = m_updates.state();
    json j = { { "phase", updatePhase(u.phase) }, { "current", u.current },   { "latest", u.latest },       { "notes", u.notes },
               { "pageUrl", u.pageUrl },          { "error", u.error },       { "checkedAt", u.checkedAt }, { "progress", u.progress } };
    send("update", dump(j));
}

void WebBridge::sendMeters() {
    json j = { { "mic", m_engine.micLevel() },
               { "voice", m_engine.voiceLevel() },
               { "speaking", m_engine.userSpeaking() },
               { "prob", m_engine.voiceProbability() },
               { "noise", m_engine.noiseFloor() },
               { "state", stateId(m_engine.status().state) } };
    send("meters", dump(j));
}

void WebBridge::sendOcrInfo() {
    std::string installed;
    for (const auto& code : OcrEngineWrapper::installedLanguages()) installed += (installed.empty() ? "" : ", ") + code;
    json j;
    if (installed.empty()) {
        j = { { "text", "Windows non ha nessuna lingua per l'OCR: aggiungine una da Impostazioni › Ora e lingua › Lingua." }, { "kind", "bad" } };
    } else if (OcrEngineWrapper::isLanguageSupported("en")) {
        j = { { "text", "OCR di Windows pronto per: " + installed + "." }, { "kind", "ok" } };
    } else {
        j = { { "text", "L'OCR per l'inglese non è installato in Windows: uso " + installed + " (stesso alfabeto, va bene). Per una lettura migliore puoi installarlo." },
              { "kind", "warn" } };
    }
    send("ocrInfo", dump(j));
}

// --- Eventi ---------------------------------------------------------------------------

void WebBridge::onEngineEvent(int event) {
    switch ((Assistant::Event)event) {
    case Assistant::Event::Status:
        sendStatus();
        sendChatNotice();
        break;
    case Assistant::Event::Conversation: sendConversation(); break;
    case Assistant::Event::Settings: sendSettings(); break;
    case Assistant::Event::Heard: sendStatus(); break;
    case Assistant::Event::ChatHistory:
        sendChatHistory();
        sendChatNotice();
        break;
    case Assistant::Event::Filtered: sendFiltered(); break;
    case Assistant::Event::Mining: sendMining(); break;
    case Assistant::Event::MiningData: sendMiningData(); break;
    case Assistant::Event::GameLog: sendGameLog(); break;
    case Assistant::Event::Refinery: sendRefinery(); break;
    case Assistant::Event::MiningLog: sendMiningLog(); break;
    default: break;
    }
}

void WebBridge::onUpdateChanged() { sendUpdate(); }

void WebBridge::onTimer() {
    ++m_ticks;
    // L'overlay degli assi si apre e si chiude col gioco anche a finestra ridotta.
    if (m_ticks % 60 == 0) axisCheckGames();
    axisTickCapture();
    if (!m_ready || !IsWindowVisible(m_window) || IsIconic(m_window)) return;
    if (m_page == "axis") axisTickPreviews();
    // Le barre del microfono e la sfera: solo nelle pagine che le mostrano.
    if (m_ticks % 2 == 0 && (m_page == "home" || m_page == "voice" || m_page == "chat")) sendMeters();
    if (m_ticks % 30 == 0) {
        if (m_page == "game") {
            sendGame();
            sendGameCommands(false); // il profilo cambiato a mano si vede da solo
        }
        if (m_page == "chat") sendChatNotice();
    }
}

void WebBridge::onAppMessage(UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_APP_BRIDGE_SEND) {
        std::unique_ptr<std::string> text(reinterpret_cast<std::string*>(lp));
        if (text && m_ready && m_send) m_send(*text);
    } else if (msg == WM_APP_BRIDGE_CAPTURED) {
        onCaptured(wp != 0, reinterpret_cast<void*>(lp));
    } else if (msg == WM_APP_BRIDGE_RUN) {
        std::function<void()> work;
        {
            std::lock_guard<std::mutex> lock(m_deferMutex);
            if (m_deferred.empty()) return;
            work = std::move(m_deferred.front());
            m_deferred.pop_front();
        }
        if (work) work();
    }
}

void WebBridge::defer(std::function<void()> work) {
    {
        std::lock_guard<std::mutex> lock(m_deferMutex);
        m_deferred.push_back(std::move(work));
    }
    PostMessageW(m_window, WM_APP_BRIDGE_RUN, 0, 0);
}

// --- Dalla pagina ------------------------------------------------------------------------

void WebBridge::onMessage(const std::string& text) {
    const json m = json::parse(text, nullptr, false);
    if (!m.is_object()) return;
    const std::string action = str(m, "a");
    if (action.empty()) return;
    try {
        handle(action, &m);
    } catch (const std::exception& e) {
        Log::error("Interfaccia: comando \"" + action + "\" non riuscito: " + e.what());
    }
}

void WebBridge::setSetting(const std::string& key, const void* valuePtr) {
    const json& v = *static_cast<const json*>(valuePtr);
    using Setter = std::function<void(AppSettings&, const json&)>;
    auto boolean = [](bool AppSettings::*field) -> Setter {
        return [field](AppSettings& s, const json& x) {
            if (x.is_boolean()) s.*field = x.get<bool>();
        };
    };
    auto number = [](int AppSettings::*field) -> Setter {
        return [field](AppSettings& s, const json& x) {
            if (x.is_number()) s.*field = (int)std::lround(x.get<double>());
        };
    };
    auto real = [](double AppSettings::*field) -> Setter {
        return [field](AppSettings& s, const json& x) {
            if (x.is_number()) s.*field = x.get<double>();
        };
    };
    static const std::map<std::string, Setter> setters = [&]() {
        std::map<std::string, Setter> t;
        for (auto [name, field] : std::initializer_list<std::pair<const char*, bool AppSettings::*>>{
                 { "wakeWordEnabled", &AppSettings::wakeWordEnabled },
                 { "speechEnabled", &AppSettings::speechEnabled },
                 { "gameEnabled", &AppSettings::gameEnabled },
                 { "overlayEnabled", &AppSettings::overlayEnabled },
                 { "autoUpdate", &AppSettings::autoUpdate },
                 { "chatEnabled", &AppSettings::chatEnabled },
                 { "autoSend", &AppSettings::autoSend },
                 { "gamingTone", &AppSettings::gamingTone },
                 { "chatOpenEnabled", &AppSettings::chatOpenEnabled },
                 { "unicodeFallback", &AppSettings::unicodeFallback },
                 { "ocrEnabled", &AppSettings::ocrEnabled },
                 { "miningEnabled", &AppSettings::miningEnabled },
                 { "miningOverlayBasic", &AppSettings::miningOverlayBasic },
                 { "miningShowBorder", &AppSettings::miningShowBorder },
                 { "miningAuto", &AppSettings::miningAuto },
                 { "miningSaveImages", &AppSettings::miningSaveImages },
                 { "miningSpeak", &AppSettings::miningSpeak },
                 { "miningLogCsv", &AppSettings::miningLogCsv },
                 { "greetOnStart", &AppSettings::greetOnStart },
                 { "closeToTray", &AppSettings::closeToTray },
                 { "quickBarEnabled", &AppSettings::quickBarEnabled },
                 { "gameLogSpeakContracts", &AppSettings::gameLogSpeakContracts },
                 { "gameLogSpeakHealth", &AppSettings::gameLogSpeakHealth },
                 { "gameLogSpeakZones", &AppSettings::gameLogSpeakZones },
                 { "gameLogSpeakParty", &AppSettings::gameLogSpeakParty },
                 { "refinerySpeak", &AppSettings::refinerySpeak } }) {
            t[name] = boolean(field);
        }
        for (auto [name, field] : std::initializer_list<std::pair<const char*, int AppSettings::*>>{
                 { "endSilenceMs", &AppSettings::endSilenceMs },
                 { "followUpSeconds", &AppSettings::followUpSeconds },
                 { "gpuMemoryLimitMb", &AppSettings::gpuMemoryLimitMb },
                 { "overlayOpacity", &AppSettings::overlayOpacity },
                 { "overlaySize", &AppSettings::overlaySize },
                 { "sourceLanguageIndex", &AppSettings::sourceLanguageIndex },
                 { "targetLanguageIndex", &AppSettings::targetLanguageIndex },
                 { "chatOpenDelayMs", &AppSettings::chatOpenDelayMs },
                 { "typingDelayMs", &AppSettings::typingDelayMs },
                 { "ocrTargetLanguageIndex", &AppSettings::ocrTargetLanguageIndex },
                 { "ocrOverlayOpacity", &AppSettings::ocrOverlayOpacity },
                 { "ocrMaxMessages", &AppSettings::ocrMaxMessages },
                 { "miningOverlayOpacity", &AppSettings::miningOverlayOpacity },
                 { "miningOverlayTextOpacity", &AppSettings::miningOverlayTextOpacity },
                 { "miningOverlaySeconds", &AppSettings::miningOverlaySeconds } }) {
            t[name] = number(field);
        }
        for (auto [name, field] : std::initializer_list<std::pair<const char*, double AppSettings::*>>{
                 { "vadThreshold", &AppSettings::vadThreshold },
                 { "speechRate", &AppSettings::speechRate },
                 { "silenceThreshold", &AppSettings::silenceThreshold } }) {
            t[name] = real(field);
        }
        t["micGain"] = [](AppSettings& s, const json& x) {
            if (x.is_number()) s.micGain = (float)x.get<double>();
        };
        t["replyStyle"] = [](AppSettings& s, const json& x) {
            if (x.is_string()) s.replyStyle = x.get<std::string>();
        };
        t["userName"] = [](AppSettings& s, const json& x) {
            if (x.is_string()) s.userName = TextUtils::trim(x.get<std::string>());
        };
        return t;
    }();
    auto it = setters.find(key);
    if (it == setters.end()) {
        Log::warn("Interfaccia: impostazione sconosciuta \"" + key + "\"");
        return;
    }
    m_engine.updateSettings([&](AppSettings& s) { it->second(s, v); });
}

void WebBridge::handle(const std::string& action, const void* messagePtr) {
    const json& m = *static_cast<const json*>(messagePtr);
    MiningScanner& mining = m_engine.mining();
    GameController& game = m_engine.game();

    if (action == "ready") {
        m_ready = true;
        m_lastGame.clear();
        m_lastNotice.clear();
        sendAll();
    } else if (action.rfind("axis", 0) == 0) {
        handleAxis(action, &m);
    } else if (action.rfind("miningLog", 0) == 0) {
        handleMiningLog(action, &m);
    } else if (action == "gameCmdSave" || action == "gameCmdDelete" || action == "gameKeyCapture") {
        handleCommands(action, &m);
    } else if (action.rfind("gameLog", 0) == 0 || action.rfind("refinery", 0) == 0 || action == "autostart" || action == "quickBarShow") {
        handleExtra(action, &m);
    } else if (action == "page") {
        m_page = str(m, "name");
        if (m_page == "axis") {
            m_axisPreviewsDirty = true;
            sendAxis();
        }
        if (m_page == "game") {
            sendGame();
            sendGameCommands(true);
        }
        if (m_page == "chat") sendChatNotice();
    } else if (action == "set") {
        if (m.contains("v")) setSetting(str(m, "k"), &m["v"]);
    }
    // --- Iris
    else if (action == "sendText") {
        const std::string text = str(m, "text");
        if (!text.empty()) m_engine.sendText(text);
    } else if (action == "stop") {
        m_engine.stopSpeaking();
    } else if (action == "newConversation") {
        m_engine.newConversation();
    } else if (action == "retry") {
        m_engine.retryStart();
    } else if (action == "copy") {
        copyToClipboard(m_window, w(str(m, "text")));
        toast("Copiato.");
    }
    // --- Ascolto e voce
    else if (action == "setMic") {
        const std::string name = str(m, "name");
        m_engine.updateSettings([&](AppSettings& s) { s.deviceNames = name.empty() ? std::vector<std::string>{} : std::vector<std::string>{ name }; });
    } else if (action == "setOutput") {
        const std::string name = str(m, "name");
        m_engine.updateSettings([&](AppSettings& s) { s.outputDeviceName = name; });
        m_engine.testVoice();
    } else if (action == "setVoice") {
        // "Microsoft Elsa (it-IT)" -> "Microsoft Elsa"
        std::string name = str(m, "name");
        const size_t paren = name.rfind(" (");
        if (paren != std::string::npos) name = name.substr(0, paren);
        if (!name.empty()) {
            m_engine.updateSettings([&](AppSettings& s) { s.voiceName = name; });
            m_engine.testVoice();
        }
    } else if (action == "setEngine") {
        const bool neural = str(m, "engine") != "windows";
        m_engine.updateSettings([&](AppSettings& s) { s.voiceEngine = neural ? "neurale" : "windows"; });
        if (!neural) m_engine.testVoice();
    } else if (action == "setEffect") {
        const std::string id = str(m, "id");
        if (VoiceEffect::isValid(id)) {
            m_engine.updateSettings([&](AppSettings& s) { s.voiceEffect = id; });
            m_engine.testVoice();
        }
    } else if (action == "testVoice") {
        m_engine.testVoice();
    } else if (action == "refreshDevices") {
        sendLists();
        sendSettings();
    } else if (action == "captureKey") {
        startCapture(str(m, "target"));
    } else if (action == "removeKey") {
        const std::string target = str(m, "target");
        const int index = integer(m, "index", -1);
        m_engine.updateSettings([&](AppSettings& s) {
            if (target == "talk" && index >= 0 && index < (int)s.triggers.size()) s.triggers.erase(s.triggers.begin() + index);
            if (target == "chat" && index >= 0 && index < (int)s.chatTriggers.size()) s.chatTriggers.erase(s.chatTriggers.begin() + index);
            if (target.rfind("mining:", 0) == 0) miningKeys(s, std::atoi(target.c_str() + 7)).clear();
        });
    }
    // --- Chat di gioco
    else if (action == "swapLanguages") {
        m_engine.updateSettings([](AppSettings& s) { std::swap(s.sourceLanguageIndex, s.targetLanguageIndex); });
    } else if (action == "translateTest") {
        const std::string text = str(m, "text");
        if (!text.empty()) m_engine.chat().translateTest(text);
    } else if (action == "calibrateSilence") {
        m_engine.chat().calibrateSilence();
    } else if (action == "openGlossary") {
        Glossary::load(); // se non c'è, lo crea con gli esempi
        openFile(m_window, Glossary::filePath(), true);
    }
    // --- Traduzione della chat letta
    else if (action == "chooseOcrRegion") {
        defer([this]() { chooseOcrRegion(); });
    } else if (action == "filterSave") {
        std::string tag = str(m, "tag");
        // Le parentesi le aggiunge il gioco: "[LSE]" e "LSE" sono la stessa cosa.
        tag.erase(std::remove_if(tag.begin(), tag.end(), [](char c) { return c == '[' || c == ']'; }), tag.end());
        tag = TextUtils::trim(tag);
        m_engine.updateSettings([&](AppSettings& s) { s.chatFilterTag = tag; });
        toast(tag.empty() ? "Filtro tolto." : "Filtro salvato: [" + tag + "]");
    } else if (action == "openOcrDebug") {
        const std::wstring path = AppPaths::ocrDebugPath();
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
            toast("Il file compare dopo la prima lettura dello schermo (attiva la traduzione e scegli la zona).", false);
        } else {
            openFile(m_window, path, true);
        }
    } else if (action == "openLanguageSettings") {
        ShellExecuteW(m_window, L"open", L"ms-settings:regionlanguage", nullptr, nullptr, SW_SHOWNORMAL);
    } else if (action == "copyFiltered") {
        std::wstring all;
        for (const auto& line : m_engine.chat().filteredMessages()) all += w(line) + L"\r\n";
        copyToClipboard(m_window, all);
        toast("Messaggi copiati.");
    } else if (action == "clearFiltered") {
        m_engine.chat().clearFilteredMessages();
    }
    // --- Riquadri sopra il gioco
    else if (action == "unlockOverlay") {
        const std::string which = str(m, "which");
        if (which == "orb") Overlay::unlockForMove(30);
        else if (which == "chat") OcrOverlay::unlockForMove(30);
        else if (which == "mining") MiningOverlay::unlockForMove(30);
    } else if (action == "resetOverlay") {
        const std::string which = str(m, "which");
        m_engine.updateSettings([&](AppSettings& s) {
            if (which == "orb") s.overlayX = s.overlayY = -1;
            else if (which == "chat") s.ocrOverlayX = s.ocrOverlayY = -1;
            else if (which == "mining") s.miningOverlayX = s.miningOverlayY = -1;
        });
    }
    // --- Mining
    else if (action == "chooseMiningRegion") {
        defer([this]() { chooseMiningRegion(); });
    } else if (action == "miningScan") {
        mining.scan("prova");
    } else if (action == "miningTest") {
        const std::string text = str(m, "text");
        if (!text.empty()) mining.submitText(text, "prova");
    } else if (action == "openMiningImages") {
        const std::wstring dir = MiningScanner::imagesDir();
        if (GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES) toast("La cartella non c'è ancora: attiva \"Salva le immagini\" e fai una lettura.", false);
        else openFile(m_window, dir, false);
    } else if (action == "tableSave") {
        std::string message;
        const bool ok = mining.setMaterialsText(str(m, "text"), message);
        toast(message, ok);
        if (ok) sendMiningData();
    } else if (action == "tableReset") {
        mining.resetMaterials();
        sendMiningData();
        toast("Tabella predefinita rimessa.");
    } else if (action == "tableImport") {
        const bool replace = m.value("replace", false);
        defer([this, replace]() {
            wchar_t file[MAX_PATH] = L"";
            OPENFILENAMEW ofn{ sizeof(ofn) };
            ofn.hwndOwner = m_window;
            ofn.lpstrFilter = L"Tabelle (CSV, materialsdb, JSON)\0*.csv;*.txt;*.json;materialsdb*;*.*\0Tutti i file\0*.*\0";
            ofn.lpstrFile = file;
            ofn.nMaxFile = MAX_PATH;
            ofn.lpstrTitle = L"Importa i materiali";
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
            if (!GetOpenFileNameW(&ofn)) return;
            std::string message;
            const bool ok = m_engine.mining().importFile(file, replace, message);
            if (ok) sendMiningData();
            toast(message, ok);
        });
    } else if (action == "tableExport") {
        defer([this]() {
            wchar_t file[MAX_PATH] = L"materiali_mining.csv";
            OPENFILENAMEW ofn{ sizeof(ofn) };
            ofn.hwndOwner = m_window;
            ofn.lpstrFilter = L"CSV\0*.csv\0";
            ofn.lpstrFile = file;
            ofn.nMaxFile = MAX_PATH;
            ofn.lpstrDefExt = L"csv";
            ofn.lpstrTitle = L"Esporta i materiali";
            ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
            if (!GetSaveFileNameW(&ofn)) return;
            std::string message;
            const bool ok = m_engine.mining().exportCsv(file, message);
            toast(message, ok);
        });
    } else if (action == "identify") {
        std::string message;
        const bool ok = mining.identify(integer(m, "value"), str(m, "name"), integer(m, "qty"), message);
        toast(message, ok);
        if (ok) sendMiningData();
    } else if (action == "removeUnknown") {
        const int value = integer(m, "value");
        if (value > 0) {
            mining.removeUnknown(value);
            toast("Tolta " + Mining::formatValue(value) + ".");
        }
    } else if (action == "clearUnknowns") {
        mining.clearUnknowns();
        toast("Elenco svuotato.");
    }
    // --- Star Citizen
    else if (action == "gameTest") {
        const std::string text = str(m, "text");
        if (text.empty()) return;
        const Cmd::Match match = game.match(text);
        char score[16];
        std::snprintf(score, sizeof(score), "%d%%", (int)(match.score * 100 + 0.5));
        json r;
        if (match.found) {
            std::string slots;
            for (const auto& [k, v] : match.slots) slots += ", " + k + " = " + v;
            r = { { "text", "→ " + match.command.id() + slots + "  (\"" + match.phrase + "\", " + score + ")" }, { "ok", true } };
        } else {
            r = { { "text", "Non è nel profilo: lo passo al modello. Il più vicino: \"" + match.phrase + "\" (" + score + ")" }, { "ok", false } };
        }
        send("gameTest", dump(r));
        m_engine.testGameCommand(text);
    } else if (action == "dryRun") {
        game.setDryRun(m.value("on", false));
        m_lastGame.clear();
        sendGame();
    } else if (action == "gameReload") {
        game.load();
        m_lastGame.clear();
        sendGame();
        toast("Profilo ricaricato.");
    } else if (action == "openProfile") {
        openFile(m_window, game.profilePath(), true);
    } else if (action == "openOcrImages") {
        const std::wstring dir = AppPaths::getAppDataDir() + L"\\debug";
        CreateDirectoryW(dir.c_str(), nullptr);
        openFile(m_window, dir, false);
    } else if (action == "openUnknownPhrases") {
        openFile(m_window, AppPaths::getAppDataDir() + L"\\frasi_non_capite.txt", true);
    } else if (action == "calibrate") {
        calibrate(integer(m, "zone"));
    } else if (action == "readZone") {
        readZone(integer(m, "zone"));
    }
    // --- Impostazioni
    else if (action == "openMemory") {
        openMemory();
    } else if (action == "openDataFolder") {
        openFile(m_window, AppPaths::getAppDataDir(), false);
    } else if (action == "openLog") {
        openFile(m_window, AppPaths::logPath(), true);
    } else if (action == "checkUpdate") {
        m_updates.checkNow();
    } else if (action == "installUpdate") {
        std::string error;
        if (m_updates.installNow(error)) {
            // La versione nuova è partita e aspetta che questa si chiuda.
            PostMessageW(m_window, WM_CLOSE, 0, 0);
        } else {
            toast("Aggiornamento non installato: " + error, false);
            sendUpdate();
        }
    } else if (action == "openReleasePage") {
        std::string url = m_updates.state().pageUrl;
        if (url.empty()) url = std::string("https://github.com/") + Updater::kRepository + "/releases/latest";
        ShellExecuteW(m_window, L"open", w(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    } else {
        Log::warn("Interfaccia: comando sconosciuto \"" + action + "\"");
    }
}

// --- Cattura di un tasto -------------------------------------------------------------------

void WebBridge::startCapture(const std::string& target) {
    if (!m_captureTarget.empty()) {
        m_captureCancel = true; // secondo clic = annulla
        return;
    }
    if (target.empty()) return;
    if (m_captureThread.joinable()) m_captureThread.join();
    m_captureTarget = target;
    m_captureCancel = false;
    m_engine.setTalkPaused(true);
    const bool allowJoystick = target != "chatOpen" && target != "chatSend";
    send("capture", dump({ { "target", target },
                           { "active", true },
                           { "text", allowJoystick ? "Premi il tasto o il pulsante del joystick da usare (Esc annulla)..."
                                                   : "Premi il tasto da usare (Esc annulla)..." } }));
    HWND window = m_window;
    m_captureThread = std::thread([this, window, allowJoystick]() {
        auto* t = new TalkTrigger();
        const bool ok = HotkeyListener::captureNextInput(*t, m_captureCancel, 15000, allowJoystick);
        if (!PostMessageW(window, WM_APP_BRIDGE_CAPTURED, ok ? 1 : 0, (LPARAM)t)) delete t;
    });
}

void WebBridge::onCaptured(bool ok, void* triggerPtr) {
    std::unique_ptr<TalkTrigger> t(static_cast<TalkTrigger*>(triggerPtr));
    if (m_captureThread.joinable()) m_captureThread.join();
    const std::string target = m_captureTarget;
    m_captureTarget.clear();
    std::string message;
    // Il tasto si salva PRIMA di riattivare i tasti per parlare.
    if (!ok || !t) {
        message = "Annullato.";
    } else if (target == "talk" || target == "chat") {
        const AppSettings s = m_engine.settings();
        const auto& list = target == "talk" ? s.triggers : s.chatTriggers;
        if (std::find(list.begin(), list.end(), *t) != list.end()) {
            message = "Questo tasto c'è già.";
        } else {
            m_engine.updateSettings([&](AppSettings& x) { (target == "talk" ? x.triggers : x.chatTriggers).push_back(*t); });
        }
    } else if (target == "chatOpen") {
        m_engine.updateSettings([&](AppSettings& x) { x.chatOpenKeyVk = t->vk; });
    } else if (target == "chatSend") {
        m_engine.updateSettings([&](AppSettings& x) { x.chatSendKeyVk = t->vk; });
    } else if (target.rfind("mining:", 0) == 0) {
        const int row = std::atoi(target.c_str() + 7);
        m_engine.updateSettings([&](AppSettings& x) { miningKeys(x, row) = { *t }; });
    }
    m_engine.setTalkPaused(false);
    send("capture", dump({ { "target", target }, { "active", false }, { "text", message } }));
}

// --- Mappa stellare: calibrazione e lettura delle zone -----------------------------------------

void WebBridge::startWork(std::function<void()> work) {
    stopWork();
    m_workCancel = false;
    m_work = std::thread(std::move(work));
}

void WebBridge::stopWork() {
    m_workCancel = true;
    if (m_work.joinable()) m_work.join();
}

void WebBridge::calibrate(int zoneIndex) {
    if (zoneIndex < 0 || zoneIndex > 3) return;
    const auto zone = (GameController::Zone)zoneIndex;
    const std::string name = GameController::zoneName(zone);
    const bool point = zone == GameController::Zone::SearchBox;
    send("calibration", dump("Calibro la " + name + ": nel gioco, con la mappa stellare aperta, " +
                             (point ? std::string("porta il mouse al centro della casella e premi F8.")
                                    : std::string("porta il mouse sull'angolo IN ALTO A SINISTRA e premi F8, poi su quello IN BASSO A "
                                                  "DESTRA e di nuovo F8.")) +
                             " Senti un suono a ogni F8; alla fine torno qui. Hai 30 secondi."));
    HWND window = m_window;
    startWork([this, zone, point, name, window]() {
        GameController& game = m_engine.game();
        auto post = [&](const std::string& text) { postJson(dump({ { "t", "calibration" }, { "v", text } })); };
        double x1 = 0, y1 = 0, x2 = 0, y2 = 0;
        std::string error;
        // Iris è in primo piano (hai appena premuto Calibra): si passa al gioco da soli.
        if (!game.bringToFront(error)) return post("Non calibrato: " + error);
        auto finish = [&](const std::string& text, bool ok) {
            if (m_workCancel) return;
            m_engine.chime(ok);
            GameInput::focus(window);
            post(text);
        };
        if (!waitF8(m_workCancel)) return finish("Calibrazione annullata (F8 non premuto entro 30 secondi).", false);
        if (!game.cursorRelative(x1, y1, error)) return finish("Non calibrato: " + error + ".", false);
        if (!point) {
            m_engine.chime(true); // primo angolo preso
            post("Primo angolo preso. Ora porta il mouse sull'angolo IN BASSO A DESTRA della " + name + " e premi F8.");
            if (!waitF8(m_workCancel)) return finish("Calibrazione annullata (F8 non premuto entro 30 secondi).", false);
            if (!game.cursorRelative(x2, y2, error)) return finish("Non calibrato: " + error + ".", false);
        }
        if (!game.setZone(zone, x1, y1, x2, y2)) return finish("Non calibrato: i due angoli sono troppo vicini. Riprova.", false);
        char buf[200];
        if (point) std::snprintf(buf, sizeof(buf), "Salvata: %s a %.3f, %.3f della finestra.", name.c_str(), x1, y1);
        else std::snprintf(buf, sizeof(buf), "Salvata: %s da %.3f, %.3f a %.3f, %.3f della finestra. Premi Leggi per provarla.", name.c_str(),
                           std::min(x1, x2), std::min(y1, y2), std::max(x1, x2), std::max(y1, y2));
        finish(buf, true);
    });
}

void WebBridge::readZone(int zoneIndex) {
    if (zoneIndex < 0 || zoneIndex > 3) return;
    const auto zone = (GameController::Zone)zoneIndex;
    send("calibration", dump("Porto il gioco davanti e leggo la " + GameController::zoneName(zone) + "..."));
    HWND window = m_window;
    startWork([this, zone, window]() {
        const std::string text = m_engine.game().readZone(zone); // porta il gioco davanti e legge
        if (m_workCancel) return;
        GameInput::focus(window);
        postJson(dump({ { "t", "calibration" }, { "v", text } }));
    });
}

// --- Zone dello schermo ---------------------------------------------------------------------------

namespace {
    // La finestra si toglie di mezzo: la "foto" dello schermo deve mostrare il gioco.
    void hideForShot(HWND window) {
        ShowWindow(window, SW_HIDE);
        DwmFlush();
        Sleep(200);
        DwmFlush();
    }
    void showAgain(HWND window) {
        ShowWindow(window, SW_SHOW);
        SetForegroundWindow(window);
    }
}

void WebBridge::chooseOcrRegion() {
    // La zona di prima (se c'è) si apre pronta da girare: la chat dell'HUD segue la curva del casco.
    const AppSettings before = m_engine.settings();
    RegionSelector::Options options;
    options.rotatable = true;
    options.angle = before.ocrRegionAngle;
    if (before.ocrRegionW > 0 && before.ocrRegionH > 0) {
        options.initial = RECT{ before.ocrRegionX, before.ocrRegionY, before.ocrRegionX + before.ocrRegionW, before.ocrRegionY + before.ocrRegionH };
    }
    hideForShot(m_window);
    RECT r{};
    double angle = 0.0;
    const bool ok = RegionSelector::selectRegion(GetModuleHandleW(nullptr), r, options, &angle);
    showAgain(m_window);
    if (!ok) return;
    m_engine.updateSettings([&](AppSettings& s) {
        s.ocrRegionX = r.left;
        s.ocrRegionY = r.top;
        s.ocrRegionW = r.right - r.left;
        s.ocrRegionH = r.bottom - r.top;
        s.ocrRegionAngle = angle;
        s.ocrEnabled = true; // scegliere la zona vuol dire volerla usare
    });
}

void WebBridge::chooseMiningRegion() {
    MiningOverlay::setBorder(RECT{}, false); // il contorno di prima non deve finire nella foto
    hideForShot(m_window);
    RECT r{};
    RegionSelector::Options options;
    options.title = L"Trascina un rettangolo intorno alla signature dello scanner";
    options.hint = L"Un po' più largo del badge con il numero. Esc o tasto destro per annullare.";
    const bool ok = RegionSelector::selectRegion(GetModuleHandleW(nullptr), r, options);
    showAgain(m_window);
    if (!ok) {
        m_engine.updateSettings([](AppSettings&) {}); // rimette il contorno com'era
        return;
    }
    m_engine.updateSettings([&](AppSettings& s) {
        s.miningRegionX = r.left;
        s.miningRegionY = r.top;
        s.miningRegionW = r.right - r.left;
        s.miningRegionH = r.bottom - r.top;
        s.miningEnabled = true;
    });
}

void WebBridge::openMemory() {
    AppPaths::ensureDirectoriesExist();
    const std::wstring path = AppPaths::memoryPath();
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            const char header[] = "# Cose che Iris ricorda di te: una per riga. Le righe con # sono ignorate.\r\n";
            DWORD written = 0;
            WriteFile(f, header, (DWORD)strlen(header), &written, nullptr);
            CloseHandle(f);
        }
    }
    openFile(m_window, path, true);
}

// --- Axis Visualizer ----------------------------------------------------------------------------

namespace {
    json axisDeviceJson(const Axis::DeviceInfo& d) {
        return { { "id", d.id },
                 { "name", d.name },
                 { "kind", d.kind },
                 { "axisNames", d.axisNames },
                 { "buttonNames", d.buttonNames },
                 { "hats", d.hats },
                 { "defaultX", d.defaultX },
                 { "defaultY", d.defaultY },
                 { "defaultZ", d.defaultZ },
                 { "defaultInvertY", d.defaultInvertY },
                 { "connected", d.connected } };
    }

    // Gli eseguibili in esecuzione, come li confronta Axis Visualizer (senza ".exe", minuscoli).
    std::set<std::string> runningProcesses() {
        std::set<std::string> names;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) return names;
        PROCESSENTRY32W e{ sizeof(e) };
        if (Process32FirstW(snap, &e)) {
            do {
                names.insert(Axis::processName(TextUtils::wideToUtf8(e.szExeFile)));
            } while (Process32NextW(snap, &e));
        }
        CloseHandle(snap);
        return names;
    }
}

void WebBridge::sendAxis() {
    const Axis::Settings& s = m_axis.settings;
    json j;
    j["settings"] = json::parse(Axis::serialize(s), nullptr, false);
    // Le periferiche collegate, più quelle scelte nei moduli anche se ora non ci sono.
    json devices = json::array();
    std::set<std::string> listed;
    for (const auto& d : Axis::devices()) {
        devices.push_back(axisDeviceJson(d));
        listed.insert(d.id);
    }
    for (const auto& p : s.active().panes) {
        if (p.deviceId.empty() || listed.count(p.deviceId)) continue;
        listed.insert(p.deviceId);
        devices.push_back(axisDeviceJson(Axis::describe(p.deviceId)));
    }
    j["devices"] = devices;
    // I tasti scelti nei moduli della tastiera, col loro nome.
    json keyNames = json::object();
    for (const auto& p : s.active().panes) {
        if (p.deviceId != "keyboard") continue;
        for (int vk : p.buttons) keyNames[std::to_string(vk)] = HotkeyListener::keyName(vk);
    }
    for (int vk : { s.keyboard.up, s.keyboard.down, s.keyboard.left, s.keyboard.right, s.keyboard.barPlus, s.keyboard.barMinus }) {
        keyNames[std::to_string(vk)] = HotkeyListener::keyName(vk);
    }
    j["keyNames"] = keyNames;
    const AxisOverlay::Status st = AxisOverlay::status();
    j["status"] = { { "open", st.open }, { "obsOnly", st.obsOnly }, { "visible", st.visible }, { "locked", st.locked }, { "windows", st.windows },
                    { "fromGame", m_axis.overlayFromGame } };
    const int port = m_axis.server.running() ? m_axis.server.port() : s.serverPort;
    const std::string base = "http://localhost:" + std::to_string(port) + "/";
    json urls = json::array();
    for (const auto& g : Axis::windowGroups(s.active())) urls.push_back({ { "key", g.key }, { "name", g.name }, { "url", base + g.key } });
    j["server"] = { { "running", m_axis.server.running() },
                    { "port", port },
                    { "error", m_axis.server.running() ? std::string() : m_axis.server.lastError() },
                    { "urls", urls },
                    { "index", base } };
    j["capture"] = m_axisCaptureIndex;
    j["message"] = m_axisMessage;
    j["axisVisualizerFound"] = GetFileAttributesW(Axis::axisVisualizerSettingsPath().c_str()) != INVALID_FILE_ATTRIBUTES;
    send("axis", dump(j));
}

void WebBridge::axisApply(bool reopen) {
    m_axis.settings.sanitize();
    Axis::save(m_axis.settings);
    // Il server per OBS: acceso o spento, sulla porta scelta.
    if (m_axis.settings.serverEnabled) {
        std::string error;
        if (!m_axis.server.running() || m_axis.server.port() != m_axis.settings.serverPort) m_axis.server.start(m_axis.settings.serverPort, error);
    } else {
        m_axis.server.stop();
    }
    const AxisOverlay::Status st = AxisOverlay::status();
    if (reopen && st.open) AxisOverlay::open(m_axis.settings, st.obsOnly);
    m_axisPreviewsDirty = true;
    sendAxis();
}

void WebBridge::axisEdit(const std::function<void(void*)>& change) {
    json root = json::parse(Axis::serialize(m_axis.settings), nullptr, false);
    if (!root.is_object()) return;
    change(&root);
    m_axis.settings = Axis::parse(root.dump());
    axisApply(true);
}

void WebBridge::axisRebuildPreviews() {
    m_axisPreviewsDirty = false;
    m_axisPreviews.clear();
    json layouts = json::array();
    for (const auto& p : m_axis.settings.active().panes) {
        if (!p.enabled()) {
            m_axisPreviews.push_back(nullptr);
            layouts.push_back(nullptr);
            continue;
        }
        m_axisPreviews.push_back(std::make_unique<AxisDraw::PaneView>(p, m_axis.settings, Axis::describe(p.deviceId).name));
        layouts.push_back(m_axisPreviews.back()->describe());
    }
    const Axis::Settings& s = m_axis.settings;
    m_axisLastLayouts = dump(layouts);
    send("axisPreview", dump({ { "layouts", layouts }, { "font", s.textSize }, { "bold", s.boldText }, { "opacity", s.opacity } }));
}

void WebBridge::axisTickPreviews() {
    if (m_axisPreviewsDirty) axisRebuildPreviews();
    json values = json::array();
    json layouts = json::array();
    std::vector<double> v;
    for (auto& p : m_axisPreviews) {
        if (!p) {
            values.push_back(nullptr);
            layouts.push_back(nullptr);
            continue;
        }
        p->tick(1.0 / 30.0);
        p->values(v);
        values.push_back(v);
        layouts.push_back(p->describe());
    }
    // Una periferica collegata adesso cambia la forma di un modulo: la pagina la ridisegna.
    const std::string now = dump(layouts);
    if (now != m_axisLastLayouts) {
        m_axisLastLayouts = now;
        const Axis::Settings& s = m_axis.settings;
        send("axisPreview", dump({ { "layouts", layouts }, { "font", s.textSize }, { "bold", s.boldText }, { "opacity", s.opacity } }));
    }
    send("axisValues", dump(values));
}

void WebBridge::axisCheckGames() {
    static int lastTriggered = -1;
    if (!m_axis.settings.autoOverlayWithGame) {
        lastTriggered = -1;
        return;
    }
    const std::set<std::string> running = runningProcesses();
    int triggered = -1;
    for (int i = 0; i < (int)m_axis.settings.profiles.size() && triggered < 0; ++i) {
        for (const auto& t : m_axis.settings.profiles[i].triggers) {
            if (running.count(Axis::processName(t))) {
                triggered = i;
                break;
            }
        }
    }
    // Solo quando il gioco parte o finisce: se chiudi l'overlay a mano mentre giochi, resta chiuso.
    if (triggered == lastTriggered) return;
    const AxisOverlay::Status st = AxisOverlay::status();
    if (triggered >= 0 && !st.open) {
        m_axis.settings.activeProfile = triggered;
        Axis::save(m_axis.settings);
        AxisOverlay::open(m_axis.settings, false);
        m_axis.overlayFromGame = true;
        m_axisPreviewsDirty = true;
        Log::info("Axis: gioco avviato, overlay del profilo " + m_axis.settings.active().name);
    } else if (triggered < 0 && st.open && m_axis.overlayFromGame) {
        AxisOverlay::close();
        m_axis.overlayFromGame = false;
        Log::info("Axis: gioco chiuso, overlay chiuso");
    }
    lastTriggered = triggered;
}

void WebBridge::axisTickCapture() {
    if (m_axisCaptureIndex < 0) return;
    auto& panes = m_axis.settings.active().panes;
    if (m_axisCaptureIndex >= (int)panes.size() || GetTickCount64() > m_axisCaptureUntil) {
        m_axisCaptureIndex = -1;
        m_axisCaptureSource.reset();
        m_axisMessage = "Nessun pulsante premuto: scelta annullata.";
        sendAxis();
        return;
    }
    const int index = m_axisCaptureIndex;
    int pressed = -1;
    if (panes[index].deviceId == "keyboard") {
        const int key = Axis::firstPressedKey();
        if (key != m_axisCaptureBaselineKey) {
            m_axisCaptureBaselineKey = key; // il tasto di prima, ancora giù, non conta
            if (key > 0) pressed = key;
        }
    } else if (m_axisCaptureSource) {
        m_axisCaptureSource->update(1.0 / 30.0);
        const auto& buttons = m_axisCaptureSource->state().buttons;
        for (size_t b = 0; b < buttons.size(); ++b) {
            const bool before = b < m_axisCaptureBaseline.size() && m_axisCaptureBaseline[b];
            if (buttons[b] && !before) {
                pressed = (int)b;
                break;
            }
        }
        m_axisCaptureBaseline = buttons;
    }
    if (pressed < 0) return;
    m_axisCaptureIndex = -1;
    m_axisCaptureSource.reset();
    m_axisMessage.clear();
    axisEdit([&](void* r) {
        json& pane = (*static_cast<json*>(r))["Profiles"][m_axis.settings.activeProfile]["Panes"][index];
        std::vector<int> list = pane.value("Buttons", std::vector<int>{});
        if (std::find(list.begin(), list.end(), pressed) == list.end()) list.push_back(pressed);
        pane["Buttons"] = list;
    });
}

void WebBridge::onAxisChanged() {
    if (!AxisOverlay::status().open) m_axis.overlayFromGame = false;
    sendAxis();
}

void WebBridge::onAxisMoved(void* positions) {
    std::unique_ptr<std::vector<std::pair<int, POINT>>> moved(static_cast<std::vector<std::pair<int, POINT>>*>(positions));
    if (!moved) return;
    auto& panes = m_axis.settings.active().panes;
    for (const auto& [index, p] : *moved) {
        if (index < 0 || index >= (int)panes.size()) continue;
        panes[index].left = (double)p.x;
        panes[index].top = (double)p.y;
    }
    Axis::save(m_axis.settings);
}

void WebBridge::handleAxis(const std::string& action, const void* messagePtr) {
    const json& m = *static_cast<const json*>(messagePtr);
    Axis::Settings& s = m_axis.settings;
    const int active = std::clamp(s.activeProfile, 0, std::max(0, (int)s.profiles.size() - 1));
    m_axisMessage.clear();

    if (action == "axisRefresh") {
        sendAxis();
    } else if (action == "axisOpen") {
        const bool obsOnly = m.value("obsOnly", false);
        if (obsOnly && !m_axis.server.running()) {
            toast("Il server per OBS è spento: accendilo qui sotto, poi riprova.", false);
            return;
        }
        if (std::none_of(s.active().panes.begin(), s.active().panes.end(), [](const Axis::Pane& p) { return p.enabled(); })) {
            toast("Aggiungi almeno un modulo con una periferica.", false);
            return;
        }
        m_axis.overlayFromGame = false;
        AxisOverlay::open(s, obsOnly);
    } else if (action == "axisClose") {
        m_axis.overlayFromGame = false;
        AxisOverlay::close();
    } else if (action == "axisLock") {
        s.locked = m.value("on", !s.locked);
        Axis::save(s);
        AxisOverlay::setLocked(s.locked);
        sendAxis();
    } else if (action == "axisToggleVisible") {
        AxisOverlay::toggleVisible();
    } else if (action == "axisSet") {
        const std::string key = str(m, "k");
        if (key.empty() || key == "Profiles" || !m.contains("v")) return;
        const json value = m["v"];
        axisEdit([&](void* r) { (*static_cast<json*>(r))[key] = value; });
    } else if (action == "axisProfile") {
        const std::string op = str(m, "op");
        if (op == "select") {
            s.activeProfile = integer(m, "index", active);
        } else if (op == "add") {
            Axis::Profile p;
            p.name = str(m, "name").empty() ? "PROFILO " + std::to_string(s.profiles.size() + 1) : str(m, "name");
            s.profiles.push_back(p);
            s.activeProfile = (int)s.profiles.size() - 1;
        } else if (op == "rename") {
            const std::string name = str(m, "name");
            if (!name.empty()) s.active().name = name;
        } else if (op == "delete" && s.profiles.size() > 1) {
            s.profiles.erase(s.profiles.begin() + active);
            s.activeProfile = std::max(0, active - 1);
        }
        axisApply(true);
    } else if (action == "axisTriggers") {
        std::vector<std::string> triggers;
        std::string part;
        for (char c : str(m, "text") + ",") {
            if (c == ',' || c == ';') {
                if (!TextUtils::trim(part).empty()) triggers.push_back(TextUtils::trim(part));
                part.clear();
            } else {
                part += c;
            }
        }
        s.active().triggers = triggers;
        axisApply(false);
        toast(triggers.empty() ? "Nessun gioco: l'overlay del profilo si apre solo a mano." : "Giochi del profilo salvati.");
    } else if (action == "axisPane") {
        const int index = integer(m, "index", -1);
        const std::string key = str(m, "k");
        if (index < 0 || index >= (int)s.active().panes.size() || key.empty() || !m.contains("v")) return;
        const json value = m["v"];
        axisEdit([&](void* r) {
            json& pane = (*static_cast<json*>(r))["Profiles"][active]["Panes"][index];
            pane[key] = value;
            if (key == "DeviceId" && value.is_string()) {
                // Periferica nuova: gli assi e i pulsanti di prima non valgono più.
                const Axis::DeviceInfo d = Axis::describe(value.get<std::string>());
                pane["AxisX"] = d.defaultX;
                pane["AxisY"] = d.defaultY;
                pane["AxisZ"] = d.defaultZ;
                pane["InvertY"] = d.defaultInvertY;
                pane["BarAxes"] = json::array();
                pane["Buttons"] = json::array();
                pane["AxisLabels"] = json::object();
                pane["ButtonLabels"] = json::object();
            }
        });
    } else if (action == "axisPaneLabel") {
        const int index = integer(m, "index", -1);
        if (index < 0 || index >= (int)s.active().panes.size()) return;
        const std::string field = str(m, "which") == "button" ? "ButtonLabels" : "AxisLabels";
        const std::string key = std::to_string(integer(m, "key", 0)), text = str(m, "text");
        axisEdit([&](void* r) {
            json& labels = (*static_cast<json*>(r))["Profiles"][active]["Panes"][index][field];
            if (text.empty()) labels.erase(key);
            else labels[key] = text;
        });
    } else if (action == "axisToggleList") {
        // Un asse delle BARRE o un pulsante: c'è -> si toglie, non c'è -> si aggiunge.
        const int index = integer(m, "index", -1), item = integer(m, "item", -1);
        const std::string field = str(m, "which") == "button" ? "Buttons" : "BarAxes";
        if (index < 0 || index >= (int)s.active().panes.size() || item < 0) return;
        axisEdit([&](void* r) {
            json& pane = (*static_cast<json*>(r))["Profiles"][active]["Panes"][index];
            std::vector<int> list = pane.value(field, std::vector<int>{});
            auto it = std::find(list.begin(), list.end(), item);
            if (it != list.end()) list.erase(it);
            else list.push_back(item);
            if (field == "BarAxes") std::sort(list.begin(), list.end());
            pane[field] = list;
        });
    } else if (action == "axisPaneAdd") {
        if (s.active().panes.size() >= 10) {
            toast("Dieci moduli al massimo per profilo.", false);
            return;
        }
        const std::string kind = str(m, "kind");
        Axis::Pane p;
        p.kind = kind == "Barre" ? Axis::Kind::Barre : kind == "Pulsanti" ? Axis::Kind::Pulsanti : Axis::Kind::Assi;
        // La prima periferica di gioco collegata, se c'è; se no il mouse.
        const auto devices = Axis::devices();
        const auto controller = std::find_if(devices.begin(), devices.end(), [](const Axis::DeviceInfo& d) { return d.kind == "controller"; });
        const Axis::DeviceInfo d = controller != devices.end() ? *controller : Axis::describe("mouse");
        p.deviceId = d.id;
        p.axisX = d.defaultX;
        p.axisY = d.defaultY;
        p.axisZ = d.defaultZ;
        p.invertY = d.defaultInvertY;
        s.active().panes.push_back(p);
        axisApply(true);
    } else if (action == "axisPaneRemove") {
        const int index = integer(m, "index", -1);
        if (index < 0 || index >= (int)s.active().panes.size()) return;
        s.active().panes.erase(s.active().panes.begin() + index);
        axisApply(true);
    } else if (action == "axisPaneMove") {
        const int index = integer(m, "index", -1), to = index + (integer(m, "dir", 1) < 0 ? -1 : 1);
        auto& panes = s.active().panes;
        if (index < 0 || to < 0 || index >= (int)panes.size() || to >= (int)panes.size()) return;
        std::swap(panes[index], panes[to]);
        axisApply(true);
    } else if (action == "axisCapture") {
        const int index = integer(m, "index", -1);
        if (m_axisCaptureIndex >= 0 || index < 0 || index >= (int)s.active().panes.size()) {
            m_axisCaptureIndex = -1; // secondo clic = annulla
            m_axisCaptureSource.reset();
            sendAxis();
            return;
        }
        m_axisCaptureIndex = index;
        m_axisCaptureUntil = GetTickCount64() + 10000;
        const Axis::Pane& pane = s.active().panes[index];
        if (pane.deviceId == "keyboard") {
            m_axisCaptureBaselineKey = Axis::firstPressedKey();
        } else {
            m_axisCaptureSource = std::make_unique<Axis::Source>(pane, s);
            m_axisCaptureSource->update(0);
            m_axisCaptureBaseline = m_axisCaptureSource->state().buttons;
        }
        m_axisMessage = "Premi il pulsante da aggiungere (10 secondi)...";
        sendAxis();
    } else if (action == "axisImport") {
        Axis::Settings imported;
        std::string message;
        if (!Axis::importFromAxisVisualizer(imported, message)) {
            toast(message, false);
            return;
        }
        m_axis.settings = imported;
        axisApply(true);
        toast(message);
    }
}

// --- Diario di bordo (Game.log), raffineria, avvio con Windows, barra rapida ------------------------

namespace {
    const char* kindId(GameLog::Kind k) {
        switch (k) {
        case GameLog::Kind::Login: return "login";
        case GameLog::Kind::Contract: return "contract";
        case GameLog::Kind::Objective: return "objective";
        case GameLog::Kind::Zone: return "zone";
        case GameLog::Kind::Jurisdiction: return "jurisdiction";
        case GameLog::Kind::Health: return "health";
        case GameLog::Kind::Party: return "party";
        case GameLog::Kind::Ship: return "ship";
        case GameLog::Kind::Location: return "location";
        case GameLog::Kind::Journal: return "journal";
        case GameLog::Kind::Hangar: return "hangar";
        case GameLog::Kind::Blueprint: return "blueprint";
        case GameLog::Kind::Combat: return "combat";
        default: return "other";
        }
    }
    const char* levelId(GameLog::Level l) {
        switch (l) {
        case GameLog::Level::Good: return "ok";
        case GameLog::Level::Warn: return "warn";
        case GameLog::Level::Bad: return "bad";
        default: return "info";
        }
    }
}

void WebBridge::sendGameLog() {
    const AppSettings s = m_engine.settings();
    GameLog::Watcher& w = m_engine.gameLog();
    const GameLog::Watcher::Status ws = w.status();
    const GameLog::State st = w.state();
    json j;
    j["enabled"] = s.gameLogEnabled;
    j["running"] = w.running();
    j["path"] = TextUtils::wideToUtf8(ws.path);
    j["custom"] = !s.gameLogPath.empty();
    j["found"] = ws.found;
    j["catchingUp"] = ws.catchingUp;
    j["lines"] = ws.lines;
    j["eventCount"] = ws.events;
    j["lastEpoch"] = ws.lastEpoch;
    j["handle"] = st.handle;
    json contracts = json::array();
    for (const auto& c : st.contracts) contracts.push_back({ { "id", c.id.empty() ? c.name : c.id }, { "name", c.name }, { "since", c.since } });
    j["contracts"] = contracts;
    json objectives = json::array();
    for (const auto& o : st.objectives) objectives.push_back({ { "text", o.text }, { "since", o.since } });
    j["objectives"] = objectives;
    j["zone"] = { { "armistice", st.armistice }, { "monitored", st.monitored }, { "privateProperty", st.privateProperty } };
    j["jurisdiction"] = st.jurisdiction;
    j["location"] = st.location;
    j["lastShip"] = st.lastShip;
    json health = json::array();
    for (const auto& h : st.health) health.push_back({ { "text", h.text }, { "epoch", h.epoch } });
    j["health"] = health;
    j["session"] = { { "accepted", st.accepted }, { "completed", st.completed }, { "failed", st.failed } };
    json events = json::array();
    for (const auto& e : st.recent) {
        events.push_back({ { "kind", kindId(e.kind) }, { "action", e.action }, { "text", e.text }, { "level", levelId(e.level) }, { "epoch", e.epoch } });
    }
    j["events"] = events;
    j["now"] = (long long)std::time(nullptr);
    send("gameLog", dump(j));
}

void WebBridge::sendRefinery() {
    json jobs = json::array();
    for (const auto& r : m_engine.refinery().jobs()) {
        jobs.push_back({ { "id", r.id }, { "place", r.place }, { "note", r.note }, { "start", r.start }, { "end", r.end } });
    }
    send("refinery", dump({ { "jobs", jobs }, { "now", (long long)std::time(nullptr) } }));
}

void WebBridge::handleExtra(const std::string& action, const void* messagePtr) {
    const json& m = *static_cast<const json*>(messagePtr);
    if (action == "autostart") {
        std::string error;
        const bool on = m.value("on", false);
        if (Autostart::set(on, &error)) toast(on ? "Iris partirà con Windows, nascosta nell'area di notifica." : "Iris non parte più con Windows.");
        else toast("Avvio con Windows non cambiato: " + error, false);
        sendSettings();
    } else if (action == "quickBarShow") {
        HudHost::toggleBar();
    } else if (action == "gameLogEnable") {
        const bool on = m.value("on", false);
        // Si accende solo con l'avviso accettato nella pagina (ToS di Star Citizen, a tuo rischio).
        if (on && !m.value("accepted", false)) return;
        m_engine.updateSettings([&](AppSettings& s) {
            s.gameLogEnabled = on;
            if (on) s.gameLogAccepted = true;
        });
        toast(on ? "Diario di bordo attivo: leggo il Game.log (a tuo rischio)." : "Diario di bordo spento: il Game.log non si legge più.");
        sendGameLog();
    } else if (action == "gameLogPath") {
        defer([this]() {
            wchar_t file[MAX_PATH] = L"Game.log";
            OPENFILENAMEW ofn{ sizeof(ofn) };
            ofn.hwndOwner = m_window;
            ofn.lpstrFilter = L"Game.log\0Game.log;*.log\0Tutti i file\0*.*\0";
            ofn.lpstrFile = file;
            ofn.nMaxFile = MAX_PATH;
            ofn.lpstrTitle = L"Scegli il Game.log di Star Citizen";
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
            if (!GetOpenFileNameW(&ofn)) return;
            const std::string path = TextUtils::wideToUtf8(file);
            m_engine.updateSettings([&](AppSettings& s) { s.gameLogPath = path; });
            toast("Leggo questo Game.log.");
        });
    } else if (action == "gameLogPathAuto") {
        m_engine.updateSettings([](AppSettings& s) { s.gameLogPath.clear(); });
        toast("Il Game.log lo cerco da sola (LIVE, PTU...).");
    } else if (action == "gameLogForget") {
        m_engine.gameLog().forgetContract(str(m, "id"));
    } else if (action == "gameLogClear") {
        m_engine.gameLog().clearRecent();
    } else if (action == "refineryAdd") {
        const auto seconds = Refinery::parseDuration(str(m, "duration"));
        if (!seconds) {
            toast("Durata non capita: scrivi per esempio 3:20, 3h20 o 200 (minuti).", false);
            return;
        }
        m_engine.refinery().add(str(m, "place"), str(m, "note"), *seconds, (long long)std::time(nullptr));
        m_engine.refineryChanged();
        toast("Ti avviso tra " + Refinery::spokenDuration(*seconds) + ".");
    } else if (action == "refineryRemove") {
        if (m_engine.refinery().remove(str(m, "id"))) m_engine.refineryChanged();
    } else if (action == "refineryClearDone") {
        m_engine.refinery().clearDone((long long)std::time(nullptr));
        m_engine.refineryChanged();
    }
}

// --- Editor dei comandi di gioco (pagina Star Citizen) ------------------------------------------------

void WebBridge::sendGameCommands(bool force) {
    GameController& game = m_engine.game();
    const GameProfile p = game.profile();
    json list = json::array();
    for (size_t i = 0; i < p.commands.size(); ++i) {
        const GameCommand& c = p.commands[i];
        json steps = json::array();
        for (const auto& s : commandSteps(p, c)) steps.push_back({ { "name", s.name }, { "combo", s.combo } });
        int ms = 800;
        auto it = c.args.find("ms");
        if (it != c.args.end()) {
            try { ms = std::stoi(it->second); } catch (...) {}
        }
        list.push_back({ { "index", (int)i }, { "intent", c.intent }, { "special", isSpecialIntent(c.intent) }, { "phrases", c.phrases },
                         { "say", c.say }, { "steps", steps }, { "ms", ms } });
    }
    json keys = json::array();
    for (const auto& [name, combo] : p.keys) keys.push_back({ { "name", name }, { "combo", combo } });
    const std::string text = dump({ { "commands", list }, { "keys", keys }, { "error", game.profileError() } });
    if (!force && text == m_lastCommands) return;
    m_lastCommands = text;
    send("gameCommands", text);
}

void WebBridge::handleCommands(const std::string& action, const void* messagePtr) {
    const json& m = *static_cast<const json*>(messagePtr);
    GameController& game = m_engine.game();
    if (action == "gameKeyCapture") {
        captureGameKey();
        return;
    }
    GameProfile p = game.profile();
    std::string error;
    bool ok = false;
    if (action == "gameCmdSave") {
        CommandEdit e;
        e.index = integer(m, "index", -1);
        e.intent = str(m, "intent");
        if (m.contains("phrases") && m["phrases"].is_array()) {
            for (const auto& x : m["phrases"]) {
                if (x.is_string()) e.phrases.push_back(x.get<std::string>());
            }
        }
        if (m.contains("steps") && m["steps"].is_array()) {
            for (const auto& x : m["steps"]) {
                if (x.is_object()) e.steps.push_back({ x.value("name", std::string()), x.value("combo", std::string()) });
            }
        }
        e.holdMs = integer(m, "ms", 800);
        e.say = str(m, "say");
        ok = applyCommandEdit(p, e, error);
        if (ok && !game.saveProfile(p)) {
            ok = false;
            error = "Il profilo non si salva (disco pieno o cartella protetta?).";
        }
        if (ok) toast(e.index < 0 ? "Comando aggiunto: prova a dire «Iris, " + TextUtils::trim(e.phrases.empty() ? "" : e.phrases.front()) + "»." : "Comando salvato.");
    } else if (action == "gameCmdDelete") {
        ok = removeCommand(p, integer(m, "index", -1), error);
        if (ok && !game.saveProfile(p)) {
            ok = false;
            error = "Il profilo non si salva (disco pieno o cartella protetta?).";
        }
        if (ok) toast("Comando tolto.");
    }
    send("gameCmdResult", dump({ { "ok", ok }, { "error", error }, { "action", action } }));
    if (ok) sendGameCommands(true);
}

void WebBridge::captureGameKey() {
    if (m_gameKeyCapturing) {
        m_gameKeyCancel = true; // secondo clic = annulla
        return;
    }
    if (m_gameKeyThread.joinable()) m_gameKeyThread.join();
    m_gameKeyCancel = false;
    m_gameKeyCapturing = true;
    m_engine.setTalkPaused(true); // il tasto premuto non deve far partire altro
    send("gameKeyCapture", dump({ { "active", true } }));
    m_gameKeyThread = std::thread([this]() {
        auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
        auto isMouse = [](int vk) { return vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || vk == VK_XBUTTON1 || vk == VK_XBUTTON2; };
        auto isGeneric = [](int vk) { return vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || vk == VK_LWIN || vk == VK_RWIN || vk == VK_APPS; };
        static const int kMods[] = { VK_LCONTROL, VK_RCONTROL, VK_LSHIFT, VK_RSHIFT, VK_LMENU, VK_RMENU };
        auto isMod = [](int vk) { return std::find(std::begin(kMods), std::end(kMods), vk) != std::end(kMods); };
        bool prev[256] = {};
        for (int vk = 1; vk < 256; ++vk) prev[vk] = down(vk); // quelli già giù (il clic) non contano
        std::string combo, lastMod;
        bool other = false, cancelled = false;
        const ULONGLONG deadline = GetTickCount64() + 15000;
        while (!m_gameKeyCancel && GetTickCount64() < deadline && combo.empty()) {
            Sleep(10);
            for (int vk = 1; vk < 256 && combo.empty(); ++vk) {
                if (isMouse(vk) || isGeneric(vk)) continue;
                const bool d = down(vk);
                if (d && !prev[vk]) {
                    if (isMod(vk)) {
                        lastMod = GameInput::nameForVirtualKey(vk);
                    } else if (vk == VK_ESCAPE) {
                        cancelled = true;
                        m_gameKeyCancel = true;
                    } else {
                        const std::string name = GameInput::nameForVirtualKey(vk);
                        if (!name.empty()) {
                            other = true;
                            for (int mvk : kMods) {
                                if (down(mvk)) combo += GameInput::nameForVirtualKey(mvk) + "+";
                            }
                            combo += name;
                        }
                    }
                } else if (!d && prev[vk] && isMod(vk) && !other && GameInput::nameForVirtualKey(vk) == lastMod) {
                    combo = lastMod; // solo un modificatore, premuto e lasciato (es. LAlt)
                }
                prev[vk] = d;
            }
        }
        if (m_gameKeyCancel && combo.empty()) cancelled = true;
        Log::info(std::string("Editor dei comandi: tasto ") + (combo.empty() ? (cancelled ? "non scelto (annullato)" : "non premuto") : "scelto: " + combo));
        defer([this, combo, cancelled]() {
            if (m_gameKeyThread.joinable()) m_gameKeyThread.join();
            m_gameKeyCapturing = false;
            m_engine.setTalkPaused(false);
            send("gameKeyCapture", dump({ { "active", false }, { "combo", combo }, { "cancelled", cancelled }, { "timeout", combo.empty() && !cancelled } }));
        });
    });
}

// --- Registro delle scansioni del mining ---------------------------------------------------------------

void WebBridge::sendMiningLog() {
    MiningLog::Recorder& rec = m_engine.miningLog();
    const std::vector<MiningLog::Entry> entries = rec.entries();
    const MiningLog::Summary sum = MiningLog::summarize(entries);
    json list = json::array();
    for (auto it = entries.rbegin(); it != entries.rend() && list.size() < 60; ++it) {
        json alts = json::array();
        for (const auto& a : it->alternatives) alts.push_back(a.label + " × " + std::to_string(a.quantity));
        list.push_back({ { "time", MiningLog::isoLocal(it->epoch).substr(11, 8) }, { "value", it->value }, { "outcome", it->outcome },
                         { "label", it->label }, { "quantity", it->quantity }, { "kind", it->kind }, { "rarity", it->rarity },
                         { "alternatives", alts }, { "source", it->source }, { "reads", it->reads } });
    }
    json materials = json::array();
    for (const auto& m : sum.materials) materials.push_back({ { "label", m.label }, { "kind", m.kind }, { "rocks", m.rocks }, { "quantity", m.quantity } });
    const std::wstring last = rec.lastFile();
    const size_t slash = last.find_last_of(L"\\/");
    json j = { { "active", rec.active() },
               { "start", rec.startedAt() },
               { "now", (long long)std::time(nullptr) },
               { "entries", list },
               { "summary", { { "finds", sum.finds }, { "reads", sum.reads }, { "sure", sum.sure }, { "toCheck", sum.toCheck }, { "materials", materials } } },
               { "lastFile", TextUtils::wideToUtf8(last) },
               { "lastFileName", TextUtils::wideToUtf8(slash == std::wstring::npos ? last : last.substr(slash + 1)) },
               { "lastFileExists", !last.empty() && GetFileAttributesW(last.c_str()) != INVALID_FILE_ATTRIBUTES },
               { "places", rec.recentPlaces() },
               { "suggestedPlace", m_engine.knownPlace() },
               { "operator", m_engine.operatorName() },
               { "desktop", TextUtils::wideToUtf8(MiningLog::desktopFolder()) } };
    send("miningLog", dump(j));
}

void WebBridge::openMiningLogDialog() {
    sendMiningLog();
    send("miningLogAsk", "true");
}

void WebBridge::handleMiningLog(const std::string& action, const void* messagePtr) {
    const json& m = *static_cast<const json*>(messagePtr);
    MiningLog::Recorder& rec = m_engine.miningLog();
    if (action == "miningLogStart") {
        rec.start((long long)std::time(nullptr));
        toast("Registro acceso: ogni roccia letta finisce nel registro.");
    } else if (action == "miningLogCancel") {
        rec.cancel();
        toast("Registro chiuso senza file.");
    } else if (action == "miningLogFinish") {
        MiningLog::Closing c;
        c.place = TextUtils::trim(str(m, "place"));
        c.quality = integer(m, "quality", -1);
        c.operatorName = m_engine.operatorName();
        c.program = "Iris " + Updater::currentVersion();
        c.end = (long long)std::time(nullptr);
        std::string error;
        if (c.place.empty()) error = "Scrivi in quale luogo ti trovavi.";
        else if (c.quality < 0 || c.quality > 1000) error = "La qualità media va da 0 a 1000.";
        const bool csv = m.value("csv", false);
        m_engine.updateSettings([&](AppSettings& s) { s.miningLogCsv = csv; });
        std::wstring path;
        const bool ok = error.empty() && rec.finish(c, MiningLog::desktopFolder(), csv, path, error);
        send("miningLogResult", dump({ { "ok", ok }, { "error", error }, { "file", TextUtils::wideToUtf8(path) } }));
        if (ok) {
            const size_t slash = path.find_last_of(L"\\/");
            toast("Registro salvato sul desktop: " + TextUtils::wideToUtf8(path.substr(slash + 1)) + (csv ? " (e il CSV per Excel)" : ""));
        }
    } else if (action == "miningLogOpen") {
        const std::wstring last = rec.lastFile();
        if (!last.empty()) ShellExecuteW(m_window, L"open", last.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    } else if (action == "miningLogFolder") {
        const std::wstring last = rec.lastFile();
        if (!last.empty()) {
            const std::wstring args = L"/select,\"" + last + L"\"";
            ShellExecuteW(m_window, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
        }
        return;
    }
    m_engine.miningLogChanged();
}
