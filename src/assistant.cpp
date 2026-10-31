#include "assistant.h"
#include "app_paths.h"
#include "gpu.h"
#include "log.h"
#include "model_downloader.h"
#include "speech_text.h"
#include "speech_utils.h"
#include "text_utils.h"
#include "vad_segmenter.h"
#include "wake_word.h"

#include <windows.h>
#include <objbase.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace {
    constexpr size_t kMaxEntries = 300;
    constexpr int kWakeWindowMs = 8000;     // dopo "Iris" da sola: tempo per dire la richiesta
    constexpr int kEchoGuardMs = 400;       // dopo che Iris ha smesso di parlare, la sua eco può arrivare ancora
    constexpr int kPttGuardMs = 400;
    constexpr double kMinVoicedSeconds = 0.15;
    // Suggerimento per whisper. Una frase intera, non solo "Iris": con il nome
    // da solo whisper lo considera "già detto" e lo toglie dall'inizio della
    // frase ("Iris, che ore sono?" -> "che ore sono?"). Provato con iris_tests --ascolto.
    constexpr const char* kWhisperHint = "Parlo con Iris, la mia assistente vocale.";
    // In gioco: anche i nomi di Star Citizen ("Seraphim" non diventa "sera a film").
    constexpr const char* kWhisperHintGame =
        "Parlo con Iris, la mia assistente vocale, mentre gioco a Star Citizen: rotta per Crusader, Seraphim, Port Tressler, Everus Harbor, "
        "Area18, Lorville, quantum, carrello, scudi.";

    constexpr long long kWhisperMinBytes = 870LL * 1000 * 1000;
    constexpr long long kVadMinBytes = 880LL * 1000;
    constexpr long long kLlmMinBytes = 4600LL * 1000 * 1000;
    constexpr long long kWhisperGpuBytes = 1300LL << 20;
    constexpr long long kVoiceMinBytes = 1800LL * 1000 * 1000;      // Qwen3-TTS-12Hz-1.7B-Base-Q8_0.gguf: 1.847.874.400 byte
    constexpr long long kVoiceMmprojMinBytes = 440LL * 1000 * 1000; // mmproj: 446.422.912 byte
    constexpr long long kVoiceGpuBytes = 2600LL << 20;

    const wchar_t* kWhisperUrl = L"https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-large-v3-turbo-q8_0.bin";
    const wchar_t* kVadUrl = L"https://huggingface.co/ggml-org/whisper-vad/resolve/main/ggml-silero-v6.2.0.bin";
    const wchar_t* kVoiceUrl = L"https://huggingface.co/ggml-org/Qwen3-TTS-12Hz-1.7B-Base-GGUF/resolve/main/Qwen3-TTS-12Hz-1.7B-Base-Q8_0.gguf";
    const wchar_t* kVoiceMmprojUrl = L"https://huggingface.co/ggml-org/Qwen3-TTS-12Hz-1.7B-Base-GGUF/resolve/main/mmproj-Qwen3-TTS-12Hz-1.7B-Base-Q8_0.gguf";
    const wchar_t* kLlmUrl = L"https://huggingface.co/bartowski/Qwen2.5-7B-Instruct-GGUF/resolve/main/Qwen2.5-7B-Instruct-Q4_K_M.gguf";

    long long nowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    std::string nowHHMM() {
        SYSTEMTIME t;
        GetLocalTime(&t);
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%02d:%02d", t.wHour, t.wMinute);
        return buf;
    }

    int inferenceThreads() {
        int logical = (int)std::thread::hardware_concurrency();
        return std::clamp(logical / 2, 2, 8);
    }

    bool fileExists(const std::wstring& path) {
        DWORD a = GetFileAttributesW(path.c_str());
        return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
    }

    long long fileSize(const std::wstring& path) {
        WIN32_FILE_ATTRIBUTE_DATA info{};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)) return -1;
        return (static_cast<long long>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    }

    std::string gigabytes(long long bytes) {
        if (bytes <= 0) return "";
        char buf[32];
        std::snprintf(buf, sizeof(buf), " (%.1f GB)", bytes / 1073741824.0);
        std::string s = buf;
        std::replace(s.begin(), s.end(), '.', ',');
        return s;
    }

    bool isNewConversationCommand(const std::string& command) {
        const std::string n = WakeWord::normalize(command);
        return n == "nuova conversazione" || n == "ricominciamo" || n == "ricominciamo da capo" || n == "dimentica tutto" ||
               n == "dimentica la conversazione" || n == "cambiamo argomento";
    }
}

Assistant::Assistant() {
    m_settings = AppSettings::load();
    m_status.text = "Avvio...";
    loadMemory();
    // Prima che la finestra legga le impostazioni: la chat di gioco con quelle di Voice to Chat.
    importVoiceToChat();
}

Assistant::~Assistant() {
    shutdown();
}

void Assistant::setListener(Listener listener) {
    std::lock_guard<std::mutex> lock(m_listenerMutex);
    m_listener = std::move(listener);
}

void Assistant::notify(Event e) {
    std::lock_guard<std::mutex> lock(m_listenerMutex);
    if (m_listener) m_listener(e);
}

// --- Avvio -----------------------------------------------------------------------

void Assistant::start() {
    if (m_worker.joinable()) return;
    m_game.load();
    AppSettings s = settings();
    m_audio.setGain(s.micGain);
    m_hotkeys.setTalkTriggers(s.triggers);
    m_hotkeys.setChatTriggers(s.chatEnabled ? s.chatTriggers : std::vector<TalkTrigger>{});
    m_hotkeys.setOnTalkStart([this]() { onTalkStart(); });
    m_hotkeys.setOnTalkStop([this]() { onTalkStop(); });
    m_hotkeys.setOnChatStart([this]() { onChatStart(); });
    m_hotkeys.setOnChatStop([this]() { onChatStop(); });
    // F10: invio automatico dei messaggi in chat sì/no, anche mentre giochi (come in Voice to Chat).
    m_hotkeys.registerToggleKey(VK_F10, [this]() {
        if (!settings().chatEnabled) return;
        bool on = false;
        updateSettings([&](AppSettings& x) {
            x.autoSend = !x.autoSend;
            on = x.autoSend;
        });
        m_chat.setNotice(on ? "Invio automatico in chat ATTIVO (F10)" : "Invio automatico in chat SPENTO: il messaggio resta da inviare (F10)");
        m_tts.chime(on ? TtsEngine::Chime::Confirm : TtsEngine::Chime::Error);
    });
    m_hotkeys.start();

    // La chat di gioco, con i modelli di Iris.
    GameChat::Host host;
    host.settings = [this]() { return settings(); };
    host.updateSettings = [this](const std::function<void(AppSettings&)>& change) { updateSettings(change); };
    host.notify = [this](GameChat::Event e) {
        switch (e) {
        case GameChat::Event::History: notify(Event::ChatHistory); break;
        case GameChat::Event::Screen: notify(Event::Screen); break;
        case GameChat::Event::Filtered: notify(Event::Filtered); break;
        case GameChat::Event::Status: break; // lo stato lo ricalcola refreshState
        }
    };
    host.micLevel = [this]() { return m_audio.getCurrentLevel(); };
    host.micOpen = [this]() { return m_audio.isOpen(); };
    host.onWritten = [this](const std::string& heard, const std::string& written, bool typed) {
        addEntry(Entry::Kind::Tool, std::string(typed ? "In chat: " : "Per la chat (non scritto, in primo piano c'era Iris): ") + written +
                                        (written != heard ? "   (detto: \"" + heard + "\")" : ""));
    };
    m_chat.start(std::move(host));

    m_tts.setVoice(s.voiceName);
    m_tts.setRate(s.speechRate);
    m_tts.setEffect(s.voiceEffect);
    m_tts.setOutputDevice(s.outputDeviceName);
    m_tts.setNeuralVoice(&m_neural);
    m_tts.start();

    m_worker = std::thread(&Assistant::workerLoop, this);
    m_speechThread = std::thread(&Assistant::speechLoop, this);
    m_listenThread = std::thread(&Assistant::listenLoop, this);
    m_timerThread = std::thread(&Assistant::timerLoop, this);
    m_startupRunning = true;
    m_startup = std::thread(&Assistant::startupThread, this);
}

