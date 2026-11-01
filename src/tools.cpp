#include "tools.h"
#include "text_utils.h"

#include <nlohmann/json.hpp>

namespace {
    using json = nlohmann::json;

    // Valori degli argomenti, tolleranti: il modello a volte scrive i numeri come testo.
    std::string argString(const json& a, const char* key) {
        if (!a.contains(key)) return {};
        const json& v = a[key];
        if (v.is_string()) return TextUtils::trim(v.get<std::string>());
        if (v.is_number()) return v.dump();
        return {};
    }

    std::string def(const char* name, const char* description, json properties, std::vector<std::string> required) {
        json f = { { "type", "function" },
                   { "function", { { "name", name }, { "description", description },
                                   { "parameters", { { "type", "object" }, { "properties", properties }, { "required", required } } } } } };
        return f.dump();
    }
}

std::vector<std::string> Tools::definitions() const {
    std::vector<std::string> d;
    d.push_back(def("remember", "Save a fact about the user to long-term memory, when the user asks you to remember something.",
                    { { "fact", { { "type", "string" }, { "description", "the fact, as a short sentence" } } } }, { "fact" }));
    for (const auto& extra : m_host.extraToolDefinitions()) d.push_back(extra);
    return d;
}

std::string Tools::describe(const std::string& name, const std::string& argumentsJson) {
    json a = json::parse(argumentsJson, nullptr, false);
    if (!a.is_object()) a = json::object();
    if (name == "remember") return "Me lo segno";
    if (name == "game_command") {
        std::string what = argString(a, "command"), where = argString(a, "destination");
        return "Star Citizen: " + what + (where.empty() ? "" : " " + where);
    }
    return "Uso lo strumento " + name;
}

std::string Tools::run(const std::string& name, const std::string& argumentsJson, const std::atomic<bool>* cancel) {
    json a = json::parse(argumentsJson, nullptr, false);
    if (!a.is_object()) a = json::object();
    std::string extra;
    if (m_host.runExtraTool(name, argumentsJson, cancel, extra)) return extra;
    if (name == "remember") {
        std::string fact = argString(a, "fact");
        if (fact.empty()) return "Errore: niente da ricordare.";
        m_host.remember(fact);
        return "Salvato in memoria: " + fact;
    }
    return "Errore: lo strumento \"" + name + "\" non c'è. Iris si occupa solo del gioco.";
}
