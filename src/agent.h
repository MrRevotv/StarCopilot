#pragma once

#include "chat_format.h"
#include "llm_engine.h"
#include "tools.h"

#include <atomic>
#include <functional>
#include <string>
#include <vector>

// La conversazione con Iris: il modello risponde, se serve usa gli strumenti
// (fino a qualche giro: comando di gioco, poi risponde) e le frasi della risposta
// escono una alla volta verso la voce.
//
// Della conversazione si tengono domande, richieste di strumenti (con i
// risultati accorciati) e risposte: "e a Milano?" funziona, e il modello
// continua a vedere che per i dati si usano gli strumenti (se vedesse solo
// risposte date "a memoria", le imiterebbe e comincerebbe a inventare).
class Agent {
public:
    enum class Result { Ok, Cancelled, Error };

    struct Callbacks {
        std::function<void(const std::string&)> onSentence; // una frase completa da dire
        std::function<void(const std::string&)> onTool;     // "Cerco su internet: ..."
        std::function<void(const std::string&)> onText;     // la risposta scritta finora
    };

    struct Persona {
        std::string userName;
        std::vector<std::string> memory; // cose da ricordare (memoria.txt)
        std::string game; // nome del gioco aperto (vuoto = nessuno): si usa game_command
        bool brief = false; // risposte di poche parole (mentre giochi, o se l'hai scelto sempre)
    };

    Agent(LlmEngine& llm, Tools& tools) : m_llm(llm), m_tools(tools) {}

    void setPersona(const Persona& persona);
    Result ask(const std::string& userText, const Callbacks& callbacks, const std::atomic<bool>* cancel, std::string& answer);
    void reset();
    size_t exchanges() const { return m_history.size(); }
    // L'ultima ask() chiedeva un'azione e il modello ha detto "fatto" senza farla (due volte): la risposta
    // è stata sostituita con "Non l'ho fatto...". Per il suono d'errore a voce spenta.
    bool lastUnfulfilled() const { return m_unfulfilled; }

    // Prepara il modello alla prossima domanda: istruzioni, strumenti e conversazione già
    // elaborati (e il modello davvero pronto sulla scheda video), così la risposta parte
    // subito. freshConversation: senza la conversazione (sta per ricominciare da capo).
    bool warmUp(const std::atomic<bool>* cancel, bool freshConversation = false);

    // Un comando fatto senza il modello (il dizionario dei comandi di gioco, la memoria): alla
    // prossima domanda il modello lo trova scritto prima ("Comandi eseguiti poco fa: «rotta per
    // Crusader»: Errore..."), così "riprova" o "è attiva" hanno un senso. Non come scambi finti
    // nella conversazione: provato, il 7B ne copiava le risposte e smetteva di usare gli strumenti.
    void noteAction(const std::string& userText, const std::string& result);

    // Le istruzioni per il modello (per le prove).
    std::string systemPrompt() const;

private:
    void trimHistory();
    // Il prompt con la conversazione, più "extra" (i messaggi della domanda in corso).
    std::string buildPrompt(const std::string& system, const std::vector<std::string>& tools, const std::vector<ChatFormat::Message>& extra) const;

    LlmEngine& m_llm;
    Tools& m_tools;
    Persona m_persona;
    // Uno scambio = domanda, eventuali strumenti, risposta.
    std::vector<std::vector<ChatFormat::Message>> m_history;
    bool m_unfulfilled = false;
    std::vector<std::string> m_actions; // vedi noteAction
};
