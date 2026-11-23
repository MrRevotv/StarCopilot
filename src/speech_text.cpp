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
                // "Allora... vediamo", "ecc. e poi": dopo il punto una minuscola non comincia una
                // frase nuova, e spezzare lì rovina l'intonazione. (Il modello scrive " parola"
                // in un pezzo solo: lo spazio arriva insieme alla lettera, non serve aspettare.)
                if (cut != std::string::npos && c == '.' && next == ' ' && i + 2 < m_buffer.size() &&
                    std::islower((unsigned char)m_buffer[i + 2])) {
                    cut = std::string::npos;
                }
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

namespace {
    const char* const kUnits[] = { "zero", "uno", "due", "tre", "quattro", "cinque", "sei", "sette", "otto", "nove", "dieci", "undici", "dodici",
                                   "tredici", "quattordici", "quindici", "sedici", "diciassette", "diciotto", "diciannove" };
    const char* const kTens[] = { "", "", "venti", "trenta", "quaranta", "cinquanta", "sessanta", "settanta", "ottanta", "novanta" };

    // 1-999 in lettere ("centottantatré"); 0 -> "".
    std::string under1000(int n) {
        std::string out, tail;
        const int h = n / 100, r = n % 100;
        if (h > 0) out = std::string(h == 1 ? "" : kUnits[h]) + "cento";
        if (r == 0) return out;
        if (r < 20) {
            tail = (r == 3 && h > 0) ? "tré" : kUnits[r];
        } else {
            std::string t = kTens[r / 10];
            const int u = r % 10;
            if (u == 1 || u == 8) t.pop_back(); // ventuno, ventotto
            tail = t + (u == 0 ? "" : u == 3 ? "tré" : kUnits[u]);
        }
        if (!out.empty() && tail.rfind("ott", 0) == 0) out.pop_back(); // centotto, centottanta
        return out + tail;
    }

    std::string digitsOneByOne(const std::string& digits) {
        std::string out;
        for (char d : digits) out += (out.empty() ? "" : " ") + std::string(kUnits[d - '0']);
        return out;
    }

    // Sigle e unità che la voce legge male (maiuscole/minuscole esatte), provate con
    // iris_tests --voce-leggi: "QT" diventava un borbottio, "SCU" "scudi", "ATC" "lattici".
    // Quelle che legge bene da sé (UEC, PVP, F7C, L1...) restano com'erano.
    const std::pair<const char*, const char*> kWords[] = {
        { "SCU", "esse ci u" }, { "QT", "quantum" }, { "IA", "i a" }, { "MFD", "emme effe di" },
        { "HUD", "acca u di" }, { "NPC", "enne pi ci" }, { "PNG", "pi enne gi" },
        { "ATC", "a ti ci" }, { "FPS", "effe pi esse" }, { "GPS", "gi pi esse" }, { "CPU", "ci pi u" }, { "GPU", "gi pi u" },
        { "PC", "pi ci" }, { "USB", "u esse bi" }, { "km", "chilometri" }, { "Km", "chilometri" }, { "kg", "chili" },
        { "Gm", "gigametri" }, { "Mm", "megametri" },
    };

    bool isLetter(char c) { return std::isalpha((unsigned char)c) != 0; }
    bool isDigit(char c) { return std::isdigit((unsigned char)c) != 0; }
}

std::string numberToWords(long long n) {
    if (n < 0) return "meno " + numberToWords(-n);
    if (n == 0) return "zero";
    std::vector<std::string> parts;
    const long long billions = n / 1000000000LL, millions = n / 1000000 % 1000, thousands = n / 1000 % 1000, rest = n % 1000;
    if (billions > 0) parts.push_back(billions == 1 ? "un miliardo" : numberToWords(billions) + " miliardi");
    if (millions > 0) parts.push_back(millions == 1 ? "un milione" : under1000((int)millions) + " milioni");
    if (thousands > 0) {
        std::string t = thousands == 1 ? "mille" : under1000((int)thousands) + "mila";
        const size_t accent = t.find("trémila"); // ventitremila, senza accento
        if (accent != std::string::npos) t.replace(accent, std::string("tré").size(), "tre");
        parts.push_back(t);
    }
    if (rest > 0) parts.push_back(under1000((int)rest));
    std::string out;
    for (const auto& p : parts) out += (out.empty() ? "" : " ") + p;
    return out;
}

