#include "game_chat.h"
#include "app_paths.h"
#include "chat_parser.h"
#include "game_input.h"
#include "gamer_slang.h"
#include "glossary.h"
#include "input_sender.h"
#include "language_guess.h"
#include "languages.h"
#include "log.h"
#include "ocr_engine.h"
#include "sc_locations.h"
#include "screen_capture.h"
#include "screen_match.h"
#include "speech_utils.h"
#include "text_utils.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>

// Il motore di Voice to Chat (engine.cpp), senza avvio, modelli, microfono e
// tasti: quelli li ha già Iris e li condivide.

namespace {
    constexpr size_t kMaxHistory = 50;
    constexpr size_t kMaxFiltered = 500;
    constexpr size_t kMaxScreenCache = 300;
    constexpr int kScreenIntervalMs = 1000;
    constexpr int kScreenConfirmMs = 400; // la seconda lettura di un messaggio nuovo arriva prima
    constexpr double kMinVoicedSeconds = 0.15;

    std::string nowHHMM() {
        SYSTEMTIME t;
        GetLocalTime(&t);
        char buf[8];
        snprintf(buf, sizeof(buf), "%02d:%02d", t.wHour, t.wMinute);
        return buf;
    }

    // Nomi propri di Star Citizen (luoghi e navi) presenti nella frase: il modello li lascia com'erano.
    std::vector<std::string> namesIn(const std::string& text) {
        std::vector<std::string> out;
        for (const auto* list : { &scKnownLocations(), &scKnownShips() }) {
            for (const auto& name : *list) {
                if (TextUtils::containsWord(text, name)) out.push_back(name);
            }
        }
        return out;
    }

    bool ourWindowInForeground() {
        HWND fg = GetForegroundWindow();
        DWORD pid = 0;
        if (fg) GetWindowThreadProcessId(fg, &pid);
        return pid == GetCurrentProcessId();
    }

    // La zona letta è la chat di Star Citizen: con un'altra finestra davanti
    // (Discord, il browser, Iris) lo schermo non si legge.
    bool starCitizenInForeground() {
        static DWORD lastPid = 0;
        static bool lastResult = false;
        HWND fg = GetForegroundWindow();
        DWORD pid = 0;
        if (fg) GetWindowThreadProcessId(fg, &pid);
        if (!pid) return false;
        if (pid == lastPid) return lastResult;
        bool result = false;
        if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
            wchar_t path[MAX_PATH];
            DWORD size = MAX_PATH;
            if (QueryFullProcessImageNameW(process, 0, path, &size)) {
                const wchar_t* file = wcsrchr(path, L'\\');
                result = _wcsicmp(file ? file + 1 : path, L"StarCitizen.exe") == 0;
            }
            CloseHandle(process);
        }
        if (!result) {
            wchar_t title[64] = {};
            GetWindowTextW(fg, title, 64);
            result = TextUtils::trim(TextUtils::wideToUtf8(title)) == "Star Citizen";
        }
        lastPid = pid;
        lastResult = result;
        return result;
    }

    bool sameScreenView(const GameChat::ScreenView& a, const GameChat::ScreenView& b) {
        return a.visible == b.visible && a.regionX == b.regionX && a.regionY == b.regionY && a.regionW == b.regionW &&
               a.regionH == b.regionH && a.lines == b.lines;
    }

    // Cosa ha letto l'OCR e come è stato diviso in messaggi.
    void writeOcrDebug(const std::vector<std::string>& lines, const std::vector<ChatMessage>& messages, const std::string& filterTag) {
        std::ofstream f(AppPaths::ocrDebugPath(), std::ios::binary | std::ios::trunc);
        if (!f.is_open()) return;
        f << "\xEF\xBB\xBF";
        f << "=== RIGHE LETTE DALL'OCR (" << lines.size() << ") ===\r\n";
        for (const auto& l : lines) f << l << "\r\n";
        f << "\r\n=== FILTRO ===\r\n\"" << filterTag << "\" (vuoto = nessun filtro)\r\n";
        f << "\r\n=== MESSAGGI RICONOSCIUTI (" << messages.size() << ") ===\r\n";
        for (const auto& m : messages) {
            std::string tags;
            for (const auto& t : m.tags) tags += "[" + t + "]";
            f << "- canali: " << (tags.empty() ? "(nessuno)" : tags) << "   giocatore: "
              << (!m.playerName.empty() ? m.playerName : m.tags.empty() ? "(seguito di un messaggio sopra)" : "(non letto)")
              << "   filtro: " << (!filterTag.empty() && ChatParser::matchesFilter(m, filterTag) ? "SI" : "no") << "\r\n"
              << "  letto come: \"" << m.rawPrefix << "\"\r\n"
              << "  testo: \"" << m.body << "\"\r\n";
        }
        if (messages.empty()) f << "(nessuna riga nel formato \"[canale] Nome: messaggio\": si traducono le righe così come sono)\r\n";
    }
}

