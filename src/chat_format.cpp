#include "chat_format.h"
#include "text_utils.h"

#include <nlohmann/json.hpp>

#include <algorithm>

namespace ChatFormat {

namespace {
    using json = nlohmann::json;
    const std::string kOpen = "<tool_call>", kClose = "</tool_call>";

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
        } else {
            // {"name": "game_command", "command": "press:shields"}: gli argomenti accanto al nome.
            for (auto it = j.begin(); it != j.end(); ++it) {
                if (it.key() != "name") args[it.key()] = it.value();
            }
        }
        call.argumentsJson = args.dump();
        return !call.name.empty();
    }

    // L'oggetto JSON che comincia in s[start] ("{"), fino alla sua "}" corrispondente;
    // end: subito dopo. Se il testo finisce prima (risposta troncata) lo si chiude.
    std::string jsonObjectAt(const std::string& s, size_t start, size_t& end) {
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
            else if (c == '}' && --depth == 0) {
                end = i + 1;
                return s.substr(start, end - start);
            }
        }
        end = s.size();
        if (depth <= 0) return {};
        return s.substr(start) + (inString ? "\"" : "") + std::string((size_t)depth, '}');
    }

    // Il primo oggetto JSON nel testo (dalla prima "{" alla sua "}" corrispondente).
    std::string firstJsonObject(const std::string& s) {
        size_t start = s.find('{');
        if (start == std::string::npos) return {};
        size_t end = 0;
        return jsonObjectAt(s, start, end);
    }

    // JSON scritto un po' alla buona: anche con gli apici singoli al posto delle virgolette.
    json parseLoose(const std::string& s) {
        json j = json::parse(s, nullptr, false);
        if (!j.is_discarded() || s.find('"') != std::string::npos || s.find('\'') == std::string::npos) return j;
        std::string t = s;
        std::replace(t.begin(), t.end(), '\'', '"');
        return json::parse(t, nullptr, false);
    }

    bool identChar(char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.';
    }

    // "functions.game_command" -> "game_command".
    std::string bareName(const std::string& ident) {
        const size_t dot = ident.rfind('.');
        return dot == std::string::npos ? ident : ident.substr(dot + 1);
    }

    // knownTools: "remember", o "game_command(command,destination)" coi nomi degli argomenti in ordine.
    std::string specName(const std::string& spec) {
        const size_t p = spec.find('(');
        return p == std::string::npos ? spec : spec.substr(0, p);
    }

    bool isKnown(const std::string& name, const std::vector<std::string>& known) {
        return std::any_of(known.begin(), known.end(), [&](const std::string& k) { return specName(k) == name; });
    }

    std::vector<std::string> paramsOf(const std::string& name, const std::vector<std::string>& known) {
        std::vector<std::string> out;
        for (const auto& k : known) {
            const size_t p = k.find('(');
            if (p == std::string::npos || k.substr(0, p) != name) continue;
            std::string list = k.substr(p + 1);
            if (!list.empty() && list.back() == ')') list.pop_back();
            size_t pos = 0;
            while (pos <= list.size()) {
                size_t comma = list.find(',', pos);
                if (comma == std::string::npos) comma = list.size();
                if (comma > pos) out.push_back(list.substr(pos, comma - pos));
                pos = comma + 1;
            }
        }
        return out;
    }

    // Un nome che può essere uno strumento: uno di quelli che ha, o comunque
    // scritto_così (nessuna parola italiana ha "_").
    bool toolLikeName(const std::string& name, const std::vector<std::string>& known) {
        if (name.empty() || !(name[0] >= 'a' && name[0] <= 'z')) return false;
        if (isKnown(name, known)) return true;
        return name.find('_') != std::string::npos && name.back() != '_';
    }

    // In text[pos] comincia "nome {"..." (o "nome: {", "nome({")? argsAt: dov'è la "{".
    // Per gli strumenti che ha, anche "remember: fact="..."" e "game_command(command="...")":
    // argsAt è allora il primo nome di argomento.
    bool plainCallAt(const std::string& text, size_t pos, const std::vector<std::string>& known, std::string& name, size_t& argsAt) {
        if (pos > 0 && identChar(text[pos - 1])) return false;
        size_t i = pos;
        while (i < text.size() && identChar(text[i])) ++i;
        if (i == pos) return false;
        name = bareName(text.substr(pos, i - pos));
        // "Game_command" in cima alla risposta: maiuscola o no, è lo stesso strumento.
        std::string lower = name;
        for (char& c : lower) c = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
        if (isKnown(lower, known)) name = lower;
        if (!toolLikeName(name, known)) return false;
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
        const char openChar = i < text.size() && (text[i] == ':' || text[i] == '(') ? text[i] : 0;
        const bool opened = openChar != 0;
        if (opened) ++i;
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\n')) ++i;
        if (i < text.size() && text[i] == '{') {
            size_t k = i + 1;
            while (k < text.size() && (text[k] == ' ' || text[k] == '\n')) ++k;
            if (k < text.size() && text[k] != '"' && text[k] != '\'' && text[k] != '}') return false;
            argsAt = i;
            return true;
        }
        // nome: chiave="valore" / nome(chiave="valore") / nome("valore", ...): solo per gli strumenti che ha.
        if (!opened || !isKnown(name, known)) return false;
        if (i < text.size() && (text[i] == '"' || text[i] == '\'') && !paramsOf(name, known).empty()) {
            argsAt = i;
            return true;
        }
        size_t k = i;
        while (k < text.size() && identChar(text[k])) ++k;
        if (k == i) return false;
        while (k < text.size() && text[k] == ' ') ++k;
        // "game_command: press:shields": i valori nudi, nell'ordine degli argomenti.
        if ((k >= text.size() || text[k] != '=') && !(openChar == ':' && !paramsOf(name, known).empty())) return false;
        argsAt = i;
        return true;
    }

    // In text[i] cominciano argomenti chiave=valore?
    bool kwargsStart(const std::string& text, size_t i) {
        size_t k = i;
        while (k < text.size() && identChar(text[k])) ++k;
        if (k == i) return false;
        while (k < text.size() && text[k] == ' ') ++k;
        return k < text.size() && text[k] == '=';
    }

    // Gli argomenti scritti come chiave="valore", chiave=valore, separati da virgole, fino a ")" o a capo.
    json kwargsAt(const std::string& text, size_t pos, size_t& end) {
        json args = json::object();
        size_t i = pos;
        while (i < text.size()) {
            const size_t keyStart = i;
            while (i < text.size() && identChar(text[i])) ++i;
            if (i == keyStart) break;
            const std::string key = text.substr(keyStart, i - keyStart);
            while (i < text.size() && text[i] == ' ') ++i;
            if (i >= text.size() || (text[i] != '=' && text[i] != ':')) break;
            ++i;
            while (i < text.size() && text[i] == ' ') ++i;
            std::string value;
            if (i < text.size() && (text[i] == '"' || text[i] == '\'')) {
                const char quote = text[i++];
                const size_t close = text.find(quote, i);
                value = text.substr(i, close == std::string::npos ? std::string::npos : close - i);
                i = close == std::string::npos ? text.size() : close + 1;
            } else {
                const size_t stop = text.find_first_of(",)\n", i);
                value = TextUtils::trim(text.substr(i, stop == std::string::npos ? std::string::npos : stop - i));
                i = stop == std::string::npos ? text.size() : stop;
            }
            args[key] = value;
            while (i < text.size() && (text[i] == ' ' || text[i] == ',')) ++i;
            if (i < text.size() && (text[i] == ')' || text[i] == '\n')) {
                if (text[i] == ')') ++i;
                break;
            }
        }
        end = i;
        return args;
    }

    // Lo strumento che ha come primo argomento "key" ({"command": ...} -> game_command); vuoto se nessuno.
    std::string toolByFirstParam(const std::string& key, const std::vector<std::string>& known) {
        for (const auto& k : known) {
            const std::vector<std::string> params = paramsOf(specName(k), known);
            if (!params.empty() && params[0] == key) return specName(k);
        }
        return {};
    }

    // In text[pos] comincia un JSON nudo che sembra una richiesta: {"name" ..., o solo gli
    // argomenti di uno strumento che ha ({"command": "press:shields"}).
    bool bareJsonAt(const std::string& text, size_t pos, const std::vector<std::string>& known) {
        if (text[pos] != '{') return false;
        size_t k = pos + 1;
        while (k < text.size() && (text[k] == ' ' || text[k] == '\n' || text[k] == '\t')) ++k;
        if (text.compare(k, 6, "\"name\"") == 0 || text.compare(k, 6, "'name'") == 0) return true;
        if (k >= text.size() || (text[k] != '"' && text[k] != '\'')) return false;
        const size_t close = text.find(text[k], k + 1);
        return close != std::string::npos && !toolByFirstParam(text.substr(k + 1, close - k - 1), known).empty();
    }

    // La richiesta che comincia in text[pos] (vedi toolCallStart); end: subito dopo.
    bool callAt(const std::string& text, size_t pos, const std::vector<std::string>& known, ToolCall& call, size_t& end) {
        std::string name;
        size_t brace = pos;
        const bool plain = text[pos] != '{';
        if (plain && !plainCallAt(text, pos, known, name, brace)) return false;
        if (plain && (text[brace] == '"' || text[brace] == '\'')) {
            // game_command("press:shields", "..."): i valori nell'ordine degli argomenti.
            const std::vector<std::string> params = paramsOf(name, known);
            json args = json::object();
            size_t i = brace, n = 0;
            while (i < text.size() && (text[i] == '"' || text[i] == '\'') && n < params.size()) {
                const char quote = text[i++];
                const size_t close = text.find(quote, i);
                if (close == std::string::npos) break;
                args[params[n++]] = text.substr(i, close - i);
                i = close + 1;
                while (i < text.size() && (text[i] == ' ' || text[i] == ',')) ++i;
            }
            if (i < text.size() && text[i] == ')') ++i;
            end = i;
            if (args.empty()) return false;
            call.name = name;
            call.argumentsJson = args.dump();
            return true;
        }
        if (plain && text[brace] != '{' && !kwargsStart(text, brace)) {
            // game_command: press:shields / game_command: set_route Port Tressler (fino a fine riga).
            const std::vector<std::string> params = paramsOf(name, known);
            size_t lineEnd = text.find('\n', brace);
            if (lineEnd == std::string::npos) lineEnd = text.size();
            const std::string line = TextUtils::trim(text.substr(brace, lineEnd - brace));
            const size_t space = params.size() > 1 ? line.find(' ') : std::string::npos; // un argomento solo: tutta la riga
            json args = json::object();
            args[params[0]] = space == std::string::npos ? line : line.substr(0, space);
            if (space != std::string::npos) args[params[1]] = TextUtils::trim(line.substr(space + 1));
            end = lineEnd;
            call.name = name;
            call.argumentsJson = args.dump();
            return true;
        }
        if (plain && text[brace] != '{') {
            const json args = kwargsAt(text, brace, end);
            if (args.empty()) return false;
            call.name = name;
            call.argumentsJson = args.dump();
            return true;
        }
        const json j = parseLoose(jsonObjectAt(text, brace, end));
        if (!j.is_object()) return false;
        if (end < text.size() && text[end] == ')') ++end;
        if (!plain) {
            if (toToolCall(j, call)) return true;
            // Solo gli argomenti: lo strumento si riconosce dal primo.
            if (j.empty()) return false;
            call.name = toolByFirstParam(j.begin().key(), known);
            call.argumentsJson = j.dump();
            return !call.name.empty();
        }
        // "game_command {"name": "game_command", "arguments": {...}}": l'oggetto intero.
        if (j.contains("name") && j["name"].is_string() && bareName(j["name"].get<std::string>()) == name &&
            (j.contains("arguments") || j.contains("parameters"))) {
            if (!toToolCall(j, call)) return false;
            call.name = name;
            return true;
        }
        call.name = name;
        call.argumentsJson = j.dump();
        return true;
    }

    // Il testo prima di una richiesta, senza un "```json" rimasto in fondo.
    std::string withoutFence(const std::string& s) {
        std::string t = s;
        const size_t fence = t.rfind("```");
        if (fence != std::string::npos && TextUtils::trim(t.substr(fence + 3)).find(' ') == std::string::npos) t = t.substr(0, fence);
        return t;
    }

    // Richieste scritte senza tag, da "from" in poi.
    std::vector<ToolCall> plainCalls(const std::string& text, size_t from, const std::vector<std::string>& known) {
        std::vector<ToolCall> calls;
        size_t pos = from;
        while (pos < text.size()) {
            const size_t rel = toolCallStart(text.substr(pos), known);
            if (rel == std::string::npos) break;
            ToolCall call;
            size_t end = 0;
            if (!callAt(text, pos + rel, known, call, end) || end <= pos + rel) break;
            calls.push_back(call);
            pos = end;
        }
        return calls;
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

std::vector<ToolCall> parseToolCalls(const std::string& output, std::string* textBefore, const std::vector<std::string>& knownTools) {
    std::vector<ToolCall> calls;
    size_t first = output.find(kOpen);
    if (first != std::string::npos) {
        if (textBefore) *textBefore = output.substr(0, first);
        size_t pos = first;
        while (pos != std::string::npos) {
            size_t start = pos + kOpen.size();
            size_t end = output.find(kClose, start);
            std::string body = output.substr(start, end == std::string::npos ? std::string::npos : end - start);
            json j = parseLoose(firstJsonObject(body));
            ToolCall call;
            if (toToolCall(j, call)) calls.push_back(call);
            else {
                // Dentro i tag, ma scritta come "game_command {...}".
                for (const auto& c : plainCalls(body, 0, knownTools)) calls.push_back(c);
            }
            if (end == std::string::npos) break;
            pos = output.find(kOpen, end + kClose.size());
        }
        return calls;
    }

    // Senza tag: JSON nudo (anche dentro ```json ... ```) o "nome {argomenti}".
    const size_t start = toolCallStart(output, knownTools);
    if (start != std::string::npos) calls = plainCalls(output, start, knownTools);
    if (textBefore) *textBefore = calls.empty() ? output : withoutFence(output.substr(0, start));
    return calls;
}

size_t toolCallStart(const std::string& text, const std::vector<std::string>& knownTools) {
    size_t best = text.find(kOpen);
    for (size_t i = 0; i < text.size() && i < best; ++i) {
        if (text[i] == '{') {
            if (bareJsonAt(text, i, knownTools)) return i;
            continue;
        }
        if (!identChar(text[i]) || (i > 0 && identChar(text[i - 1]))) continue;
        std::string name;
        size_t brace = 0;
        if (plainCallAt(text, i, knownTools, name, brace)) return i;
    }
    return best;
}

size_t possibleToolCallTail(const std::string& original, const std::vector<std::string>& knownTools) {
    if (original.empty()) return 0;
    std::string text = original; // anche "Game_command" in cima alla frase
    for (char& c : text) c = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    size_t keep = 0;
    // "<tool_c": forse l'inizio del tag.
    for (size_t k = std::min(text.size(), kOpen.size() - 1); k > 0; --k) {
        if (text.compare(text.size() - k, k, kOpen, 0, k) == 0) {
            keep = k;
            break;
        }
    }
    // Una "{" ancora aperta in fondo: forse un JSON che comincia.
    const size_t brace = text.rfind('{');
    if (brace != std::string::npos && text.find('}', brace) == std::string::npos && text.size() - brace <= 12) {
        keep = std::max(keep, text.size() - brace);
    }
    // Il nome di uno strumento che ha, e dopo niente di finito ("remember: fact"): si aspetta.
    for (const auto& spec : knownTools) {
        const std::string k = specName(spec);
        size_t at = text.rfind(k);
        while (at != std::string::npos && ((at > 0 && identChar(text[at - 1])) || (at + k.size() < text.size() && identChar(text[at + k.size()])))) {
            at = at == 0 ? std::string::npos : text.rfind(k, at - 1);
        }
        if (at != std::string::npos && text.find_first_of(".!?\n", at + k.size()) == std::string::npos) keep = std::max(keep, text.size() - at);
    }
    // L'ultima parola (con gli spazi, i ":" o la "(" dopo): forse il nome di uno strumento.
    // Col punto in fondo no: è la fine di una frase.
    if (text.back() == '.') return keep;
    size_t end = text.size();
    while (end > 0 && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == ':' || text[end - 1] == '(' || text[end - 1] == '\n')) --end;
    size_t begin = end;
    while (begin > 0 && identChar(text[begin - 1])) --begin;
    if (begin < end) {
        const std::string name = bareName(text.substr(begin, end - begin));
        const bool complete = end < text.size(); // dopo la parola c'è già uno spazio: è intera
        bool maybe = toolLikeName(name, knownTools);
        if (!maybe && !complete) {
            for (const auto& spec : knownTools) {
                const std::string k = specName(spec);
                maybe = maybe || (k.size() > name.size() && k.compare(0, name.size(), name) == 0);
            }
        }
        if (maybe) keep = std::max(keep, text.size() - begin);
    }
    return keep;
}

