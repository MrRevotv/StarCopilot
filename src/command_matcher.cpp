#include "command_matcher.h"
#include "text_utils.h"

#include <algorithm>
#include <map>
#include <set>
#include <sstream>

namespace Cmd {

namespace {
    // Lettere accentate -> semplici (italiano e lingue vicine).
    wchar_t fold(wchar_t c) {
        static const wchar_t* from = L"àáâãäåèéêëìíîïòóôõöùúûüçñÀÁÂÃÄÅÈÉÊËÌÍÎÏÒÓÔÕÖÙÚÛÜÇÑ";
        static const wchar_t* to = L"aaaaaaeeeeiiiiooooouuuucnaaaaaaeeeeiiiiooooouuuucn";
        for (int i = 0; from[i]; ++i) {
            if (from[i] == c) return to[i];
        }
        return c;
    }

    std::vector<std::string> words(const std::string& s) {
        std::vector<std::string> out;
        std::istringstream in(s);
        std::string w;
        while (in >> w) out.push_back(w);
        return out;
    }

    std::string join(const std::vector<std::string>& w, size_t from, size_t to) {
        std::string out;
        for (size_t i = from; i < to && i < w.size(); ++i) {
            if (!out.empty()) out += ' ';
            out += w[i];
        }
        return out;
    }

    int levenshtein(const std::string& a, const std::string& b) {
        std::vector<int> prev(b.size() + 1), cur(b.size() + 1);
        for (size_t j = 0; j <= b.size(); ++j) prev[j] = (int)j;
        for (size_t i = 1; i <= a.size(); ++i) {
            cur[0] = (int)i;
            for (size_t j = 1; j <= b.size(); ++j) {
                int cost = a[i - 1] == b[j - 1] ? 0 : 1;
                cur[j] = std::min({ cur[j - 1] + 1, prev[j] + 1, prev[j - 1] + cost });
            }
            std::swap(prev, cur);
        }
        return prev[b.size()];
    }

    // Una frase del dizionario divisa intorno allo slot: "rotta per {destination}" -> ["rotta","per"], "destination", [].
    struct Template {
        std::vector<std::string> before, after;
        std::string slot; // vuoto = frase senza slot
    };

    // Le parole del "verso" di un comando: accendere e spegnere, aprire e chiudere... (senza accenti).
    const std::set<std::string>& onWords() {
        static const std::set<std::string> w = { "accendi", "accendere", "accenda", "attiva", "attivare", "attivi", "alza", "alzare",
                                                 "apri", "aprire", "abilita", "abilitare", "avvia", "avviare", "inserisci", "arma",
                                                 "armare", "imposta", "impostare", "on" };
        return w;
    }
    const std::set<std::string>& offWords() {
        static const std::set<std::string> w = { "spegni", "spengi", "spegnere", "spenga", "disattiva", "disattivare", "disattivi",
                                                 "abbassa", "abbassare", "chiudi", "chiudere", "disabilita", "disabilitare", "ferma",
                                                 "fermare", "togli", "togliere", "disarma", "disarmare", "annulla", "annullare",
                                                 "cancella", "cancellare", "elimina", "eliminare", "rimuovi", "off" };
        return w;
    }
    // "spegnile", "riaccendilo", "alzali": il verbo senza il pronome attaccato e senza il "ri" davanti
    // ("spegni", "accendi", "alza"). Vuoto se non è un verso conosciuto.
    bool knownVerb(const std::string& x) { return onWords().count(x) > 0 || offWords().count(x) > 0; }

    std::string verbWithoutRi(const std::string& x) {
        if (knownVerb(x)) return x;
        if (x.size() > 4 && x.compare(0, 2, "ri") == 0 && knownVerb(x.substr(2))) return x.substr(2); // "riaccendi"
        return std::string();
    }

    bool endsWithClitic(const std::string& w) {
        if (w.size() <= 5) return false; // "alla", "palle": troppo corte per essere verbo + pronome
        for (const char* clitic : { "lo", "la", "li", "le", "ne" }) {
            if (w.compare(w.size() - 2, 2, clitic) == 0) return true;
        }
        return false;
    }