std::string GameChat::whisperHint() {
    std::string hint = "Star Citizen: ";
    const auto& names = scKnownLocations();
    for (size_t i = 0; i < names.size(); ++i) hint += (i ? ", " : "") + names[i];
    for (const auto& e : Glossary::load()) {
        // whisper tiene solo gli ultimi ~220 token del suggerimento.
        if (hint.size() + e.first.size() > 900) break;
        hint += ", " + e.first;
    }
    return hint + ".";
}

GameChat::~GameChat() {
    shutdown();
}

void GameChat::start(Host host) {
    if (m_started.exchange(true)) return;
    m_host = std::move(host);
    m_worker = std::thread(&GameChat::workerLoop, this);
    m_screenThread = std::thread(&GameChat::screenLoop, this);
}

void GameChat::shutdown() {
    if (m_stopping.exchange(true)) return;
    m_screenCancel = true;
    {
        std::lock_guard<std::mutex> lock(m_jobMutex);
    }
    m_jobCv.notify_all();
    {
        std::lock_guard<std::mutex> lock(m_screenMutex);
        m_screenWake = true;
    }
    m_screenCv.notify_all();
    if (m_worker.joinable()) m_worker.join();
    if (m_screenThread.joinable()) m_screenThread.join();
}

void GameChat::setReady(bool ready) {
    m_ready = ready;
    if (!ready) {
        m_screenCancel = true;
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_screenMutex);
        m_screenWake = true;
    }
    m_screenCv.notify_all();
}

void GameChat::setRecording(bool on) {
    m_recording = on;
    if (on) m_screenCancel = true;
}

void GameChat::setIrisBusy(bool busy) {
    m_irisBusy = busy;
    if (busy) {
        m_screenCancel = true;
        return;
    }
    std::lock_guard<std::mutex> lock(m_screenMutex);
    m_screenWake = true;
    m_screenCv.notify_all();
}

void GameChat::screenSettingsChanged(bool enabled) {
    if (!enabled) setScreenView({});
    m_screenReset = true;
    {
        std::lock_guard<std::mutex> lock(m_screenMutex);
        m_screenWake = true;
    }
    m_screenCv.notify_all();
}

// --- Stato ---------------------------------------------------------------------------

void GameChat::notify(Event e) {
    if (m_host.notify) m_host.notify(e);
}

std::string GameChat::busyText() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_busyText;
}

GameChat::Notice GameChat::notice() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_notice;
}

void GameChat::setBusyText(const std::string& text) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_busyText = text;
    }
    notify(Event::Status);
}

void GameChat::setNotice(const std::string& text, bool error) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_notice = { text, GetTickCount64(), error };
    }
    if (error) Log::warn("Chat: " + text);
    notify(Event::Status);
}

std::vector<GameChat::HistoryEntry> GameChat::history() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return std::vector<HistoryEntry>(m_history.begin(), m_history.end());
}

GameChat::ScreenView GameChat::screenView() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_screenView;
}

std::vector<std::string> GameChat::filteredMessages() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<std::string> out;
    for (const auto& f : m_filtered) out.push_back(f.second);
    return out;
}

void GameChat::clearFilteredMessages() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_filtered.clear();
    }
    notify(Event::Filtered);
}

void GameChat::addHistory(HistoryEntry entry) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_history.push_back(std::move(entry));
        while (m_history.size() > kMaxHistory) m_history.pop_front();
    }
    notify(Event::History);
}

// --- Voce ------------------------------------------------------------------------------

void GameChat::enqueue(std::function<void()> job) {
    m_screenCancel = true;
    m_pendingJobs++;
    {
        std::lock_guard<std::mutex> lock(m_jobMutex);
        m_jobs.push_back(std::move(job));
    }
    m_jobCv.notify_one();
    notify(Event::Status);
}