void Assistant::retryStart() {
    if (m_stopping || m_ready || m_startupRunning) return;
    if (m_startup.joinable()) m_startup.join();
    m_startupRunning = true;
    m_startup = std::thread(&Assistant::startupThread, this);
}

void Assistant::startupThread() {
    struct Done {
        std::atomic<bool>& flag;
        ~Done() { flag = false; }
    } done{ m_startupRunning };

    Log::info("Avvio di Iris");
    setStatus(State::Starting, "Avvio...");
    if (!AppPaths::ensureDirectoriesExist()) {
        setStatus(State::Error, "Impossibile creare la cartella %appdata%\\Iris", true);
        return;
    }
    if (!m_whisper.isReady()) {
        if (!ensureModel(kWhisperUrl, AppPaths::whisperModelPath(), false, kWhisperMinBytes, "modello vocale", 1)) return;
        if (m_stopping) return;
        if (!ensureModel(kVadUrl, AppPaths::vadModelPath(), false, kVadMinBytes, "rilevatore di voce", 2)) return;
        if (m_stopping) return;
    }
    if (!m_llm.isReady()) {
        const std::string custom = settings().llmModelFile;
        if (custom.empty()) {
            if (!ensureModel(kLlmUrl, AppPaths::llmModelPath(), true, kLlmMinBytes, "modello di linguaggio", 3)) return;
        } else if (!fileExists(AppPaths::llmModelPath(custom))) {
            setStatus(State::Error, "Il modello \"" + custom + "\" non c'è in %appdata%\\VoiceToChat\\models. Mettilo lì o torna al modello predefinito.", true);
            return;
        }
        if (m_stopping) return;
    }
    // La voce neurale: se il download non riesce, parla la voce di Windows (Iris parte lo stesso).
    if (settings().voiceEngine == "neurale" && !m_neural.isReady() && Gpu::bestDevice()) {
        if (!ensureModel(kVoiceUrl, AppPaths::neuralVoiceModelPath(), true, kVoiceMinBytes, "modello della voce", 4, 5) ||
            !ensureModel(kVoiceMmprojUrl, AppPaths::neuralVoiceMmprojPath(), true, kVoiceMmprojMinBytes, "decodificatore della voce", 5, 5)) {
            Log::warn("Voce neurale non scaricata: uso la voce di Windows");
        }
        if (m_stopping) return;
    }
    if (!loadModels()) return;
    if (!m_vad.usesModel()) m_vad.init(AppPaths::toLibraryPath(AppPaths::vadModelPath()), 1);

    AppSettings s = settings();
    openAudio(s.deviceNames);
    m_audio.setStreaming(s.wakeWordEnabled);
    m_ready = true;
    m_chat.setReady(true);
    if (!m_audio.isOpen()) {
        setStatus(State::Error, "Nessun microfono disponibile: collegane uno e sceglilo nelle impostazioni. Puoi comunque scrivermi.");
    }
    refreshState();
    Log::info("Iris pronta");
    if (s.speechEnabled) {
        say(s.userName.empty() ? "Sistemi online. Sono pronta." : "Ciao " + s.userName + ", sono online e pronta.");
    }
}

bool Assistant::loadModels() {
    while (true) {
        const AppSettings s = settings();
        const int limitMb = s.gpuMemoryLimitMb;
        if ((m_whisper.isReady() || m_llm.isReady()) && (limitMb != m_loadedGpuLimitMb || s.llmModelFile != m_loadedModelFile)) {
            m_whisper.unload();
            m_llm.unload();
            m_neural.unload();
        }
        m_loadedGpuLimitMb = limitMb;
        m_loadedModelFile = s.llmModelFile;

        if (!m_whisper.isReady()) {
            setStatus(State::Starting, "Carico il modello vocale...");
            if (!m_whisper.init(AppPaths::toLibraryPath(AppPaths::whisperModelPath()), AppPaths::toLibraryPath(AppPaths::vadModelPath()),
                                inferenceThreads(), limitMb != 0)) {
                setStatus(State::Error, "Il modello vocale non si carica. Cancella ggml-large-v3-turbo-q8_0.bin da %appdata%\\VoiceToChat\\models e premi Riprova.", true);
                return false;
            }
        }
        if (m_stopping) return false;

        // La voce neurale: solo sulla scheda video (sul processore sarebbe più lenta del parlato).
        const bool wantNeural = s.voiceEngine == "neurale" && limitMb != 0 && Gpu::bestDevice() &&
                                fileExists(AppPaths::neuralVoiceModelPath()) && fileExists(AppPaths::neuralVoiceMmprojPath());
        if (!wantNeural && m_neural.isReady()) {
            m_tts.setUseNeural(false);
            m_neural.unload();
        }
        if (wantNeural && !m_neural.isReady()) {
            setStatus(State::Starting, "Carico la voce neurale...");
            if (m_neural.load(AppPaths::toLibraryPath(AppPaths::neuralVoiceModelPath()), AppPaths::toLibraryPath(AppPaths::neuralVoiceMmprojPath()),
                              inferenceThreads())) {
                // La prima frase sulla scheda video è lenta (prepara i suoi programmi): una a vuoto adesso.
                m_neural.speak("Pronta.", [](const float*, size_t) { return true; });
            } else {
                Log::warn("Voce neurale non caricata: uso la voce di Windows");
            }
        }
        m_tts.setUseNeural(m_neural.isReady() && s.voiceEngine == "neurale");
        if (m_stopping) return false;

        if (!m_llm.isReady()) {
            setStatus(State::Starting, "Carico il cervello di Iris...");
            const long long voiceBytes = !m_whisper.onGpu() ? 0 : m_whisper.gpuBytes() > 0 ? m_whisper.gpuBytes() : kWhisperGpuBytes;
            const long long neuralBytes = !m_neural.isReady() ? 0 : m_neural.gpuBytes() > 0 ? m_neural.gpuBytes() : kVoiceGpuBytes;
            const long long budget = limitMb < 0 ? -1 : std::max(0LL, ((long long)limitMb << 20) - voiceBytes - neuralBytes);
            if (!m_llm.init(AppPaths::toLibraryPath(AppPaths::llmModelPath(s.llmModelFile)), inferenceThreads(), budget)) {
                setStatus(State::Error, "Il modello di linguaggio non si carica. Controlla il file .gguf in %appdata%\\VoiceToChat\\models e premi Riprova.", true);
                return false;
            }
        }
        if (m_stopping) return false;

        std::string summary;
        if (!Gpu::bestDevice()) {
            summary = "Nessuna scheda video compatibile: tutto sul processore.";
        } else {
            const std::string voice = m_whisper.onGpu() ? "modello vocale" + gigabytes(m_whisper.gpuBytes()) : "";
            const int layers = m_llm.gpuLayers(), total = m_llm.totalLayers();
            const std::string brain = layers <= 0 ? ""
                                      : layers >= total ? "cervello intero" + gigabytes(m_llm.gpuBytes())
                                                        : "cervello, " + std::to_string(layers) + " parti su " + std::to_string(total) + gigabytes(m_llm.gpuBytes());
            const std::string speech = m_neural.isReady() ? "voce neurale" + gigabytes(m_neural.gpuBytes()) : "";
            std::vector<std::string> parts;
            for (const std::string& p : { voice, speech, brain }) {
                if (!p.empty()) parts.push_back(p);
            }
            if (parts.empty()) summary = "Niente sulla scheda video, tutto sul processore.";
            else {
                summary = "Sulla scheda video: ";
                for (size_t i = 0; i < parts.size(); ++i) summary += (i == 0 ? "" : i + 1 == parts.size() ? " e " : ", ") + parts[i];
                summary += std::string(".") + (layers < total ? " Il resto del cervello sul processore." : "");
            }
        }
        summary += " Modello: " + m_llm.modelName() + ".";
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_gpuSummary = summary;
        }
        Log::info("Memoria video: " + summary);

        const AppSettings now = settings();
        if (now.gpuMemoryLimitMb == limitMb && now.llmModelFile == s.llmModelFile && now.voiceEngine == s.voiceEngine) return true;
    }
}