bool plainToolCallComplete(const std::string& text, const std::vector<std::string>& knownTools) {
    if (text.find(kOpen) != std::string::npos) return false;
    const size_t start = toolCallStart(text, knownTools);
    if (start == std::string::npos) return false;
    std::string name;
    size_t argsAt = 0;
    if (text[start] != '{' && plainCallAt(text, start, knownTools, name, argsAt) && (text[argsAt] == '"' || text[argsAt] == '\'')) {
        return text.find(')', argsAt) != std::string::npos; // game_command("..."): finita alla ")"
    }
    if (text[start] != '{' && plainCallAt(text, start, knownTools, name, argsAt) && text[argsAt] != '{' && !kwargsStart(text, argsAt)) {
        return text.find('\n', argsAt) != std::string::npos; // game_command: press:shields, finita a fine riga
    }
    if (text[start] != '{' && plainCallAt(text, start, knownTools, name, argsAt) && text[argsAt] != '{') {
        // remember(fact="..."): finita alla ")" (o a capo).
        size_t end = 0;
        kwargsAt(text, argsAt, end);
        return end > argsAt && end <= text.size() && (text[end - 1] == ')' || (end < text.size() && text[end] == '\n'));
    }
    const size_t brace = text.find('{', start);
    if (brace == std::string::npos) return false;
    int depth = 0;
    bool inString = false, escape = false;
    for (size_t i = brace; i < text.size(); ++i) {
        const char c = text[i];
        if (inString) {
            if (escape) escape = false;
            else if (c == '\\') escape = true;
            else if (c == '"') inString = false;
            continue;
        }
        if (c == '"') inString = true;
        else if (c == '{') depth++;
        else if (c == '}' && --depth == 0) return true;
    }
    return false;
}

