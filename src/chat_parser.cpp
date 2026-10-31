#include "chat_parser.h"
#include "text_utils.h"

#include <windows.h>

#include <cctype>
#include <cstring>
#include <cwchar>

namespace {
    // Parentesi come le legge l'OCR quando il testo è piccolo: [ ( { < |
    bool isOpener(char c) { return c == '[' || c == '(' || c == '{' || c == '<' || c == '|'; }
    bool isCloser(char c) { return c == ']' || c == ')' || c == '}' || c == '>' || c == '|'; }
    bool isBracket(char c) { return isOpener(c) || isCloser(c); }

    // Pallini a inizio riga (Star Citizen ne mette uno davanti ai messaggi
    // nuovi) e come l'OCR a volte li legge.
    size_t skipBullet(const std::string& s, size_t i) {
        static const char* bullets[] = {
            "\xE2\x80\xA2", // •
            "\xC2\xB7",     // ·
            "\xE2\x97\x8F", // ●
            "\xE2\x88\x99", // ∙
            "\xE2\x97\xA6", // ◦
            "\xC2\xBB",     // »
            "*", "-", ">", "~", ".", ",",
        };
        for (const char* b : bullets) {
            size_t n = std::strlen(b);
            if (s.compare(i, n, b) == 0) {
                size_t j = i + n;
                // "-" e "." valgono come pallino solo se seguiti da uno spazio o da "[".
                if (n == 1 && j < s.size() && s[j] != ' ' && !isOpener(s[j])) return i;
                return j;
            }
        }
        return i;
    }