    std::string verbOf(const std::string& w) {
        const std::string v = verbWithoutRi(w);
        if (!v.empty()) return v;
        return endsWithClitic(w) ? verbWithoutRi(w.substr(0, w.size() - 2)) : std::string();
    }

    const std::set<std::string>& articles() {
        static const std::set<std::string> w = { "il", "lo", "la", "i", "gli", "le", "l", "un", "uno", "una", "del", "dello", "della",
                                                 "dei", "degli", "delle", "al", "allo", "alla", "ai", "agli", "alle", "the", "a" };
        return w;
    }

    // Quello su cui agisce la frase, senza verso e articoli: "disattiva gli scudi" -> "scudi".
    std::string objectOf(const std::vector<std::string>& w) {
        std::string out;
        for (const auto& x : w) {
            if (!verbOf(x).empty() || articles().count(x)) continue; // anche "spegnile", "riaccendi"
            out += (out.empty() ? "" : " ") + x;
        }
        return out;
    }

    // Prima di una parte fissa saltata non ci devono essere "non" o un verso contrario:
    // "annulla la rotta per Crusader" non è "rotta per Crusader".
    bool blocksSkip(const std::string& word) {
        return word == "non" || word == "no" || offWords().count(verbOf(word)) > 0;
    }

    // Le cifre di un nome di luogo, anche dette a parole: "cru l uno" -> "1", "area diciotto" -> "18".
    std::string digitsOf(const std::string& normalized) {
        static const std::map<std::string, std::string> numbers = {
            { "zero", "0" }, { "uno", "1" }, { "due", "2" }, { "tre", "3" }, { "quattro", "4" }, { "cinque", "5" }, { "sei", "6" },
            { "sette", "7" }, { "otto", "8" }, { "nove", "9" }, { "dieci", "10" }, { "undici", "11" }, { "dodici", "12" },
            { "tredici", "13" }, { "quattordici", "14" }, { "quindici", "15" }, { "sedici", "16" }, { "diciassette", "17" },
            { "diciotto", "18" }, { "diciannove", "19" }, { "venti", "20" }, { "one", "1" }, { "two", "2" }, { "three", "3" },
            { "four", "4" }, { "five", "5" }, { "eighteen", "18" },
        };
        std::string out;
        for (const auto& w : words(normalized)) {
            auto it = numbers.find(w);
            std::string d = it != numbers.end() ? it->second : std::string();
            if (d.empty()) {
                for (char c : w) {
                    if (c >= '0' && c <= '9') d += c;
                }
            }
            if (!d.empty()) out += (out.empty() ? "" : " ") + d;
        }
        return out;
    }