std::string formatToolCall(const ToolCall& call) {
    // Come la scrive il modello stesso: prima il nome, poi gli argomenti (nell'ordine in cui li ha dati).
    nlohmann::ordered_json args = nlohmann::ordered_json::parse(call.argumentsJson, nullptr, false);
    if (!args.is_object()) args = nlohmann::ordered_json::object();
    try {
        return kOpen + "\n{\"name\": " + json(call.name).dump() + ", \"arguments\": " + args.dump() + "}\n" + kClose;
    } catch (const std::exception&) {
        return kOpen + "\n{\"name\": \"tool\", \"arguments\": {}}\n" + kClose;
    }
}

std::string toolSignature(const std::string& definition) {
    const nlohmann::ordered_json d = nlohmann::ordered_json::parse(definition, nullptr, false);
    const std::string name = toolName(definition);
    if (name.empty()) return {};
    std::string params;
    if (d.is_object() && d.contains("function") && d["function"].contains("parameters") && d["function"]["parameters"].contains("properties") &&
        d["function"]["parameters"]["properties"].is_object()) {
        for (auto it = d["function"]["parameters"]["properties"].begin(); it != d["function"]["parameters"]["properties"].end(); ++it) {
            params += (params.empty() ? "" : ",") + it.key();
        }
    }
    return params.empty() ? name : name + "(" + params + ")";
}