    size_t skipSpaces(const std::string& s, size_t i) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
        return i;
    }

    // Nomi dei giocatori di Star Citizen: lettere, cifre, "_" e "-".
    bool isValidName(const std::string& name) {
        if (name.size() < 2 || name.size() > 40) return false;
        bool letter = false;
        for (unsigned char c : name) {
            if (std::isalpha(c)) letter = true;
            else if (!std::isdigit(c) && c != '_' && c != '-' && c != '.') return false;
        }
        return letter;
    }

    // Il nome con i due punti letti come un altro segno attaccato: "Kleran3464}",
    // "Swashpiates%", "Synethc»a", "Kieran346'E". Il nome è la parte prima del
    // segno, se dopo restano al più due caratteri; se no, vuoto.
    std::string nameBeforeMisreadColon(const std::string& token) {
        size_t k = 0;
        while (k < token.size() && (std::isalnum((unsigned char)token[k]) || token[k] == '_' || token[k] == '-' || token[k] == '.')) ++k;
        if (k < 3 || k == token.size() || TextUtils::utf8ToWide(token.substr(k)).size() > 2) return {};
        const std::string name = token.substr(0, k);
        return isValidName(name) ? name : std::string{};
    }

    // L'OCR legge la "_" dei nomi come una lineetta: "Cpt—Andromeda" è "Cpt_Andromeda".
    std::string dashesToUnderscore(std::string name) {
        for (const char* dash : { "\xE2\x80\x94", "\xE2\x80\x93" }) { // — –
            size_t p;
            while ((p = name.find(dash)) != std::string::npos) name.replace(p, std::strlen(dash), "_");
        }
        return name;
    }

    // Errori dell'OCR nelle frasi inglesi che il modello non capisce: "l'm",
    // "l'rn" e "Ilm" sono "I'm", una "l" da sola è "I". Parole intere soltanto.
    std::string fixEnglishMisreads(const std::string& body) {
        static const std::pair<const char*, const char*> kFixes[] = {
            { "l'm", "I'm" }, { "l'rn", "I'm" }, { "I'rn", "I'm" }, { "Ilm", "I'm" },
            { "l'll", "I'll" }, { "l've", "I've" }, { "l'd", "I'd" }, { "l", "I" },
        };
        std::string out, word;
        auto flush = [&]() {
            for (const auto& f : kFixes) {
                if (word == f.first) {
                    word = f.second;
                    break;
                }
            }
            out += word;
            word.clear();
        };
        for (char c : body) {
            if (c == ' ' || c == ',' || c == '.' || c == '?' || c == '!') {
                flush();
                out += c;
            } else {
                word += c;
            }
        }
        flush();
        return out;
    }

    // Una parola prima del nome senza quadre riconosciute: è un tag con le
    // quadre lette male ("tLSEJ", "ELSEI") solo se è quasi tutta maiuscola.
    bool looksLikeMisreadTag(const std::string& token) {
        if (token.empty() || token.size() > 24) return false;
        for (char c : token) if (isBracket(c)) return true;
        int upper = 0, letters = 0;
        for (unsigned char c : token) {
            if (std::isalpha(c)) {
                letters++;
                if (std::isupper(c)) upper++;
            }
        }
        return upper >= 2 && upper * 2 >= letters;
    }

    std::string stripBrackets(const std::string& s) {
        std::string out;
        for (char c : s) if (!isBracket(c)) out += c;
        return TextUtils::trim(out);
    }

    // Come l'OCR legge una "]" (o una "[" dopo una "]") nel testo piccolo.
    bool misreadBracket(char c) { return c == 'I' || c == 'l' || c == '1' || c == 'J' || c == '|' || c == '!'; }

    // Il primo blocco di quadre letto male, prima dello spazio che precede il nome:
    //   "[SQUADRAI Trime98"  -> "[SQUADRA] Trime98"
    //   "[TUTTOIISDRI Tadzyk" -> "[TUTTO][SDR] Tadzyk"
    //   "[TUTTOI[SDRI Tadzyk" -> "[TUTTO][SDR] Tadzyk"
    // Si usa solo se la riga, letta com'è, non si capisce.
    std::string repairBrackets(const std::string& prefix) {
        if (prefix.empty() || !isOpener(prefix[0])) return prefix;
        const size_t end = prefix.find(' ');
        if (end == std::string::npos) return prefix; // tutto attaccato al nome: non si può separare
        const std::string block = prefix.substr(0, end);
        std::string fixed;
        for (size_t k = 0; k < block.size(); ++k) {
            const char c = block[k];
            if (k > 0 && misreadBracket(c)) {
                if (k + 1 == block.size()) { fixed += ']'; continue; }                     // in fondo: "]"
                if (isOpener(block[k + 1])) { fixed += ']'; continue; }                    // "I[" = "]["
                if (c == 'I' && block[k + 1] == 'I' && k + 2 < block.size()) {             // "II" = "]["
                    fixed += "][";
                    ++k;
                    continue;
                }
            }
            fixed += c;
        }
        return fixed + prefix.substr(end);
    }

    // Una parola che è un tag con TUTTE le quadre lette male: "ITUTTOIISDRI" ->
    // {"TUTTO", "SDR"}, "tLSEJ" -> {"LSE"}. Se non si capisce, la parola com'è.
    std::vector<std::string> splitMisreadTag(const std::string& token) {
        std::string t = token;
        const bool restUpper = t.size() > 2 && std::isupper((unsigned char)t[1]);
        if (!t.empty() && !isOpener(t[0]) && (misreadBracket(t[0]) || (std::islower((unsigned char)t[0]) && restUpper))) t[0] = '[';
        if (!t.empty() && isOpener(t[0])) {
            const std::string fixed = TextUtils::trim(repairBrackets(t + " "));
            std::vector<std::string> groups;
            size_t j = 0;
            while (j < fixed.size() && isOpener(fixed[j])) {
                size_t k = j + 1;
                while (k < fixed.size() && !isCloser(fixed[k])) ++k;
                if (k >= fixed.size() || k == j + 1) break;
                groups.push_back(fixed.substr(j + 1, k - j - 1));
                j = k + 1;
            }
            if (!groups.empty() && j >= fixed.size()) return groups;
        }
        std::string plain = stripBrackets(token);
        if (plain.empty()) return {};
        return { plain };
    }

    // "[canali] nome" -> canali e nome. false se non sembra l'inizio di un messaggio.
    bool parsePrefix(const std::string& prefix, std::vector<std::string>& tags, std::string& name) {
        tags.clear();
        size_t j = 0;
        // Gruppi tra quadre, attaccati o separati da spazi: "[TUTTO][SDR]", "[TEAM] [ANZ]".
        while (true) {
            j = skipSpaces(prefix, j);
            if (j >= prefix.size() || !isOpener(prefix[j])) break;
            size_t k = j + 1;
            while (k < prefix.size() && !isCloser(prefix[k]) && prefix[k] != ' ') ++k;
            if (k >= prefix.size() || !isCloser(prefix[k]) || k == j + 1 || k - j - 1 > 24) return false;
            tags.push_back(prefix.substr(j + 1, k - j - 1));
            j = k + 1;
        }

        // Il resto: [parole che sembrano tag con le quadre lette male] nome
        std::vector<std::string> tokens;
        {
            std::string rest = prefix.substr(j), cur;
            for (char c : rest) {
                if (c == ' ' || c == '\t') {
                    if (!cur.empty()) tokens.push_back(cur), cur.clear();
                } else {
                    cur += c;
                }
            }
            if (!cur.empty()) tokens.push_back(cur);
        }
        if (tokens.empty() || tokens.size() > 3) return false;

        // Nome spezzato in due dall'OCR ("Trim e98"): la seconda parte comincia
        // in minuscolo o con una cifra. Solo dopo un canale riconosciuto, per
        // non scambiare per un nome due parole qualsiasi seguite da ":".
        if (tokens.size() == 2 && !tags.empty() && !looksLikeMisreadTag(tokens[0])) {
            const unsigned char first = (unsigned char)tokens[1][0];
            if (std::islower(first) || std::isdigit(first)) tokens = { tokens[0] + tokens[1] };
        }

        name = tokens.back();
        // Un tag attaccato al nome ma letto con una sola parentesi: "SQUADRA]Trime98".
        size_t closer = name.find_last_of("])}>|");
        if (closer != std::string::npos && closer + 1 < name.size()) {
            std::string glued = stripBrackets(name.substr(0, closer));
            if (!glued.empty()) tags.push_back(glued);
            name = name.substr(closer + 1);
        }
        name = dashesToUnderscore(stripBrackets(name));
        if (!isValidName(name)) return false;

        for (size_t t = 0; t + 1 < tokens.size(); ++t) {
            if (!looksLikeMisreadTag(tokens[t])) return false;
            for (const auto& tag : splitMisreadTag(tokens[t])) tags.push_back(tag);
        }
        return true;
    }

    // "[canali] nome: testo", con la riga che comincia in line[i].
    bool parseColonHeader(const std::string& line, size_t i, ChatMessage& out) {
        // I due punti dopo il nome (l'OCR a volte li legge come ";"). Dopo deve
        // esserci il messaggio, non "//" (un indirizzo web) né una cifra (un orario).
        size_t colon = std::string::npos;
        for (size_t k = i; k < line.size() && k < i + 90; ++k) {
            if (line[k] == ':' || line[k] == ';') { colon = k; break; }
        }
        if (colon == std::string::npos || colon == i) return false;
        if (colon + 1 < line.size() && (line[colon + 1] == '/' || std::isdigit((unsigned char)line[colon + 1]))) return false;

        const std::string prefix = line.substr(i, colon - i);
        std::vector<std::string> tags;
        std::string name;
        // Prima com'è; se non va, con le quadre "riparate" (l'OCR legge "]" come "I").
        if (!parsePrefix(prefix, tags, name)) {
            const std::string repaired = repairBrackets(prefix);
            if (repaired == prefix || !parsePrefix(repaired, tags, name)) return false;
        }

        // "Cpt_Andromeda: ciao" letto "Cpt: —Andromeda: ciao": la "_" è diventata ": —".
        size_t bodyStart = colon + 1;
        const size_t d = skipSpaces(line, colon + 1);
        for (const char* dash : { "\xE2\x80\x94", "\xE2\x80\x93", "_" }) {
            const size_t n = std::strlen(dash);
            if (line.compare(d, n, dash) != 0) continue;
            const size_t colon2 = line.find(':', d + n);
            if (colon2 == std::string::npos) break;
            const std::string joined = name + "_" + line.substr(d + n, colon2 - d - n);
            if (isValidName(joined)) {
                name = joined;
                bodyStart = colon2 + 1;
            }
            break;
        }

        out.tags = tags;
        out.playerName = name;
        out.rawPrefix = line.substr(0, bodyStart);
        out.body = TextUtils::collapseSpaces(TextUtils::trim(line.substr(bodyStart)));
        return true;
    }

    // I canali della chat di Star Citizen (e l'organizzazione). Conoscendo i
    // nomi si riconoscono anche con le quadre lette male: "tGLOBALEl",
    // "IGLOBALEI", "GLOBALE)". I più lunghi prima: "GLOBALE" non è "GLOBAL" + "E".
    const char* const kKnownChannels[] = { "GLOBALE", "GLOBAL", "GRUPPO", "GROUP", "PARTY", "SQUAD", "LSE" };

    // Una lettera del nome di un canale, anche letta come la cifra che le somiglia.
    bool sameChannelChar(char got, char want) {
        if (got == want) return true;
        switch (want) {
            case 'O': return got == '0';
            case 'S': return got == '5';
            case 'B': return got == '8';
            case 'L': return got == '1' || got == 'l' || got == '|';
            default: return false;
        }
    }

    // Una quadra letta come una lettera o una cifra ("t", "l", "1", "I", "E"...).
    bool bracketLike(char c) { return isBracket(c) || std::isalnum((unsigned char)c) || c == '!'; }

    size_t matchKnownChannel(const std::string& s, size_t i, std::string& channel, bool& framed);

    // Dopo un canale: uno spazio, la fine, una "[", un altro canale o il pallino dei messaggi nuovi.
    bool channelBoundary(const std::string& s, size_t q) {
        std::string unusedName;
        bool unusedFramed = false;
        return q >= s.size() || s[q] == ' ' || isOpener(s[q]) || skipBullet(s, q) != q ||
               matchKnownChannel(s, q, unusedName, unusedFramed) != q;
    }

    // Il canale conosciuto che comincia in s[i], con le quadre anche lette
    // male o perse. Ritorna dove finisce (i = nessun canale). framed = le
    // quadre si vedono: una vera, o tutte e due lette male.
    size_t matchKnownChannel(const std::string& s, size_t i, std::string& channel, bool& framed) {
        for (size_t open = 0; open <= 1; ++open) {
            if (open == 1 && (i >= s.size() || !bracketLike(s[i]))) break;
            const size_t p = i + open;
            for (const char* name : kKnownChannels) {
                const size_t n = std::strlen(name);
                if (p + n > s.size()) continue;
                bool same = true;
                for (size_t k = 0; k < n && same; ++k) same = sameChannelChar(s[p + k], name[k]);
                if (!same) continue;
                const size_t q = p + n;
                const bool realOpen = open == 1 && isOpener(s[i]);
                // Dopo il nome deve finire la parola: "SQUADRA" non è "SQUAD".
                size_t end = i;
                if (q < s.size() && isCloser(s[q])) {                                         // "]" (il nome può essere attaccato)
                    end = q + 1;
                    framed = true;
                } else if (channelBoundary(s, q)) {                                           // "]" persa
                    end = q;
                    framed = realOpen;
                } else if (q < s.size() && bracketLike(s[q]) && channelBoundary(s, q + 1)) {  // "]" letta "l", "1", "I"...
                    end = q + 1;
                    framed = realOpen || open == 1;
                }
                if (end != i) {
                    channel = name;
                    return end;
                }
            }
        }
        return i;
    }

    // I canali conosciuti all'inizio della riga, uno dopo l'altro ("[GLOBALE] [LSE]").
    // Ritorna dove finisce l'ultimo (gli spazi dopo restano).
    size_t readKnownChannels(const std::string& s, size_t i, std::vector<std::string>& channels, bool& framed) {
        size_t end = i;
        while (true) {
            const size_t j = skipSpaces(s, end);
            std::string channel;
            bool f = false;
            const size_t k = matchKnownChannel(s, j, channel, f);
            if (k == j) return end;
            channels.push_back(channel);
            framed = framed || f;
            end = k;
        }
    }

    // Nella chat a sinistra della "[" del canale non c'è niente: quello che
    // l'OCR legge lì (pallini, pezzi dello sfondo) è rumore e si taglia. Solo
    // se è corto e senza parole, e il canale ha le quadre: "scrivi in [PARTY]"
    // resta una frase. Ritorna dove finiscono i canali.
    size_t findKnownChannels(const std::string& s, std::vector<std::string>& channels, bool& framed) {
        for (size_t p = 0; p < s.size() && p <= 12; ++p) {
            if (p >= 2 && std::isalpha((unsigned char)s[p - 2]) && std::isalpha((unsigned char)s[p - 1])) break;
            if (s[p] == ' ') continue;
            channels.clear();
            framed = false;
            const size_t end = readKnownChannels(s, p, channels, framed);
            if (!channels.empty() && (p == 0 || framed)) return end;
        }
        channels.clear();
        framed = false;
        return 0;
    }
}