void GameChat::workerLoop() {
    while (true) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lock(m_jobMutex);
            m_jobCv.wait(lock, [&]() { return m_stopping.load() || !m_jobs.empty(); });
            if (m_stopping) return;
            job = std::move(m_jobs.front());
            m_jobs.pop_front();
        }
        try {
            job();
        } catch (const std::exception& e) {
            Log::error(std::string("Chat: errore imprevisto: ") + e.what());
            setNotice("Errore imprevisto, riprova (dettagli nel registro)", true);
        } catch (...) {
            Log::error("Chat: errore imprevisto");
            setNotice("Errore imprevisto, riprova", true);
        }
        setBusyText("");
        if (--m_pendingJobs == 0) {
            // Libero: la traduzione dello schermo può riprendere subito.
            std::lock_guard<std::mutex> lock(m_screenMutex);
            m_screenWake = true;
            m_screenCv.notify_all();
        }
        notify(Event::Status);
    }
}

void GameChat::dictate(std::vector<float> samples) {
    enqueue([this, s = std::move(samples)]() { processUtterance(s); });
}

void GameChat::sendText(const std::string& heard) {
    const std::string text = TextUtils::collapseSpaces(TextUtils::trim(heard));
    if (text.empty()) return;
    enqueue([this, text]() { writeToChat(text); });
}

bool GameChat::translateVoice(const AppSettings& s, const std::string& text, std::string& out) {
    const auto& langs = supportedLanguages();
    TranslationRequest r;
    r.text = text;
    r.sourceLanguage = langs[s.sourceLanguageIndex].englishName;
    r.targetLanguage = langs[s.targetLanguageIndex].englishName;
    r.gamingTone = s.gamingTone;
    r.glossary = Glossary::relevantTo(Glossary::load(), text);
    // Il vocabolario del videogiocatore, dove il glossario personale non dice già altro.
    for (const auto& e : GamerSlang::forSending(text, r.sourceLanguage, s.gamingTone)) {
        const bool mine = std::any_of(r.glossary.begin(), r.glossary.end(), [&](const auto& g) {
            return TextUtils::toLower(g.first) == TextUtils::toLower(e.first);
        });
        if (!mine) r.glossary.push_back(e);
    }
    r.keepNames = namesIn(text);
    LlmEngine::Result res = m_llm.translate(r, out, LlmEngine::Channel::Voice, &m_stopping);
    if (res != LlmEngine::Result::Ok) {
        Log::warn("Chat: traduzione non riuscita: " + text);
        return false;
    }
    return true;
}

void GameChat::processUtterance(const std::vector<float>& samples) {
    const AppSettings s = settings();
    const auto& langs = supportedLanguages();
    if (!m_ready || !m_whisper.isReady()) {
        setNotice("Sto ancora caricando i modelli: riprova tra qualche secondo.", true);
        return;
    }
    const double voiced = SpeechUtils::voicedSeconds(samples, s.silenceThreshold);
    if (voiced < kMinVoicedSeconds) {
        Log::info("Chat: nessuna voce (" + std::to_string(samples.size() / SpeechUtils::kSampleRate) + " s registrati, livello medio " +
                  std::to_string(SpeechUtils::rms(samples)) + ")");
        setNotice("Non ho sentito parlare. Microfono basso? Alza il volume o rifai la calibrazione.", true);
        return;
    }
    std::vector<float> speech = SpeechUtils::trimSilence(samples, s.silenceThreshold, 300);
    setBusyText("Trascrivo...");
    const std::string heard = m_whisper.transcribe(speech, langs[s.sourceLanguageIndex].code, whisperHint());
    if (m_stopping) return;
    if (heard.empty()) {
        setNotice("Non ho capito, riprova", true);
        return;
    }
    Log::info("Chat, sentito: " + heard);
    writeToChat(heard);
}