void Assistant::reloadModels() {
    if (m_stopping || m_startupRunning || !m_ready) return;
    if (m_startup.joinable()) m_startup.join();
    m_ready = false;
    m_chat.setReady(false);
    m_cancel = true;
    m_tts.stop();
    m_startupRunning = true;
    m_startup = std::thread([this]() {
        struct Done {
            std::atomic<bool>& flag;
            ~Done() { flag = false; }
        } done{ m_startupRunning };
        Log::info("Ricarico i modelli");
        if (!loadModels()) return;
        m_ready = true;
        m_chat.setReady(true);
        refreshState();
    });
}

std::string Assistant::gpuSummary() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_gpuSummary;
}

bool Assistant::ensureModel(const std::wstring& url, const std::wstring& path, bool gguf, long long minBytes, const char* label, int index,
                            int total) {
    const auto kind = gguf ? ModelDownloader::Kind::Gguf : ModelDownloader::Kind::Whisper;
    if (fileExists(path)) {
        const bool valid = ModelDownloader::looksValid(path, kind);
        const long long size = fileSize(path);
        if (valid && size >= minBytes) return true;
        if (valid) {
            Log::warn("Modello incompleto (" + std::to_string(size) + " byte): riprendo il download");
            MoveFileExW(path.c_str(), (path + L".part").c_str(), MOVEFILE_REPLACE_EXISTING);
        } else {
            Log::warn("Modello non valido, lo riscarico: " + TextUtils::wideToUtf8(path));
            DeleteFileW(path.c_str());
        }
    }
    const std::string what = std::string(label) + " (" + std::to_string(index) + " di " + std::to_string(total) + ")";
    setStatus(State::Starting, "Scarico il " + what + "...");
    Log::info("Download: " + TextUtils::wideToUtf8(url));
    auto lastUpdate = Clock::now() - std::chrono::seconds(1);
    std::string error;
    bool ok = ModelDownloader::downloadFile(url, path,
        [&](long long done, long long total) {
            auto now = Clock::now();
            if (now - lastUpdate < std::chrono::milliseconds(250)) return;
            lastUpdate = now;
            std::string text = "Scarico il " + what + ": ";
            if (total > 0) text += std::to_string(done * 100 / total) + "% (" + std::to_string(done / 1000000) + " di " + std::to_string(total / 1000000) + " MB)";
            else text += std::to_string(done / 1000000) + " MB";
            setStatus(State::Starting, text);
        },
        &m_stopping, error);
    if (!ok) {
        if (m_stopping) return false;
        setStatus(State::Error, "Download del " + std::string(label) + " non riuscito (" + error +
                  "). Controlla la connessione e premi Riprova: riparte da dove si era fermato.", true);
        return false;
    }
    if (!ModelDownloader::looksValid(path, kind)) {
        DeleteFileW(path.c_str());
        setStatus(State::Error, "Il file scaricato non è un modello valido. Premi Riprova.", true);
        return false;
    }
    return true;
}

void Assistant::openAudio(const std::vector<std::string>& deviceNames) {
    std::lock_guard<std::mutex> lock(m_audioMutex);
    m_pttActive = false;
    m_audio.open(deviceNames);
    m_audio.setGain(settings().micGain);
}

void Assistant::shutdown() {
    if (m_stopping.exchange(true)) return;
    Log::info("Chiusura");
    m_cancel = true;
    m_hotkeys.stop();
    m_chat.shutdown();
    m_tts.stop();
    { std::lock_guard<std::mutex> lock(m_jobMutex); }
    m_jobCv.notify_all();
    { std::lock_guard<std::mutex> lock(m_speechMutex); }
    m_speechCv.notify_all();
    { std::lock_guard<std::mutex> lock(m_timerMutex); }
    m_timerCv.notify_all();
    if (m_startup.joinable()) m_startup.join();
    if (m_listenThread.joinable()) m_listenThread.join();
    if (m_speechThread.joinable()) m_speechThread.join();
    if (m_worker.joinable()) m_worker.join();
    if (m_timerThread.joinable()) m_timerThread.join();
    m_tts.shutdown();
    std::lock_guard<std::mutex> lock(m_audioMutex);
    m_audio.close();
}

// --- Stato condiviso con la finestra --------------------------------------------

Assistant::Status Assistant::status() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_status;
}

std::vector<Assistant::Entry> Assistant::conversation() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return std::vector<Entry>(m_entries.begin(), m_entries.end());
}

std::string Assistant::lastHeard() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_lastHeard;
}

AppSettings Assistant::settings() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_settings;
}

std::vector<std::string> Assistant::memory() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_memory;
}

void Assistant::updateSettings(const std::function<void(AppSettings&)>& change) {
    AppSettings before, after;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        before = m_settings;
        change(m_settings);
        m_settings.sanitize();
        after = m_settings;
    }
    if (!after.save()) Log::warn("Impostazioni non salvate (disco pieno o cartella protetta?)");
    applyChanges(before, after);
    notify(Event::Settings);
}