    Template parseTemplate(const std::string& phrase) {
        Template t;
        // Lo slot va estratto PRIMA di normalizzare (le graffe sparirebbero).
        size_t open = phrase.find('{'), close = phrase.find('}');
        if (open != std::string::npos && close != std::string::npos && close > open) {
            t.slot = phrase.substr(open + 1, close - open - 1);
            t.before = words(normalize(phrase.substr(0, open)));
            t.after = words(normalize(phrase.substr(close + 1)));
        } else {
            t.before = words(normalize(phrase));
        }
        return t;
    }
}

std::string normalize(const std::string& text) {
    std::wstring w = TextUtils::utf8ToWide(TextUtils::toLower(text));
    std::wstring out;
    bool space = false;
    for (wchar_t c : w) {
        c = fold(c);
        if (iswalnum(c)) {
            if (space && !out.empty()) out += L' ';
            out += c;
            space = false;
        } else {
            space = true; // apostrofi, trattini, punteggiatura: separano le parole
        }
    }
    return TextUtils::wideToUtf8(out);
}

double similarity(const std::string& a, const std::string& b) {
    if (a.empty() || b.empty()) return 0.0;
    if (a == b) return 1.0;
    const double lev = 1.0 - (double)levenshtein(a, b) / (double)std::max(a.size(), b.size());
    std::vector<std::string> wa = words(a), wb = words(b);
    std::set<std::string> sa(wa.begin(), wa.end()), sb(wb.begin(), wb.end());
    size_t inter = 0;
    for (const auto& x : sa) inter += sb.count(x);
    const double uni = (double)(sa.size() + sb.size() - inter);
    const double jac = uni > 0 ? inter / uni : 0.0;
    return std::max(lev, 0.45 * lev + 0.55 * jac);
}

std::string applyCorrections(const std::string& normalized, const std::map<std::string, std::string>& corrections) {
    if (corrections.empty()) return normalized;
    // Prima le correzioni più lunghe ("quantum and mode" prima di "quantum").
    std::vector<std::pair<std::string, std::string>> list;
    for (const auto& [from, to] : corrections) {
        std::string f = normalize(from);
        if (!f.empty()) list.push_back({ f, normalize(to) });
    }
    std::sort(list.begin(), list.end(), [](const auto& x, const auto& y) { return x.first.size() > y.first.size(); });
    std::string s = " " + normalized + " ";
    for (const auto& [from, to] : list) {
        const std::string key = " " + from + " ";
        size_t pos = 0;
        while ((pos = s.find(key, pos)) != std::string::npos) {
            s.replace(pos, key.size(), " " + to + " ");
            pos += to.size() + 1;
        }
    }
    return TextUtils::collapseSpaces(s);
}

std::string resolveAlias(const GameProfile& profile, const std::string& slot, const std::string& spoken, double* scoreOut) {
    auto it = profile.aliases.find(slot);
    if (scoreOut) *scoreOut = 0.0;
    if (it == profile.aliases.end()) return {};
    const std::string norm = normalize(spoken);
    const std::string spokenDigits = digitsOf(norm);
    std::string best;
    double bestScore = 0.0;
    for (const auto& [canonical, variants] : it->second) {
        std::vector<std::string> all = variants;
        all.push_back(canonical);
        for (const auto& v : all) {
            const std::string nv = normalize(v);
            // Numeri diversi, luoghi diversi: "Cru L2" non è "Cru-L1" (era finita lì, rotta sbagliata).
            const std::string variantDigits = digitsOf(nv);
            if (!spokenDigits.empty() && !variantDigits.empty() && spokenDigits != variantDigits) continue;
            double s = similarity(norm, nv);
            // Nei nomi corti due lettere diverse sono un altro posto: "cru l2" non è "arc l2" (0,67).
            if (std::max(norm.size(), nv.size()) <= 6 && s < 0.75) s = 0.0;
            // "stazione seraphim" contiene "seraphim": conta come quasi uguale.
            if (nv.size() >= 4 && (" " + norm + " ").find(" " + nv + " ") != std::string::npos) s = std::max(s, 0.9);
            if (s > bestScore) {
                bestScore = s;
                best = canonical;
            }
        }
    }
    if (scoreOut) *scoreOut = bestScore;
    return bestScore >= 0.66 ? best : std::string();
}

int polarity(const std::string& normalized) {
    bool on = false, off = false;
    for (const auto& w : words(normalized)) {
        const std::string v = verbOf(w); // anche "spegnile", "riaccendi", "alzali"
        on = on || onWords().count(v) > 0;
        off = off || offWords().count(v) > 0;
    }
    return on == off ? 0 : on ? 1 : -1;
}

bool hasPronounVerb(const std::string& normalized) {
    for (const auto& w : words(normalized)) {
        if (!knownVerb(w) && endsWithClitic(w) && !verbOf(w).empty()) return true;
    }
    return false;
}

namespace {
    Match matchWhole(const GameProfile& profile, const std::string& spoken) {
        Match best;
        const GameCommand* bestCommand = nullptr;
        const std::string norm = applyCorrections(normalize(spoken), profile.corrections);
        if (norm.empty()) return best;
        const std::vector<std::string> said = words(norm);
        const int saidPolarity = polarity(norm);
        const std::string saidObject = objectOf(said);

        for (const auto& command : profile.commands) {
            for (const auto& phrase : command.phrases) {
                const Template t = parseTemplate(phrase);
                // I contrari non si confondono mai: "disattiva gli scudi" non è "alza gli scudi",
                // "chiudi la mappa" non è "apri la mappa", per quanto le lettere si somiglino.
                const int phrasePolarity = polarity(join(t.before, 0, t.before.size()) + " " + join(t.after, 0, t.after.size()));
                if (saidPolarity != 0 && phrasePolarity != 0 && saidPolarity != phrasePolarity) continue;
                Match m;
                m.phrase = phrase;
                if (t.slot.empty()) {
                    m.score = similarity(norm, join(t.before, 0, t.before.size()));
                    // Lo stesso oggetto con un altro verso dello stesso senso: "disattiva gli scudi" e
                    // "abbassa gli scudi", "spegni i motori" e "motori". Solo per i tasti singoli (sono
                    // interruttori): "spegni il quantum" non deve mai far partire un salto quantico.
                    const std::string phraseObject = objectOf(t.before);
                    if (command.intent == "press" && saidPolarity != 0 && saidObject.size() >= 4 && phraseObject.size() >= 4) {
                        const double objectScore = similarity(saidObject, phraseObject);
                        if (objectScore >= 0.85) m.score = std::max(m.score, 0.9 * objectScore);
                    }
                } else {
                    // Parte fissa prima e dopo lo slot: deve combaciare (o quasi), lo slot è quello che resta.
                    // whisper a volte storpia l'inizio ("Mi posso una rotta per Seraphim" era "imposta una
                    // rotta per"): la parte fissa si cerca anche dopo fino a tre parole in più, con uno sconto.
                    bool any = false;
                    Match bestHere;
                    for (size_t skip = 0; skip <= 3; ++skip) {
                        if (said.size() <= skip + t.before.size() + t.after.size()) break;
                        if (skip > 0 && (t.before.empty() || blocksSkip(said[skip - 1]))) break;
                        const std::string head = join(said, skip, skip + t.before.size());
                        const std::string tail = join(said, said.size() - t.after.size(), said.size());
                        const std::string wantHead = join(t.before, 0, t.before.size());
                        const std::string wantTail = join(t.after, 0, t.after.size());
                        const double headScore = wantHead.empty() ? 1.0 : similarity(head, wantHead);
                        const double tailScore = wantTail.empty() ? 1.0 : similarity(tail, wantTail);
                        if (headScore < 0.62 || tailScore < 0.62) continue;
                        // Un verso nella parte detta che la frase non ha: "annulla la rotta per X" non è
                        // "traccia la rotta per X", anche se le lettere si somigliano.
                        const int headPolarity = polarity(head);
                        if (headPolarity != 0 && headPolarity != polarity(wantHead)) continue;
                        Match c;
                        c.phrase = phrase;
                        // Frasi con una parte fissa corta ("destinazione X") valgono un po' meno.
                        const double fixedWords = (double)(t.before.size() + t.after.size());
                        c.score = (headScore == 1.0 && tailScore == 1.0) ? 0.95 : 0.55 + 0.35 * std::min(headScore, tailScore);
                        if (fixedWords < 2) c.score -= 0.03;
                        c.score -= 0.05 * (double)skip;
                        const std::string value = join(said, skip + t.before.size(), said.size() - t.after.size());
                        double aliasScore = 0.0;
                        const std::string canonical = resolveAlias(profile, t.slot, value, &aliasScore);
                        if (!canonical.empty()) {
                            c.slots[t.slot] = canonical;
                            c.score = std::min(1.0, c.score * (0.75 + 0.25 * aliasScore));
                        } else {
                            // Luogo sconosciuto: si prova lo stesso, con le iniziali maiuscole (come nel gioco).
                            std::string pretty;
                            for (const auto& w : words(value)) {
                                if (!pretty.empty()) pretty += ' ';
                                std::string x = w;
                                if (!x.empty() && x[0] >= 'a' && x[0] <= 'z') x[0] = (char)(x[0] - 'a' + 'A');
                                pretty += x;
                            }
                            c.slots[t.slot] = pretty;
                            c.score -= 0.05;
                        }
                        if (!any || c.score > bestHere.score) {
                            bestHere = c;
                            any = true;
                        }
                    }
                    if (!any) continue;
                    m = bestHere;
                }
                if (m.score > best.score) {
                    best = m;
                    bestCommand = &command;
                }
            }
        }
        if (bestCommand) best.command = *bestCommand;
        best.found = bestCommand && best.score >= profile.matchThreshold;
        best.heard = norm;
        return best;
    }
}

Match match(const GameProfile& profile, const std::string& spoken) {
    Match best = matchWhole(profile, spoken);
    if (best.found) return best;
    // "Spegni le cose, come si chiamano? Spegni i motori.": quando ci si corregge vale l'ultima frase.
    std::string s = TextUtils::trim(spoken);
    while (!s.empty() && std::string(".?!;, ").find(s.back()) != std::string::npos) s.pop_back();
    const size_t cut = s.find_last_of(".?!;");
    if (cut == std::string::npos) return best;
    const std::string last = TextUtils::trim(s.substr(cut + 1));
    if (normalize(last).empty()) return best;
    Match tail = matchWhole(profile, last);
    return tail.found ? tail : best;
}

std::string ocrFold(const std::string& text) {
    const std::string n = normalize(text);
    std::string out;
    out.reserve(n.size());
    for (size_t i = 0; i < n.size(); ++i) {
        char c = n[i];
        if (c == '1' || c == 'i') c = 'l';
        else if (c == '0') c = 'o';
        else if (c == '5') c = 's';
        else if (c == '8') c = 'b';
        else if (c == 'r' && i + 1 < n.size() && n[i + 1] == 'n') {
            out += 'm';
            ++i;
            continue;
        }
        out += c;
    }
    return out;
}

double ocrContains(const std::string& read, const std::string& wanted) {
    const std::string r = ocrFold(read), w = ocrFold(wanted);
    if (r.empty() || w.empty()) return 0.0;
    auto compact = [](std::string x) {
        x.erase(std::remove(x.begin(), x.end(), ' '), x.end());
        return x;
    };
    const std::string rc = compact(r), wc = compact(w);
    if (wc.size() >= 3 && rc.find(wc) != std::string::npos) return 1.0;
    double best = similarity(r, w);
    // Il pezzo di parole lette più simile (una parola in più o in meno di quelle cercate).
    const std::vector<std::string> rw = words(r);
    const size_t n = words(w).size();
    for (size_t k = std::max<size_t>(1, n > 0 ? n - 1 : 1); k <= n + 1; ++k) {
        for (size_t i = 0; i + k <= rw.size(); ++i) best = std::max(best, similarity(join(rw, i, i + k), w));
    }
    // E senza spazi: l'OCR a volte li toglie o li aggiunge ("CRU-L1" letto "CRUL1").
    if (wc.size() >= 4) {
        for (size_t len = wc.size() - 1; len <= wc.size() + 1; ++len) {
            for (size_t i = 0; i + len <= rc.size(); ++i) {
                const std::string piece = rc.substr(i, len);
                best = std::max(best, 1.0 - (double)levenshtein(piece, wc) / (double)std::max(piece.size(), wc.size()));
            }
        }
    }
    return best;
}

int bestText(const std::vector<std::string>& candidates, const std::string& target, double threshold, double* scoreOut) {
    const std::string t = normalize(target), tf = ocrFold(target);
    int best = -1;
    double bestScore = 0.0;
    for (size_t i = 0; i < candidates.size(); ++i) {
        const std::string c = normalize(candidates[i]);
        if (c.empty()) continue;
        const std::string cf = ocrFold(candidates[i]);
        // Anche con le lettere che l'OCR confonde unite: "CRU-LI" è "Cru-L1", "ANNULIA" è "ANNULLA".
        double s = std::max(similarity(c, t), similarity(cf, tf));
        // L'OCR legge la riga intera ("STAZIONE ORBITALE SERAPHIM 12 KM"): se contiene il nome, va bene.
        if (t.size() >= 4 && (c.find(t) != std::string::npos || cf.find(tf) != std::string::npos)) s = std::max(s, 0.9);
        if (s > bestScore) {
            bestScore = s;
            best = (int)i;
        }
    }
    if (scoreOut) *scoreOut = bestScore;
    return bestScore >= threshold ? best : -1;
}

} // namespace Cmd
