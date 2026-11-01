#include "mining_decoder.h"
#include "mining_image.h"
#include "text_utils.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <set>
#include <sstream>

namespace Mining {

namespace {
    bool isDigit(char c) { return c >= '0' && c <= '9'; }

    std::string trimQuotes(std::string s) {
        s = TextUtils::trim(s);
        if (s.size() >= 2 && (s.front() == '"' || s.front() == '\'') && s.back() == s.front()) s = s.substr(1, s.size() - 2);
        return TextUtils::trim(s);
    }

    // "4,240" / "4.240" / "4240" -> 4240; -1 se non è un intero.
    long long parseInteger(const std::string& field) {
        std::string digits;
        for (char c : field) {
            if (isDigit(c)) digits += c;
            else if (c == '.' || c == ',' || c == '\'' || c == ' ') continue;
            else return -1;
        }
        if (digits.empty() || digits.size() > 9) return -1;
        return std::atoll(digits.c_str());
    }

    std::string normalizeKind(const std::string& k) {
        const std::string l = TextUtils::toLower(TextUtils::trim(k));
        if (l == "salvage" || l == "oggetto" || l == "rottame" || l == "scrap" || l == "relitto") return "salvage";
        if (l == "roc" || l == "veicolo") return "roc";
        if (l == "fps" || l == "a mano" || l == "mano") return "fps";
        return "minerale";
    }

    // --- Candidati numerici dal testo dell'OCR ----------------------------------------

    enum class Origin { Plain, Joined, Stripped };
    struct Candidate {
        long long value;      // 0 = ci sono cifre illeggibili (vedi pattern)
        Origin origin;
        int digits;
        std::string pattern;  // le cifre, con '?' al posto di quelle illeggibili
        bool grouped = false; // scritto con la virgola delle migliaia ("16,960"): il formato del badge
    };

    bool isSep(char c) { return c == ',' || c == '.' || c == '\''; }
    bool isDigitLike(char c) { return isDigit(c) || c == '?'; }

    // Lettere che l'OCR scambia per cifre in modo prevedibile.
    char confusableDigit(char c) {
        switch (c) {
        case 'O': case 'o': case 'D': case 'Q': return '0';
        case 'l': case 'I': case 'i': case '|': case '!': case 'L': return '1';
        case 'Z': case 'z': return '2';
        case 'S': case 's': case '$': return '5';
        case 'G': case 'b': return '6';
        case 'T': return '7';
        case 'B': return '8';
        case 'g': case 'q': return '9';
        default: return 0;
        }
    }

    // Dentro una "parola" fatta soprattutto di cifre ("l6,96O", "7,OCO", "17,08d"):
    // le lettere confondibili diventano cifre, le altre '?' (cifra illeggibile).
    // Le parole vere ("Iron", "SHIP PORTS", "245m") restano come sono.
    std::string cleanForNumbers(const std::string& text) {
        std::string out;
        auto isWordByte = [](unsigned char c) { return std::isalnum(c) || c == '|' || c == '!' || c == '$' || c >= 0x80 || isSep((char)c); };
        size_t i = 0;
        while (i < text.size()) {
            if (!isWordByte((unsigned char)text[i])) {
                out += text[i++];
                continue;
            }
            size_t j = i;
            while (j < text.size() && isWordByte((unsigned char)text[j])) ++j;
            // Cifre vere, lettere "quasi cifre" (O, l, S...) e altre lettere.
            int digits = 0, similar = 0, letters = 0;
            for (size_t k = i; k < j; ++k) {
                const unsigned char ch = (unsigned char)text[k];
                if (isDigit((char)ch)) ++digits;
                else if (isSep((char)ch) || (ch >= 0x80 && ch < 0xC0)) continue; // separatore o seguito di una lettera UTF-8
                else if (confusableDigit((char)ch)) ++similar;
                else ++letters;
            }
            const bool hasSep = std::any_of(text.begin() + i, text.begin() + j, [](char ch) { return isSep(ch); });
            const bool numberLike = (digits >= 1 && letters <= 2 && (digits + similar) * 2 >= digits + similar + letters) ||
                                    (digits == 0 && letters == 0 && similar >= 3 && hasSep); // "IS,soo" = 15,500
            if (numberLike) {
                for (size_t k = i; k < j; ++k) {
                    const unsigned char ch = (unsigned char)text[k];
                    if (isDigit((char)ch) || isSep((char)ch)) out += (char)ch;
                    else if (ch >= 0x80 && ch < 0xC0) continue;
                    else if (const char d = confusableDigit((char)ch)) out += d;
                    else out += '?';
                }
            } else {
                out.append(text, i, j - i);
            }
            i = j;
        }
        return out;
    }