size_t mentionedTool(const std::string& original, const std::vector<std::string>& knownTools) {
    std::string text = original; // anche "Game_command" in cima alla frase
    for (char& c : text) c = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    size_t best = std::string::npos;
    for (const auto& spec : knownTools) {
        const std::string k = specName(spec);
        for (size_t at = text.find(k); at != std::string::npos; at = text.find(k, at + 1)) {
            const bool startOk = at == 0 || !identChar(text[at - 1]);
            const bool endOk = at + k.size() >= text.size() || !identChar(text[at + k.size()]) || text[at + k.size()] == '.';
            if (startOk && endOk) {
                best = std::min(best, at);
                break;
            }
        }
    }
    return best;
}

std::string toolName(const std::string& definition) {
    const json d = json::parse(definition, nullptr, false);
    if (d.is_object() && d.contains("function") && d["function"].is_object() && d["function"].contains("name") && d["function"]["name"].is_string()) {
        return d["function"]["name"].get<std::string>();
    }
    return {};
}

std::string cleanAnswer(const std::string& output) {
    std::string s = output;
    // Un eventuale ragionamento <think>...</think> non fa parte della risposta.
    size_t thinkEnd = s.find("</think>");
    if (thinkEnd != std::string::npos) s = s.substr(thinkEnd + 8);
    size_t tool = s.find(kOpen);
    if (tool != std::string::npos) s = s.substr(0, tool);
    for (const char* tag : { "<|im_end|>", "<|endoftext|>", "<|im_start|>", "<think>", "</tool_call>" }) {
        size_t p;
        while ((p = s.find(tag)) != std::string::npos) s.erase(p, std::string(tag).size());
    }
    return TextUtils::trim(s);
}

} // namespace ChatFormat