void Assistant::applyChanges(const AppSettings& before, const AppSettings& after) {
    if (!(before.triggers == after.triggers)) m_hotkeys.setTalkTriggers(after.triggers);
    if (!(before.chatTriggers == after.chatTriggers) || before.chatEnabled != after.chatEnabled) {
        m_hotkeys.setChatTriggers(after.chatEnabled ? after.chatTriggers : std::vector<TalkTrigger>{});
    }
    const bool screenChanged = before.ocrEnabled != after.ocrEnabled || before.ocrRegionX != after.ocrRegionX ||
                               before.ocrRegionY != after.ocrRegionY || before.ocrRegionW != after.ocrRegionW ||
                               before.ocrRegionH != after.ocrRegionH || before.ocrTargetLanguageIndex != after.ocrTargetLanguageIndex ||
                               before.ocrMaxMessages != after.ocrMaxMessages || before.chatFilterTag != after.chatFilterTag;
    if (screenChanged) m_chat.screenSettingsChanged(after.ocrEnabled);
    if (before.micGain != after.micGain) m_audio.setGain(after.micGain);
    if (before.voiceName != after.voiceName) m_tts.setVoice(after.voiceName);
    if (before.speechRate != after.speechRate) m_tts.setRate(after.speechRate);
    if (before.voiceEffect != after.voiceEffect) m_tts.setEffect(after.voiceEffect);
    if (before.outputDeviceName != after.outputDeviceName) m_tts.setOutputDevice(after.outputDeviceName);
    if (before.wakeWordEnabled != after.wakeWordEnabled) m_audio.setStreaming(after.wakeWordEnabled);
    if (before.gpuMemoryLimitMb != after.gpuMemoryLimitMb || before.llmModelFile != after.llmModelFile) reloadModels();
    if (before.voiceEngine != after.voiceEngine) {
        // Verso Windows: subito. Verso la voce neurale: la si carica (qualche secondo; se manca, la si scarica al prossimo avvio).
        m_tts.setUseNeural(after.voiceEngine == "neurale" && m_neural.isReady());
        if (after.voiceEngine == "windows" || !m_neural.isReady()) reloadModels();
    }
    if (before.deviceNames != after.deviceNames && m_ready) {
        openAudio(after.deviceNames);
        m_audio.setStreaming(after.wakeWordEnabled);
    }
    if (before.speechEnabled && !after.speechEnabled) m_tts.stop();
}

void Assistant::setStatus(State state, const std::string& text, bool canRetry) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_status.state == state && m_status.text == text && m_status.canRetry == canRetry) return;
        m_status.state = state;
        m_status.text = text;
        m_status.canRetry = canRetry;
    }
    if (state == State::Error) Log::error(text);
    notify(Event::Status);
}

void Assistant::refreshState() {
    if (!m_ready) return;
    const long long now = nowMs();
    const bool speaking = m_tts.isSpeaking();
    if (m_wasSpeaking && !speaking) m_ttsEndedMs = now;
    m_wasSpeaking = speaking;

    std::string busyText, speakingText;
    AppSettings s;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        busyText = m_busyText;
        speakingText = m_speakingText;
        s = m_settings;
    }
    if (!m_audio.isOpen() && s.wakeWordEnabled) {
        setStatus(State::Error, "Nessun microfono disponibile: collegane uno e sceglilo nelle impostazioni. Puoi comunque scrivermi.");
        return;
    }
    bool chatOnlyKey = false;
    if (m_pttActive) {
        std::lock_guard<std::mutex> lock(m_audioMutex);
        chatOnlyKey = m_pttUsedChat && !m_pttUsedIris;
    }
    const GameChat::Notice chatNotice = m_chat.notice();
    const bool recentNotice = !chatNotice.text.empty() && GetTickCount64() - chatNotice.tick < 5000;
    if (speaking) setStatus(State::Speaking, speakingText);
    else if (m_pttActive && chatOnlyKey) setStatus(State::Listening, "Detti per la chat: rilascia il tasto quando hai finito");
    else if (m_chat.busy()) {
        const std::string t = m_chat.busyText();
        setStatus(State::Thinking, "Chat: " + (t.empty() ? std::string("in coda...") : t));
    } else if (m_busy || m_pendingForIris > 0) setStatus(State::Thinking, busyText.empty() ? "Ci penso..." : busyText);
    else if (m_pttActive) setStatus(State::Listening, "Ti ascolto: rilascia il tasto quando hai finito");
    else if (recentNotice) setStatus(chatNotice.error ? State::Error : State::Idle, chatNotice.text);
    else if (now < m_awakeUntilMs) setStatus(State::Listening, "Ti ascolto...");
    else if (s.wakeWordEnabled) setStatus(State::Idle, "Dì \"Iris\" per chiamarmi");
    else if (!s.triggers.empty()) setStatus(State::Idle, "Tieni premuto " + HotkeyListener::describeTrigger(s.triggers.front()) + " per parlarmi");
    else setStatus(State::Idle, "Scrivimi qui sotto, o accendi l'ascolto di \"Iris\" nelle impostazioni");
}

size_t Assistant::addEntry(Entry::Kind kind, const std::string& text, bool pending) {
    size_t id;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_entries.push_back({ kind, nowHHMM(), text, pending });
        while (m_entries.size() > kMaxEntries) {
            m_entries.pop_front();
            m_firstEntryId++;
        }
        id = m_firstEntryId + m_entries.size() - 1;
    }
    notify(Event::Conversation);
    return id;
}

void Assistant::updateEntry(size_t id, const std::string& text, bool pending) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (id < m_firstEntryId || id - m_firstEntryId >= m_entries.size()) return;
        Entry& e = m_entries[id - m_firstEntryId];
        if (e.text == text && e.pending == pending) return;
        e.text = text;
        e.pending = pending;
    }
    notify(Event::Conversation);
}

void Assistant::setLastHeard(const std::string& text) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_lastHeard = text;
    }
    notify(Event::Heard);
}

// --- Ascolto -----------------------------------------------------------------------

