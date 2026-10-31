#include "wake_word.h"
#include "text_utils.h"

#include <algorithm>
#include <cwctype>

namespace WakeWord {

namespace {
    // Come whisper scrive "Iris" quando non la sente bene.
    const char* const kVariants[] = { "iris", "irys", "iriss", "airis", "hiris", "yris", "iride", "ayris", "eiris" };

    // Parole che si dicono prima del nome: "ehi Iris", "ok Iris", "senti Iris".
    const char* const kFillers[] = { "ehi", "ehy", "hey", "ei", "e", "ed", "ok", "okay", "ciao", "allora", "senti", "ah", "eh", "oh", "o", "buongiorno",
                                     "buonasera", "salve", "dai", "scusa", "hei" };

    const char* const kStop[] = { "stop", "basta", "zitta", "zitto", "silenzio", "fermati", "ferma", "smettila", "taci", "annulla",
                                  "lascia stare", "lascia perdere", "niente", "basta così", "ok basta", "basta grazie", "grazie basta",
                                  "stop grazie", "fine", "niente niente", "non importa" };

    struct Token {
        size_t start, end; // posizione nel testo originale (byte)
        std::string norm;  // normalizzato
    };

    std::vector<Token> tokenize(const std::string& text) {
        std::vector<Token> out;
        size_t i = 0;
        while (i < text.size()) {
            while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\r' || text[i] == '\n')) ++i;
            if (i >= text.size()) break;
            size_t start = i;
            while (i < text.size() && !(text[i] == ' ' || text[i] == '\t' || text[i] == '\r' || text[i] == '\n')) ++i;
            std::string norm = normalize(text.substr(start, i - start));
            // "Iris," -> "iris"; "dell'Iris" -> "dell iris": prende l'ultima parte.
            size_t space = norm.rfind(' ');
            if (space != std::string::npos) norm = norm.substr(space + 1);
            if (!norm.empty()) out.push_back({ start, i, norm });
        }
        return out;
    }

    bool isFiller(const std::string& w) {
        return std::any_of(std::begin(kFillers), std::end(kFillers), [&](const char* f) { return w == f; });
    }

    // Toglie spazi e punteggiatura all'inizio o alla fine del comando.
    std::string trimPunct(const std::string& s) {
        const char* junk = " \t\r\n,.;:!-\xE2\x80\x94\xE2\x80\xA6"; // anche — e …
        std::string out = s;
        size_t a = out.find_first_not_of(junk);
        if (a == std::string::npos) return {};
        out = out.substr(a);
        // In fondo: via virgole e spazi, ma "?" e "." restano (la domanda resta domanda).
        while (!out.empty() && (out.back() == ',' || out.back() == ' ' || out.back() == ';' || out.back() == ':' || out.back() == '-')) out.pop_back();
        return out;
    }
}

std::string normalize(const std::string& text) {
    std::wstring w = TextUtils::utf8ToWide(TextUtils::toLower(text));
    std::wstring out;
    bool space = false;
    for (wchar_t c : w) {
        if (std::iswalnum(c) || (c >= 0xC0 && c != 0xD7 && c != 0xF7 && c < 0x250)) {
            if (space && !out.empty()) out += L' ';
            out += c;
            space = false;
        } else {
            space = true;
        }
    }
    return TextUtils::wideToUtf8(out);
}

bool isWakeToken(const std::string& word) {
    const std::string n = normalize(word);
    if (n.empty()) return false;
    if (std::any_of(std::begin(kVariants), std::end(kVariants), [&](const char* v) { return n == v; })) return true;
    // Una lettera sbagliata in una parola di 4-6 lettere ("irus", "irisi").
    if (n.size() >= 4 && n.size() <= 6 && n[0] == 'i' && TextUtils::similarity(n, "iris") >= 0.74) return true;
    return false;
}

Match detect(const std::string& transcript) {
    Match m;
    const std::vector<Token> tokens = tokenize(transcript);
    if (tokens.empty()) return m;

    // All'inizio, dopo al massimo due parole di saluto.
    for (size_t i = 0; i < tokens.size() && i < 3; ++i) {
        if (isWakeToken(tokens[i].norm)) {
            m.found = true;
            m.command = trimPunct(transcript.substr(tokens[i].end));
            return m;
        }
        if (!isFiller(tokens[i].norm)) break;
    }
    // Alla fine: "che ore sono, Iris?"
    const Token& last = tokens.back();
    if (tokens.size() >= 2 && isWakeToken(last.norm)) {
        m.found = true;
        std::string before = transcript.substr(0, last.start);
        std::string after = transcript.substr(last.start); // "Iris?": il punto di domanda è attaccato al nome
        std::string command = trimPunct(before);
        if (after.find('?') != std::string::npos && (command.empty() || command.back() != '?')) command += "?";
        m.command = command;
        return m;
    }
    return m;
}

bool isStopCommand(const std::string& command) {
    std::string n = normalize(command);
    if (n.empty()) return false;
    if (std::any_of(std::begin(kStop), std::end(kStop), [&](const char* s) { return n == s; })) return true;
    // "basta così grazie", "stop stop": corto e comincia con una parola di stop.
    const size_t words = std::count(n.begin(), n.end(), ' ') + 1;
    if (words <= 3 && (TextUtils::startsWith(n, "basta") || TextUtils::startsWith(n, "stop") || TextUtils::startsWith(n, "zitt"))) return true;
    return false;
}

} // namespace WakeWord