void GameChat::writeToChat(const std::string& heard) {
    AppSettings s = settings();
    if (!m_ready || !m_llm.isReady()) {
        addHistory({ nowHHMM(), heard, "", false, "non scritta: i modelli si stanno ancora caricando" });
        setNotice("Sto ancora caricando i modelli: riprova tra qualche secondo.", true);
        return;
    }
    std::string finalText = heard;
    if (s.sourceLanguageIndex != s.targetLanguageIndex) {
        setBusyText("Traduco: " + heard);
        if (!translateVoice(s, heard, finalText)) {
            if (m_stopping) return;
            addHistory({ nowHHMM(), heard, "", false, "traduzione non riuscita: niente scritto in chat" });
            setNotice("Traduzione non riuscita, riprova", true);
            return;
        }
    }
    finalText = TextUtils::fixShoutingCase(finalText);
    Log::info("Chat, da scrivere: " + finalText);

    HistoryEntry entry{ nowHHMM(), heard, finalText, false, "" };
    if (ourWindowInForeground()) {
        // Non si scrive dentro Iris: così si prova microfono e traduzione senza il gioco.
        entry.note = "non scritta: in primo piano c'era Iris";
        addHistory(entry);
        if (m_host.onWritten) m_host.onWritten(heard, finalText, false);
        setNotice("Tradotta (non scritta in chat: in primo piano c'era Iris): " + finalText);
        return;
    }

    // Le impostazioni di invio si rileggono ora: F10 potrebbe essere stato premuto nel frattempo.
    s = settings();
    setBusyText("Scrivo in chat...");
    {
        // Un comando della nave in corso finisce prima: i tasti non si mescolano.
        std::lock_guard<std::recursive_mutex> keys(GameInput::inputLock());
        InputSender::waitForModifiersReleased(1500);
        InputSender::Options o;
        o.openChat = s.chatOpenEnabled;
        o.openKeyVk = s.chatOpenKeyVk;
        o.autoSend = s.autoSend;
        o.sendKeyVk = s.chatSendKeyVk;
        o.delayAfterOpenMs = s.chatOpenDelayMs;
        o.interCharDelayMs = s.typingDelayMs;
        o.unmappable = s.unicodeFallback ? InputSender::Unmappable::Unicode : InputSender::Unmappable::Transliterate;
        InputSender::sendChatMessage(finalText, o);
    }
    entry.typed = true;
    if (!s.autoSend) entry.note = "scritta, non inviata (invio automatico spento)";
    addHistory(entry);
    if (m_host.onWritten) m_host.onWritten(heard, finalText, true);
    setNotice((s.autoSend ? "Inviato in chat: " : "Scritto in chat, premi Invio per mandarlo: ") + finalText);
}

void GameChat::translateTest(const std::string& rawText) {
    const std::string text = TextUtils::collapseSpaces(TextUtils::trim(rawText));
    if (text.empty()) return;
    enqueue([this, text]() {
        if (!m_ready || !m_llm.isReady()) {
            addHistory({ nowHHMM(), text, "", false, "prova non fatta: i modelli si stanno ancora caricando" });
            return;
        }
        AppSettings s = settings();
        std::string out = text;
        std::string note = "prova: non scritta nel gioco";
        if (s.sourceLanguageIndex == s.targetLanguageIndex) {
            note = "lingua parlata e di traduzione uguali: nessuna traduzione";
        } else {
            setBusyText("Traduco la prova: " + text);
            if (!translateVoice(s, text, out)) {
                if (m_stopping) return;
                addHistory({ nowHHMM(), text, "", false, "traduzione non riuscita" });
                setNotice("Traduzione non riuscita", true);
                return;
            }
        }
        out = TextUtils::fixShoutingCase(out);
        addHistory({ nowHHMM(), text, out, false, note });
        setNotice("Prova: " + out);
    });
}

void GameChat::calibrateSilence() {
    enqueue([this]() {
        if (m_host.micOpen && !m_host.micOpen()) {
            setNotice("Nessun microfono aperto: sceglilo nella pagina Ascolto e voce.", true);
            return;
        }
        setBusyText("Calibrazione: resta in silenzio per 2 secondi...");
        std::vector<float> levels;
        for (int i = 0; i < 100 && !m_stopping; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            levels.push_back(m_host.micLevel ? m_host.micLevel() : 0.0f);
        }
        if (m_stopping || levels.empty()) return;
        std::sort(levels.begin(), levels.end());
        // Il rumore di fondo "alto" (90%): la soglia sta un po' sopra.
        const double noise = levels[levels.size() * 9 / 10];
        const double threshold = std::clamp(noise * 2.5, 0.003, 0.05);
        if (m_host.updateSettings) m_host.updateSettings([&](AppSettings& x) { x.silenceThreshold = threshold; });
        char buf[160];
        snprintf(buf, sizeof(buf), "Calibrato: soglia del silenzio %.4f (rumore di fondo %.4f)", threshold, noise);
        Log::info(buf);
        setNotice(buf);
    });
}

