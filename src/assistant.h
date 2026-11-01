#pragma once

#include "agent.h"
#include "game_chat.h"
#include "game_controller.h"
#include "game_log.h"
#include "mining_log.h"
#include "refinery.h"
#include "app_settings.h"
#include "audio_capture.h"
#include "hotkey_listener.h"
#include "llm_engine.h"
#include "mining_scanner.h"
#include "neural_voice.h"
#include "tools.h"
#include "tts_engine.h"
#include "voice_detector.h"
#include "whisper_engine.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Il cuore di Iris, senza finestre.
//
// Thread:
//  - avvio: scarica/carica i modelli, poi finisce;
//  - ascolto: audio continuo -> Silero -> frasi intere (e lo stato per la finestra);
//  - trascrizione: whisper su ogni frase, poi "è per Iris?" (parola "Iris",
//    finestra di ascolto aperta, tasto per parlare);
//  - lavoro: le richieste, una alla volta (modello + strumenti + voce);
//  - tasti (HotkeyListener); voce (TtsEngine).
// Iris è un'assistente di GIOCO: comandi della nave, chat, mining e memoria.
// Niente controlli di Windows (programmi, siti, volume, musica, timer, internet).
// La finestra legge lo stato con i metodi qui sotto; il "listener" la avvisa
// quando qualcosa cambia (da qualunque thread: deve solo fare PostMessage).
class Assistant : public ToolHost {
public:
    enum class State { Starting, Idle, Listening, Thinking, Speaking, Error };

    struct Status {
        State state = State::Starting;
        std::string text;
        bool canRetry = false;
    };

    struct Entry {
        enum class Kind { User, Iris, Tool, Info };
        Kind kind = Kind::Info;
        std::string time; // "21:43"
        std::string text;
        bool pending = false; // Iris sta ancora scrivendo
    };

    // ChatHistory, Screen, Filtered: dalla chat di gioco (GameChat).
    // Mining: una lettura nuova o l'inserimento manuale (il riquadro a schermo);
    // MiningData: tabella dei materiali o sconosciute cambiate (la pagina Mining).
    // GameLog: il diario di bordo è cambiato. Refinery: i timer della raffineria. QuickBar / HideAll: i tasti
    // della barra rapida (mostra/nascondi la barra, nascondi tutto quello di Iris sullo schermo).
    // MiningLog: il registro delle scansioni del mining è cambiato.
    enum class Event { Status, Conversation, Settings, Heard, ChatHistory, Screen, Filtered, Mining, MiningData, GameLog, Refinery, QuickBar, HideAll, MiningLog };
    using Listener = std::function<void(Event)>;

    Assistant();
    ~Assistant() override;
    Assistant(const Assistant&) = delete;
    Assistant& operator=(const Assistant&) = delete;

    void setListener(Listener listener);
    // JoystickInput::initialize() va chiamato prima, dal thread della finestra.
    void start();
    void retryStart();
    void shutdown();

    Status status() const;
    bool isReady() const { return m_ready.load(); }
    std::vector<Entry> conversation() const;
    // L'ultima frase sentita (anche se non era per Iris): per capire se ti sente bene.
    std::string lastHeard() const;

    AppSettings settings() const;
    void updateSettings(const std::function<void(AppSettings&)>& change);

    float micLevel() const { return m_audio.getCurrentLevel(); }
    float voiceLevel() const { return m_tts.level(); }
    bool userSpeaking() const { return m_userSpeaking.load(); }
    // Per la diagnosi dell'ascolto: probabilità di voce adesso (0..1) e rumore di fondo (RMS).
    float voiceProbability() const { return m_voiceProbability.load(); }
    float noiseFloor() const { return m_noiseFloor.load(); }
    int openMicrophoneCount() const { return m_audio.openDeviceCount(); }
    std::string gpuSummary() const;

    void setTalkPaused(bool paused) { m_hotkeys.setPaused(paused); }

