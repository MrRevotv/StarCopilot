#include "speech_text.h"
#include "text_utils.h"

#include <algorithm>
#include <cctype>

namespace SpeechText {

namespace {
    // Abbreviazioni dopo cui il punto non chiude la frase.
    const char* const kAbbreviations[] = { "es", "ecc", "etc", "sig", "sigg", "dott", "prof", "ing", "avv", "geom", "arch", "n", "nr",
                                           "pag", "vol", "cap", "art", "st", "mr", "mrs", "dr", "vs", "ca", "max", "min", "tel", "rag",
                                           "on", "sen", "gen", "col", "magg", "ten", "cfr", "p", "pp" };

    bool isAbbreviation(const std::string& buffer, size_t dot) {
        size_t start = dot;
        while (start > 0 && std::isalpha((unsigned char)buffer[start - 1])) --start;
        if (start == dot) return false;
        std::string word = TextUtils::toLower(buffer.substr(start, dot - start));
        // Iniziale di un nome: "J. R. R. Tolkien".
        if (word.size() == 1 && std::isupper((unsigned char)buffer[start])) return true;
        return std::any_of(std::begin(kAbbreviations), std::end(kAbbreviations), [&](const char* a) { return word == a; });
    }

    constexpr size_t kLongSentence = 220; // oltre, si spezza alla virgola: la voce parte prima
}

std::vector<std::string> SentenceSplitter::push(const std::string& piece) {
    m_buffer += piece;
    std::vector<std::string> out;
    size_t from = 0;
    for (size_t i = 0; i < m_buffer.size(); ++i) {
        const char c = m_buffer[i];
        size_t cut = std::string::npos;
        if (c == '\n') {
            cut = i + 1;
        } else if ((c == '.' || c == '!' || c == '?') && i + 1 < m_buffer.size()) {
            const char next = m_buffer[i + 1];
            if (next == ' ' || next == '\n' || next == '\t') {
                if (c != '.' || !isAbbreviation(m_buffer, i)) cut = i + 1;
            }
        }
        if (cut == std::string::npos && i - from > kLongSentence && c == ',' && i + 1 < m_buffer.size() && m_buffer[i + 1] == ' ') {
            cut = i + 1;
        }
        if (cut != std::string::npos) {
            std::string sentence = TextUtils::trim(m_buffer.substr(from, cut - from));
            if (!sentence.empty()) out.push_back(sentence);
            from = cut;
        }
    }
    m_buffer.erase(0, from);
    return out;
}

std::string SentenceSplitter::flush() {
    std::string rest = TextUtils::trim(m_buffer);
    m_buffer.clear();
    return rest;
}

std::string speakable(const std::string& input) {
    std::string text = input;

    // [testo](indirizzo) -> testo
    for (size_t open = text.find('['); open != std::string::npos; open = text.find('[', open + 1)) {
        size_t close = text.find("](", open);
        if (close == std::string::npos) break;
        size_t end = text.find(')', close);
        if (end == std::string::npos) break;
        text = text.substr(0, open) + text.substr(open + 1, close - open - 1) + text.substr(end + 1);
    }

    // Indirizzi web: non si leggono.
    for (const char* prefix : { "https://", "http://", "www." }) {
        size_t pos;
        while ((pos = text.find(prefix)) != std::string::npos) {
            size_t end = pos;
            while (end < text.size() && !std::isspace((unsigned char)text[end])) ++end;
            // La punteggiatura in fondo resta ("vedi www.x.it." -> "vedi .")
            while (end > pos && (text[end - 1] == '.' || text[end - 1] == ',' || text[end - 1] == ')')) --end;
            text.erase(pos, end - pos);
        }
    }

    // Riga per riga: titoli, elenchi, citazioni.
    std::string out;
    for (const std::string& rawLine : TextUtils::splitLines(text)) {
        std::string line = TextUtils::trim(rawLine);
        while (!line.empty() && (line[0] == '#' || line[0] == '>')) line = TextUtils::trim(line.substr(1));
        if (TextUtils::startsWith(line, "- ") || TextUtils::startsWith(line, "* ") || TextUtils::startsWith(line, "+ ")) line = line.substr(2);
        if (TextUtils::startsWith(line, "\xE2\x80\xA2")) line = TextUtils::trim(line.substr(3)); // •
        // Elenco numerato: "1. Primo" / "2) Secondo" -> "Primo" / "Secondo".
        size_t digits = 0;
        while (digits < line.size() && digits < 3 && std::isdigit((unsigned char)line[digits])) ++digits;
        if (digits > 0 && digits + 1 < line.size() && (line[digits] == '.' || line[digits] == ')') && line[digits + 1] == ' ') {
            line = TextUtils::trim(line.substr(digits + 2));
        }
        if (line.empty()) continue;
        // Un elenco letto ad alta voce: ogni voce è una frase.
        if (!out.empty()) {
            char last = out.back();
            out += (last == '.' || last == '!' || last == '?' || last == ':' || last == ',' || last == ';') ? " " : ". ";
        }
        out += line;
    }
    text = out;

    // Simboli di formattazione e emoji.
    std::wstring w = TextUtils::utf8ToWide(text);
    std::wstring clean;
    for (size_t i = 0; i < w.size(); ++i) {
        wchar_t c = w[i];
        if (c == L'*' || c == L'_' || c == L'`' || c == L'#' || c == L'~' || c == L'|') continue;
        if (c == 0x200D || c == 0xFE0F) continue;                   // giunzioni delle emoji
        if (c >= 0xD800 && c <= 0xDBFF) { ++i; continue; }          // emoji (fuori dal piano base)
        if (c >= 0x2600 && c <= 0x27BF) continue;                   // simboli e dingbat
        if (c >= 0x2190 && c <= 0x21FF) { clean += L' '; continue; } // frecce
        clean += c;
    }
    text = TextUtils::wideToUtf8(clean);

    auto replaceAll = [&](const std::string& from, const std::string& to) {
        size_t pos = 0;
        while ((pos = text.find(from, pos)) != std::string::npos) {
            text.replace(pos, from.size(), to);
            pos += to.size();
        }
    };
    replaceAll("\xC2\xB0" "C", " gradi");  // °C
    replaceAll("\xC2\xB0", " gradi");      // °
    replaceAll("km/h", " chilometri orari");
    replaceAll("m/s", " metri al secondo");
    replaceAll(" - ", ", ");

    return TextUtils::collapseSpaces(text);
}

} // namespace SpeechText
