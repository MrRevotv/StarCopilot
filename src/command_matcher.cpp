#include "command_matcher.h"
#include "text_utils.h"

#include <algorithm>
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
    std::string best;
    double bestScore = 0.0;
    for (const auto& [canonical, variants] : it->second) {
        std::vector<std::string> all = variants;
        all.push_back(canonical);
        for (const auto& v : all) {
            const std::string nv = normalize(v);
            double s = similarity(norm, nv);
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

Match match(const GameProfile& profile, const std::string& spoken) {
    Match best;
    const GameCommand* bestCommand = nullptr;
    const std::string norm = applyCorrections(normalize(spoken), profile.corrections);
    if (norm.empty()) return best;
    const std::vector<std::string> said = words(norm);

    for (const auto& command : profile.commands) {
        for (const auto& phrase : command.phrases) {
            const Template t = parseTemplate(phrase);
            Match m;
            m.phrase = phrase;
            if (t.slot.empty()) {
                m.score = similarity(norm, join(t.before, 0, t.before.size()));
            } else {
                // Parte fissa prima e dopo lo slot: deve combaciare (o quasi), lo slot è quello che resta.
                if (said.size() <= t.before.size() + t.after.size()) continue;
                const std::string head = join(said, 0, t.before.size());
                const std::string tail = join(said, said.size() - t.after.size(), said.size());
                const std::string wantHead = join(t.before, 0, t.before.size());
                const std::string wantTail = join(t.after, 0, t.after.size());
                const double headScore = wantHead.empty() ? 1.0 : similarity(head, wantHead);
                const double tailScore = wantTail.empty() ? 1.0 : similarity(tail, wantTail);
                if (headScore < 0.62 || tailScore < 0.62) continue;
                // Frasi con una parte fissa corta ("destinazione X") valgono un po' meno.
                const double fixedWords = (double)(t.before.size() + t.after.size());
                m.score = (headScore == 1.0 && tailScore == 1.0) ? 0.95 : 0.55 + 0.35 * std::min(headScore, tailScore);
                if (fixedWords < 2) m.score -= 0.03;
                const std::string value = join(said, t.before.size(), said.size() - t.after.size());
                double aliasScore = 0.0;
                const std::string canonical = resolveAlias(profile, t.slot, value, &aliasScore);
                if (!canonical.empty()) {
                    m.slots[t.slot] = canonical;
                    m.score = std::min(1.0, m.score * (0.75 + 0.25 * aliasScore));
                } else {
                    // Luogo sconosciuto: si prova lo stesso, con le iniziali maiuscole (come nel gioco).
                    std::string pretty;
                    for (const auto& w : words(value)) {
                        if (!pretty.empty()) pretty += ' ';
                        std::string x = w;
                        if (!x.empty() && x[0] >= 'a' && x[0] <= 'z') x[0] = (char)(x[0] - 'a' + 'A');
                        pretty += x;
                    }
                    m.slots[t.slot] = pretty;
                    m.score -= 0.05;
                }
            }
            if (m.score > best.score) {
                best = m;
                bestCommand = &command;
            }
        }
    }
    if (bestCommand) best.command = *bestCommand;
    best.found = bestCommand && best.score >= profile.matchThreshold;
    return best;
}

int bestText(const std::vector<std::string>& candidates, const std::string& target, double threshold, double* scoreOut) {
    const std::string t = normalize(target);
    int best = -1;
    double bestScore = 0.0;
    for (size_t i = 0; i < candidates.size(); ++i) {
        const std::string c = normalize(candidates[i]);
        if (c.empty()) continue;
        double s = similarity(c, t);
        // L'OCR legge la riga intera ("STAZIONE ORBITALE SERAPHIM 12 KM"): se contiene il nome, va bene.
        if (t.size() >= 4 && c.find(t) != std::string::npos) s = std::max(s, 0.9);
        if (s > bestScore) {
            bestScore = s;
            best = (int)i;
        }
    }
    if (scoreOut) *scoreOut = bestScore;
    return bestScore >= threshold ? best : -1;
}

} // namespace Cmd
