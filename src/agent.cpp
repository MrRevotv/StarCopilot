#include "agent.h"
#include "log.h"
#include "speech_text.h"
#include "text_utils.h"
#include "wake_word.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>

namespace {
    constexpr int kMaxToolRounds = 4;
    constexpr size_t kMaxExchanges = 12;      // domande (con strumenti e risposta) tenute nella conversazione
    // Oltre il massimo se ne tolgono tante insieme: togliendone una alla volta l'inizio della
    // conversazione cambierebbe a ogni domanda e il modello la rielaborerebbe tutta ogni volta.
    constexpr size_t kExchangesAfterTrim = 7;
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

    // "game_command(command,destination)", "remember(fact)": per riconoscere le richieste scritte come testo.
    std::vector<std::string> namesOf(const std::vector<std::string>& definitions) {
        std::vector<std::string> names;
        for (const auto& d : definitions) {
            std::string n = ChatFormat::toolSignature(d);
            if (!n.empty()) names.push_back(n);
        }
        return names;
    }

    // I risultati lunghi degli strumenti, accorciati senza spezzare una lettera accentata.
    std::string shortened(const std::string& text) {
        if (text.size() <= kToolResultInHistory) return text;
        size_t cut = kToolResultInHistory;
        while (cut > 0 && ((unsigned char)text[cut] & 0xC0) == 0x80) --cut;
        return text.substr(0, cut) + " [...]";
    }

    // "Fatto: scudi attivi.", "Rotta per Seraphim impostata.": la frase dice che un'azione è fatta.
    // Se lo strumento non è stato usato, è falso (il 7B a volte "fa finta", e poi continua così).
    bool looksLikeActionClaim(const std::string& sentence) {
        static const char* const stems[] = { "impostat", "attivat", "disattivat", "accese", "acceso", "accesa", "accesi", "spento", "spenta",
                                             "spenti", "spente", "aperto", "aperta", "aperti", "chiuso", "chiusa", "chiusi", "alzat",
                                             "abbassat", "estratt", "ritratt", "lanciat", "eseguit", "armat", "disarmat", "inserit",
                                             "inviat", "abbandonat", "uscit" };
        const std::string n = WakeWord::normalize(sentence);
        size_t pos = 0;
        bool first = true;
        while (pos < n.size()) {
            size_t sp = n.find(' ', pos);
            if (sp == std::string::npos) sp = n.size();
            const std::string w = n.substr(pos, sp - pos);
            if (first && (w == "fatto" || w == "fatta")) return true;
            for (const char* st : stems) {
                if (TextUtils::startsWith(w, st)) return true;
            }
            first = false;
            pos = sp + 1;
        }
        return false;
    }

    // "Alza gli scudi", "Riprova!", "Spegni le cose...": comincia con un verbo da comando di bordo.
    bool looksLikeRequest(const std::string& text) {
        static const char* const verbs[] = { "alza", "abbassa", "accend", "spegn", "apri", "chiud", "attiv", "disattiv", "impost", "metti", "togli",
                                             "lancia", "arma", "disarma", "richied", "chiama", "chiedi", "vai", "porta", "esci", "prepara",
                                             "avvia", "ferma", "cancella", "annulla", "riprova", "rifai", "ripeti", "inserisci", "sgancia",
                                             "manda", "scansiona", "pinga", "atterra", "decolla", "salta", "entra", "torna", "seleziona",
                                             "cambia", "passa", "fai", "dai", "parti", "rotta" };
        const std::string n = WakeWord::normalize(text);
        size_t pos = 0;
        for (int i = 0; i < 2 && pos < n.size(); ++i) {
            size_t sp = n.find(' ', pos);
            if (sp == std::string::npos) sp = n.size();
            const std::string w = n.substr(pos, sp - pos);
            for (const char* v : verbs) {
                if (TextUtils::startsWith(w, v)) return true;
            }
            pos = sp + 1;
        }
        return false;
    }