// --- Traduzione dello schermo ----------------------------------------------------------

void GameChat::setScreenView(ScreenView view) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (sameScreenView(m_screenView, view)) return;
        m_screenView = std::move(view);
    }
    notify(Event::Screen);
}

void GameChat::addFiltered(const std::string& key, const std::string& line) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        // Lo stesso messaggio resta a schermo per un po' (e l'OCR lo legge con
        // piccole differenze): si tiene una volta sola.
        size_t checked = 0;
        for (auto it = m_filtered.rbegin(); it != m_filtered.rend() && checked < 100; ++it, ++checked) {
            if (it->first == key || TextUtils::similarity(it->first, key) >= 0.9) return;
        }
        m_filtered.push_back({ key, nowHHMM() + "  " + line });
        while (m_filtered.size() > kMaxFiltered) m_filtered.pop_front();
    }
    notify(Event::Filtered);
}

void GameChat::screenLoop() {
    while (!m_stopping) {
        {
            std::unique_lock<std::mutex> lock(m_screenMutex);
            m_screenCv.wait_for(lock, std::chrono::milliseconds(m_screenAwaiting ? kScreenConfirmMs : kScreenIntervalMs),
                                [&]() { return m_screenWake || m_stopping.load(); });
            m_screenWake = false;
        }
        if (m_stopping) break;

        if (m_screenReset.exchange(false)) {
            m_shown.clear();
            m_lastScreenHash = 0;
        }
        AppSettings s = settings();
        if (!s.ocrEnabled || s.ocrRegionW < 10 || s.ocrRegionH < 10) {
            setScreenView({});
            m_lastScreenHash = 0;
            continue;
        }
        // Mentre parli, una tua frase è in lavorazione o Iris usa il modello, lo schermo aspetta.
        if (!m_ready || m_recording || m_pendingJobs > 0 || m_irisBusy) continue;
        // Con un'altra finestra davanti il riquadro resta com'era.
        if (!starCitizenInForeground()) continue;
        try {
            screenStep(s);
        } catch (const std::exception& e) {
            Log::error(std::string("Schermo: errore imprevisto: ") + e.what());
        } catch (...) {
            Log::error("Schermo: errore imprevisto");
        }
    }
}

