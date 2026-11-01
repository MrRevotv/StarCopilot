#include "agent.h"
#include "log.h"
#include "speech_text.h"
#include "text_utils.h"

#include <windows.h>

#include <cstdio>

namespace {
    constexpr int kMaxToolRounds = 4;
    constexpr size_t kMaxExchanges = 12;      // domande (con strumenti e risposta) tenute nella conversazione
    constexpr size_t kToolResultInHistory = 700; // caratteri dei risultati degli strumenti tenuti dopo la risposta

    std::string todayItalian() {
        static const char* const days[] = { "domenica", "lunedì", "martedì", "mercoledì", "giovedì", "venerdì", "sabato" };
        static const char* const months[] = { "gennaio", "febbraio", "marzo", "aprile", "maggio", "giugno",
                                              "luglio", "agosto", "settembre", "ottobre", "novembre", "dicembre" };
        SYSTEMTIME t;
        GetLocalTime(&t);
        return std::string(days[t.wDayOfWeek]) + " " + std::to_string(t.wDay) + " " + months[t.wMonth - 1] + " " + std::to_string(t.wYear);
    }

    std::string nowHHMM() {
        SYSTEMTIME t;
        GetLocalTime(&t);
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%02d:%02d", t.wHour, t.wMinute);
        return buf;
    }

    // Quanti caratteri in fondo potrebbero essere l'inizio di "<tool_call>":
    // quelli si tengono da parte finché non si sa.
    size_t possibleTagStart(const std::string& s) {
        static const std::string tag = "<tool_call>";
        for (size_t keep = std::min(s.size(), tag.size() - 1); keep > 0; --keep) {
            if (s.compare(s.size() - keep, keep, tag, 0, keep) == 0) return keep;
        }
        return 0;
    }
}

void Agent::setPersona(const Persona& persona) {
    m_persona = persona;
}

void Agent::reset() {
    m_history.clear();
}

std::string Agent::systemPrompt() const {
    const std::string who = m_persona.userName.empty() ? "dell'utente" : "di " + m_persona.userName;
    std::string s =
        "Sei Iris, l'assistente di gioco " + who +
        " per Star Citizen: copilota di bordo, comandi della nave, chat e mining. Ti ispiri a J.A.R.V.I.S. di Iron Man: competente, "
        "rapida, cortese, con un filo di ironia elegante.\n"
        "Oggi è " + todayItalian() + ". Ogni messaggio dell'utente comincia con l'ora attuale tra parentesi quadre: tienine conto, "
        "ma non ripeterla se non serve.\n\n"
        "Come rispondi:\n";
    if (m_persona.brief) {
        // Niente esempi di conferme di azioni qui: il modello 7B li copierebbe senza chiamare lo strumento.
        s += "- Sempre in italiano. MODALITÀ BREVE: l'utente non vuole chiacchiere. La risposta finale è una sola frase di poche "
             "parole (al massimo una decina): niente saluti, niente cortesie, non ripetere la domanda, nessuna domanda finale. Gli "
             "strumenti si usano esattamente come sempre: prima lo strumento, poi la frase breve.\n";
    } else {
        s += "- Sempre in italiano, con frasi naturali da dire ad alta voce: di solito una o due frasi, di più solo se l'utente lo chiede.\n";
    }
    s += "- Niente elenchi puntati, titoli, grassetto, emoji o indirizzi web: le tue parole vengono lette da una voce sintetica.\n"
        "- Scrivi numeri e orari in modo che si leggano bene (\"21 gradi\", \"alle 18 e 30\").\n"
        "- La richiesta arriva da un riconoscimento vocale e può contenere parole sbagliate: capisci l'intenzione più probabile.\n";
    s += "- Ti occupi solo del gioco. Non hai internet e non controlli Windows: non apri programmi o siti, non regoli volume o "
         "musica, non imposti timer, non dai meteo o notizie. Se te lo chiedono, dillo in una frase e ricorda cosa sai fare.\n"
         "- Non inventare dati che non hai (prezzi aggiornati, patch, eventi): se non sei sicura dillo con sincerità.\n"
         "- Se l'utente ti chiede di ricordare qualcosa usa remember.";
    if (!m_persona.game.empty()) {
        s += "\n- " + m_persona.game + " è aperto e l'utente sta giocando: per comandare la nave (tasti, rotta sulla mappa stellare) "
             "usa game_command, una volta per ogni azione, e poi conferma con due o tre parole al massimo.";
    }
    if (!m_persona.memory.empty()) {
        s += "\n\nCose che sai dell'utente (te le ha chieste di ricordare):";
        for (const auto& m : m_persona.memory) s += "\n- " + m;
    }
    // Alla fine: è la regola che il modello deve avere più presente.
    if (m_persona.brief) s += "\n\nRicorda: MODALITÀ BREVE. Risposta finale di una frase, al massimo una decina di parole.";
    return s;
}

