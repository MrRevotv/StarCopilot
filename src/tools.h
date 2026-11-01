#pragma once

#include <atomic>
#include <string>
#include <vector>

// Quello che serve agli strumenti da parte del resto del programma (lo fa Assistant).
class ToolHost {
public:
    virtual ~ToolHost() = default;
    virtual void remember(const std::string& fact) = 0;
    // Strumenti in più, che dipendono dallo stato (es. game_command quando il gioco è aperto).
    virtual std::vector<std::string> extraToolDefinitions() { return {}; }
    // true = lo strumento era suo (result riceve il risultato).
    virtual bool runExtraTool(const std::string& name, const std::string& argumentsJson, const std::atomic<bool>* cancel,
                              std::string& result) {
        return false;
    }
};

// Gli strumenti che il modello può usare. Iris è un'assistente di gioco: i
// comandi del gioco (game_command, dal ToolHost quando il gioco è aperto) e la
// memoria. Niente controlli di Windows, niente internet.
class Tools {
public:
    explicit Tools(ToolHost& host) : m_host(host) {}

    // Le definizioni per il prompt (una stringa JSON per strumento).
    std::vector<std::string> definitions() const;

    // Esegue lo strumento; ritorna il risultato da dare al modello (testo).
    std::string run(const std::string& name, const std::string& argumentsJson, const std::atomic<bool>* cancel);

    // Cosa sta facendo, in una frase per la finestra: "Star Citizen: carrello".
    static std::string describe(const std::string& name, const std::string& argumentsJson);

private:
    ToolHost& m_host;
};
