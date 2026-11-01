#pragma once

#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// Diario di bordo: lettura del Game.log di Star Citizen.
//
// ATTENZIONE: la lettura del file Game.log è vietata dai Termini di Servizio di
// Star Citizen. È spenta in partenza e si attiva solo consapevolmente, dopo
// l'avviso, a rischio dell'utente; lo sviluppatore non si assume alcuna
// responsabilità per le conseguenze di un uso improprio (vedi AppSettings::gameLogEnabled).
//
// Sola lettura: niente memoria del gioco, niente input, niente scritture nella
// cartella del gioco. Le righe che interessano sono poche e riconoscibili (le
// notifiche dell'HUD, in italiano o in inglese, con MissionId/ObjectiveId; il
// login; il posto di pilotaggio lasciato; l'inventario di una località; gli
// stati del personaggio). Idee e formati da citizen-assistant (MIT, fishtaco7),
// verificati su Game.log reali.
namespace GameLog {

    enum class Kind { Login, Contract, Objective, Zone, Jurisdiction, Health, Party, Ship, Location, Journal, Hangar, Blueprint, Combat, Other };
    enum class Level { Info, Good, Warn, Bad };

    struct Event {
        Kind kind = Kind::Other;
        // Contratti: accepted, completed, failed, shared, available, refused, abandoned.
        // Obiettivi: new, completed, failed, withdrawn. Zone: armistice_in/out, monitored_in/out/down/up,
        // private_in/out. Salute: injury, effect, emergency, death. Gruppo: qt. Nave: left.
        // Combattimento: kill, killed, disabled, destroyed. Altri: login, inventory, journal, hangar, blueprint, notice.
        std::string action;
        std::string subject;   // contratto, obiettivo, giurisdizione, compagno, nave, località...
        std::string detail;    // es. "grave · Braccio sinistro"
        std::string missionId, objectiveId;
        std::string text;      // la riga per il diario, in italiano
        Level level = Level::Info;
        long long epoch = 0;   // secondi UTC, dalla riga
    };

    // Una riga del Game.log -> evento (niente stato, niente file). handle = il tuo nome nel gioco
    // (serve per gli stati del personaggio e per i combattimenti; vuoto = non ancora noto).
    std::optional<Event> parseLine(const std::string& line, const std::string& handle);

    // "AEGS_Gladius_749027674818" -> "Aegis Gladius"; "RR_CRU_LEO" -> "Seraphim Station".
    std::string shipName(const std::string& code);
    std::string locationName(const std::string& code);
    // "<2026-11-01T09:31:24.099Z> ..." -> secondi UTC (0 se manca).
    long long lineEpoch(const std::string& line);

    struct Contract {
        std::string id, name;
        long long since = 0;
    };
    struct Objective {
        std::string id, missionId, text;
        long long since = 0;
    };
    struct Mark {
        std::string text;
        long long epoch = 0;
    };

    // Quello che si sa dal diario: missioni in corso, zona, salute, ultimi eventi.
    struct State {
        std::string handle;
        std::vector<Contract> contracts;   // in corso
        std::vector<Objective> objectives; // obiettivi in corso
        std::deque<Event> recent;          // gli ultimi eventi (il più nuovo in fondo)
        int armistice = -1, monitored = -1, privateProperty = -1; // -1 = non si sa, 0 = no, 1 = sì
        std::string jurisdiction, location, lastShip;
        std::vector<Mark> health;          // ferite e stati (i più recenti in fondo)
        int accepted = 0, completed = 0, failed = 0; // in questa sessione di Iris
    };
    // Applica un evento allo stato. Restituisce false se non cambia niente di importante.
    bool apply(State& state, const Event& e, size_t maxRecent = 80);

    // Dove sta il Game.log: LIVE, PTU, EPTU, HOTFIX, TECH-PREVIEW nelle cartelle solite di ogni disco fisso
    // (il più recente se ce n'è più d'uno). Vuoto se non c'è.
    std::wstring discover();

    // Legge il file in un thread suo: all'apertura rilegge la sessione di gioco già scritta (live = false:
    // niente avvisi a voce), poi segue le righe nuove (live = true). Se il gioco riparte (file nuovo) ricomincia.
    class Watcher {
    public:
        using Callback = std::function<void(const Event& e, bool live)>;
        using Changed = std::function<void()>; // stato cambiato (anche alla fine della rilettura)

        Watcher() = default;
        ~Watcher() { stop(); }
        Watcher(const Watcher&) = delete;
        Watcher& operator=(const Watcher&) = delete;

        void start(const std::wstring& pathOverride, Callback onEvent, Changed onChanged);
        void stop();
        bool running() const { return m_running; }

        struct Status {
            std::wstring path;
            bool found = false;     // file trovato e aperto
            bool catchingUp = false;
            long long bytes = 0;    // letti
            int lines = 0, events = 0;
            long long lastEpoch = 0;
        };
        Status status() const;
        State state() const;

        // Missioni ricordate tra un avvio e l'altro (%appdata%\Iris\missioni.json).
        void forgetContract(const std::string& id);
        void clearRecent();

    private:
        void loop();
        bool openFile(const std::wstring& path);
        void closeFile();
        void readNew();
        void handleLine(const std::string& line);
        void loadContracts();
        void saveContracts();

        std::wstring m_override;
        Callback m_onEvent;
        Changed m_onChanged;
        std::thread m_thread;
        std::atomic<bool> m_running{ false };
        std::atomic<bool> m_stop{ false };

        mutable std::mutex m_mutex;
        State m_state;
        Status m_status;

        // Solo dal thread di lettura
        void* m_file = nullptr;
        unsigned long m_fileIndexHigh = 0, m_fileIndexLow = 0;
        long long m_offset = 0, m_catchUpEnd = 0;
        std::string m_partial;
        bool m_live = false;
        bool m_contractsDirty = false;
    };

} // namespace GameLog