    std::vector<Candidate> candidatesIn(const std::string& rawText) {
        const std::string s = cleanForNumbers(rawText);
        std::vector<Candidate> out;
        auto add = [&](std::string pattern, Origin o, bool grouped = false) {
            while (pattern.size() > 1 && pattern[0] == '0') pattern.erase(0, 1);
            if (pattern.empty() || pattern.size() > 9) return;
            const size_t wild = (size_t)std::count(pattern.begin(), pattern.end(), '?');
            if (wild > 2) return;
            const long long v = wild ? 0 : std::atoll(pattern.c_str());
            if (!wild && v <= 0) return;
            for (const auto& c : out) {
                if (c.pattern == pattern) return;
            }
            out.push_back({ v, o, (int)pattern.size(), pattern, grouped });
        };

        size_t i = 0;
        while (i < s.size()) {
            if (!isDigitLike(s[i])) { ++i; continue; }
            // Primo gruppo: le cifre di fila.
            size_t j = i;
            while (j < s.size() && isDigitLike(s[j])) ++j;
            const std::string first = s.substr(i, j - i);

            // Gruppi delle migliaia con virgola, punto o apostrofo: "16,960", "1.234.567".
            std::string grouped = first;
            size_t k = j;
            bool usedHardSeparator = false;
            if (first.size() <= 3) {
                while (k + 3 < s.size() && isSep(s[k]) && isDigitLike(s[k + 1]) && isDigitLike(s[k + 2]) && isDigitLike(s[k + 3]) &&
                       (k + 4 >= s.size() || !isDigitLike(s[k + 4]))) {
                    grouped += s.substr(k + 1, 3);
                    k += 4;
                    usedHardSeparator = true;
                }
            }
            if (usedHardSeparator) {
                add(grouped, Origin::Plain, true);
                i = k;
                continue;
            }

            if (first.find('?') != std::string::npos) {
                // Senza separatori una cifra illeggibile non si indovina: valgono i pezzi sicuri.
                std::string part;
                for (char ch : first) {
                    if (ch == '?') {
                        if (!part.empty()) add(part, Origin::Plain);
                        part.clear();
                    } else {
                        part += ch;
                    }
                }
                if (!part.empty()) add(part, Origin::Plain);
                i = j;
                continue;
            }
            add(first, Origin::Plain);

            // Spazio come separatore ("16 960"): è un'ipotesi in più, non l'unica
            // (potrebbe essere il segnaposto letto come "2" seguito da "700").
            if (first.size() <= 3 && j + 4 <= s.size() && s[j] == ' ' && isDigit(s[j + 1]) && isDigit(s[j + 2]) && isDigit(s[j + 3]) &&
                (j + 4 == s.size() || !isDigitLike(s[j + 4]))) {
                add(first + s.substr(j + 1, 3), Origin::Joined);
            }
            i = j;
        }

        // Il segnaposto incollato davanti al numero ("216,960"): ipotesi senza la prima cifra.
        // Solo per numeri da 6 cifre in su (come in Mining Signature Calculator): "17,740"
        // ha la virgola dopo il "17", la prima cifra è vera.
        const size_t n = out.size();
        for (size_t c = 0; c < n; ++c) {
            if (out[c].value >= 100000) add(std::to_string(out[c].value).substr(1), Origin::Stripped);
        }
        return out;
    }

    // Tutti i valori che un pattern con '?' può essere.
    std::vector<long long> expandPattern(const std::string& pattern) {
        std::vector<std::string> acc = { "" };
        for (char c : pattern) {
            std::vector<std::string> next;
            for (const auto& a : acc) {
                if (c == '?') {
                    for (char d = '0'; d <= '9'; ++d) next.push_back(a + d);
                } else {
                    next.push_back(a + c);
                }
            }
            acc.swap(next);
        }
        std::vector<long long> out;
        for (const auto& a : acc) {
            if (a[0] != '0') out.push_back(std::atoll(a.c_str()));
        }
        return out;
    }