    // Una richiesta scritta nella finestra, come se l'avessi detta.
    void sendText(const std::string& text);
    void stopSpeaking();
    void newConversation();
    void testVoice();
    std::vector<std::string> memory() const;
    // I comandi di gioco (pagina Star Citizen: stato, prova, calibrazione).
    GameController& game() { return m_game; }
    // Un comando di gioco scritto nella pagina Star Citizen (come se l'avessi detto).
    void testGameCommand(const std::string& text);
    // Un suono di conferma o di errore (es. F8 della calibrazione, mentre sei nel gioco).
    void chime(bool ok) { m_tts.chime(ok ? TtsEngine::Chime::Confirm : TtsEngine::Chime::Error); }
    // La chat di gioco (le funzioni di Voice to Chat): detta, traduzione della chat letta, filtrati.
    GameChat& chat() { return m_chat; }
    // "scrivi in chat: ..." -> il testo da scrivere (vuoto se non è un comando di chat).
    static std::string chatCommandText(const std::string& command);
    // Il mining (Mining Signature Calculator dentro Iris): letture, tabella, sconosciute.
    MiningScanner& mining() { return m_mining; }
    // Il diario di bordo (Game.log, solo se l'utente l'ha attivato) e i timer della raffineria.
    GameLog::Watcher& gameLog() { return m_gameLog; }
    Refinery::Timers& refinery() { return m_refinery; }
    // Il registro delle scansioni del mining (file sul desktop alla chiusura).
    MiningLog::Recorder& miningLog() { return m_miningLog; }
    void miningLogChanged() { notify(Event::MiningLog); }
    // Chi sta minando, per il registro: il nome nel gioco (dal diario di bordo) o quello in Iris.
    std::string operatorName();
    // Dove sei secondo il diario di bordo (località o giurisdizione), se acceso. Vuoto se non si sa.
    std::string knownPlace();
    void refineryChanged() { notify(Event::Refinery); }
    // Ogni secondo, dal thread della finestra: gli ordini appena pronti si annunciano.
    void tickRefinery();

    // ToolHost
    void remember(const std::string& fact) override;
    std::vector<std::string> extraToolDefinitions() override;
    bool runExtraTool(const std::string& name, const std::string& argumentsJson, const std::atomic<bool>* cancel, std::string& result) override;

private:
    using Clock = std::chrono::steady_clock;

    struct Utterance {
        std::vector<float> samples;
        bool fromPtt = false;             // tasto per parlare: è per Iris di sicuro
        bool sharedKey = false;           // tasto in comune con la chat: con "Iris" è per lei, se no va in chat
        bool startedAwake = false;        // cominciata con la finestra di ascolto aperta
        bool startedWhileSpeaking = false; // cominciata mentre Iris parlava (forse è la sua voce)
    };

    // Avvio e modelli
    void startupThread();
    bool ensureModel(const std::wstring& url, const std::wstring& path, bool gguf, long long minBytes, const char* label, int index,
                     int total = 3);
    bool loadModels();
    void reloadModels();
    void openAudio(const std::vector<std::string>& deviceNames);
    // Ogni 2 secondi: periferica predefinita di Windows cambiata, microfono staccato e
    // ricollegato, cuffie nuove... si riapre da soli, senza riavviare il programma.
    void checkDevices();
    void applyChanges(const AppSettings& before, const AppSettings& after);

    // Ascolto e trascrizione
    void listenLoop();
    void speechLoop();
    void handleTranscript(const Utterance& u, const std::string& text);
    // Tasti per parlare: quelli di Iris e quelli della chat. Si registra finché
    // almeno uno è premuto; al rilascio si decide a chi va la frase.
    void onTalkStart();
    void onTalkStop();
    void onChatStart();
    void onChatStop();
    void pttDown(bool forChat);
    void pttUp(bool forChat);
    // Al primo avvio dopo l'unione: le impostazioni e il glossario di Voice to Chat.
    void importVoiceToChat();

    // Richieste
    void dispatchCommand(const std::string& text);
    void workerLoop();
    void processCommand(const std::string& text);
    // Comando di gioco riconosciuto dal dizionario: tasti subito, senza modello.
    void runGameCommand(const std::string& text, const Cmd::Match& match);
    // "Iris, che minerale è?" / "Iris, signature 16960": true se era un comando del mining.
    bool runMiningCommand(const AppSettings& s, const std::string& text);
    // I tasti del mining (leggi, a mano, contorno, acceso/spento) e della barra rapida verso HotkeyListener.
    void applyMiningKeys(const AppSettings& s);
    // Il diario di bordo: acceso solo se l'utente l'ha attivato dopo l'avviso.
    void applyGameLog(const AppSettings& s);
    void onGameLogEvent(const GameLog::Event& e, bool live);
    std::string refineryTool(const std::string& argumentsJson);
    std::string gameStatusText();
    bool gameActive(); // comandi di gioco accesi e gioco aperto
    bool briefReplies(); // risposte brevi adesso (sempre, o mentre giochi: vedi AppSettings::replyStyle)
    Agent::Persona persona() const;

