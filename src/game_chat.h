#pragma once

#include "app_settings.h"
#include "llm_engine.h"
#include "whisper_engine.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// La chat di gioco: le funzioni di Voice to Chat dentro Iris, con gli STESSI
// modelli (whisper e Qwen in memoria una volta sola).
//  - Detta: tieni premuto il tasto della chat, parli in italiano, la frase va
//    nella chat del gioco già tradotta (e inviata, se l'invio automatico è acceso).
//  - Dal testo: "Iris, scrivi in chat: ..." o il tasto in comune con Iris senza
//    dire "Iris": stesso percorso, senza ritrascrivere.
//  - Traduzione della chat degli altri, letta dallo schermo (OCR), in un riquadro
//    sotto la chat; i messaggi di un canale/organizzazione in una pagina a parte.
// Thread:
//  - lavoro: le frasi, UNA ALLA VOLTA e nell'ordine in cui sono state dette;
//  - schermo: legge la zona e traduce i messaggi nuovi; cede sempre il passo
//    alla voce e a Iris (si ferma mentre parli e mentre il modello serve a loro).
// I tasti verso il gioco passano dalla stessa coda dei comandi della nave
// (GameInput::inputLock): un messaggio e un comando non si mescolano mai.
class GameChat {
public:
    struct HistoryEntry {
        std::string time;     // "21:43"
        std::string heard;    // cosa ha capito whisper (o la frase di prova)
        std::string written;  // cosa è andato in chat
        bool typed = false;   // scritto nel gioco? (no per le prove)
        std::string note;     // es. "non scritta: in primo piano c'era Iris"
    };

    // Una riga del riquadro della traduzione dello schermo.
    struct ScreenLine {
        std::string prefix;   // "[SQUADRA] Trime98: " (vuoto se non è una chat)
        std::string text;     // la traduzione, o l'originale finché non è pronta
        bool pending = false; // traduzione in corso
        bool operator==(const ScreenLine& o) const { return prefix == o.prefix && text == o.text && pending == o.pending; }
    };

    struct ScreenView {
        bool visible = false;
        int regionX = 0, regionY = 0, regionW = 0, regionH = 0;
        std::vector<ScreenLine> lines;
    };

    enum class Event { History, Screen, Filtered, Status };

    struct Host {
        std::function<AppSettings()> settings;
        std::function<void(const std::function<void(AppSettings&)>&)> updateSettings;
        std::function<void(Event)> notify;
        std::function<float()> micLevel;
        std::function<bool()> micOpen;
        // Un messaggio scritto in chat (per la conversazione di Iris).
        std::function<void(const std::string& heard, const std::string& written, bool typed)> onWritten;
    };

    GameChat(WhisperEngine& whisper, LlmEngine& llm) : m_whisper(whisper), m_llm(llm) {}
    ~GameChat();
    GameChat(const GameChat&) = delete;
    GameChat& operator=(const GameChat&) = delete;

    void start(Host host);
    void shutdown();
    void setReady(bool ready); // modelli caricati: lo schermo può cominciare

    // Dal tasto della chat: l'audio registrato (trascrive, traduce, scrive).
    void dictate(std::vector<float> samples);
    // Una frase già trascritta: traduce e scrive.
    void sendText(const std::string& heard);
    // Traduce una frase scritta a mano: va nella cronologia, NON nel gioco.
    void translateTest(const std::string& text);
    // Ascolta l'ambiente per 2 secondi e regola la soglia del silenzio.
    void calibrateSilence();

    // Il modello serve alla voce o a Iris: la traduzione dello schermo si ferma subito (riprende dopo).
    void yieldScreen() { m_screenCancel = true; }
    void setRecording(bool on);
    void setIrisBusy(bool busy);
    // Zona, lingua, filtro o accensione cambiati: la lettura di prima non vale più.
    void screenSettingsChanged(bool enabled);

    // Un avviso da mostrare nello stato (es. F10: invio automatico acceso/spento).
    void setNotice(const std::string& text, bool error = false);

    bool busy() const { return m_pendingJobs.load() > 0; }
    std::string busyText() const;
    // L'ultimo risultato ("Inviato: ...") con l'ora (GetTickCount64) e se è un errore.
    struct Notice {
        std::string text;
        unsigned long long tick = 0;
        bool error = false;
    };
    Notice notice() const;

    std::vector<HistoryEntry> history() const;
    ScreenView screenView() const;
    std::vector<std::string> filteredMessages() const;
    void clearFilteredMessages();

    // Il suggerimento per whisper quando detti in chat: luoghi di Star Citizen e glossario.
    static std::string whisperHint();

private:
    void enqueue(std::function<void()> job);
    void workerLoop();
    void processUtterance(const std::vector<float>& samples);
    void writeToChat(const std::string& heard);
    bool translateVoice(const AppSettings& s, const std::string& text, std::string& out);

    void screenLoop();
    void screenStep(const AppSettings& s);
    void setScreenView(ScreenView view);
    void addFiltered(const std::string& key, const std::string& line);

    void setBusyText(const std::string& text);
    void addHistory(HistoryEntry entry);
    void notify(Event e);
    AppSettings settings() const { return m_host.settings ? m_host.settings() : AppSettings{}; }

    WhisperEngine& m_whisper;
    LlmEngine& m_llm;
    Host m_host;

    mutable std::mutex m_mutex; // cronologia, schermo, filtrati, testi di stato
    std::deque<HistoryEntry> m_history;
    ScreenView m_screenView;
    std::deque<std::pair<std::string, std::string>> m_filtered; // {chiave, riga}
    std::string m_busyText;
    Notice m_notice;

    std::atomic<bool> m_ready{ false };
    std::atomic<bool> m_stopping{ false };
    std::atomic<bool> m_started{ false };

    // Coda delle frasi.
    std::mutex m_jobMutex;
    std::condition_variable m_jobCv;
    std::deque<std::function<void()>> m_jobs;
    std::thread m_worker;
    std::atomic<int> m_pendingJobs{ 0 };
    std::atomic<bool> m_recording{ false };
    std::atomic<bool> m_irisBusy{ false };

    // Traduzione dello schermo (solo il thread dello schermo, tranne i flag).
    std::thread m_screenThread;
    std::mutex m_screenMutex;
    std::condition_variable m_screenCv;
    bool m_screenWake = false;
    std::atomic<bool> m_screenCancel{ false };
    std::atomic<bool> m_screenReset{ false };
    struct CachedTranslation {
        std::string text;
        unsigned long long lastUsed = 0;
    };
    std::map<std::string, CachedTranslation> m_screenCache;
    std::string m_screenCacheTarget;
    unsigned long long m_screenTick = 0;
    unsigned long long m_lastScreenHash = 0;
    struct ShownMessage {
        std::string key;
        std::string prefix, body;
        std::string translated;
    };
    // Il riquadro ha la sua memoria: i messaggi già tradotti, dal più vecchio al
    // più nuovo, al massimo ocrMaxMessages. Si traducono solo quelli che
    // compaiono dopo l'ultimo in memoria (quando la chat compare, solo l'ultimo
    // che si vede); ognuno nuovo entra in fondo e spinge su gli altri.
    std::deque<ShownMessage> m_memory;
    std::vector<ShownMessage> m_waiting; // nuovi letti una volta: si traducono se la lettura dopo li conferma
    std::string m_memoryFor;             // zona e lingua per cui vale la memoria
    bool m_screenAwaiting = false;
    int m_emptyReads = 0;
    int m_ocrMaxDimension = 0;
    int m_ocrScale = 3;
    std::string m_lastOcrText;
    std::string m_lastOcrError;
};