    // "scudi su." -> "Scudi su." (le risposte brevi a volte cominciano in minuscolo).
    std::string capitalized(std::string s) {
        if (!s.empty() && s[0] >= 'a' && s[0] <= 'z') s[0] = (char)(s[0] - 'a' + 'A');
        return s;
    }
}

void Agent::setPersona(const Persona& persona) {
    m_persona = persona;
}

void Agent::reset() {
    m_history.clear();
    m_actions.clear();
}

void Agent::trimHistory() {
    if (m_history.size() > kMaxExchanges) m_history.erase(m_history.begin(), m_history.end() - kExchangesAfterTrim);
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
        "- La richiesta arriva da un riconoscimento vocale e può contenere parole sbagliate: capisci l'intenzione più probabile. "
        "Se è solo un pezzo di frase o non si capisce cosa vuole, chiedi in poche parole di ripetere.\n";
    s += "- Ti occupi solo del gioco. Non hai internet e non controlli Windows: non apri programmi o siti, non regoli volume o "
         "musica, non imposti timer, non dai meteo o notizie. Se te lo chiedono, dillo in una frase (senza offrirti di farlo) e "
         "ricorda cosa sai fare.\n"
         "- Non inventare dati che non hai (prezzi aggiornati, patch, eventi): se non sei sicura dillo con sincerità.\n"
         "- Se l'utente ti chiede di ricordare qualcosa usa remember.";
    if (!m_persona.game.empty()) {
        s += "\n- " + m_persona.game + " è aperto e l'utente sta giocando: per comandare la nave (tasti, rotta sulla mappa stellare) "
             "usa game_command, una volta per ogni azione, e poi conferma con due o tre parole al massimo. Usa solo i comandi del suo "
             "elenco: se per quello che chiede non ce n'è uno adatto, dillo invece di usarne un altro. Di' che un'azione è fatta solo "
             "dopo che game_command ha risposto \"Fatto\"; se ti chiede di riprovare, usalo di nuovo.";
    }
    // Il 7B a volte scrive la richiesta come testo ("game_command {...}"): la voce la leggerebbe.
    s += "\n- Gli strumenti si usano solo col formato <tool_call>: mai il loro nome o i loro argomenti nel testo della risposta.";
    if (!m_persona.memory.empty()) {
        s += "\n\nCose che sai dell'utente (te le ha chieste di ricordare):";
        for (const auto& m : m_persona.memory) s += "\n- " + m;
    }
    // Alla fine: è la regola che il modello deve avere più presente.
    if (m_persona.brief) s += "\n\nRicorda: MODALITÀ BREVE. Risposta finale di una frase, al massimo una decina di parole.";
    return s;
}

std::string Agent::buildPrompt(const std::string& system, const std::vector<std::string>& tools,
                               const std::vector<ChatFormat::Message>& extra) const {
    std::vector<ChatFormat::Message> messages;
    for (const auto& exchange : m_history) messages.insert(messages.end(), exchange.begin(), exchange.end());
    messages.insert(messages.end(), extra.begin(), extra.end());
    return ChatFormat::buildPrompt(system, tools, messages, m_llm.isQwen3());
}

bool Agent::warmUp(const std::atomic<bool>* cancel, bool freshConversation) {
    if (!m_llm.isReady()) return false;
    trimHistory();
    const std::vector<std::string> tools = m_tools.definitions();
    const std::string system = systemPrompt();
    std::string prompt;
    if (freshConversation) {
        prompt = ChatFormat::buildPrompt(system, tools, { { ChatFormat::Message::Role::User, "" } }, m_llm.isQwen3());
    } else {
        prompt = buildPrompt(system, tools, { { ChatFormat::Message::Role::User, "" } });
    }
    // Fino a "<|im_start|>user\n": la domanda (con l'ora) arriverà dopo.
    const std::string marker = "<|im_start|>user\n";
    const size_t at = prompt.rfind(marker);
    if (at == std::string::npos) return false;
    prompt.resize(at + marker.size());
    return m_llm.prefill(prompt, cancel);
}

void Agent::noteAction(const std::string& userText, const std::string& result) {
    m_actions.push_back("«" + userText + "»: " + shortened(result));
    while (m_actions.size() > 4) m_actions.erase(m_actions.begin());
}