std::string ChatMessage::displayPrefix() const {
    std::string p;
    for (const auto& t : tags) p += "[" + t + "]";
    if (playerName.empty()) return p.empty() ? "" : p + " ";
    if (!p.empty()) p += " ";
    return p + playerName + ": ";
}

namespace ChatParser {

bool parseHeader(const std::string& rawLine, ChatMessage& out) {
    const std::string line = TextUtils::trim(rawLine);

    std::vector<std::string> channels;
    bool framed = false;
    const size_t end = findKnownChannels(line, channels, framed);
    if (channels.empty()) return parseColonHeader(line, skipSpaces(line, skipBullet(line, 0)), out);

    // Il pallino dei messaggi nuovi a volte finisce tra il canale e il nome:
    // "tGLOBALEl•Bearprime3C30:", "[GLOBALE] •Knu112C32:".
    size_t rest = end;
    const size_t beforeBullet = skipSpaces(line, end);
    if (skipBullet(line, beforeBullet) != beforeBullet) rest = skipBullet(line, beforeBullet);

    // Canali conosciuti: si riscrivono con le quadre giuste e il resto si legge come al solito.
    std::string clean;
    for (const auto& c : channels) clean += "[" + c + "]";
    const size_t cleanHead = clean.size();
    clean += line.substr(rest);
    if (parseColonHeader(clean, 0, out)) {
        out.rawPrefix = line.substr(0, rest) + out.rawPrefix.substr(cleanHead);
        return true;
    }

    // I due punti dopo il nome non ci sono (persi, o la riga è tagliata dal
    // bordo della zona letta) o sono seguiti da una cifra: il nome è la prima
    // parola. Solo se le quadre si vedono, per non prendere per un messaggio
    // una riga di testo che comincia con "LSE" o "PARTY".
    if (!framed) return false;
    const size_t j = skipSpaces(line, rest);
    size_t k = j;
    while (k < line.size() && line[k] != ' ' && line[k] != ':' && line[k] != ';') ++k;
    std::string name = dashesToUnderscore(line.substr(j, k - j));
    if (!isValidName(name)) name = nameBeforeMisreadColon(name);
    out.tags = channels;
    if (!name.empty()) {
        if (k < line.size() && line[k] != ' ') ++k;
        out.playerName = name;
        out.rawPrefix = line.substr(0, k);
        out.body = TextUtils::collapseSpaces(TextUtils::trim(line.substr(k)));
        return true;
    }

    // Il nome non si legge, o la riga è solo "GLOBALE)" con il resto sotto
    // (l'OCR a volte la spezza così): il canale dice comunque che qui comincia
    // un messaggio nuovo, non il seguito di quello sopra.
    out.playerName.clear();
    out.rawPrefix = line.substr(0, rest);
    out.body = TextUtils::collapseSpaces(TextUtils::trim(line.substr(rest)));
    return true;
}

namespace {
    // L'OCR a volte mette due messaggi sulla stessa riga ("...persevering
    // tGLOBALEl Foxbat: i thought..."): si divide prima di un canale con le
    // quadre seguito da "nome:". Non se prima c'è solo rumore (lo taglia parseHeader).
    std::vector<std::string> splitMerged(const std::string& line) {
        std::vector<std::string> parts;
        size_t start = 0;
        for (size_t p = 1; p < line.size(); ++p) {
            if (line[p - 1] != ' ') continue;
            std::string channel;
            bool framed = false;
            if (matchKnownChannel(line, p, channel, framed) == p || !framed) continue;
            // Con nome e due punti: "join [PARTY] chat now" resta una frase.
            ChatMessage m;
            if (!parseHeader(line.substr(p), m) || m.playerName.empty() || m.rawPrefix.find_first_of(":;") == std::string::npos) continue;
            const std::string before = TextUtils::trim(line.substr(start, p - start));
            bool words = false;
            for (size_t k = 1; k < before.size(); ++k) {
                if (std::isalpha((unsigned char)before[k - 1]) && std::isalpha((unsigned char)before[k])) words = true;
            }
            if (!words) continue;
            parts.push_back(before);
            start = p;
        }
        parts.push_back(TextUtils::trim(line.substr(start)));
        return parts;
    }
}

std::vector<ChatMessage> parse(const std::vector<std::string>& rawLines) {
    std::vector<std::string> lines;
    for (const auto& raw : rawLines) {
        for (auto& part : splitMerged(TextUtils::collapseSpaces(TextUtils::trim(raw)))) lines.push_back(part);
    }

    std::vector<ChatMessage> messages;
    std::vector<std::string> leading; // righe prima del primo messaggio riconosciuto
    for (const auto& line : lines) {
        if (line.empty()) continue;
        ChatMessage m;
        if (parseHeader(line, m)) {
            messages.push_back(std::move(m));
        } else if (!messages.empty()) {
            ChatMessage& last = messages.back();
            last.body = last.body.empty() ? line : last.body + " " + line;
        } else {
            leading.push_back(line);
        }
    }
    if (messages.empty()) return messages;

    // La coda di un messaggio cominciato sopra la zona letta: si tiene solo se
    // è una frase vera (non i nomi delle schede della chat o simili).
    if (!leading.empty()) {
        std::string text;
        for (const auto& l : leading) text += (text.empty() ? "" : " ") + l;
        int words = 1;
        for (char c : text) if (c == ' ') words++;
        if (words >= 3) {
            ChatMessage tail;
            tail.body = text;
            messages.insert(messages.begin(), tail);
        }
    }

    // Un messaggio senza testo (il resto è fuori dalla zona letta) non serve.
    std::vector<ChatMessage> result;
    for (auto& m : messages) {
        if (m.body.empty()) continue;
        m.body = fixEnglishMisreads(m.body);
        result.push_back(std::move(m));
    }
    return result;
}

bool matchesFilter(const ChatMessage& message, const std::string& filterTag) {
    const std::string filter = stripBrackets(filterTag);
    if (filter.empty() || (message.playerName.empty() && message.tags.empty())) return false;
    for (const auto& t : message.tags) {
        if (TextUtils::containsIgnoreCase(t, filter)) return true;
    }
    // Quadre lette in modo strano: si cerca nella parte prima del nome.
    size_t namePos = message.rawPrefix.rfind(message.playerName);
    std::string before = namePos == std::string::npos ? message.rawPrefix : message.rawPrefix.substr(0, namePos);
    return TextUtils::containsIgnoreCase(before, filter);
}

namespace {
    bool garbledWord(const std::wstring& raw) {
        // La punteggiatura ai bordi ("suits?", "(ok)", "50%") non conta.
        static const std::wstring edges = L".,!?;:\"'()[]{}*%$€…~";
        size_t a = 0, b = raw.size();
        while (a < b && edges.find(raw[a]) != std::wstring::npos) ++a;
        while (b > a && edges.find(raw[b - 1]) != std::wstring::npos) --b;
        const std::wstring w = raw.substr(a, b - a);
        auto letter = [](wchar_t c) { return IsCharAlphaW(c) != 0; };
        auto digit = [](wchar_t c) { return c >= L'0' && c <= L'9'; };
        for (size_t k = 0; k < w.size(); ++k) {
            const wchar_t c = w[k], prev = k > 0 ? w[k - 1] : 0, next = k + 1 < w.size() ? w[k + 1] : 0;
            // Un simbolo dentro la parola: "wh*o", "•ef". Si accettano quelli di
            // orari, numeri, indirizzi e nomi: "12:30", "1,000", "2-3", "w/", "Cpt_A".
            if (!letter(c) && !digit(c) && !std::wcschr(L"'’-./:,_", c)) return true;
            if (prev && next && IsCharLowerW(prev) && IsCharUpperW(c) && IsCharLowerW(next)) return true; // "sTlts"
            if (prev && next && letter(prev) && digit(c) && letter(next)) return true;                    // "fy3afft"
            // Una lettera accentata in mezzo a una parola inglese: "flightèuits". Non
            // "perché", "dzięki", "préféré": dopo ci sono al massimo due lettere semplici.
            if (c > 0x7F && letter(c) && prev && prev < 0x80 && letter(prev)) {
                size_t ascii = 0;
                for (size_t j = k + 1; j < w.size() && w[j] < 0x80 && letter(w[j]); ++j) ascii++;
                if (ascii >= 3) return true;
            }
        }
        return false;
    }
}

int garbledWords(const std::string& text, int* words) {
    int total = 0, garbled = 0;
    std::wstring word;
    auto flush = [&]() {
        bool content = false;
        for (wchar_t c : word) content = content || IsCharAlphaNumericW(c);
        if (content) {
            total++;
            if (garbledWord(word)) garbled++;
        }
        word.clear();
    };
    for (wchar_t c : TextUtils::utf8ToWide(text)) {
        if (c == L' ' || c == L'\t') flush();
        else word += c;
    }
    flush();
    if (words) *words = total;
    return garbled;
}

bool unreadable(const std::string& text) {
    int words = 0;
    const int garbled = garbledWords(text, &words);
    // Almeno due parole rovinate, e una su quattro: un nome come "LaffyJaff"
    // o una sigla come "C8X" da soli non bastano.
    return garbled >= 2 && garbled * 4 >= words;
}

} // namespace ChatParser
