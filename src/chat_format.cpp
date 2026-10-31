#include "chat_format.h"
#include "text_utils.h"

#include <nlohmann/json.hpp>

namespace ChatFormat {

namespace {
    using json = nlohmann::json;

    bool toToolCall(const json& j, ToolCall& call) {
        if (!j.is_object() || !j.contains("name") || !j["name"].is_string()) return false;
        call.name = j["name"].get<std::string>();
        json args = json::object();
        if (j.contains("arguments")) {
            const json& a = j["arguments"];
            if (a.is_object()) args = a;
            else if (a.is_string()) {
                // Alcuni modelli mettono gli argomenti come testo JSON.
                json parsed = json::parse(a.get<std::string>(), nullptr, false);
                if (parsed.is_object()) args = parsed;
            }
        } else if (j.contains("parameters") && j["parameters"].is_object()) {
            args = j["parameters"];
        }
        call.argumentsJson = args.dump();
        return !call.name.empty();
    }

    // Il primo oggetto JSON nel testo (dalla prima "{" alla sua "}" corrispondente).
    std::string firstJsonObject(const std::string& s) {
        size_t start = s.find('{');
        if (start == std::string::npos) return {};
        int depth = 0;
        bool inString = false, escape = false;
        for (size_t i = start; i < s.size(); ++i) {
            char c = s[i];
            if (inString) {
                if (escape) escape = false;
                else if (c == '\\') escape = true;
                else if (c == '"') inString = false;
                continue;
            }
            if (c == '"') inString = true;
            else if (c == '{') depth++;
            else if (c == '}' && --depth == 0) return s.substr(start, i - start + 1);
        }
        return {};
    }
}

std::string buildPrompt(const std::string& system, const std::vector<std::string>& tools, const std::vector<Message>& messages, bool noThink) {
    std::string p = "<|im_start|>system\n" + system;
    if (!tools.empty()) {
        p += "\n\n# Tools\n\nYou may call one or more functions to assist with the user query.\n\n"
             "You are provided with function signatures within <tools></tools> XML tags:\n<tools>";
        for (const auto& t : tools) p += "\n" + t;
        p += "\n</tools>\n\nFor each function call, return a json object with function name and arguments within <tool_call></tool_call> "
             "XML tags:\n<tool_call>\n{\"name\": <function-name>, \"arguments\": <args-json-object>}\n</tool_call>";
    }
    p += "<|im_end|>\n";

    for (size_t i = 0; i < messages.size(); ++i) {
        const Message& m = messages[i];
        if (m.role == Message::Role::Tool) {
            // Le risposte degli strumenti di fila vanno in un solo messaggio "user".
            const bool first = i == 0 || messages[i - 1].role != Message::Role::Tool;
            const bool last = i + 1 == messages.size() || messages[i + 1].role != Message::Role::Tool;
            if (first) p += "<|im_start|>user";
            p += "\n<tool_response>\n" + m.content + "\n</tool_response>";
            if (last) p += "<|im_end|>\n";
            continue;
        }
        p += m.role == Message::Role::User ? "<|im_start|>user\n" : "<|im_start|>assistant\n";
        p += m.content + "<|im_end|>\n";
    }
    p += "<|im_start|>assistant\n";
    if (noThink) p += "<think>\n\n</think>\n\n";
    return p;
}

std::vector<ToolCall> parseToolCalls(const std::string& output, std::string* textBefore) {
    std::vector<ToolCall> calls;
    const std::string open = "<tool_call>", close = "</tool_call>";
    size_t first = output.find(open);
    if (textBefore) *textBefore = first == std::string::npos ? output : output.substr(0, first);

    size_t pos = first;
    while (pos != std::string::npos) {
        size_t start = pos + open.size();
        size_t end = output.find(close, start);
        std::string body = output.substr(start, end == std::string::npos ? std::string::npos : end - start);
        json j = json::parse(firstJsonObject(body), nullptr, false);
        ToolCall call;
        if (toToolCall(j, call)) calls.push_back(call);
        if (end == std::string::npos) break;
        pos = output.find(open, end + close.size());
    }
    if (!calls.empty() || first != std::string::npos) return calls;

    // Senza tag: JSON nudo (anche dentro ```json ... ```) che sembra una chiamata.
    std::string trimmed = TextUtils::trim(output);
    if (TextUtils::startsWith(trimmed, "```")) {
        size_t nl = trimmed.find('\n');
        trimmed = nl == std::string::npos ? "" : trimmed.substr(nl + 1);
    }
    if (TextUtils::startsWith(trimmed, "{") && trimmed.find("\"name\"") != std::string::npos &&
        (trimmed.find("\"arguments\"") != std::string::npos || trimmed.find("\"parameters\"") != std::string::npos)) {
        json j = json::parse(firstJsonObject(trimmed), nullptr, false);
        ToolCall call;
        if (toToolCall(j, call)) {
            calls.push_back(call);
            if (textBefore) textBefore->clear();
        }
    }
    return calls;
}

std::string cleanAnswer(const std::string& output) {
    std::string s = output;
    // Un eventuale ragionamento <think>...</think> non fa parte della risposta.
    size_t thinkEnd = s.find("</think>");
    if (thinkEnd != std::string::npos) s = s.substr(thinkEnd + 8);
    size_t tool = s.find("<tool_call>");
    if (tool != std::string::npos) s = s.substr(0, tool);
    for (const char* tag : { "<|im_end|>", "<|endoftext|>", "<|im_start|>", "<think>", "</tool_call>" }) {
        size_t p;
        while ((p = s.find(tag)) != std::string::npos) s.erase(p, std::string(tag).size());
    }
    return TextUtils::trim(s);
}

} // namespace ChatFormat