Agent::Result Agent::ask(const std::string& userText, const Callbacks& cb, const std::atomic<bool>* cancel, std::string& answer) {
    answer.clear();
    m_unfulfilled = false;
    // Le azioni fatte senza il modello da quando gli si è parlato l'ultima volta, prima della domanda.
    std::string actions;
    for (const auto& a : m_actions) actions += (actions.empty() ? "" : "; ") + a;
    m_actions.clear();
    const std::string userMessage =
        "[" + nowHHMM() + "] " + (actions.empty() ? std::string() : "(Comandi eseguiti poco fa senza di te: " + actions + ") ") + userText;
    const std::vector<std::string> tools = m_tools.definitions();
    const std::vector<std::string> toolNames = namesOf(tools);
    const std::string system = systemPrompt();

    trimHistory();
    std::vector<ChatFormat::Message> turn; // i messaggi di questa domanda (strumenti compresi)
    turn.push_back({ ChatFormat::Message::Role::User, userMessage });

    LlmEngine::Sampling sampling;
    std::string spokenSoFar;
    // Col gioco aperto, una richiesta (non una domanda) a cui il modello risponde "fatto" senza
    // usare game_command: quella frase non si dice e si riprova una volta, con un promemoria.
    // Solo per le richieste ("alza gli scudi", "riprova"): "È attiva." non va spinta verso un'azione.
    const std::string trimmedUser = TextUtils::trim(userText);
    const bool checkClaims = std::any_of(toolNames.begin(), toolNames.end(),
                                         [](const std::string& t) { return t == "game_command" || TextUtils::startsWith(t, "game_command("); }) &&
                             !trimmedUser.empty() && trimmedUser.back() != '?' && looksLikeRequest(trimmedUser);
    bool usedTool = false, toolOk = false, retried = false;

    for (int round = 0; round <= kMaxToolRounds; ++round) {
        // Il prompt: se è troppo lungo si dimenticano le domande più vecchie.
        std::string prompt;
        while (true) {
            // All'ultimo giro niente più strumenti: deve rispondere.
            prompt = buildPrompt(system, round < kMaxToolRounds ? tools : std::vector<std::string>{}, turn);
            if (m_history.empty() || m_llm.countTokens(prompt) + sampling.maxTokens < m_llm.contextSize()) break;
            m_history.erase(m_history.begin());
        }

        // La risposta arriva a pezzi: il testo va alla voce frase per frase,
        // le richieste di strumenti no (né coi tag, né scritte come testo).
        enum class Mode { Undecided, Speak, Tool };
        Mode mode = Mode::Undecided;
        SpeechText::SentenceSplitter splitter;
        std::string output;
        size_t fed = 0; // caratteri di "output" già passati al divisore di frasi
        bool falseClaim = false;
        // Anche dopo il richiamo dice "fatto" senza aver usato lo strumento ("Alimentazione, scudi, motori
        // accesi." e la nave restava spenta): quella frase non si dice.
        bool stillClaiming = false;
        auto feed = [&](size_t upTo) {
            if (upTo <= fed || falseClaim || stillClaiming) return;
            for (const auto& sentence : splitter.push(output.substr(fed, upTo - fed))) {
                if (checkClaims && !toolOk && spokenSoFar.empty() && looksLikeActionClaim(sentence)) {
                    if (retried) stillClaiming = true;
                    else falseClaim = true;
                    break;
                }
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
                    // Una parola sola sulla prima riga ("ronics" al posto di <tool_call>, visto nel registro vero):
                    // si guarda cosa segue prima di dirla.
                    const size_t nl = t.find('\n'), space = t.find(' ');
                    const bool oneWordLine = space == std::string::npos || (nl != std::string::npos && space > nl);
                    if (oneWordLine && nl == std::string::npos && t.size() < 24 && std::string(".!?").find(t.back()) == std::string::npos) return true;
                    bool junkThenCall = false;
                    if (oneWordLine && nl != std::string::npos) {
                        const std::string next = TextUtils::trim(t.substr(nl + 1));
                        if (next.empty()) return true;
                        junkThenCall = next[0] == '{' || next[0] == '<' || ChatFormat::toolCallStart(next, toolNames) == 0;
                    }
                    if (junkThenCall || ChatFormat::toolCallStart(t, toolNames) == 0) mode = Mode::Tool;
                    else if (ChatFormat::possibleToolCallTail(t, toolNames) == t.size()) return true; // forse l'inizio di una richiesta
                    else if (t == "`" || t == "``" || TextUtils::startsWith(t, "```")) {
                        if (t.size() < 8) return true; // ```json { ... } ?
                        mode = t.find('{') != std::string::npos ? Mode::Tool : Mode::Speak;
                    } else mode = Mode::Speak;
                }
                if (mode == Mode::Speak) {
                    const size_t start = ChatFormat::toolCallStart(output, toolNames);
                    if (start != std::string::npos) {
                        feed(start);
                        mode = Mode::Tool;
                    } else {
                        const size_t safe = output.size() - ChatFormat::possibleToolCallTail(output, toolNames);
                        feed(safe);
                        if (cb.onText) cb.onText(ChatFormat::cleanAnswer(output.substr(0, safe)));
                    }
                }
                // Una richiesta di strumento completa: inutile continuare a scrivere.
                if (mode == Mode::Tool &&
                    (output.find("</tool_call>") != std::string::npos || ChatFormat::plainToolCallComplete(output, toolNames))) {
                    return false;
                }
                if (falseClaim || stillClaiming) return false;
                // Il modello comincia a inventarsi il turno dopo ("<|im_start|>user..."): basta.
                if (output.find("<|im_start|>") != std::string::npos) return false;
                return !(cancel && cancel->load());
            },
            raw, cancel);

        if (r == LlmEngine::Result::Cancelled || (cancel && cancel->load())) {
            answer = spokenSoFar.empty() ? ChatFormat::cleanAnswer(output.substr(0, std::min(output.size(), fed))) : spokenSoFar;
            return Result::Cancelled;
        }
        if (r != LlmEngine::Result::Ok) return Result::Error;
        if (!falseClaim && !stillClaiming && checkClaims && !toolOk && mode == Mode::Speak && spokenSoFar.empty() &&
            looksLikeActionClaim(ChatFormat::cleanAnswer(output))) {
            // Risposta di una frase sola, senza punto: si controlla anche quella.
            if (retried) stillClaiming = true;
            else falseClaim = true;
        }
        if (stillClaiming) {
            Log::warn("Di nuovo \"fatto\" senza strumento, non lo dico: " + raw.substr(0, 200));
            m_unfulfilled = true;
            answer = usedTool ? "Non è andato: il comando non ha funzionato." : "Non l'ho fatto: non ho trovato il comando giusto.";
            if (cb.onSentence) cb.onSentence(answer);
            if (cb.onText) cb.onText(answer);
            for (auto& m : turn) {
                if (m.role == ChatFormat::Message::Role::Tool) m.content = shortened(m.content);
            }
            turn.push_back({ ChatFormat::Message::Role::Assistant, answer });
            turn[0].content = userMessage;
            m_history.push_back(std::move(turn));
            return Result::Ok;
        }
        if (falseClaim) {
            Log::info("Detto \"fatto\" senza usare lo strumento, riprovo: " + raw.substr(0, 200));
            retried = true;
            turn[0].content = userMessage + (usedTool ? "\n(Nota per te: game_command non ha risposto \"Fatto\": di' cosa non è andato, "
                                                        "senza dire che è fatto.)"
                                                      : "\n(Nota per te: non hai usato game_command. Se è un'azione della nave usalo adesso; "
                                                        "se non c'è un comando adatto dillo, senza dire che è fatto.)");
            --round;
            continue;
        }

        std::string textBefore;
        std::vector<ChatFormat::ToolCall> calls = round < kMaxToolRounds ? ChatFormat::parseToolCalls(output, &textBefore, toolNames)
                                                                         : std::vector<ChatFormat::ToolCall>{};
        if (!calls.empty()) {
            // Nella conversazione la richiesta resta nella forma giusta (coi tag), anche se il
            // modello l'ha scritta come testo: così non impara la forma sbagliata.
            std::string assistantText = ChatFormat::cleanAnswer(textBefore);
            if (assistantText.find(' ') == std::string::npos) assistantText.clear(); // "ronics": non è una frase
            for (const auto& call : calls) assistantText += (assistantText.empty() ? "" : "\n") + ChatFormat::formatToolCall(call);
            if (mode != Mode::Tool || output.find("<tool_call>") == std::string::npos) Log::info("Richiesta di strumento scritta come testo: " + raw.substr(0, 200));
            turn.push_back({ ChatFormat::Message::Role::Assistant, assistantText });
            usedTool = true;
            // La frase detta prima della richiesta, anche senza punto in fondo ("Imposto la rotta
            // <tool_call>..."), va alla voce prima di eseguire lo strumento. Una parola sola ("ronics") no.
            const std::string pending = TextUtils::trim(splitter.flush());
            if (pending.find(' ') != std::string::npos && cb.onSentence) {
                cb.onSentence(pending);
                spokenSoFar += (spokenSoFar.empty() ? "" : " ") + pending;
            }
            for (const auto& call : calls) {
                const std::string what = Tools::describe(call.name, call.argumentsJson);
                Log::info("Strumento: " + call.name + " " + call.argumentsJson);
                if (cb.onTool) cb.onTool(what);
                std::string result = m_tools.run(call.name, call.argumentsJson, cancel);
                if (cancel && cancel->load()) return Result::Cancelled;
                Log::info("Risultato di " + call.name + ": " + std::to_string(result.size()) + " caratteri");
                if (!TextUtils::startsWith(result, "Errore")) toolOk = true;
                turn.push_back({ ChatFormat::Message::Role::Tool, result });
            }
            continue;
        }

        // Risposta finale.
        if (mode != Mode::Speak) {
            // Sembrava una richiesta di strumento ma non si capisce (JSON rotto).
            std::string cleaned = ChatFormat::cleanAnswer(output);
            if (fed > 0) output = output.substr(0, fed); // il testo prima del tag è già stato detto: basta così
            else if (ChatFormat::toolCallStart(cleaned, toolNames) != std::string::npos || TextUtils::startsWith(cleaned, "{") || cleaned.empty()) {
                output = "Scusa, non sono riuscita a completare la richiesta.";
            } else output = cleaned;
            if (mode == Mode::Tool) Log::warn("Risposta non capita come strumento: " + raw.substr(0, 300));
        }
        // Il nome di uno strumento scritto in un modo che non si capisce: da lì in poi non si dice
        // e non resta nella conversazione (il modello lo imiterebbe alla domanda dopo).
        const size_t mention = ChatFormat::mentionedTool(output, toolNames);
        if (mention != std::string::npos && mention >= fed) {
            Log::warn("Nome di uno strumento nella risposta, tolto: " + raw.substr(0, 200));
            output = TextUtils::trim(output.substr(0, mention));
            if (output.empty() && spokenSoFar.empty()) output = "Scusa, non sono riuscita a completare la richiesta.";
        }
        feed(output.size());
        const std::string rest = splitter.flush();
        if (!rest.empty() && cb.onSentence) cb.onSentence(rest);
        answer = capitalized(ChatFormat::cleanAnswer(output));
        if (cb.onText) cb.onText(answer);
        if (answer.empty()) answer = "...";
        // Nella conversazione resta tutto, ma i risultati degli strumenti accorciati.
        for (auto& m : turn) {
            if (m.role == ChatFormat::Message::Role::Tool) m.content = shortened(m.content);
        }
        turn.push_back({ ChatFormat::Message::Role::Assistant, answer });
        turn[0].content = userMessage; // il promemoria non resta nella conversazione
        m_history.push_back(std::move(turn));
        return Result::Ok;
    }
    return Result::Error;
}
