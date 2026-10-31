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
    // scritto prima della prima richiesta. Accetta anche il JSON nudo
    // {"name":...,"arguments":...} che i modelli piccoli a volte scrivono senza i tag.
    std::vector<ToolCall> parseToolCalls(const std::string& output, std::string* textBefore = nullptr);

    // La risposta senza tag rimasti (<tool_call> a metà, <|im_end|>, <think>...).
    std::string cleanAnswer(const std::string& output);

} // namespace ChatFormat