std::string forVoice(const std::string& input) {
    std::string text = input;
    auto replaceAll = [&](const std::string& from, const std::string& to) {
        size_t pos = 0;
        while ((pos = text.find(from, pos)) != std::string::npos) {
            text.replace(pos, from.size(), to);
            pos += to.size();
        }
    };
    replaceAll("km/h", " chilometri orari");
    replaceAll("m/s", " metri al secondo");
    replaceAll("\xC2\xB0" "C", " gradi"); // °C
    replaceAll("\xC2\xB0", " gradi");     // °
    replaceAll("%", " per cento");
    replaceAll("\xE2\x82\xAC", " euro");  // €
    replaceAll("\xC3\x97", " x ");        // ×

    // 1) Parole: sigle, unità, lettere attaccate ai numeri ("L1", "Area18", "x4", "300i").
    std::string words;
    const size_t n = text.size();
    for (size_t i = 0; i < n;) {
        if (!std::isalnum((unsigned char)text[i])) {
            words += text[i++];
            continue;
        }
        size_t j = i;
        bool letters = false, digits = false;
        while (j < n && std::isalnum((unsigned char)text[j])) {
            letters = letters || isLetter(text[j]);
            digits = digits || isDigit(text[j]);
            ++j;
        }
        const std::string token = text.substr(i, j - i);
        if (!digits) {
            std::string out = token;
            for (const auto& [from, to] : kWords) {
                if (token == from) out = to;
            }
            // "4 x 4", "x 3": per
            size_t next = j;
            while (next < n && text[next] == ' ') ++next;
            if ((token == "x" || token == "X") && next < n && isDigit(text[next])) out = "per";
            words += out;
        } else if (!letters) {
            words += token;
        } else {
            // Lettere e cifre attaccate: i codici ("F7C", "Area18", "L1", "300i") la voce li legge
            // bene da sé (misurato con iris_tests --voce-leggi). Si staccano solo il "per" di
            // "x4"/"4x" e le unità ("10km").
            std::string out;
            bool changed = false;
            for (size_t a = 0; a < token.size();) {
                size_t b = a;
                const bool num = isDigit(token[a]);
                while (b < token.size() && isDigit(token[b]) == num) ++b;
                std::string piece = token.substr(a, b - a);
                if (!num) {
                    if (piece == "x" || piece == "X") {
                        piece = "per";
                        changed = true;
                    }
                    for (const auto& [from, to] : kWords) {
                        if (piece == from) {
                            piece = to;
                            changed = true;
                        }
                    }
                }
                out += (out.empty() ? "" : " ") + piece;
                a = b;
            }
            words += changed ? out : token;
        }
        i = j;
    }
    text = words;

    // 2) Numeri: migliaia ("16.960", "1.200.000", "16,960"), decimali ("2,5", "3.5"), ore ("14:30"), negativi.
    std::string out;
    const size_t m = text.size();
    auto digitAt = [&](size_t p) { return p < m && isDigit(text[p]); };
    for (size_t i = 0; i < m;) {
        if (!isDigit(text[i])) {
            out += text[i++];
            continue;
        }
        size_t j = i;
        while (digitAt(j)) ++j;
        if ((i > 0 && isLetter(text[i - 1])) || (j < m && isLetter(text[j]))) { // un codice ("F7C", "Area18"): com'è
            out += text.substr(i, j - i);
            i = j;
            continue;
        }
        std::string number = text.substr(i, j - i);
        if (number.size() <= 3 && number != "0" && j < m && (text[j] == '.' || text[j] == ',')) {
            const char sep = text[j];
            size_t k = j;
            std::string grouped = number;
            while (k < m && text[k] == sep && digitAt(k + 1) && digitAt(k + 2) && digitAt(k + 3) && !digitAt(k + 4)) {
                grouped += text.substr(k + 1, 3);
                k += 4;
            }
            if (k > j) {
                number = grouped;
                j = k;
            }
        }
        std::string said = number.size() > 12 ? digitsOneByOne(number) : numberToWords(std::stoll(number));
        if (j + 1 < m && (text[j] == ',' || text[j] == '.') && digitAt(j + 1)) {
            size_t k = j + 1;
            while (digitAt(k)) ++k;
            const std::string fraction = text.substr(j + 1, k - j - 1);
            said += " virgola " + (fraction[0] == '0' || fraction.size() > 2 ? digitsOneByOne(fraction) : numberToWords(std::stoi(fraction)));
            j = k;
        } else if (number.size() <= 2 && j + 2 < m && text[j] == ':' && digitAt(j + 1) && digitAt(j + 2) && !digitAt(j + 3)) {
            const int minutes = std::stoi(text.substr(j + 1, 2));
            if (minutes > 0) said += " e " + numberToWords(minutes);
            j += 3;
        }
        if (i > 0 && text[i - 1] == '-' && (i == 1 || text[i - 2] == ' ' || text[i - 2] == '(')) {
            out.pop_back();
            said = "meno " + said;
        }
        if (!out.empty() && isLetter(out.back())) out += ' ';
        out += said;
        if (j < m && isLetter(text[j])) out += ' ';
        i = j;
    }
    return TextUtils::collapseSpaces(out);
}

} // namespace SpeechText