void Assistant::listenLoop() {
    VadSegmenter segmenter;
    std::vector<float> buffer, probs;
    bool startedAwake = false, startedWhileSpeaking = false;
    bool streaming = false;

    int tick = 0;
    while (!m_stopping) {
        Sleep(30);
        refreshState();
        if (++tick % 66 == 0 && m_ready) checkDevices(); // ogni 2 secondi
        const AppSettings s = settings();
        if (!m_ready || !s.wakeWordEnabled) {
            if (streaming) {
                buffer.clear();
                segmenter.reset();
                m_userSpeaking = false;
                streaming = false;
            }
            if (s.wakeWordEnabled) m_audio.takeStream(); // durante il caricamento: si butta
            continue;
        }
        streaming = true;

        VadParams p = segmenter.params();
        p.startThreshold = (float)s.vadThreshold;
        p.endThreshold = std::max(0.15f, (float)s.vadThreshold - 0.15f);
        p.endSilenceMs = s.endSilenceMs;
        if (p.startThreshold != segmenter.params().startThreshold || p.endSilenceMs != segmenter.params().endSilenceMs) segmenter.setParams(p);

        std::vector<float> chunk = m_audio.takeStream();
        buffer.insert(buffer.end(), chunk.begin(), chunk.end());
        const int frames = (int)(buffer.size() / VoiceDetector::kFrameSamples);
        if (frames == 0) continue;
        m_vad.process(buffer.data(), frames * VoiceDetector::kFrameSamples, probs);

        for (int f = 0; f < frames && f < (int)probs.size(); ++f) {
            const long long now = nowMs();
            // Col tasto premuto la frase la prende il tasto: niente doppioni.
            if (m_pttActive || now - m_pttEndedMs < kPttGuardMs) {
                if (segmenter.inSpeech()) {
                    segmenter.reset();
                    m_vad.reset();
                }
                continue;
            }
            const bool was = segmenter.inSpeech();
            const bool done = segmenter.push(buffer.data() + (size_t)f * VoiceDetector::kFrameSamples, probs[f]);
            if (!was && segmenter.inSpeech()) {
                startedAwake = now < m_awakeUntilMs;
                startedWhileSpeaking = m_tts.isSpeaking() || now - m_ttsEndedMs < kEchoGuardMs;
            }
            if (was && !segmenter.inSpeech() && !done) Log::info("Ascolto: rumore troppo breve, scartato");
            if (done) {
                Utterance u;
                u.samples = segmenter.takeUtterance();
                char info[160];
                std::snprintf(info, sizeof(info), "Ascolto: frase di %.1f s (rumore di fondo %.4f)%s%s", u.samples.size() / 16000.0,
                              segmenter.noiseFloor(), startedAwake ? ", finestra aperta" : "", startedWhileSpeaking ? ", mentre parlavo" : "");
                Log::info(info);
                u.startedAwake = startedAwake;
                u.startedWhileSpeaking = startedWhileSpeaking;
                if (u.startedAwake && !u.startedWhileSpeaking) m_pendingForIris++;
                m_vad.reset();
                {
                    std::lock_guard<std::mutex> lock(m_speechMutex);
                    m_utterances.push_back(std::move(u));
                }
                m_speechCv.notify_one();
            }
        }
        m_userSpeaking = segmenter.inSpeech();
        m_voiceProbability = segmenter.lastProbability();
        m_noiseFloor = segmenter.noiseFloor();
        buffer.erase(buffer.begin(), buffer.begin() + (size_t)frames * VoiceDetector::kFrameSamples);
    }
}

void Assistant::checkDevices() {
    const AppSettings s = settings();

    // Uscita: se si usa la predefinita di Windows e cambia (cuffie accese, casse...), la si segue.
    const std::string defaultOut = TtsEngine::defaultOutputName();
    if (s.outputDeviceName.empty() && !m_lastDefaultOutput.empty() && defaultOut != m_lastDefaultOutput && !defaultOut.empty()) {
        Log::info("Uscita predefinita cambiata: " + defaultOut);
        m_tts.reopenOutput();
    }
    m_lastDefaultOutput = defaultOut;

    // Microfono.
    const std::string defaultIn = AudioCapture::defaultCaptureName();
    bool reopen = false;
    std::string why;
    if (s.deviceNames.empty()) {
        if (!m_lastDefaultCapture.empty() && defaultIn != m_lastDefaultCapture && !defaultIn.empty()) {
            reopen = true;
            why = "microfono predefinito cambiato: " + defaultIn;
        }
    } else {
        // Quello scelto c'è? (scollegato, o ricollegato dopo)
        bool present = false;
        for (const auto& d : AudioCapture::listCaptureDevices()) {
            if (d.name == s.deviceNames.front()) present = true;
        }
        if (!present && !m_deviceMissing) Log::warn("Il microfono scelto non c'è più: " + s.deviceNames.front());
        if (present && m_deviceMissing) {
            reopen = true;
            why = "microfono ricollegato: " + s.deviceNames.front();
        }
        m_deviceMissing = !present;
    }
    m_lastDefaultCapture = defaultIn;
    // Aperto ma muto da 3 secondi: il driver si è perso il microfono.
    if (!reopen && m_audio.isOpen() && m_audio.msSinceLastData() > 3000) {
        reopen = true;
        why = "dal microfono non arriva più audio";
    }
    if (!reopen && !m_audio.isOpen() && (!s.deviceNames.empty() ? !m_deviceMissing : !defaultIn.empty())) {
        reopen = true;
        why = "microfono di nuovo disponibile";
    }
    if (reopen) {
        Log::info("Riapro il microfono (" + why + ")");
        openAudio(s.deviceNames);
        m_audio.setStreaming(s.wakeWordEnabled);
    }
}

void Assistant::onTalkStart() { pttDown(false); }
void Assistant::onTalkStop() { pttUp(false); }
void Assistant::onChatStart() { pttDown(true); }
void Assistant::onChatStop() { pttUp(true); }

void Assistant::pttDown(bool forChat) {
    if (!m_ready || m_stopping) return;
    std::lock_guard<std::mutex> lock(m_audioMutex);
    if (!m_pttActive) m_pttUsedIris = m_pttUsedChat = false; // registrazione nuova
    (forChat ? m_pttChatDown : m_pttIrisDown) = true;
    (forChat ? m_pttUsedChat : m_pttUsedIris) = true;
    if (m_pttActive || !m_audio.isOpen()) return; // già in registrazione (l'altro gruppo, o un tasto in comune)
    if (!forChat) {
        // Il tasto di Iris mentre parla: si zittisce, vuoi dire qualcosa tu.
        m_tts.stop();
        m_cancel = true;
    }
    m_chat.setRecording(true);
    m_audio.startRecording();
    m_pttActive = true;
}

void Assistant::pttUp(bool forChat) {
    std::vector<float> samples;
    bool usedIris = false, usedChat = false;
    {
        std::lock_guard<std::mutex> lock(m_audioMutex);
        (forChat ? m_pttChatDown : m_pttIrisDown) = false;
        if (!m_pttActive || m_pttChatDown || m_pttIrisDown) return; // un altro tasto è ancora premuto
        samples = m_audio.stopRecording();
        m_pttActive = false;
        m_pttEndedMs = nowMs();
        usedIris = m_pttUsedIris;
        usedChat = m_pttUsedChat;
    }
    m_chat.setRecording(false);
    // Solo il tasto della chat: va in chat (trascrive col vocabolario di Star Citizen, traduce, scrive).
    if (usedChat && !usedIris) {
        if (settings().chatEnabled) m_chat.dictate(std::move(samples));
        return;
    }
    if (SpeechUtils::voicedSeconds(samples, 0.008) < kMinVoicedSeconds) return;
    Utterance u;
    u.samples = std::move(samples);
    u.fromPtt = true;
    u.sharedKey = usedChat && usedIris; // tasto in comune: decide "Iris" detto o no
    m_pendingForIris++;
    {
        std::lock_guard<std::mutex> lock(m_speechMutex);
        m_utterances.push_back(std::move(u));
    }
    m_speechCv.notify_one();
}

