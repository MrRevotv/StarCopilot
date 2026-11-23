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

    // Alla fine della frase, senza virgola: dopo queste parole "Iris" è di chi si parla, non a chi
    // ("parlavo con Iris", "la mia Iris", "sono Iris", "si chiama Iris").
    const char* const kNotBeforeName[] = { "con", "di", "del", "a", "al", "ad", "da", "dal", "in", "nel", "su", "sul", "per", "tra", "fra",
                                           "il", "la", "lo", "le", "gli", "i", "una", "un", "uno", "mia", "mio", "tua", "tuo", "sua",
                                           "suo", "nostra", "nostro", "vostra", "vostro", "questa", "questo", "quella", "quello", "sono",
                                           "è", "sei", "era", "chiama", "chiamo", "chiamata", "chiamato", "usare", "usando", "provare" };
    // "ho aggiornato Iris", "abbiamo provato Iris": un participio dopo uno di questi.
    const char* const kAuxiliaries[] = { "ho", "hai", "ha", "abbiamo", "avete", "hanno", "avevo", "aveva", "avevamo", "è", "sono", "sei", "siamo",
                                         "era", "ero", "stato", "stata" };

    const char* const kStop[] = { "stop", "basta", "zitta", "zitto", "silenzio", "fermati", "ferma", "smettila", "taci", "annulla",
                                  "lascia stare", "lascia perdere", "niente", "basta così", "ok basta", "basta grazie", "grazie basta",
                                  "stop grazie", "fine", "niente niente", "non importa" };
    // Parole che in una frase corta vogliono dire solo "stai zitta" ("Madonna, stai zitta!", "No, stop.").
    const char* const kStopWords[] = { "zitta", "zitto", "zitti", "taci", "stop", "smettila", "silenzio", "shh", "sst", "shhh" };

    const char* const kHesitations[] = { "ehm", "eh", "ehh", "ehmm", "mm", "mmm", "mmh", "mh", "uhm", "uh", "um", "ah", "ahh", "oh", "ohh",
                                         "boh", "mah", "beh", "bah", "hmm", "hm", "uff", "ahah", "ahahah", "haha", "hahaha", "eheh" };

    struct Token {
        size_t start, end; // posizione nel testo originale (byte)
        std::string norm;  // normalizzato
        bool elided = false; // "dell'Iris", "l'Iris": c'era un articolo attaccato
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
            bool elided = false;
            if (space != std::string::npos) {
                norm = norm.substr(space + 1);
                elided = true;
            }
            if (!norm.empty()) out.push_back({ start, i, norm, elided });
        }
        return out;
    }

    template <size_t N>
    bool inList(const std::string& w, const char* const (&list)[N]) {
        return std::any_of(std::begin(list), std::end(list), [&](const char* f) { return w == f; });
    }

    bool isFiller(const std::string& w) { return inList(w, kFillers); }

    std::vector<std::string> words(const std::string& normalized) {
        std::vector<std::string> out;
        size_t pos = 0;
        while (pos < normalized.size()) {
            size_t sp = normalized.find(' ', pos);
            if (sp == std::string::npos) sp = normalized.size();
            if (sp > pos) out.push_back(normalized.substr(pos, sp - pos));
            pos = sp + 1;
        }
        return out;
    }

    bool endsWith(const std::string& s, const std::string& tail) {
        return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
    }

    bool looksLikeParticiple(const std::string& w) {
        if (w.size() < 5) return false;
        for (const char* t : { "ato", "ata", "ito", "ita", "uto", "uta", "atto", "otto", "esso", "itto" }) {
            if (endsWith(w, t)) return true;
        }
        return false;
    }

    // "Che ore sono, Iris?" sì; "ho aggiornato Iris" no. tokens[at] è il nome in fondo.
    bool addressedAtEnd(const std::string& transcript, const std::vector<Token>& tokens, size_t at) {
        const Token& prev = tokens[at - 1];
        const std::string prevRaw = transcript.substr(prev.start, prev.end - prev.start);
        // Una virgola (o un altro segno) prima del nome: si parla a lei.
        if (!prevRaw.empty() && std::string(",;:!?.").find(prevRaw.back()) != std::string::npos) return true;
        if (endsWith(prevRaw, "\xE2\x80\x94") || endsWith(prevRaw, "\xE2\x80\xA6")) return true; // — e …
        if (inList(prev.norm, kNotBeforeName)) return false;
        if (looksLikeParticiple(prev.norm)) {
            for (size_t k = at >= 4 ? at - 4 : 0; k + 1 < at; ++k) {
                if (inList(tokens[k].norm, kAuxiliaries)) return false;
            }
        }
        return true;
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
    auto wake = [&](size_t i) { return !tokens[i].elided && isWakeToken(tokens[i].norm); };

    // All'inizio, dopo al massimo due parole di saluto.
    for (size_t i = 0; i < tokens.size() && i < 3; ++i) {
        if (wake(i)) {
            // "Iris, Iris, carrello": il nome ripetuto non fa parte della richiesta.
            size_t last = i;
            while (last + 1 < tokens.size() && wake(last + 1)) ++last;
            m.found = true;
            m.command = trimPunct(transcript.substr(tokens[last].end));
            return m;
        }
        if (!isFiller(tokens[i].norm)) break;
    }
    // Alla fine: "che ore sono, Iris?"
    size_t first = tokens.size() - 1;
    if (tokens.size() >= 2 && wake(first)) {
        while (first > 0 && wake(first - 1)) --first; // "carrello, Iris, Iris"
        if (first == 0) {
            m.found = true; // solo il nome, ripetuto
            return m;
        }
        if (!addressedAtEnd(transcript, tokens, first)) return m;
        m.found = true;
        std::string before = transcript.substr(0, tokens[first].start);
        std::string after = transcript.substr(tokens[first].start); // "Iris?": il punto di domanda è attaccato al nome
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
    const std::vector<std::string> w = words(n);
    // "basta così grazie", "stop stop": corto e comincia con una parola di stop.
    if (w.size() <= 3 && (TextUtils::startsWith(n, "basta") || TextUtils::startsWith(n, "stop") || TextUtils::startsWith(n, "zitt"))) return true;
    // "Madonna, stai zitta!", "Zitta, sai zitta, basta, non mi interessa": in una frase non lunga, "zitta" o "stop" bastano.
    if (w.size() <= 10 && std::any_of(w.begin(), w.end(), [](const std::string& x) { return inList(x, kStopWords); })) return true;
    // "Eh, basta!", "va bene basta": "basta" in fondo a una frase corta.
    if (w.size() <= 5 && w.back() == "basta") return true;
    return false;
}

bool isHesitation(const std::string& text) {
    const std::vector<std::string> w = words(normalize(text));
    return !w.empty() && std::all_of(w.begin(), w.end(), [](const std::string& x) { return inList(x, kHesitations); });
}

bool isUnfinished(const std::string& text) {
    const std::string t = TextUtils::trim(text);
    return endsWith(t, "...") || endsWith(t, "\xE2\x80\xA6");
}

size_t wordCount(const std::string& text) {
    return words(normalize(text)).size();
}

} // namespace WakeWord
