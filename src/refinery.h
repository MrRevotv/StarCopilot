#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <vector>

// Raffineria: gli ordini di lavorazione con il loro conto alla rovescia. Il gioco
// non li scrive nel Game.log, quindi si aggiungono a mano (pagina Mining, barra
// rapida) o a voce ("Iris, raffineria a Levski tra 3 ore e 20"). Restano dopo il
// riavvio (%appdata%\Iris\raffineria.json); quando uno finisce Iris avvisa.
namespace Refinery {

    struct Job {
        std::string id;
        std::string place;   // stazione
        std::string note;    // materiale, quantità...
        long long start = 0; // secondi UTC
        long long end = 0;
        bool announced = false;
        bool done(long long now) const { return now >= end; }
    };

    // "3h20", "3:20", "3 ore e 20 minuti", "200" (minuti), "1g 2h", "45m", "90 s"... -> secondi.
    std::optional<long long> parseDuration(const std::string& text);
    // "2 h 05 min", "12 min", "45 s", "pronto".
    std::string formatRemaining(long long seconds);
    // A voce: "3 ore e 20 minuti", "un'ora", "12 minuti".
    std::string spokenDuration(long long seconds);

    class Timers {
    public:
        void load();
        bool save() const;
        std::string add(const std::string& place, const std::string& note, long long seconds, long long now);
        bool remove(const std::string& id);
        void clearDone(long long now);
        std::vector<Job> jobs() const;
        // I lavori finiti adesso e non ancora annunciati (si segnano come annunciati).
        std::vector<Job> takeFinished(long long now);

    private:
        mutable std::mutex m_mutex;
        std::vector<Job> m_jobs;
    };

} // namespace Refinery