void Assistant::importVoiceToChat() {
    if (settings().voiceToChatImported) return;
    std::string content;
    {
        std::ifstream f(AppPaths::voiceToChatSettingsPath(), std::ios::binary);
        if (f.is_open()) {
            std::stringstream b;
            b << f.rdbuf();
            content = b.str();
        }
    }
    // Il glossario si copia (quello di Voice to Chat non si tocca).
    AppPaths::ensureDirectoriesExist();
    const std::wstring mine = AppPaths::glossaryPath(), theirs = AppPaths::voiceToChatGlossaryPath();
    if (!fileExists(mine) && fileExists(theirs) && CopyFileW(theirs.c_str(), mine.c_str(), TRUE)) Log::info("Glossario di Voice to Chat copiato");
    updateSettings([&](AppSettings& s) {
        if (!content.empty()) s.importVoiceToChat(content);
        else s.voiceToChatImported = true;
    });
    if (content.empty()) {
        Log::info("Voice to Chat non trovato: chat di gioco con le impostazioni predefinite");
        return;
    }
    const AppSettings s = settings();
    std::string keys;
    for (const auto& t : s.chatTriggers) {
        const bool shared = std::find(s.triggers.begin(), s.triggers.end(), t) != s.triggers.end();
        keys += (keys.empty() ? "" : ", ") + HotkeyListener::describeTrigger(t) + (shared ? " (in comune con Iris)" : "");
    }
    Log::info("Impostazioni di Voice to Chat importate: tasti della chat " + (keys.empty() ? std::string("nessuno") : keys));
}

std::string Assistant::chatCommandText(const std::string& command) {
    // Minuscole solo ASCII: le lunghezze restano quelle del testo originale.
    std::string low = command;
    for (char& c : low) {
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    }
    static const char* const prefixes[] = {
        "scrivi in chat", "scrivi nella chat", "scrivi sulla chat", "scrivi alla chat", "manda in chat", "manda nella chat",
        "mandi in chat", "invia in chat", "invia nella chat", "di' in chat", "dì in chat", "di in chat", "dici in chat",
        "scrivi ai giocatori", "scrivi agli altri", "messaggio in chat", "metti in chat",
    };
    size_t start = std::string::npos;
    for (const char* p : prefixes) {
        if (TextUtils::startsWith(low, p)) {
            start = std::strlen(p);
            break;
        }
    }
    if (start == std::string::npos) return "";
    std::string rest = command.substr(start);
    // "scrivi in chat: ...", "scrivi in chat che ..."
    size_t i = 0;
    while (i < rest.size() && (rest[i] == ' ' || rest[i] == ':' || rest[i] == ',' || rest[i] == ';' || rest[i] == '-')) ++i;
    rest = rest.substr(i);
    std::string restLow = rest;
    for (char& c : restLow) {
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    }
    if (TextUtils::startsWith(restLow, "che ")) rest = rest.substr(4);
    rest = TextUtils::trim(rest);
    // Le virgolette intorno al messaggio ("scrivi in chat: «arrivo»") non vanno in chat.
    while (!rest.empty() && (rest.front() == '"' || rest.front() == '\'')) rest.erase(0, 1);
    while (!rest.empty() && (rest.back() == '"' || rest.back() == '\'')) rest.pop_back();
    return TextUtils::trim(rest);
}

void Assistant::speechLoop() {
    while (true) {
        Utterance u;
        {
            std::unique_lock<std::mutex> lock(m_speechMutex);
            m_speechCv.wait(lock, [&] { return m_stopping || !m_utterances.empty(); });
            if (m_stopping) return;
            u = std::move(m_utterances.front());
            m_utterances.pop_front();
        }
        const bool counted = u.fromPtt || (u.startedAwake && !u.startedWhileSpeaking);
        std::string text;
        const auto t0 = Clock::now();
        // Tasto in comune con la chat: anche i nomi di Star Citizen (potrebbe essere un messaggio per gli altri).
        if (m_whisper.isReady()) text = m_whisper.transcribe(u.samples, "it", gameActive() || u.sharedKey ? kWhisperHintGame : kWhisperHint);
        const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
        if (text.empty()) Log::info("Whisper (" + std::to_string(ms) + " ms): niente di comprensibile");
        else Log::info("Whisper (" + std::to_string(ms) + " ms): \"" + text + "\"");
        if (!text.empty()) {
            setLastHeard(text);
            handleTranscript(u, text);
        }
        if (counted) m_pendingForIris--;
    }
}

void Assistant::handleTranscript(const Utterance& u, const std::string& text) {
    const bool awake = u.fromPtt || (u.startedAwake && !u.startedWhileSpeaking);
    // Sentita mentre Iris parlava e uguale a quello che diceva: è la sua voce dalle casse.
    if (u.startedWhileSpeaking && !u.fromPtt && isOwnEcho(text)) {
        Log::info("Eco della mia voce, ignorata: \"" + text + "\"");
        return;
    }
    WakeWord::Match m = WakeWord::detect(text);
    // Tasto in comune con la chat, senza dire "Iris" e col gioco davanti: è un messaggio per
    // gli altri giocatori. Fuori dal gioco resta per Iris (non si scrive nel browser o in Discord).
    if (u.sharedKey && !m.found && settings().chatEnabled && m_game.gameInForeground()) {
        Log::info("Per la chat (tasto in comune, gioco in primo piano, senza \"Iris\"): \"" + text + "\"");
        m_chat.sendText(text);
        return;
    }
    std::string command;
    if (m.found) {
        if (m.command.empty()) {
            // Solo "Iris": ti ascolta (e se stava parlando, si ferma).
            Log::info("Chiamata: \"" + text + "\"");
            if (m_busy || m_tts.isSpeaking()) {
                m_cancel = true;
                m_tts.stop();
            }
            m_tts.chime(TtsEngine::Chime::Wake);
            m_awakeUntilMs = nowMs() + kWakeWindowMs;
            return;
        }
        command = m.command;
    } else if (awake) {
        command = text;
    } else {
        Log::info("Sentito (non per Iris): \"" + text + "\"");
        return;
    }

    if (WakeWord::isStopCommand(command)) {
        Log::info("Stop: \"" + text + "\"");
        stopSpeaking();
        m_awakeUntilMs = 0;
        return;
    }
    if (isNewConversationCommand(command)) {
        newConversation();
        if (settings().speechEnabled) say("Va bene, ricominciamo da capo.");
        return;
    }
    Log::info("Richiesta: \"" + command + "\"");
    dispatchCommand(command);
}

// --- Richieste ------------------------------------------------------------------------

void Assistant::sendText(const std::string& text) {
    const std::string t = TextUtils::trim(text);
    if (t.empty()) return;
    if (WakeWord::isStopCommand(t)) {
        stopSpeaking();
        return;
    }
    if (isNewConversationCommand(t)) {
        newConversation();
        return;
    }
    dispatchCommand(t);
}

void Assistant::dispatchCommand(const std::string& text) {
    // Una richiesta nuova interrompe quella in corso (e la voce).
    if (m_busy || m_tts.isSpeaking()) {
        m_cancel = true;
        m_tts.stop();
    }
    m_awakeUntilMs = 0;
    {
        std::lock_guard<std::mutex> lock(m_jobMutex);
        m_jobs.push_back(text);
    }
    m_jobCv.notify_one();
}

void Assistant::stopSpeaking() {
    m_cancel = true;
    m_tts.stop();
}

void Assistant::newConversation() {
    m_cancel = true;
    m_tts.stop();
    {
        std::lock_guard<std::mutex> lock(m_jobMutex);
        m_jobs.push_back(std::string("\x01reset"));
    }
    m_jobCv.notify_one();
}

void Assistant::testVoice() {
    AppSettings s = settings();
    m_tts.stop();
    say(s.userName.empty() ? "Ciao, sono Iris. Questa è la mia voce." : "Ciao " + s.userName + ", sono Iris. Questa è la mia voce.");
}