    std::string patternWithDots(const std::string& pattern) {
        std::string s = pattern;
        for (int pos = (int)s.size() - 3; pos > 0; pos -= 3) s.insert((size_t)pos, ".");
        return s;
    }

    // Coppie di cifre che l'OCR confonde (forme simili).
    const char* similarDigits(char d) {
        switch (d) {
        case '0': return "869";
        case '1': return "7";
        case '2': return "7";
        case '3': return "85";
        case '5': return "63";
        case '6': return "580";
        case '7': return "12";
        case '8': return "3690";
        case '9': return "80";
        default: return "";
        }
    }

    std::string withDots(long long v) {
        std::string s = std::to_string(v);
        for (int pos = (int)s.size() - 3; pos > 0; pos -= 3) s.insert((size_t)pos, ".");
        return s;
    }
}

std::vector<Material> defaultMaterials() {
    // Firme base da mining_ocr (lookup_table.json) e da Mining Signature Calculator
    // (materialsdb). Agricium: 3885 come in mining_ocr (MSC aveva 3895, fuori dalla
    // serie a passi di 15 dei suoi vicini); Tungsten e Quantanium (spazio) solo in MSC.
    return {
        { "Quantanium (spazio)", 3110, "minerale" },
        { "Quantanium", 3170, "minerale" },
        { "Stileron", 3185, "minerale" },
        { "Savrilium", 3200, "minerale" },
        { "Ouratite", 3370, "minerale" },
        { "Riccite", 3385, "minerale" },
        { "Lindinium", 3400, "minerale" },
        { "Beryl", 3540, "minerale" },
        { "Taranite", 3555, "minerale" },
        { "Borase", 3570, "minerale" },
        { "Gold (Oro)", 3585, "minerale" },
        { "Bexalite", 3600, "minerale" },
        { "Laranite", 3825, "minerale" },
        { "Aslarite", 3840, "minerale" },
        { "Titanium (Titanio)", 3855, "minerale" },
        { "Tungsten (Tungsteno)", 3870, "minerale" },
        { "Agricium", 3885, "minerale" },
        { "Torite", 3900, "minerale" },
        { "Hephaestanite", 4180, "minerale" },
        { "Tin (Stagno)", 4195, "minerale" },
        { "Quartz (Quarzo)", 4210, "minerale" },
        { "Corundum (Corindone)", 4225, "minerale" },
        { "Copper (Rame)", 4240, "minerale" },
        { "Silicon (Silicio)", 4255, "minerale" },
        { "Iron (Ferro)", 4270, "minerale" },
        { "Aluminum (Alluminio)", 4285, "minerale" },
        { "Ice (Ghiaccio)", 4300, "minerale" },
        { "Scrap (salvage)", 2000, "salvage" },
        // Depositi a terra (da Citizen Assistant / SCMINER): la signature dice solo quanti
        // sono, non il minerale, che dipende dal posto.
        { "Deposito ROC", 4000, "roc" },
        { "Deposito a mano (FPS)", 3000, "fps" },
    };
}

std::vector<Material> groundDeposits() {
    std::vector<Material> out;
    for (const auto& m : defaultMaterials()) {
        if (m.kind == "roc" || m.kind == "fps") out.push_back(m);
    }
    return out;
}

const OreInfo* oreInfo(const std::string& materialName) {
    // Rarità e minerali secondari dalla tabella di Citizen Assistant (SCMINER, patch 4.10.2).
    // 1 comune ... 5 leggendario.
    static const OreInfo kOres[] = {
        { "Quantanium", 5, "Beryl 10-20%" },
        { "Stileron", 4, "Taranite 10-20%" },
        { "Savrilium", 4, "Gold 10-20%" },
        { "Ouratite", 4, "Agricium 10-20%" },
        { "Riccite", 4, "Laranite 10-20%" },
        { "Lindinium", 4, "Tungsten 10-20%" },
        { "Beryl", 3, "" },
        { "Taranite", 4, "" },
        { "Borase", 4, "Gold / Bexalite 5-10%" },
        { "Gold", 4, "Borase / Bexalite 5-10%" },
        { "Bexalite", 4, "Gold / Borase 5-10%" },
        { "Laranite", 3, "Tungsten 10-20%" },
        { "Aslarite", 3, "Titanium / Agricium 5-10%" },
        { "Titanium", 3, "Aslarite / Agricium 5-10%" },
        { "Tungsten", 3, "Laranite 10-20%" },
        { "Agricium", 3, "Titanium / Aslarite 5-10%" },
        { "Torite", 2, "" },
        { "Hephaestanite", 2, "Quartz / Silicon 5-10%" },
        { "Tin", 2, "Copper 10-20%" },
        { "Quartz", 2, "Hephaestanite / Silicon 5-10%" },
        { "Corundum", 2, "Aluminum 10-20%" },
        { "Copper", 2, "Tin 10-20%" },
        { "Silicon", 2, "Hephaestanite / Quartz 5-10%" },
        { "Iron", 1, "" },
        { "Aluminum", 1, "Corundum 10-20%" },
        { "Ice", 1, "" },
    };
    // "Copper (Rame)" -> "Copper"; il nome inglese è quello della tabella.
    std::string name = TextUtils::trim(materialName);
    const size_t paren = name.find(" (");
    if (paren != std::string::npos) name = name.substr(0, paren);
    for (const auto& o : kOres) {
        if (TextUtils::toLower(name) == TextUtils::toLower(o.name)) return &o;
    }
    return nullptr;
}

std::string rarityName(int rarity) {
    switch (rarity) {
    case 5: return "LEGGENDARIO";
    case 4: return "EPICO";
    case 3: return "RARO";
    case 2: return "NON COMUNE";
    case 1: return "COMUNE";
    default: return "";
    }
}

std::vector<Material> parseTable(const std::string& text, std::vector<std::string>* errors) {
    std::vector<Material> out;
    int lineNo = 0;
    for (std::string line : TextUtils::splitLines(text)) {
        ++lineNo;
        if (lineNo == 1 && TextUtils::startsWith(line, "\xEF\xBB\xBF")) line.erase(0, 3);
        const size_t hash = line.find('#');
        if (hash != std::string::npos) line = line.substr(0, hash);
        line = TextUtils::trim(line);
        if (line.empty()) continue;

        char sep = ',';
        if (line.find(';') != std::string::npos) sep = ';';
        else if (line.find('\t') != std::string::npos) sep = '\t';
        std::vector<std::string> f;
        std::stringstream ss(line);
        std::string part;
        while (std::getline(ss, part, sep)) f.push_back(trimQuotes(part));
        if (f.size() < 2) {
            if (errors) errors->push_back("riga " + std::to_string(lineNo) + ": serve \"NOME,FIRMA\"");
            continue;
        }
        const long long base = parseInteger(f[1]);
        if (base <= 0) {
            // L'intestazione di un CSV ("name,base_signature") non è un errore.
            const std::string l = TextUtils::toLower(f[1]);
            const bool header = l.find("sig") != std::string::npos || l.find("firma") != std::string::npos || l.find("base") != std::string::npos ||
                                l.find("code") != std::string::npos || l.find("valore") != std::string::npos;
            if (!header && errors) errors->push_back("riga " + std::to_string(lineNo) + ": la firma \"" + f[1] + "\" non è un numero");
            continue;
        }
        if (f[0].empty()) {
            if (errors) errors->push_back("riga " + std::to_string(lineNo) + ": manca il nome");
            continue;
        }
        Material m;
        m.name = f[0];
        m.base = (int)base;
        m.kind = f.size() > 2 ? normalizeKind(f[2]) : (TextUtils::containsIgnoreCase(m.name, "scrap") ? "salvage" : "minerale");
        // Stesso nome due volte: vale l'ultima riga.
        auto same = std::find_if(out.begin(), out.end(), [&](const Material& x) { return TextUtils::toLower(x.name) == TextUtils::toLower(m.name); });
        if (same != out.end()) *same = m;
        else out.push_back(m);
    }
    return out;
}

std::string formatTable(const std::vector<Material>& materials) {
    std::string out =
        "# Materiali per il mining di Iris: NOME,FIRMA BASE,TIPO (minerale, salvage, roc o fps)\n"
        "# Una signature letta = firma base x quantità. Righe con # = commenti.\n";
    std::vector<Material> sorted = materials;
    auto order = [](const std::string& kind) { return kind == "minerale" ? 0 : kind == "salvage" ? 1 : kind == "roc" ? 2 : 3; };
    std::stable_sort(sorted.begin(), sorted.end(), [&](const Material& a, const Material& b) {
        if (a.kind != b.kind) return order(a.kind) < order(b.kind);
        return a.base < b.base;
    });
    for (const auto& m : sorted) out += m.name + "," + std::to_string(m.base) + "," + (m.kind.empty() ? "minerale" : m.kind) + "\n";
    return out;
}

bool decodeMscDatabase(const std::string& bytes, std::string& csvOut) {
    static const char kMagic[] = "MSCDB1\n";
    static const char kKey[] = "MiningSignatureCalculator";
    const size_t magicLen = sizeof(kMagic) - 1, keyLen = sizeof(kKey) - 1;
    if (bytes.size() <= magicLen || bytes.compare(0, magicLen, kMagic) != 0) return false;
    std::string payload = bytes.substr(magicLen);
    for (size_t i = 0; i < payload.size(); ++i) payload[i] = (char)(payload[i] ^ kKey[i % keyLen]);
    return MiningImage::zlibInflate(payload, csvOut);
}

std::vector<Match> findMatches(int value, const std::vector<Material>& materials, int maxQuantity) {
    std::vector<Match> out;
    if (value <= 0) return out;
    for (const auto& m : materials) {
        if (m.base <= 0 || value % m.base != 0) continue;
        const int q = value / m.base;
        // I depositi a terra arrivano al massimo a 8 (SCMINER), le rocce e i rottami fino al limite scelto.
        const int limit = m.kind == "roc" || m.kind == "fps" ? std::min(maxQuantity, 8) : maxQuantity;
        if (q < 1 || q > limit) continue;
        out.push_back({ m.name, m.kind, m.base, q });
    }
    // Prima le rocce, poi i rottami, poi i depositi a terra (nello spazio non ce ne sono: come
    // in Citizen Assistant restano un'alternativa); a parità, dalla quantità più piccola.
    auto order = [](const std::string& kind) { return kind == "minerale" ? 0 : kind == "salvage" ? 1 : 2; };
    std::sort(out.begin(), out.end(), [&](const Match& a, const Match& b) {
        if (order(a.kind) != order(b.kind)) return order(a.kind) < order(b.kind);
        if (a.quantity != b.quantity) return a.quantity < b.quantity;
        return a.name < b.name;
    });
    return out;
}

std::vector<int> numbersInText(const std::string& text) {
    std::vector<int> out;
    for (const auto& c : candidatesIn(text)) {
        if (c.origin != Origin::Stripped && c.value > 0) out.push_back((int)c.value);
    }
    return out;
}

Decoded decodeValue(int value, const std::vector<Material>& materials, int maxQuantity) {
    Decoded d;
    d.hasValue = value > 0;
    d.value = d.readValue = value;
    d.matches = findMatches(value, materials, maxQuantity);
    d.candidates = { value };
    return d;
}

Decoded decodeText(const std::string& ocrText, const std::vector<Material>& materials, int maxQuantity) {
    Decoded d;
    std::vector<Candidate> cands = candidatesIn(ocrText);
    // Le signature sono sempre almeno di qualche centinaio: "2", "16" da soli sono pezzi o segnaposto.
    cands.erase(std::remove_if(cands.begin(), cands.end(),
                               [](const Candidate& c) { return c.value != 0 && (c.value < 100 || c.value > 9999999); }),
                cands.end());
    std::vector<Candidate> direct, stripped, wild;
    for (const auto& c : cands) {
        if (c.value == 0) wild.push_back(c);
        else if (c.origin == Origin::Stripped) stripped.push_back(c);
        else {
            direct.push_back(c);
            d.candidates.push_back((int)c.value);
        }
    }
    if (direct.empty() && stripped.empty() && wild.empty()) return d;

    auto matches = [&](long long v) { return v > 0 && v <= 9999999 && !findMatches((int)v, materials, maxQuantity).empty(); };
    // Per le correzioni (segnaposto tolto, cifre illeggibili o simili) i depositi a terra non
    // contano: sono multipli tondi (3.000, 4.000) e una correzione li "troverebbe" in qualsiasi
    // numero dell'HUD ("9.0km" -> 9.000). Valgono solo letti così come sono.
    auto matchesFix = [&](long long v) {
        if (v <= 0 || v > 9999999) return false;
        for (const auto& m : findMatches((int)v, materials, maxQuantity)) {
            if (m.kind != "roc" && m.kind != "fps") return true;
        }
        return false;
    };
    auto choose = [&](long long value, bool corrected, const std::string& note) {
        d.hasValue = true;
        d.value = (int)value;
        d.corrected = corrected;
        d.note = note;
        d.matches = findMatches(d.value, materials, maxQuantity);
    };

    // 1. Un numero letto così com'è che torna con la tabella. Prima quelli con la virgola delle
    //    migliaia (il formato del badge: "16,960"), poi i più lunghi; gli altri numeri dell'HUD
    //    ("80 100", "245m") vengono dopo.
    auto better = [](const Candidate& a, const Candidate& b) {
        if (a.grouped != b.grouped) return a.grouped;
        if ((a.origin == Origin::Joined) != (b.origin == Origin::Joined)) return b.origin == Origin::Joined;
        return a.digits > b.digits;
    };
    std::stable_sort(direct.begin(), direct.end(), better);
    for (const auto& c : direct) {
        if (matches(c.value)) {
            choose(c.value, false, "");
            d.readValue = d.value;
            return d;
        }
    }

    // Il numero "migliore" letto: il primo nell'ordine di prima (a parità, il più grande).
    const Candidate* best = nullptr;
    for (const auto& c : direct) {
        if (!best || better(c, *best) || (!better(*best, c) && c.value > best->value)) best = &c;
    }
    if (best) d.readValue = (int)best->value;

    // 2. Senza la prima cifra (il segnaposto incollato davanti a un numero da 6 cifre in su).
    for (const auto& c : stripped) {
        if (matchesFix(c.value)) {
            choose(c.value, true, "tolta la prima cifra (il segnaposto letto come numero)");
            return d;
        }
    }

    // 3. Cifre illeggibili ("7,?80"): si accetta solo se una sola possibilità torna con la tabella.
    std::set<long long> wildFixes;
    std::string wildPattern;
    for (const auto& c : wild) {
        for (long long v : expandPattern(c.pattern)) {
            if (matchesFix(v)) wildFixes.insert(v);
        }
        if (wildPattern.empty()) wildPattern = patternWithDots(c.pattern);
    }
    if (wildFixes.size() == 1) {
        choose(*wildFixes.begin(), true, "cifra illeggibile nel testo dell'OCR (" + wildPattern + ")");
        return d;
    }

    // 4. Una cifra confusa dall'OCR (8/0/3/6...): si accetta solo se la correzione possibile è una sola.
    std::set<long long> fixes;
    if (best) {
        const std::string digits = std::to_string(best->value);
        for (size_t pos = 0; pos < digits.size(); ++pos) {
            for (const char* alt = similarDigits(digits[pos]); *alt; ++alt) {
                std::string v = digits;
                v[pos] = *alt;
                if (v[0] == '0') continue;
                const long long value = std::atoll(v.c_str());
                if (matchesFix(value)) fixes.insert(value);
            }
        }
    }
    if (fixes.size() == 1) {
        choose(*fixes.begin(), true, "letto " + withDots(best->value) + ", corretta una cifra simile");
        d.digitFix = true;
        return d;
    }

    // 5. Nessuna corrispondenza: il numero letto (troppo grande = segnaposto incollato davanti).
    if (best) {
        long long shown = best->value;
        bool strippedShown = false;
        if (best->value > 200000 && !stripped.empty()) {
            shown = stripped.front().value;
            strippedShown = true;
        }
        choose(shown, strippedShown, strippedShown ? "tolta la prima cifra (il segnaposto letto come numero)" : "");
        std::set<long long> maybe = fixes;
        maybe.insert(wildFixes.begin(), wildFixes.end());
        if (maybe.size() > 1) {
            std::string list;
            for (long long f : maybe) list += (list.empty() ? "" : ", ") + withDots(f);
            d.note = "forse " + list + " (cifra letta male dall'OCR)";
        }
    } else {
        // Solo cifre illeggibili: niente valore, ma si dice cosa si è visto.
        d.note = "cifre illeggibili: " + wildPattern;
        if (wildFixes.size() > 1) {
            std::string list;
            for (long long f : wildFixes) list += (list.empty() ? "" : ", ") + withDots(f);
            d.note += " (forse " + list + ")";
        }
    }
    return d;
}

std::string formatValue(int value) {
    return withDots(value);
}

std::string describeMatch(const Match& m) {
    return m.name + " × " + std::to_string(m.quantity);
}

std::string summary(const Decoded& d) {
    if (!d.hasValue) return "nessun numero letto";
    std::string out = formatValue(d.value);
    if (d.matches.empty()) {
        out += ": sconosciuta";
    } else {
        out += " = ";
        for (size_t i = 0; i < d.matches.size(); ++i) out += (i ? " / " : "") + describeMatch(d.matches[i]);
    }
    if (d.corrected) out += " (probabile)";
    return out;
}

// --- Numeri in lettere --------------------------------------------------------------

long long parseItalianNumber(const std::string& words) {
    // Tutto attaccato e senza accenti ("sedicimila novecento" -> "sedicimilanovecento").
    std::string s;
    for (const std::string& w0 : [&] {
             std::vector<std::string> v;
             std::stringstream ss(TextUtils::toLower(words));
             std::string w;
             while (ss >> w) v.push_back(w);
             return v;
         }()) {
        if (w0 == "e") continue; // "mille e cento"
        s += w0;
    }
    // "tré" -> "tre"
    for (size_t p; (p = s.find("\xC3\xA9")) != std::string::npos;) s.replace(p, 2, "e");
    if (s.empty()) return -1;

    struct Morph {
        const char* text;
        int value;
        int type; // 0 = cifre/decine, 1 = cento, 2 = mila/mille
    };
    static const Morph morphs[] = {
        { "diciassette", 17, 0 }, { "diciannove", 19, 0 }, { "quattordici", 14, 0 }, { "diciotto", 18, 0 },
        { "cinquanta", 50, 0 },   { "cinquant", 50, 0 },   { "quaranta", 40, 0 },     { "quarant", 40, 0 },
        { "sessanta", 60, 0 },    { "sessant", 60, 0 },    { "settanta", 70, 0 },     { "settant", 70, 0 },
        { "quindici", 15, 0 },    { "tredici", 13, 0 },    { "ottanta", 80, 0 },      { "ottant", 80, 0 },
        { "novanta", 90, 0 },     { "novant", 90, 0 },     { "trenta", 30, 0 },       { "trent", 30, 0 },
        { "undici", 11, 0 },      { "dodici", 12, 0 },     { "sedici", 16, 0 },       { "quattro", 4, 0 },
        { "cinque", 5, 0 },       { "dieci", 10, 0 },      { "venti", 20, 0 },        { "vent", 20, 0 },
        { "sette", 7, 0 },        { "zero", 0, 0 },        { "uno", 1, 0 },           { "due", 2, 0 },
        { "tre", 3, 0 },          { "sei", 6, 0 },         { "otto", 8, 0 },          { "nove", 9, 0 },
        { "un", 1, 0 },           { "cento", 100, 1 },     { "cent", 100, 1 },        { "mille", 1000, 2 },
        { "mila", 1000, 2 },
    };

    // Ricerca con ritorno indietro: "centotto" = cent + otto, non cento + "tto".
    std::vector<const Morph*> seq;
    std::function<bool(size_t)> parse = [&](size_t pos) -> bool {
        if (pos == s.size()) return true;
        for (const auto& m : morphs) {
            const size_t len = std::strlen(m.text);
            if (s.compare(pos, len, m.text) == 0) {
                seq.push_back(&m);
                if (parse(pos + len)) return true;
                seq.pop_back();
            }
        }
        return false;
    };
    if (!parse(0)) return -1;

    long long total = 0, current = 0;
    bool any = false;
    for (const Morph* m : seq) {
        any = true;
        if (m->type == 0) current += m->value;
        else if (m->type == 1) current = (current == 0 ? 1 : current) * 100;
        else {
            total += (current == 0 ? 1 : current) * 1000;
            current = 0;
        }
    }
    return any ? total + current : -1;
}

VoiceCommand parseVoice(const std::string& command) {
    VoiceCommand vc;
    const std::string lower = TextUtils::toLower(command);
    // Parole (lettere e cifre), punteggiatura tolta, ma "16.960" resta un numero.
    std::vector<std::string> words;
    {
        std::string cur;
        for (size_t i = 0; i < lower.size(); ++i) {
            const unsigned char c = (unsigned char)lower[i];
            const bool sepInNumber = (c == '.' || c == ',') && !cur.empty() && isDigit(cur.back()) && i + 1 < lower.size() && isDigit(lower[i + 1]);
            if (std::isalnum(c) || c >= 0x80 || sepInNumber) cur += (char)c;
            else if (!cur.empty()) {
                words.push_back(cur);
                cur.clear();
            }
        }
        if (!cur.empty()) words.push_back(cur);
    }
    if (words.empty()) return vc;
    std::string joined;
    for (const auto& w : words) joined += (joined.empty() ? "" : " ") + w;

    auto has = [&](const char* phrase) { return (" " + joined + " ").find(std::string(" ") + phrase + " ") != std::string::npos; };
    auto hasPrefix = [&](const char* prefix) {
        return std::any_of(words.begin(), words.end(), [&](const std::string& w) { return TextUtils::startsWith(w, prefix); });
    };

    // Il numero: in cifre ("16960", "16.960", "16 960") o in lettere.
    long long number = -1;
    {
        std::string digitsText;
        for (const auto& w : words) digitsText += w + " ";
        for (int v : numbersInText(digitsText)) number = std::max<long long>(number, v);
        // In lettere: la sequenza di parole-numero più lunga.
        for (size_t i = 0; i < words.size(); ++i) {
            for (size_t j = words.size(); j > i; --j) {
                std::string span;
                for (size_t k = i; k < j; ++k) span += words[k] + " ";
                const long long v = parseItalianNumber(span);
                if (v > number) number = v;
                if (v >= 0) break;
            }
        }
    }

    const bool keyword = hasPrefix("signatur") || hasPrefix("segnatur") || hasPrefix("firma") || hasPrefix("minera") ||
                         hasPrefix("material") || hasPrefix("roccia") || hasPrefix("rocce") || hasPrefix("salvage") || has("valore") ||
                         hasPrefix("scansion") || hasPrefix("mining");
    // Domande di prezzi e commercio ("quanto vale un minerale da 3000 aUEC?") vanno al modello.
    const bool trade = hasPrefix("quant") || hasPrefix("prezz") || hasPrefix("cost") || hasPrefix("auec") || hasPrefix("vend") ||
                       hasPrefix("compr") || hasPrefix("guadagn");
    if (trade) return vc;

    if (number >= 100 && number <= 9999999 && keyword) {
        vc.kind = VoiceKind::Value;
        vc.value = (int)number;
        return vc;
    }
    // Solo un numero ("Iris, settemilaottanta"): con una signature plausibile vale come valore.
    if (number >= 1000 && number <= 9999999) {
        bool onlyNumber = true;
        for (const auto& w : words) {
            const bool numericWord = std::all_of(w.begin(), w.end(), [](char c) { return isDigit(c) || c == '.' || c == ','; }) ||
                                     parseItalianNumber(w) >= 0 || w == "e";
            if (!numericWord) { onlyNumber = false; break; }
        }
        if (onlyNumber) {
            vc.kind = VoiceKind::Value;
            vc.value = (int)number;
            return vc;
        }
    }

    const bool scan = has("che minerale") || has("che materiale") || has("che roccia") || has("quale minerale") || has("quale materiale") ||
                      has("che cos è questa roccia") || has("cos è questa roccia") || has("che c è nella roccia") || has("cosa c è nella roccia") ||
                      hasPrefix("scansiona") || has("leggi la signature") || has("leggi la firma") || has("leggi la segnatura") ||
                      has("analizza la roccia") || has("leggi il minerale") || has("leggi la roccia") || has("scansione mineraria") ||
                      has("che salvage") || has("leggi il salvage");
    if (scan) vc.kind = VoiceKind::Scan;
    return vc;
}

} // namespace Mining
