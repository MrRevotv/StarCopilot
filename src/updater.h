#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

// Aggiornamento automatico: le versioni nuove si pubblicano come release su
// GitHub (pubblica.bat), con l'exe allegato. Iris controlla l'ultima release,
// la scarica in %appdata%\Iris\aggiornamenti, ne verifica l'impronta SHA-256
// (quella che GitHub calcola per ogni file allegato) e la installa:
//  - da sola all'avvio successivo (prima ancora di aprire la finestra);
//  - subito, se premi "Riavvia e aggiorna".
// Installare = rinominare l'exe in uso in ".old" (Windows lo permette anche
// mentre gira), copiare al suo posto quello nuovo e avviarlo. Il ".old" si
// cancella al giro dopo. Se la cartella del programma non è scrivibile
// (es. Programmi) si avvisa e basta.
namespace Updater {

    // Dove si pubblicano le versioni nuove.
    inline constexpr const char* kRepository = "MrRevotv/StarCopilot";

    std::string currentVersion(); // "1.2.0" (dal file VERSION, al momento della compilazione)

    // -1 / 0 / 1. "V1.1" = "1.1.0": la V davanti e i numeri mancanti non contano.
    // Pura: provata in tests/tests.cpp.
    int compareVersions(const std::string& a, const std::string& b);

    struct Release {
        std::string version;  // "1.3.0"
        std::string tag;      // "V1.3.0"
        std::string name;     // "Iris V1.3.0"
        std::string notes;    // il testo della release (cosa cambia)
        std::string pageUrl;  // la pagina della release
        std::string assetUrl; // l'exe da scaricare
        std::string sha256;   // impronta dell'exe (minuscolo), vuota se GitHub non la dà
        long long size = 0;
    };

    // La risposta di /releases/latest. Pura: provata nei test.
    bool parseRelease(const std::string& json, Release& out, std::string& error);

    // L'ultima release pubblicata (rete).
    bool fetchLatest(Release& out, std::string& error, const std::atomic<bool>* cancel);

    std::wstring updatesDir(); // %appdata%\Iris\aggiornamenti
    // Il file dove sta (o starà) l'exe di quella versione, già controllato.
    std::wstring downloadedPath(const std::string& version);

    // Scarica l'exe della release e lo controlla (dimensione, impronta, "MZ").
    // Se c'è già, scaricato e controllato, non lo riscarica.
    bool download(const Release& release, const std::function<void(long long, long long)>& onProgress,
                  const std::atomic<bool>* cancel, std::wstring& path, std::string& error);

    // Una versione più nuova di questa già scaricata e controllata (vuoto = nessuna).
    std::wstring pendingUpdate(std::string* version = nullptr);

    // Mette newExe al posto del programma in uso e lo avvia: chi chiama deve poi uscire.
    bool install(const std::wstring& newExe, std::string& error);

    // All'avvio: toglie l'exe vecchio e gli scaricati che non servono più.
    void cleanup();

    // SHA-256 di un file, in esadecimale minuscolo.
    bool sha256File(const std::wstring& path, std::string& hex);

} // namespace Updater

// Il controllo in sottofondo: all'avvio (dopo qualche secondo) e poi ogni
// qualche ora; se c'è una versione nuova la scarica da sola. La finestra
// legge lo stato e mostra "Aggiornamento pronto".
class UpdateManager {
public:
    enum class Phase { Idle, Checking, UpToDate, Downloading, Ready, Error, Disabled };
    struct State {
        Phase phase = Phase::Idle;
        std::string current;  // questa versione
        std::string latest;   // l'ultima pubblicata (se nota)
        std::string notes;
        std::string pageUrl;
        std::string error;
        std::string checkedAt; // "21:43"
        int progress = 0;      // 0-100 durante il download
    };

    UpdateManager() = default;
    ~UpdateManager() { stop(); }
    UpdateManager(const UpdateManager&) = delete;
    UpdateManager& operator=(const UpdateManager&) = delete;

    // enabled: l'aggiornamento automatico è acceso? notify: lo stato è cambiato (qualunque thread).
    void start(std::function<bool()> enabled, std::function<void()> notify);
    void stop();
    void checkNow();
    State state() const;
    // Installa quello scaricato e avvia la versione nuova: se true, il programma deve chiudersi.
    bool installNow(std::string& error);

private:
    void loop();
    void runCheck(bool manual);
    void set(const std::function<void(State&)>& change);

    mutable std::mutex m_mutex;
    State m_state;
    std::wstring m_readyPath;
    std::function<bool()> m_enabled;
    std::function<void()> m_notify;
    std::thread m_thread;
    std::condition_variable m_cv;
    bool m_wake = false;
    bool m_manual = false;
    std::atomic<bool> m_stopping{ false };
};
