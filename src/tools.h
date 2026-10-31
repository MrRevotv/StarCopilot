#pragma once

#include <atomic>
#include <string>
#include <vector>

// Quello che serve agli strumenti da parte del resto del programma (lo fa Assistant).
class ToolHost {
public:
    virtual ~ToolHost() = default;
    virtual void addTimer(int seconds, const std::string& label) = 0;
    virtual int cancelTimers() = 0;
    virtual std::string homeCity() = 0;
    virtual void remember(const std::string& fact) = 0;
    // Strumenti in più, che dipendono dallo stato (es. game_command quando il gioco è aperto).
    virtual std::vector<std::string> extraToolDefinitions() { return {}; }
    // true = lo strumento era suo (result riceve il risultato).
    virtual bool runExtraTool(const std::string& name, const std::string& argumentsJson, const std::atomic<bool>* cancel,
                              std::string& result) {
        return false;
    }
};

// Gli strumenti che il modello può usare: ricerca su internet, pagine web,
// meteo, notizie, programmi, siti, timer, volume, musica, memoria.
class Tools {
public:
    explicit Tools(ToolHost& host) : m_host(host) {}

    // Le definizioni per il prompt (una stringa JSON per strumento). Senza
    // internet gli strumenti che lo usano non ci sono proprio.
    std::vector<std::string> definitions(bool internet) const;

    // Esegue lo strumento; ritorna il risultato da dare al modello (testo).
    std::string run(const std::string& name, const std::string& argumentsJson, bool internet, const std::atomic<bool>* cancel);

    // Cosa sta facendo, in una frase per la finestra: "Cerco su internet: meteo Roma".
    static std::string describe(const std::string& name, const std::string& argumentsJson);

    // Codice meteo WMO (Open-Meteo) in parole: 61 -> "pioggia debole". Pura, provata nei test.
    static std::string weatherDescription(int code);

private:
    std::string webSearch(const std::string& query, const std::atomic<bool>* cancel);
    std::string readWebpage(const std::string& url, const std::atomic<bool>* cancel);
    std::string weather(const std::string& location, const std::atomic<bool>* cancel);
    std::string news(const std::string& topic, const std::atomic<bool>* cancel);

    ToolHost& m_host;
};
