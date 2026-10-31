#include "gamer_slang.h"
#include "text_utils.h"

#include <windows.h>

#include <cwctype>

namespace {

    struct Term {
        const char* chat;    // come si scrive in chat, più forme separate da "|": la prima è quella che si scrive
        const char* italian; // come si legge in italiano
        const char* phrases; // frasi italiane che diventano la sigla, col gergo da videogiocatore acceso
        const char* slang;   // gergo italiano dei giocatori, si traduce sempre; "detto=scritto" per i verbi
        bool common;         // la sigla è anche una parola normale ("mia", "push"): non si tocca
    };

    // Dal "vocabolario del videogiocatore". Non ci sono "W" e "L" da sole
    // (lettere singole: "L'ho visto"), solo "big w" e "take the l".
    const Term kTerms[] = {
        // Saluti, cortesie e fine partita
        { "gg", "gg", "bella partita", "", false },
        { "gg wp|ggwp", "gg wp", "bella partita ben giocata|ben giocata|ben giocato", "", false },
        { "glhf", "buona fortuna e divertitevi", "buona fortuna e divertitevi|buona fortuna e divertiamoci", "", false },
        { "gh", "bel primo tempo", "bel primo tempo", "", true },
        { "nt", "bel tentativo", "bel tentativo", "", false },
        { "ns", "bel colpo", "bel tiro|bel colpo", "", true },
        { "nc", "bella chiamata", "bella chiamata|bella intuizione", "", true },
        { "gr", "bel round", "bel round", "", true },
        { "ty|thx", "grazie", "grazie", "", false },
        { "yw", "prego", "di niente", "", false },
        { "np", "figurati", "nessun problema", "", false },
        { "gtg|g2g", "devo andare", "devo andare|devo scappare", "", false },
        { "brb", "torno subito", "torno subito", "", false },
        { "afk", "afk", "", "", false },
        // Comunicazioni tattiche
        { "ff", "ff", "arrendiamoci", "", false },
        { "mb", "colpa mia", "colpa mia", "", false },
        { "oom", "senza mana", "ho finito il mana", "", false },
        { "cd", "cd", "", "", false },
        { "ult", "ult", "", "", false },
        { "tp", "tp", "teletrasporto", "", false },
        { "omw", "sto arrivando", "sto arrivando|sono per strada|arrivo subito", "", false },
        { "mia", "nemico sparito", "nemico sparito|nemico scomparso", "", true },
        { "ss", "nemico sparito", "", "", true },
        { "ks", "ks", "", "", false },
        { "hp", "hp", "", "", false },
        { "1hp", "1hp", "", "", false },
        { "low", "quasi morto", "", "", true },
        { "cc", "cc", "", "", true },
        { "aoe", "aoe", "", "", false },
        { "dot", "dot", "", "", true },
        { "dps", "dps", "", "", false },
        // Direttive e comandi veloci
        { "push", "attaccare", "", "pushare|pushiamo|pusha|pushate|pushano|pushato=pushed|pushando=pushing", true },
        { "rotate", "spostarsi in un'altra zona", "", "", true },
        { "bait", "fare da esca", "", "baitare|baita|baitiamo|baitato=baited", true },
        { "gank", "imboscata", "", "gankare|ganka|gankiamo|gankato=ganked", false },
        { "peel", "proteggere un compagno", "", "", true },
        { "focus", "concentrare il fuoco", "", "focussare|focussate|focussiamo|focussa", true },
        { "hold", "tenere la posizione", "tenete la posizione|teniamo la posizione|tieni la posizione", "", true },
        { "fall back", "ritirarsi", "ritiriamoci|ritiratevi", "backare|backa|backate|backiamo|backo|backato=fell back", true },
        { "def", "difendiamo", "difendiamo", "", true },
        { "atk", "attacchiamo", "attacchiamo", "", false },
        { "req", "richiesta", "", "", true },
        // Reazioni e gergo sociale
        { "lol|lmao", "ahah", "", "", false },
        { "diff", "diff", "", "", false },
        { "ez", "ez", "", "", false },
        { "gdm", "gdm", "", "", false },
        { "smurf", "smurf", "", "smurfare|smurfa|smurfando=smurfing", false },
        { "noob", "noob", "", "nabbo|nabbi=noobs|niubbo|niubbi=noobs", false },
        { "throw", "throware", "", "throware|throwa|throwi|throwate|throwato=threw|throwando=throwing", true },
        { "feed", "feeddare", "", "feeddare|feedda|feeddi|feeddate|feeddato=fed|feeddando=feeding", true },
        { "tilted|tilt", "tiltato", "", "tiltato|tiltata|tiltati|tiltare=tilt|tilto", true },
        { "ragequit", "ragequit", "", "ragequittare|ragequitta|ragequittato", false },
        { "tryhard", "tryhard", "", "tryhardare|tryharda", false },
        { "clutch", "clutch", "", "clutchare|clutcha|clutchato=clutched", false },
        { "big w", "grande vittoria", "", "", false },
        { "take the l", "accetta la sconfitta", "", "", false },
    };