Agent::Result Agent::ask(const std::string& userText, const Callbacks& cb, const std::atomic<bool>* cancel, std::string& answer) {
    answer.clear();
    const std::string userMessage = "[" + nowHHMM() + "] " + userText;
    const std::vector<std::string> tools = m_tools.definitions();
    const std::string system = systemPrompt();

    while (m_history.size() > kMaxExchanges) m_history.erase(m_history.begin());
    std::vector<ChatFormat::Message> turn; // i messaggi di questa domanda (strumenti compresi)
    turn.push_back({ ChatFormat::Message::Role::User, userMessage });

    LlmEngine::Sampling sampling;
    std::string spokenSoFar;

    for (int round = 0; round <= kMaxToolRounds; ++round) {
        // Il prompt: se è troppo lungo si dimenticano le domande più vecchie.
        std::vector<ChatFormat::Message> messages;
        std::string prompt;
        while (true) {
            messages.clear();
            for (const auto& exchange : m_history) messages.insert(messages.end(), exchange.begin(), exchange.end());
            messages.insert(messages.end(), turn.begin(), turn.end());
            // All'ultimo giro niente più strumenti: deve rispondere.
            prompt = ChatFormat::buildPrompt(system, round < kMaxToolRounds ? tools : std::vector<std::string>{}, messages, m_llm.isQwen3());
            if (m_history.empty() || m_llm.countTokens(prompt) + sampling.maxTokens < m_llm.contextSize()) break;
            m_history.erase(m_history.begin());
        }

        // La risposta arriva a pezzi: il testo va alla voce frase per frase,
        // le richieste di strumenti no.
        enum class Mode { Undecided, Speak, Tool };
        Mode mode = Mode::Undecided;
        SpeechText::SentenceSplitter splitter;
        std::string output;
        size_t fed = 0; // caratteri di "output" già passati al divisore di frasi
        auto feed = [&](size_t upTo) {
            if (upTo <= fed) return;
            for (const auto& sentence : splitter.push(output.substr(fed, upTo - fed))) {
                if (cb.onSentence) cb.onSentence(sentence);
                spokenSoFar += (spokenSoFar.empty() ? "" : " ") + sentence;
            }
            fed = upTo;
        };

        std::string raw;
        LlmEngine::Result r = m_llm.generate(prompt, sampling,
            [&](const std::string& piece) {
                output += piece;
                if (mode == Mode::Undecided) {
                    std::string t = TextUtils::trim(output);
                    if (t.empty()) return true;
                    if (TextUtils::startsWith(t, "<tool_call>") || TextUtils::startsWith(t, "{")) mode = Mode::Tool;
                    else if (std::string("<tool_call>").compare(0, t.size(), t) == 0) return true; // forse l'inizio del tag
                    else if (t == "`" || t == "``" || TextUtils::startsWith(t, "```")) {
                        if (t.size() < 8) return true; // ```json { ... } ?
                        mode = t.find('{') != std::string::npos ? Mode::Tool : Mode::Speak;
                    } else mode = Mode::Speak;
                }
                if (mode == Mode::Speak) {
                    size_t tag = output.find("<tool_call>");
                    if (tag != std::string::npos) {
                        feed(tag);
                        mode = Mode::Tool;
                    } else {
                        feed(output.size() - possibleTagStart(output));
                        if (cb.onText) cb.onText(ChatFormat::cleanAnswer(output));
                    }
                }
                // Una richiesta di strumento completa: inutile continuare a scrivere.
                if (mode == Mode::Tool && output.find("</tool_call>") != std::string::npos) return false;
                // Il modello comincia a inventarsi il turno dopo ("<|im_start|>user..."): basta.
                if (output.find("<|im_start|>") != std::string::npos) return false;
                return !(cancel && cancel->load());
            },
            raw, cancel);

        if (r == LlmEngine::Result::Cancelled || (cancel && cancel->load())) {
            answer = spokenSoFar.empty() ? ChatFormat::cleanAnswer(output) : spokenSoFar;
            return Result::Cancelled;
        }
        if (r != LlmEngine::Result::Ok) return Result::Error;

        std::string textBefore;
        std::vector<ChatFormat::ToolCall> calls = round < kMaxToolRounds ? ChatFormat::parseToolCalls(output, &textBefore)
                                                                         : std::vector<ChatFormat::ToolCall>{};
        if (!calls.empty()) {
            // Nella conversazione la richiesta resta com'è (con il tag chiuso).
            std::string assistantText = TextUtils::trim(output);
            if (assistantText.find("</tool_call>") == std::string::npos && assistantText.find("<tool_call>") != std::string::npos) {
                assistantText += "\n</tool_call>";
            }
            turn.push_back({ ChatFormat::Message::Role::Assistant, assistantText });
            for (const auto& call : calls) {
                const std::string what = Tools::describe(call.name, call.argumentsJson);
                Log::info("Strumento: " + call.name + " " + call.argumentsJson);
                if (cb.onTool) cb.onTool(what);
                std::string result = m_tools.run(call.name, call.argumentsJson, cancel);
                if (cancel && cancel->load()) return Result::Cancelled;
                Log::info("Risultato di " + call.name + ": " + std::to_string(result.size()) + " caratteri");
                turn.push_back({ ChatFormat::Message::Role::Tool, result });
            }
            continue;
        }

        // Risposta finale.
        if (mode != Mode::Speak) {
            // Sembrava una richiesta di strumento ma non si capisce (JSON rotto).
            std::string cleaned = ChatFormat::cleanAnswer(output);
            if (fed > 0) output = output.substr(0, fed); // il testo prima del tag è già stato detto: basta così
            else if (TextUtils::startsWith(cleaned, "{") || cleaned.empty()) output = "Scusa, non sono riuscita a completare la richiesta.";
            else output = cleaned;
            Log::warn("Risposta non capita come strumento: " + raw.substr(0, 300));
        }
        feed(output.size());
        const std::string rest = splitter.flush();
        if (!rest.empty() && cb.onSentence) cb.onSentence(rest);
        answer = ChatFormat::cleanAnswer(output);
        if (cb.onText) cb.onText(answer);
        if (answer.empty()) answer = "...";
        // Nella conversazione resta tutto, ma i risultati degli strumenti accorciati.
        for (auto& m : turn) {
            if (m.role == ChatFormat::Message::Role::Tool && m.content.size() > kToolResultInHistory) {
                size_t cut = kToolResultInHistory;
                while (cut > 0 && ((unsigned char)m.content[cut] & 0xC0) == 0x80) --cut;
                m.content = m.content.substr(0, cut) + " [...]";
            }
        }
        turn.push_back({ ChatFormat::Message::Role::Assistant, answer });
        m_history.push_back(std::move(turn));
        return Result::Ok;
    }
    return Result::Error;
}