Agent::Persona Assistant::persona() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    Agent::Persona p;
    p.userName = m_settings.userName;
    p.internet = m_settings.internetEnabled;
    p.memory = m_memory;
    return p;
}

void Assistant::workerLoop() {
    // ShellExecute (aprire programmi) e il volume di Windows vogliono COM su questo thread.
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    while (true) {
        std::string job;
        {
            std::unique_lock<std::mutex> lock(m_jobMutex);
            m_jobCv.wait(lock, [&] { return m_stopping || !m_jobs.empty(); });
            if (m_stopping) break;
            job = std::move(m_jobs.front());
            m_jobs.pop_front();
        }
        m_cancel = false;
        if (job == "\x01reset") {
            m_agent.reset();
            addEntry(Entry::Kind::Info, "Nuova conversazione");
            continue;
        }
        processCommand(job);
    }
    CoUninitialize();
}

bool Assistant::gameActive() {
    return settings().gameEnabled && m_game.gameRunning();
}

bool Assistant::briefReplies() {
    const std::string style = settings().replyStyle;
    return style == "brevi" || (style == "gioco" && gameActive());
}

namespace {
    // Risposte brevi: quante parole si dicono al massimo (la prima frase si dice comunque).
    constexpr size_t kBriefWords = 14;

    size_t wordCount(const std::string& s) {
        size_t n = 0;
        bool inWord = false;
        for (unsigned char c : s) {
            const bool space = c == ' ' || c == '\t' || c == '\n' || c == '\r';
            if (!space && !inWord) ++n;
            inWord = !space;
        }
        return n;
    }

    // Comando di gioco in modalità breve: una o due parole, o solo il suono di conferma.
    std::string briefSpoken(const GameController::Result& r) {
        if (!r.brief.empty()) return r.brief;
        if (r.spoken.empty()) return "";
        if (wordCount(r.spoken) <= 2) return r.spoken; // "Carrello.", "Scudi su."
        return r.ok ? "" : "Non riuscito.";
    }
}

void Assistant::processCommand(const std::string& text) {
    const AppSettings s = settings();
    // Mentre Iris lavora il modello è suo: la traduzione della chat letta aspetta.
    struct IrisBusy {
        GameChat& chat;
        explicit IrisBusy(GameChat& c) : chat(c) { chat.setIrisBusy(true); }
        ~IrisBusy() { chat.setIrisBusy(false); }
    } irisBusy{ m_chat };
    // "Iris, scrivi in chat: arrivo tra due minuti": va nella chat del gioco, tradotto.
    const std::string chatText = s.chatEnabled ? chatCommandText(text) : std::string();
    if (!chatText.empty()) {
        addEntry(Entry::Kind::User, text);
        Log::info("Messaggio per la chat: \"" + chatText + "\"");
        m_chat.sendText(chatText);
        return;
    }
    // Star Citizen aperto: prima il dizionario dei comandi (istantaneo, funziona anche col modello ancora in caricamento).
    if (s.gameEnabled) {
        m_game.reloadIfChanged();
        if (m_game.gameRunning()) {
            const Cmd::Match gm = m_game.match(text);
            if (gm.found) {
                runGameCommand(text, gm);
                return;
            }
            m_game.noteUnrecognized(text, gm);
        }
    }
    if (!m_llm.isReady()) {
        addEntry(Entry::Kind::User, text);
        addEntry(Entry::Kind::Info, "Sto ancora caricando i modelli: riprova tra qualche secondo.");
        return;
    }
    // Dopo tanto silenzio si ricomincia da capo (le domande di prima non c'entrano più).
    if (m_agent.exchanges() > 0 && Clock::now() - m_lastInteraction > std::chrono::minutes(s.conversationTimeoutMin)) {
        m_agent.reset();
        addEntry(Entry::Kind::Info, "Nuova conversazione");
    }
    m_lastInteraction = Clock::now();

    addEntry(Entry::Kind::User, text);
    m_tts.stop();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_busyText = "Ci penso...";
        m_speakingText.clear();
    }
    m_busy = true;
    Agent::Persona currentPersona = persona();
    if (gameActive()) currentPersona.game = m_game.profile().name;
    const bool brief = briefReplies();
    currentPersona.brief = brief;
    m_agent.setPersona(currentPersona);

    size_t irisEntry = (size_t)-1;
    auto lastUpdate = Clock::now() - std::chrono::seconds(1);
    size_t spokenWords = 0, spokenSentences = 0;
    Agent::Callbacks cb;
    cb.onSentence = [&](const std::string& sentence) {
        if (!s.speechEnabled || m_cancel) return;
        // Brevi: la prima frase sì, poi solo finché si resta in poche parole (il resto è scritto nella conversazione).
        const size_t words = wordCount(sentence);
        if (brief && spokenSentences > 0 && spokenWords + words > kBriefWords) return;
        spokenWords += words;
        spokenSentences++;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_speakingText = sentence;
        }
        say(SpeechText::speakable(sentence));
    };
    cb.onTool = [&](const std::string& what) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_busyText = what;
        }
        addEntry(Entry::Kind::Tool, what);
    };
    cb.onText = [&](const std::string& partial) {
        if (partial.empty()) return;
        if (irisEntry == (size_t)-1) {
            irisEntry = addEntry(Entry::Kind::Iris, partial, true);
            return;
        }
        auto now = Clock::now();
        if (now - lastUpdate < std::chrono::milliseconds(80)) return; // la finestra non serve ridisegnarla a ogni parola
        lastUpdate = now;
        updateEntry(irisEntry, partial, true);
    };

    std::string answer;
    Agent::Result r = m_agent.ask(text, cb, &m_cancel, answer);
    m_busy = false;
    switch (r) {
    case Agent::Result::Ok:
        if (irisEntry == (size_t)-1) irisEntry = addEntry(Entry::Kind::Iris, answer);
        else updateEntry(irisEntry, answer, false);
        break;
    case Agent::Result::Cancelled:
        if (irisEntry != (size_t)-1) updateEntry(irisEntry, (answer.empty() ? std::string("...") : answer) + " (interrotta)", false);
        return; // niente finestra di ascolto: è arrivata un'altra richiesta, o "basta"
    case Agent::Result::Error: {
        const std::string sorry = "Scusa, qualcosa è andato storto. Riprova.";
        if (irisEntry != (size_t)-1) updateEntry(irisEntry, answer, false);
        addEntry(Entry::Kind::Info, "Errore del modello (dettagli nel registro).");
        if (s.speechEnabled) say(sorry);
        break;
    }
    }
    Log::info("Risposta: \"" + answer + "\"");

    // Finita la voce, resta in ascolto qualche secondo: puoi rispondere senza dire "Iris".
    while (!m_stopping && !m_cancel && m_tts.isSpeaking()) Sleep(40);
    if (!m_cancel && s.followUpSeconds > 0) m_awakeUntilMs = nowMs() + kEchoGuardMs + s.followUpSeconds * 1000LL;
    m_lastInteraction = Clock::now();
}

void Assistant::say(const std::string& text) {
    if (TextUtils::trim(text).empty()) return;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_recentSpoken.push_back({ nowMs(), WakeWord::normalize(text) });
        while (m_recentSpoken.size() > 12) m_recentSpoken.pop_front();
    }
    m_tts.speak(text);
}

