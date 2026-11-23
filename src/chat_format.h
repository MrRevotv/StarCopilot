#pragma once

#include <string>
#include <vector>

// Il formato delle conversazioni per i modelli Qwen (ChatML), con gli
// strumenti nel formato ufficiale di Qwen2.5 (<tools>, <tool_call>,
// <tool_response>). Funzioni pure: provate in tests/tests.cpp.
namespace ChatFormat {

    struct Message {
        enum class Role { User, Assistant, Tool };
        Role role;
        std::string content;
    };

    struct ToolCall {
        std::string name;
        std::string argumentsJson; // sempre un oggetto JSON ("{}" se mancano)
    };

    // tools: una definizione JSON per strumento ({"type":"function","function":{...}}).
    // noThink: per i modelli Qwen3, la risposta parte senza "ragionamento" (più veloce).
    std::string buildPrompt(const std::string& system, const std::vector<std::string>& tools, const std::vector<Message>& messages,
                            bool noThink = false);

    // Le richieste di strumenti nella risposta del modello. textBefore: il testo
    // scritto prima della prima richiesta. Accetta anche le forme sbagliate che il
    // modello 7B a volte scrive senza i tag: il JSON nudo {"name":...,"arguments":...}
    // e il nome seguito dagli argomenti ("game_command {"command": ...}",
    // "remember({"fact": ...})", "remember: fact=...", "game_command("press:shields")").
    // knownTools: i nomi degli strumenti che ha, anche con gli argomenti (vedi toolSignature);
    // un nome senza "_" vale solo se è tra questi.
    std::vector<ToolCall> parseToolCalls(const std::string& output, std::string* textBefore = nullptr,
                                         const std::vector<std::string>& knownTools = {});

    // Dove comincia nel testo una richiesta di strumento (in una delle forme sopra);
    // std::string::npos se non c'è. Per non dire a voce "game_command {...}".
    size_t toolCallStart(const std::string& text, const std::vector<std::string>& knownTools = {});
    // Quanti caratteri in fondo potrebbero essere l'inizio di una richiesta ("<tool_c",
    // "game_comm", "remember "): si tengono da parte finché non si sa.
    size_t possibleToolCallTail(const std::string& text, const std::vector<std::string>& knownTools = {});
    // Una richiesta scritta come testo (non coi tag) è finita: l'oggetto JSON dopo il nome è chiuso.
    bool plainToolCallComplete(const std::string& text, const std::vector<std::string>& knownTools = {});

    // La richiesta nella forma giusta (coi tag), per la conversazione: il modello
    // vede sempre quella e non impara le forme sbagliate.
    std::string formatToolCall(const ToolCall& call);
    // Il nome dalla definizione JSON di uno strumento (vuoto se non si capisce).
    std::string toolName(const std::string& definition);
    // Il nome con gli argomenti in ordine, "game_command(command,destination)": va bene come
    // voce di knownTools e fa capire anche game_command("press:shields").
    std::string toolSignature(const std::string& definition);
    // Dove compare nel testo il nome di uno strumento che ha (anche scritto in un modo che non si
    // capisce): da lì in poi non si dice a voce. std::string::npos se non c'è.
    size_t mentionedTool(const std::string& text, const std::vector<std::string>& knownTools);

    // La risposta senza tag rimasti (<tool_call> a metà, <|im_end|>, <think>...).
    std::string cleanAnswer(const std::string& output);

} // namespace ChatFormat