    void loadMemory();

    // Voce: tutto quello che Iris dice passa da qui (e si ricorda, per riconoscere la sua eco).
    void say(const std::string& text);
    bool isOwnEcho(const std::string& heard);

    // Stato e conversazione
    void refreshState();
    void setStatus(State state, const std::string& text, bool canRetry = false);
    size_t addEntry(Entry::Kind kind, const std::string& text, bool pending = false);
    void updateEntry(size_t id, const std::string& text, bool pending);
    void setLastHeard(const std::string& text);
    void notify(Event e);

    Listener m_listener;
    std::mutex m_listenerMutex;

    mutable std::mutex m_mutex; // stato, conversazione, impostazioni, memoria, testi di stato
    Status m_status;
    std::deque<Entry> m_entries;
    size_t m_firstEntryId = 0; // id della prima voce in m_entries (le vecchie si tolgono)
    AppSettings m_settings;
    std::vector<std::string> m_memory;
    std::string m_gpuSummary;
    std::string m_lastHeard;
    std::string m_busyText;     // "Cerco su internet: ..."
    std::string m_speakingText; // l'ultima frase detta
    std::deque<std::pair<long long, std::string>> m_recentSpoken; // {quando, frase normalizzata}: per riconoscere l'eco

    AudioCapture m_audio;
    std::mutex m_audioMutex;
    WhisperEngine m_whisper;
    VoiceDetector m_vad;
    LlmEngine m_llm;
    GameChat m_chat{ m_whisper, m_llm };
    GameController m_game;
    MiningScanner m_mining;
    GameLog::Watcher m_gameLog;
    Refinery::Timers m_refinery;
    MiningLog::Recorder m_miningLog;
    NeuralVoice m_neural;
    TtsEngine m_tts;
    Tools m_tools{ *this };
    Agent m_agent{ m_llm, m_tools };
    HotkeyListener m_hotkeys;

    std::atomic<bool> m_ready{ false };
    std::atomic<bool> m_stopping{ false };
    std::thread m_startup;
    std::atomic<bool> m_startupRunning{ false };
    int m_loadedGpuLimitMb = -2;
    std::string m_loadedModelFile = "\x01";

    // Ascolto
    std::thread m_listenThread;
    std::atomic<bool> m_userSpeaking{ false };
    std::atomic<float> m_voiceProbability{ 0.0f };
    std::atomic<float> m_noiseFloor{ 0.0f };
    std::atomic<bool> m_pttActive{ false };
    std::atomic<long long> m_pttEndedMs{ 0 };
    // Quali gruppi di tasti sono premuti adesso, e quali sono stati premuti in questa registrazione.
    bool m_pttIrisDown = false, m_pttChatDown = false, m_pttUsedIris = false, m_pttUsedChat = false; // con m_audioMutex
    std::atomic<long long> m_awakeUntilMs{ 0 };   // finestra di ascolto senza "Iris"
    std::atomic<long long> m_ttsEndedMs{ 0 };
    std::atomic<bool> m_wasSpeaking{ false };
    std::string m_lastDefaultCapture, m_lastDefaultOutput; // solo il thread di ascolto
    bool m_deviceMissing = false;

    // Trascrizione
    std::thread m_speechThread;
    std::mutex m_speechMutex;
    std::condition_variable m_speechCv;
    std::deque<Utterance> m_utterances;
    std::atomic<int> m_pendingForIris{ 0 };

    // Richieste
    std::thread m_worker;
    std::mutex m_jobMutex;
    std::condition_variable m_jobCv;
    std::deque<std::string> m_jobs;
    std::atomic<bool> m_busy{ false };
    std::atomic<bool> m_cancel{ false };
    Clock::time_point m_lastInteraction = Clock::now();
};