void GameChat::screenStep(const AppSettings& s) {
    ScreenCapture::Image image = ScreenCapture::capture(s.ocrRegionX, s.ocrRegionY, s.ocrRegionW, s.ocrRegionH);
    if (image.empty()) return;
    // La zona fa parte dell'impronta: la stessa immagine in un'altra zona è un'altra lettura.
    const unsigned long long hash = ScreenCapture::hash(image) ^ ((unsigned long long)s.ocrRegionX * 73856093ULL) ^
                                    ((unsigned long long)s.ocrRegionY * 19349663ULL) ^
                                    ((unsigned long long)s.ocrTargetLanguageIndex << 56) ^ ((unsigned long long)s.ocrMaxMessages << 48);
    // Stessa immagine: niente di nuovo, a meno che ci sia un messaggio da confermare.
    if (hash == m_lastScreenHash && !m_screenAwaiting) return;

    if (m_ocrMaxDimension <= 0) m_ocrMaxDimension = OcrEngineWrapper::maxImageDimension();
    const int scale = m_ocrScale;
    ScreenCapture::Image prepared = ScreenCapture::prepareForOcr(image, scale, m_ocrMaxDimension);

    // Si traduce solo l'inglese: l'OCR legge in inglese (o con la lingua latina installata in Windows).
    const LanguageOption& ocrTarget = supportedLanguages()[s.ocrTargetLanguageIndex];

    std::string error;
    std::vector<OcrEngineWrapper::Line> ocrLines = OcrEngineWrapper::recognize("en", prepared, error);
    if (!error.empty()) {
        if (error != m_lastOcrError) Log::warn("OCR: " + error);
        m_lastOcrError = error;
        return;
    }
    m_lastOcrError.clear();

    // L'OCR di Windows legge meglio il testo alto circa 54 pixel: si misura
    // l'altezza vera delle righe e si sceglie l'ingrandimento per le letture successive.
    if (!ocrLines.empty()) {
        std::vector<int> heights;
        for (const auto& l : ocrLines) heights.push_back(l.height);
        std::sort(heights.begin(), heights.end());
        const double textHeight = std::max(1.0, (double)heights[heights.size() / 2] / scale);
        const double ideal = 54.0 / textHeight;
        const int wanted = std::clamp((int)std::lround(ideal), 1, 3);
        if (wanted != m_ocrScale && std::abs(ideal - m_ocrScale) > 0.75) {
            Log::info("OCR: testo alto " + std::to_string((int)textHeight) + " px, ingrandimento x" + std::to_string(wanted));
            m_ocrScale = wanted;
            m_lastScreenHash = 0; // si rilegge subito con la misura giusta
            return;
        }
    }

    std::vector<std::string> lines;
    for (const auto& l : ocrLines) lines.push_back(l.text);
    std::vector<ChatMessage> messages = ChatParser::parse(lines);

    const std::string joined = OcrEngineWrapper::joinLines(ocrLines);
    if (joined != m_lastOcrText) {
        m_lastOcrText = joined;
        writeOcrDebug(lines, messages, s.chatFilterTag);
    }

    ScreenView view;
    view.regionX = s.ocrRegionX;
    view.regionY = s.ocrRegionY;
    view.regionW = s.ocrRegionW;
    view.regionH = s.ocrRegionH;

    struct Item {
        std::string prefix, body, key, player, translated;
        std::string source;     // il testo da tradurre: la lettura più pulita tra questa e quella prima
        bool filtered = false;
        bool fragment = false;  // la coda di un messaggio cominciato sopra la zona letta
        bool waiting = false;   // letto una volta sola: si traduce quando la lettura si conferma
        bool translate = false; // confermato: va al modello
    };
    // Solo i messaggi della chat: menu, scritte e HUD non si traducono.
    std::vector<Item> items;
    const size_t first = messages.size() > (size_t)s.ocrMaxMessages ? messages.size() - s.ocrMaxMessages : 0;
    for (size_t i = first; i < messages.size(); ++i) {
        const ChatMessage& m = messages[i];
        Item it;
        it.prefix = m.displayPrefix();
        it.body = m.body;
        it.player = m.playerName;
        it.key = TextUtils::normalizeForMatch(m.body);
        it.filtered = !s.chatFilterTag.empty() && ChatParser::matchesFilter(m, s.chatFilterTag);
        it.fragment = m.playerName.empty() && m.tags.empty();
        if (!it.key.empty() && !ChatParser::unreadable(it.body)) items.push_back(std::move(it));
    }

    // La chat di Star Citizen si nasconde da sola dopo un po': il riquadro sparisce con lei.
    auto noChat = [&]() {
        if (++m_emptyReads >= 2) setScreenView(view);
        m_lastScreenHash = hash;
        m_screenAwaiting = false;
    };
    if (items.empty()) return noChat();

    const bool sameLanguage = std::string(ocrTarget.englishName) == "English";
    const bool cjkTarget = std::string(ocrTarget.code) == "zh" || std::string(ocrTarget.code) == "ja";
    const std::string cacheTarget = ocrTarget.englishName;
    if (cacheTarget != m_screenCacheTarget) {
        m_screenCache.clear();
        m_screenCacheTarget = cacheTarget;
    }

    auto lookup = [&](const std::string& key, std::string& out) -> bool {
        auto it = m_screenCache.find(key);
        if (it == m_screenCache.end()) {
            for (auto c = m_screenCache.begin(); c != m_screenCache.end(); ++c) {
                if (ScreenMatch::sameText(c->first, key) && ScreenMatch::sameNumbers(c->first, key)) {
                    it = c;
                    break;
                }
            }
        }
        if (it == m_screenCache.end()) return false;
        it->second.lastUsed = ++m_screenTick;
        out = it->second.text;
        return true;
    };
    auto remember = [&](const std::string& key, const std::string& text) {
        m_screenCache[key] = { text, ++m_screenTick };
        if (m_screenCache.size() > kMaxScreenCache) {
            auto oldest = std::min_element(m_screenCache.begin(), m_screenCache.end(),
                                           [](const auto& a, const auto& b) { return a.second.lastUsed < b.second.lastUsed; });
            m_screenCache.erase(oldest);
        }
    };

    // Le tue frasi scritte in chat: si mostra quello che hai detto, senza ritradurle.
    std::vector<HistoryEntry> typed;
    if (s.sourceLanguageIndex == s.ocrTargetLanguageIndex) {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto h = m_history.rbegin(); h != m_history.rend() && typed.size() < 5; ++h) {
            if (h->typed) typed.push_back(*h);
        }
    }
    auto ownMessage = [&](const std::string& key, std::string& out) -> bool {
        for (const auto& h : typed) {
            if (ScreenMatch::likeness(TextUtils::normalizeForMatch(h.written), key) >= 0.8) {
                out = h.heard;
                return true;
            }
        }
        return false;
    };

    // Ogni messaggio si traduce una volta sola.
    std::vector<std::string> previousKeys, currentKeys;
    for (const auto& p : m_shown) previousKeys.push_back(p.key);
    for (const auto& it : items) currentKeys.push_back(it.key);
    const std::vector<int> match = ScreenMatch::align(previousKeys, currentKeys);

    m_screenAwaiting = false;
    for (size_t i = 0; i < items.size(); ++i) {
        Item& it = items[i];
        it.source = it.body;
        const ShownMessage* before = match[i] >= 0 ? &m_shown[match[i]] : nullptr;
        if (before && !before->translated.empty()) {
            it.translated = before->translated;
            if (!before->prefix.empty()) it.prefix = before->prefix;
            if (!it.fragment && it.key != before->key) remember(it.key, it.translated);
            continue;
        }
        // Solo i messaggi in inglese: gli altri restano come sono.
        if (sameLanguage || !LanguageGuess::isEnglish(it.body)) {
            it.translated = it.body;
            continue;
        }
        if (lookup(it.key, it.translated) || ownMessage(it.key, it.translated)) continue;
        if (it.fragment) continue;
        // Si traduce solo una lettura confermata (letta quasi uguale anche al giro prima).
        if (before && ScreenMatch::sameText(before->key, it.key)) {
            it.translate = true;
            if (ChatParser::garbledWords(before->body) < ChatParser::garbledWords(it.body)) it.source = before->body;
        } else {
            it.waiting = true;
            m_screenAwaiting = true;
        }
    }
    items.erase(std::remove_if(items.begin(), items.end(), [](const Item& it) { return it.fragment && it.translated.empty(); }),
                items.end());
    if (items.empty()) return noChat();
    m_emptyReads = 0;

    auto render = [&]() {
        view.lines.clear();
        for (const auto& it : items) {
            view.lines.push_back({ it.prefix, it.translated.empty() ? it.body : it.translated, it.translated.empty() });
        }
        view.visible = true;
        setScreenView(view);
    };
    render();

    bool complete = true;
    for (auto& it : items) {
        if (!it.translate) continue;
        if (m_stopping || m_recording || m_pendingJobs > 0 || m_irisBusy) {
            complete = false;
            break;
        }
        m_screenCancel = false;
        TranslationRequest r;
        r.text = it.source;
        r.sourceLanguage = "English";
        r.targetLanguage = ocrTarget.englishName;
        r.gamingTone = false;
        r.fromScreen = true;
        r.keepNames = namesIn(it.source);
        std::string out;
        LlmEngine::Result res = m_llm.translate(r, out, LlmEngine::Channel::Screen, &m_screenCancel);
        if (res == LlmEngine::Result::Cancelled) {
            complete = false;
            break;
        }
        // Meglio l'originale che niente, o che una risposta finita in cinese.
        if (res != LlmEngine::Result::Ok || out.empty() || (!cjkTarget && LanguageGuess::hasCjk(out) && !LanguageGuess::hasCjk(it.source))) {
            out = it.source;
        } else {
            out = GamerSlang::explain(out, r.targetLanguage); // le sigle rimaste: "omw" -> "sto arrivando"
        }
        Log::info("Schermo: " + it.prefix + it.source + "  ->  " + out);
        it.translated = out;
        remember(it.key, out);
        render();
    }

    m_shown.clear();
    for (const auto& it : items) m_shown.push_back({ it.key, it.prefix, it.body, it.translated });

    for (const auto& it : items) {
        if (!it.filtered || it.translated.empty()) continue;
        addFiltered(TextUtils::normalizeForMatch(it.player + " " + it.body),
                    "[" + s.chatFilterTag + "] " + (it.player.empty() ? "" : it.player + ": ") + it.translated);
    }
    if (complete) m_lastScreenHash = hash;
}
