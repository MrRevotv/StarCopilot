#pragma once

#include "app_settings.h"
#include "mining_decoder.h"
#include "mining_image.h"

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Il mining dentro Iris (Mining Signature Calculator + mining_ocr).
//  - Lettura: cattura la zona scelta, trova il badge (segnaposto + numero),
//    lo pulisce (mining_image), lo legge con l'OCR di Windows su più versioni
//    dell'immagine e tiene la lettura che torna con la tabella dei materiali.
//  - Il risultato (che minerale o oggetto, quante rocce o pezzi) va A SCHERMO:
//    la finestra principale lo mostra nel riquadro del mining.
//  - Inserimento manuale dal tastierino: cifre, Backspace, Invio, Esc. Mentre
//    è attivo quei tasti NON arrivano al gioco (in Star Citizen Invio apre la chat).
//  - Lettura continua (facoltativa): con Star Citizen in primo piano, ogni
//    tanto; mostra solo valori letti due volte di fila (niente sfarfallio).
//  - Tabella dei materiali in %appdata%\Iris\mining_materiali.csv (al primo
//    avvio quella dentro il programma) e signature sconosciute in
//    mining_sconosciute.csv: niente file da portarsi dietro.
// Thread: uno suo (letture una alla volta) + quello del tastierino quando serve.
class MiningScanner {
public:
    enum class Event { Reading, Manual, Data };

    struct Host {
        std::function<AppSettings()> settings;
        std::function<void(Event)> notify;
        // Annuncio a voce ("Borase per 3"): solo se acceso nelle impostazioni.
        std::function<void(const std::string&)> speak;
    };
    struct Reading;
    // Ogni lettura pubblicata (non le prove della pagina): per il registro delle scansioni.
    void setOnReading(std::function<void(const Reading&)> callback) { m_onReading = std::move(callback); }

    struct Reading {
        bool valid = false;         // c'è una lettura (anche fallita)
        std::string time;           // "21:43:05"
        std::string source;         // "tasto", "continua", "voce", "manuale", "prova"
        std::string ocrText;        // cosa ha letto l'OCR (versione scelta)
        Mining::Decoded decoded;
        std::string error;          // "zona non scelta", "OCR non disponibile"...
        bool badgeFound = false;
        int variant = -1;           // quale versione dell'immagine ha funzionato
        std::string engine;         // "PP-OCR", "binaria"... (quale OCR e quale immagine)
        float confidence = -1.0f;   // sicurezza di PP-OCR (0..1), -1 con l'OCR di Windows
        long long durationMs = 0;
        unsigned long long tick = 0; // GetTickCount64 della lettura
    };

    struct Unknown {
        int value = 0;
        std::string time; // "01/11 21:43"
        int count = 1;    // quante volte è stata letta
    };

    struct ManualState {
        bool active = false;
        std::string buffer;
    };

    MiningScanner() = default;
    ~MiningScanner();
    MiningScanner(const MiningScanner&) = delete;
    MiningScanner& operator=(const MiningScanner&) = delete;

    void start(Host host);
    void shutdown();
    // Impostazioni cambiate (lettura continua, zona): il thread si risveglia.
    void settingsChanged();

    // Una lettura dello schermo (asincrona: il risultato arriva con Event::Reading).
    void scan(const std::string& source);
    // Un valore già noto (detto a voce, scritto nella pagina, dal tastierino).
    void submitValue(int value, const std::string& source);
    // Testo libero (es. "2 16,960"): passa dalla stessa decodifica dell'OCR.
    void submitText(const std::string& text, const std::string& source);

    void beginManualInput();
    void cancelManualInput();
    ManualState manualState() const;

    Reading lastReading() const;
    std::vector<Reading> history() const; // dalla più recente

    // --- Tabella dei materiali ----------------------------------------------------
    std::vector<Mining::Material> materials() const;
    std::string materialsText() const;
    // Salva il testo della tabella (righe "NOME,FIRMA[,TIPO]"). false = niente di valido.
    bool setMaterialsText(const std::string& text, std::string& message);
    void resetMaterials();
    // CSV, il "materialsdb" di Mining Signature Calculator o lookup_table.json di mining_ocr.
    bool importFile(const std::wstring& path, bool replace, std::string& message);
    bool exportCsv(const std::wstring& path, std::string& message);

    // --- Sconosciute ----------------------------------------------------------------
    std::vector<Unknown> unknowns() const;
    void removeUnknown(int value);
    void clearUnknowns();
    // "Questa era Rame × 4": firma base = valore / quantità, aggiunta (o corretta) nella tabella.
    bool identify(int value, const std::string& material, int quantity, std::string& message);

    static std::wstring materialsPath();
    static std::wstring unknownsPath();
    static std::wstring imagesDir(); // %appdata%\Iris\debug\mining (immagini dell'ultima lettura)

    // La lettura vera e propria su un'immagine (anche da file, per le prove).
    // saveImagesDir: se non vuota, ci salva i passaggi.
    static Reading readImage(const ScreenCapture::Image& image, const std::vector<Mining::Material>& materials, int maxQuantity,
                             const std::wstring& saveImagesDir);

private:
    struct Job {
        enum class Kind { Scan, Value, Text } kind = Kind::Scan;
        std::string source;
        int value = 0;
        std::string text;
    };

    std::function<void(const Reading&)> m_onReading;
    void enqueue(Job job);
    void workerLoop();
    void runScan(const AppSettings& s, const std::string& source, bool continuous);
    void publish(Reading r, bool remember);
    void noteUnknown(int value);
    void loadData();
    void saveMaterials();
    void saveUnknowns();
    void notify(Event e);

    // Tastierino
    void manualThread();
    static LRESULT CALLBACK keyboardHook(int code, WPARAM wp, LPARAM lp);
    bool onManualKey(DWORD vk); // true = tasto "mangiato"

    Host m_host;
    mutable std::mutex m_mutex; // letture, tabella, sconosciute, tastierino
    Reading m_last;
    std::deque<Reading> m_history;
    std::vector<Mining::Material> m_materials;
    std::vector<Unknown> m_unknowns;
    ManualState m_manual;

    std::thread m_worker;
    std::mutex m_jobMutex;
    std::condition_variable m_jobCv;
    std::deque<Job> m_jobs;
    std::atomic<bool> m_stopping{ false };
    std::atomic<bool> m_wake{ false };

    // Lettura continua: l'ultimo valore visto e quello mostrato.
    int m_autoCandidate = 0;
    int m_autoShown = 0;
    unsigned long long m_autoShownTick = 0;
    uint64_t m_autoLastHash = 0;

    // Annuncio a voce: la roccia già detta (minerale, quantità, valore) e quando, per non ripeterla.
    std::string m_spokenKey;
    int m_spokenValue = 0;
    unsigned long long m_spokenTick = 0;
    void announce(const Reading& r);

public:
    // Cosa direbbe a voce per una lettura ("" = niente: valore incerto o ambiguo). Pura, per le prove.
    static std::string spokenText(const Mining::Decoded& d);

    std::thread m_manualThread;
    std::atomic<DWORD> m_manualThreadId{ 0 };
    unsigned long long m_manualStartTick = 0;
};