bool Assistant::isOwnEcho(const std::string& heard) {
    const std::string h = WakeWord::normalize(heard);
    if (h.empty()) return false;
    const long long now = nowMs();
    std::lock_guard<std::mutex> lock(m_mutex);
    std::string joined; // un pezzo sentito può stare a cavallo di due frasi
    for (const auto& [when, said] : m_recentSpoken) {
        if (now - when > 60000) continue;
        if (said.find(h) != std::string::npos || TextUtils::similarity(h, said) > 0.6) return true;
        joined += (joined.empty() ? "" : " ") + said;
    }
    return !joined.empty() && joined.find(h) != std::string::npos;
}

// --- Gioco --------------------------------------------------------------------------------

void Assistant::runGameCommand(const std::string& text, const Cmd::Match& match) {
    const AppSettings s = settings();
    addEntry(Entry::Kind::User, text);
    m_tts.stop();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_busyText = m_game.profile().name + ": " + match.phrase;
        m_speakingText.clear();
    }
    m_busy = true;
    char score[16];
    std::snprintf(score, sizeof(score), "%d%%", (int)(match.score * 100 + 0.5));
    Log::info("Comando di gioco: \"" + match.phrase + "\" (" + score + ")");
    const bool brief = briefReplies();
    auto speak = [&](const std::string& t) {
        if (s.speechEnabled && !t.empty()) say(t);
    };
    // Brevi: niente annuncio prima ("Calcolo la rotta per..."), alla fine una o due parole o il suono.
    auto announce = [&](const std::string& t) {
        if (!brief) speak(t);
    };
    const GameController::Result r = m_game.execute(match, announce, &m_cancel);
    m_busy = false;
    addEntry(Entry::Kind::Tool, "Comando di gioco: " + r.shown + " (\"" + match.phrase + "\", " + score + ")");
    const std::string spoken = brief ? briefSpoken(r) : r.spoken;
    if (!r.spoken.empty()) addEntry(Entry::Kind::Iris, r.spoken); // per intero, scritto: i dettagli restano leggibili
    if (!spoken.empty()) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_speakingText = spoken;
        }
        speak(spoken);
    }
    if (spoken.empty() || !s.speechEnabled) m_tts.chime(r.ok ? TtsEngine::Chime::Confirm : TtsEngine::Chime::Error);
    // Come dopo una risposta: qualche secondo per il comando successivo senza dire "Iris".
    while (!m_stopping && !m_cancel && m_tts.isSpeaking()) Sleep(40);
    if (!m_cancel && s.followUpSeconds > 0) m_awakeUntilMs = nowMs() + kEchoGuardMs + s.followUpSeconds * 1000LL;
    m_lastInteraction = Clock::now();
}

void Assistant::testGameCommand(const std::string& text) {
    const std::string t = TextUtils::trim(text);
    if (t.empty()) return;
    if (!m_game.gameRunning()) {
        addEntry(Entry::Kind::User, t);
        addEntry(Entry::Kind::Info, m_game.profile().name + " non è aperto: comando non eseguito.");
        return;
    }
    dispatchCommand(t);
}

std::vector<std::string> Assistant::extraToolDefinitions() {
    if (!gameActive()) return {};
    return { m_game.toolDefinition() };
}

bool Assistant::runExtraTool(const std::string& name, const std::string& argumentsJson, const std::atomic<bool>* cancel, std::string& result) {
    if (name != "game_command") return false;
    if (!gameActive()) {
        result = "Errore: il gioco non è aperto (o i comandi di gioco sono spenti).";
        return true;
    }
    nlohmann::json a = nlohmann::json::parse(argumentsJson, nullptr, false);
    const std::string command = a.is_object() && a.contains("command") && a["command"].is_string() ? a["command"].get<std::string>() : "";
    const std::string destination = a.is_object() && a.contains("destination") && a["destination"].is_string() ? a["destination"].get<std::string>() : "";
    const bool speech = settings().speechEnabled && !briefReplies(); // brevi: niente annuncio, parla solo la risposta finale
    const GameController::Result r = m_game.executeById(command, destination, [&](const std::string& t) {
        if (speech && !t.empty()) say(t);
    }, cancel);
    addEntry(Entry::Kind::Tool, "Comando di gioco: " + r.shown);
    result = (r.ok ? "Fatto: " : "Errore: ") + (r.shown.empty() ? r.spoken : r.shown) + (r.spoken.empty() || r.spoken == r.shown ? "" : " (" + r.spoken + ")");
    return true;
}

// --- Timer e memoria ---------------------------------------------------------------------

void Assistant::addTimer(int seconds, const std::string& label) {
    {
        std::lock_guard<std::mutex> lock(m_timerMutex);
        m_timers.push_back({ Clock::now() + std::chrono::seconds(seconds), label, seconds });
    }
    m_timerCv.notify_all();
    Log::info("Timer: " + std::to_string(seconds) + " s " + label);
}

int Assistant::cancelTimers() {
    std::lock_guard<std::mutex> lock(m_timerMutex);
    int n = (int)m_timers.size();
    m_timers.clear();
    m_timerCv.notify_all();
    return n;
}

void Assistant::timerLoop() {
    std::unique_lock<std::mutex> lock(m_timerMutex);
    while (!m_stopping) {
        if (m_timers.empty()) {
            m_timerCv.wait(lock);
            continue;
        }
        auto next = std::min_element(m_timers.begin(), m_timers.end(), [](const Timer& a, const Timer& b) { return a.at < b.at; });
        if (Clock::now() < next->at) {
            m_timerCv.wait_until(lock, next->at);
            continue;
        }
        Timer t = *next;
        m_timers.erase(next);
        lock.unlock();
        const std::string what = t.label.empty() ? "Il timer è finito." : "Il timer per " + t.label + " è finito.";
        m_tts.chime(TtsEngine::Chime::Timer);
        if (settings().speechEnabled) say(what);
        addEntry(Entry::Kind::Info, "Timer scaduto" + (t.label.empty() ? std::string() : ": " + t.label));
        lock.lock();
    }
}

std::string Assistant::homeCity() {
    return settings().homeCity;
}

void Assistant::loadMemory() {
    std::ifstream f(AppPaths::memoryPath(), std::ios::binary);
    std::vector<std::string> lines;
    if (f.is_open()) {
        std::stringstream ss;
        ss << f.rdbuf();
        std::string content = ss.str();
        if (TextUtils::startsWith(content, "\xEF\xBB\xBF")) content.erase(0, 3);
        for (const auto& line : TextUtils::splitLines(content)) {
            std::string t = TextUtils::trim(line);
            if (!t.empty() && t[0] != '#') lines.push_back(t);
        }
    }
    // Le ultime 40: il prompt non deve diventare enorme.
    if (lines.size() > 40) lines.erase(lines.begin(), lines.end() - 40);
    std::lock_guard<std::mutex> lock(m_mutex);
    m_memory = lines;
}

void Assistant::remember(const std::string& fact) {
    AppPaths::ensureDirectoriesExist();
    {
        std::ofstream f(AppPaths::memoryPath(), std::ios::binary | std::ios::app);
        if (f.is_open()) f << TextUtils::trim(fact) << "\r\n";
    }
    loadMemory();
    Log::info("Memoria: " + fact);
}