    std::vector<std::string> forms(const char* list) {
        std::vector<std::string> out;
        std::string cur;
        for (const char* p = list; *p; ++p) {
            if (*p == '|') {
                if (!cur.empty()) out.push_back(cur);
                cur.clear();
            } else {
                cur += *p;
            }
        }
        if (!cur.empty()) out.push_back(cur);
        return out;
    }

    // La prima forma dell'elenco presente nel testo (parola intera): {detta, scritta}.
    // "feeddando=feeding" -> {"feeddando", "feeding"}; senza "=" si scrive "write".
    GamerSlang::Entry firstFound(const std::string& text, const char* list, const std::string& write) {
        for (const auto& f : forms(list)) {
            const size_t eq = f.find('=');
            const std::string said = f.substr(0, eq);
            if (TextUtils::containsWord(text, said)) return { said, eq == std::string::npos ? write : f.substr(eq + 1) };
        }
        return {};
    }

    // "bella partita ben giocata" contiene "bella partita" e "ben giocata":
    // vale solo la frase più lunga.
    std::vector<GamerSlang::Entry> withoutOverlaps(const std::vector<GamerSlang::Entry>& found) {
        std::vector<GamerSlang::Entry> out;
        for (const auto& e : found) {
            bool inside = false;
            for (const auto& other : found) {
                if (other.first.size() > e.first.size() && TextUtils::containsWord(other.first, e.first)) inside = true;
            }
            if (!inside) out.push_back(e);
        }
        return out;
    }

    std::wstring lowered(std::wstring s) {
        if (!s.empty()) CharLowerBuffW(s.data(), (DWORD)s.size());
        return s;
    }

    // Ogni "term" (minuscolo) come parola intera diventa "with"; con la
    // maiuscola davanti ("Omw", "TY") anche la sostituzione comincia maiuscola.
    std::wstring replaceWord(std::wstring text, const std::wstring& term, const std::wstring& with) {
        auto isWordChar = [](wchar_t c) { return std::iswalnum(c) != 0 || c == L'_'; };
        std::wstring lower = lowered(text);
        size_t pos = 0;
        while ((pos = lower.find(term, pos)) != std::wstring::npos) {
            const size_t end = pos + term.size();
            if ((pos > 0 && isWordChar(lower[pos - 1])) || (end < lower.size() && isWordChar(lower[end]))) {
                pos = end;
                continue;
            }
            std::wstring rep = with;
            if (IsCharUpperW(text[pos])) CharUpperBuffW(rep.data(), 1);
            text.replace(pos, term.size(), rep);
            lower.replace(pos, term.size(), lowered(rep));
            pos += rep.size();
        }
        return text;
    }
}

namespace GamerSlang {

std::vector<Entry> forSending(const std::string& text, const std::string& sourceLanguage, bool gamingTone) {
    const bool italian = sourceLanguage == "Italian";
    std::vector<Entry> found;
    for (const Term& t : kTerms) {
        const std::string write = forms(t.chat).front();
        Entry e;
        if (italian) e = firstFound(text, t.slang, write);
        if (e.first.empty() && italian && gamingTone) e = firstFound(text, t.phrases, write);
        // Una sigla detta ("sono afk") resta una sigla.
        if (e.first.empty() && !t.common) {
            e = firstFound(text, t.chat, "");
            e.second = e.first;
        }
        if (!e.first.empty()) found.push_back(e);
    }
    return withoutOverlaps(found);
}

std::string explain(const std::string& translated, const std::string& targetLanguage) {
    if (targetLanguage != "Italian") return translated;
    std::wstring text = TextUtils::utf8ToWide(translated);
    for (const Term& t : kTerms) {
        if (t.common) continue;
        for (const auto& f : forms(t.chat)) {
            if (f != t.italian) text = replaceWord(text, TextUtils::utf8ToWide(f), TextUtils::utf8ToWide(t.italian));
        }
    }
    return TextUtils::wideToUtf8(text);
}

} // namespace GamerSlang
