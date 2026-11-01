#pragma once

#include <mutex>
#include <string>
#include <vector>

// Registro delle scansioni del mining: con l'opzione accesa ogni lettura dello
// scanner (tasto, lettura continua, voce, tastierino, barra rapida) finisce in un
// registro; alla chiusura Iris chiede il luogo e la qualità media dei materiali e
// salva il file sul desktop: JSON (per il sito) e, se vuoi, CSV (per Excel).
// La stessa roccia letta più volte di seguito conta una volta sola.
// Il registro in corso sopravvive a un riavvio (%appdata%\Iris\registro_mining.json).
// Il formato del file è descritto in docs/registro-mining.md.
namespace MiningLog {

    struct Alternative {
        std::string material; // nome nel gioco ("Savrilium")
        std::string label;    // nome nella tabella di Iris ("Savrilium", "Copper (Rame)")
        std::string kind;     // "minerale", "salvage", "roc", "fps"
        int quantity = 0;
    };

    struct Entry {
        long long epoch = 0;       // secondi UTC
        int value = 0;             // la signature
        std::string outcome;       // "certo", "probabile", "ambiguo", "sconosciuto"
        std::string material, label, kind, rarity;
        int quantity = 0;
        std::vector<Alternative> alternatives; // solo se "ambiguo"
        std::string source;        // "tasto", "continua", "voce", "manuale", "barra"
        int reads = 1;             // quante letture della stessa roccia
    };

    struct MaterialTotal {
        std::string material, label, kind, rarity;
        int rocks = 0;    // rilevamenti
        int quantity = 0; // somma delle quantità
    };

    struct Summary {
        int finds = 0, reads = 0, sure = 0, toCheck = 0;
        std::vector<MaterialTotal> materials; // dalla quantità più alta
    };

    struct Closing {
        std::string place;
        int quality = -1;          // qualità media 0..1000 (-1 = non detta)
        std::string operatorName;  // chi ha minato (nome nel gioco o in Iris)
        std::string program;       // "Iris 1.2.0"
        long long start = 0, end = 0;
    };

    // "Copper (Rame)" -> "Copper": il nome nel gioco, senza la traduzione tra parentesi.
    std::string gameName(const std::string& label);
    Summary summarize(const std::vector<Entry>& entries);
    // "2026-11-01T21:40:12+01:00": ora locale con il fuso, come la vuole un sito.
    std::string isoLocal(long long epoch);
    // Il file per il sito e quello per Excel (separatore ";", UTF-8 con BOM).
    std::string toJson(const std::vector<Entry>& entries, const Closing& closing);
    std::string toCsv(const std::vector<Entry>& entries, const Closing& closing);
    // "Iris mining 2026-11-01 21-40" (senza estensione), ora locale.
    std::string fileStem(long long epoch);
    // La cartella Desktop dell'utente (anche se è spostata in OneDrive). Con --cartella-dati: quella cartella.
    std::wstring desktopFolder();

    class Recorder {
    public:
        void load();
        bool active() const;
        long long startedAt() const;
        std::vector<Entry> entries() const;
        std::vector<std::string> recentPlaces() const;
        std::wstring lastFile() const;

        void start(long long now);
        void cancel();
        // Una lettura: true se il registro è cambiato (rilevamento nuovo o stessa roccia riletta).
        bool add(const Entry& e);
        // Chiude il registro e scrive i file in folder. jsonPath: il file JSON scritto.
        bool finish(const Closing& closing, const std::wstring& folder, bool csv, std::wstring& jsonPath, std::string& error);

    private:
        void save() const;
        mutable std::mutex m_mutex;
        bool m_active = false;
        long long m_start = 0;
        long long m_lastSeen = 0; // ultima lettura dell'ultimo rilevamento
        std::vector<Entry> m_entries;
        std::vector<std::string> m_places;
        std::wstring m_lastFile;
    };

} // namespace MiningLog
