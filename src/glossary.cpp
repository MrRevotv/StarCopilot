#include "glossary.h"
#include "app_paths.h"
#include "text_utils.h"

#include <windows.h>
#include <fstream>
#include <sstream>

namespace {
    const char* kTemplate =
        "# Glossario personalizzato Voice to Chat\r\n"
        "# Una riga per termine, formato: termine originale=traduzione desiderata\r\n"
        "# Le righe vuote o che iniziano con # sono ignorate.\r\n"
        "# Utile per il gergo della community che non è italiano standard:\r\n"
        "# whisper lo riconosce meglio e la traduzione lo rende sempre allo stesso modo.\r\n"
        "#\r\n"
        "# Esempi (togli il # iniziale per attivarli):\r\n"
        "# QT=quantum travel\r\n"
        "# atterraggio forzato=crash landing\r\n";

    // Il file può essere UTF-8 (Blocco note di oggi) o ANSI (editor vecchi):
    // se non è UTF-8 valido lo leggiamo come ANSI e lo convertiamo.
    std::string toUtf8(const std::string& raw) {
        if (raw.empty()) return raw;
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, raw.data(), (int)raw.size(), nullptr, 0) > 0) return raw;
        int n = MultiByteToWideChar(CP_ACP, 0, raw.data(), (int)raw.size(), nullptr, 0);
        std::wstring w(n, L'\0');
        MultiByteToWideChar(CP_ACP, 0, raw.data(), (int)raw.size(), w.data(), n);
        return TextUtils::wideToUtf8(w);
    }
}

namespace Glossary {

std::wstring filePath() { return AppPaths::glossaryPath(); }

std::vector<Entry> load() {
    std::vector<Entry> result;
    std::ifstream file(filePath(), std::ios::binary);
    if (!file.is_open()) {
        AppPaths::ensureDirectoriesExist();
        std::ofstream newFile(filePath(), std::ios::binary | std::ios::trunc);
        if (newFile.is_open()) newFile << "\xEF\xBB\xBF" << kTemplate;
        return result;
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();
    if (TextUtils::startsWith(content, "\xEF\xBB\xBF")) content.erase(0, 3);
    content = toUtf8(content);

    for (const std::string& rawLine : TextUtils::splitLines(content)) {
        std::string line = TextUtils::trim(rawLine);
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string term = TextUtils::trim(line.substr(0, eq));
        std::string translation = TextUtils::trim(line.substr(eq + 1));
        if (!term.empty() && !translation.empty()) result.push_back({ term, translation });
    }
    return result;
}

std::vector<Entry> relevantTo(const std::vector<Entry>& entries, const std::string& text) {
    std::vector<Entry> result;
    for (const auto& e : entries) {
        if (TextUtils::containsWord(text, e.first)) result.push_back(e);
    }
    return result;
}

std::string asWhisperHint(const std::vector<Entry>& entries) {
    std::string result;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (i > 0) result += ", ";
        result += entries[i].first;
    }
    return result;
}

} // namespace Glossary
