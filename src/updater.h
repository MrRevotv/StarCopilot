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

    // Un controllo non riuscito detto in italiano, e tra quanti minuti riprovare: GitHub che limita le
    // richieste (403/429) va lasciato in pace un'ora, un errore suo (5xx) o la rete che manca mezz'ora.
    // status = codice HTTP (0 se non è arrivata nessuna risposta). Pura: provata nei test.
    struct Failure {
        std::string text;
        int retryMinutes = 240;
    };
    Failure describeFailure(int status, const std::string& networkError);
    // Il ritardo dopo "failures" errori di fila: raddoppia a ogni errore, al massimo 4 ore. Pura.
    int retryDelayMinutes(int baseMinutes, int failures);

    // L'ultima release pubblicata (rete). retryMinutes (se non è riuscito): quando riprovare.
    bool fetchLatest(Release& out, std::string& error, const std::atomic<bool>* cancel, int* retryMinutes = nullptr);

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

    // Una versione scaricata che, installata, non parte come quella versione (es. release pubblicata
    // con l'exe sbagliato): si cancella e non si riscarica più, così Iris non si reinstalla in cerchio.
    void markBroken(const std::string& version);
    bool isBroken(const std::string& version);
    // Da chiamare all'avvio dopo un aggiornamento (--dopo-aggiornamento): se c'è ancora uno scaricato
    // più nuovo di questa versione, l'exe installato non era quello giusto e lo scaricato si scarta.
    void confirmInstalled();

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
        std::string retryAt;   // dopo un errore: a che ora riprova da sola ("22:13"), vuoto = tra 4 ore
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
    int runCheck(bool manual); // minuti fino al prossimo controllo
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
    int m_failures = 0; // controlli non riusciti di fila (per aspettare sempre di più)
    std::atomic<bool> m_stopping{ false };
};
