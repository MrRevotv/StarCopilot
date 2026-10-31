#include "language_guess.h"
#include "text_utils.h"

#include <windows.h>

#include <set>

namespace {

    // Parole frequenti solo in inglese. Fuori quelle uguali in altre lingue:
    // "i" e "a" (polacco "e"), "to" e "on" (polacco), "me", "no", "in", "do", "so".
    const std::set<std::wstring>& englishWords() {
        static const std::set<std::wstring> words = {
            L"the", L"you", L"your", L"you're", L"youre", L"i'm", L"im", L"i've", L"i'll", L"i'd", L"is", L"are", L"was",
            L"were", L"be", L"been", L"am", L"does", L"did", L"don't", L"dont", L"doesn't", L"didn't", L"have", L"has",
            L"had", L"will", L"would", L"can", L"can't", L"cant", L"could", L"should", L"what", L"where", L"when", L"who",
            L"why", L"how", L"this", L"that", L"that's", L"thats", L"there", L"these", L"those", L"it's", L"its", L"it",
            L"of", L"at", L"for", L"with", L"from", L"by", L"and", L"or", L"but", L"not", L"just", L"if", L"then", L"my",
            L"him", L"her", L"them", L"us", L"our", L"they", L"we", L"he", L"she", L"get", L"got", L"go", L"going",
            L"gonna", L"wanna", L"need", L"want", L"know", L"think", L"thought", L"like", L"all", L"any", L"some",
            L"about", L"up", L"out", L"now", L"too", L"very", L"yes", L"yeah", L"yea", L"nah", L"ok", L"okay", L"hey",
            L"hi", L"hello", L"guys", L"anyone", L"someone", L"thanks", L"thank", L"please", L"pls", L"plz", L"lol",
            L"lmao", L"gg", L"ty", L"thx", L"u", L"ur", L"np", L"omw", L"brb", L"afk", L"here", L"more", L"much", L"one",
            L"see", L"back", L"good", L"bad", L"really", L"still", L"also", L"only", L"looking", L"send", L"help",
            L"ship", L"anybody", L"everyone", L"which", L"wont", L"won't", L"cool", L"nice",
        };
        return words;
    }

    // Parole frequenti nelle altre lingue dei server: italiano, polacco, tedesco,
    // spagnolo, francese, portoghese.
    const std::set<std::wstring>& otherWords() {
        static const std::set<std::wstring> words = {
            // italiano (non "come": "come on" è inglese)
            L"il", L"lo", L"gli", L"che", L"di", L"della", L"del", L"per", L"sono", L"non", L"una", L"uno", L"ciao",
            L"cosa", L"perché", L"perche", L"ho", L"hai", L"mi", L"ti", L"ci", L"ma", L"anche", L"io", L"tu",
            L"noi", L"voi", L"questo", L"questa", L"quello", L"grazie", L"raga", L"ragazzi", L"dove", L"quando", L"sei",
            L"siamo", L"molto", L"tutto", L"tutti", L"bene", L"però", L"poi", L"già", L"qui", L"qua", L"andiamo",
            L"vado", L"fatto", L"fare", L"vuoi", L"tua", L"tuo", L"mio", L"mia", L"nella", L"sul", L"alla",
            // polacco
            L"nie", L"jest", L"się", L"sie", L"tak", L"czy", L"jak", L"ja", L"jestem", L"jesteś", L"jestes", L"mam",
            L"masz", L"tylko", L"już", L"juz", L"dzięki", L"dzieki", L"dobra", L"dobrze", L"gdzie", L"kto", L"mnie",
            L"tego", L"że", L"ze", L"ale", L"dla", L"dawaj", L"siema", L"cześć", L"czesc", L"kurwa",
            // tedesco
            L"ich", L"du", L"der", L"das", L"und", L"nicht", L"ist", L"ein", L"eine", L"mit", L"auf", L"nein", L"bin",
            L"bist", L"wir", L"ihr", L"mein", L"dein", L"hallo", L"danke", L"gut", L"wie", L"auch", L"noch", L"schon",
            L"bitte",
            // spagnolo e portoghese
            L"el", L"los", L"las", L"que", L"es", L"por", L"para", L"con", L"hola", L"gracias", L"pero", L"como",
            L"donde", L"yo", L"estoy", L"esta", L"muy", L"bien", L"vamos", L"amigo", L"não", L"nao", L"obrigado",
            L"você", L"voce", L"tudo", L"muito", L"aqui", L"isso", L"sim", L"então", L"eu", L"ele", L"ela",
            // francese
            L"le", L"les", L"et", L"est", L"je", L"pas", L"une", L"des", L"pour", L"avec", L"oui", L"merci",
            L"bonjour", L"salut", L"mais", L"nous", L"vous", L"c'est", L"ça", L"moi", L"toi",
        };
        return words;
    }

    bool latinWithAccent(wchar_t c) { return c >= 0xC0 && c <= 0x24F && c != 0xD7 && c != 0xF7; }
    bool otherScript(wchar_t c) { return c >= 0x370 && IsCharAlphaW(c); } // greco, cirillico, ideogrammi...
}

namespace LanguageGuess {

bool isEnglish(const std::string& text) {
    std::wstring t = TextUtils::utf8ToWide(TextUtils::toLower(text));
    int letters = 0, foreignScript = 0;
    for (wchar_t& c : t) {
        if (c == 0x2019) c = L'\''; // l'apostrofo tipografico
        if (IsCharAlphaW(c)) {
            letters++;
            if (otherScript(c)) foreignScript++;
        }
    }
    if (letters == 0) return false;
    if (foreignScript * 2 > letters) return false; // cirillico, giapponese...

    // Le parole: lettere e apostrofi, il resto separa.
    int english = 0;
    double other = 0;
    std::wstring word;
    auto flush = [&]() {
        while (!word.empty() && word.back() == L'\'') word.pop_back();
        while (!word.empty() && word.front() == L'\'') word.erase(word.begin());
        if (word.empty()) return;
        if (englishWords().count(word)) english++;
        else if (otherWords().count(word)) other += 1;
        else {
            bool accent = false;
            for (wchar_t c : word) accent = accent || latinWithAccent(c);
            // Lettere che l'inglese non ha, o i gruppi del polacco ("przyda", "szybko").
            if (accent || word.find(L"rz") != std::wstring::npos || word.find(L"cz") != std::wstring::npos ||
                word.find(L"sz") != std::wstring::npos) {
                other += 0.5;
            }
        }
        word.clear();
    };
    for (wchar_t c : t) {
        if (IsCharAlphaW(c) || c == L'\'') word += c;
        else flush();
    }
    flush();

    if (english == 0 && other == 0) return true; // nessun indizio: la lingua dei server
    return english > other;
}

bool hasCjk(const std::string& text) {
    for (wchar_t c : TextUtils::utf8ToWide(text)) {
        if ((c >= 0x3040 && c <= 0x30FF) || (c >= 0x3400 && c <= 0x9FFF) || (c >= 0xAC00 && c <= 0xD7AF) ||
            (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFF00 && c <= 0xFFEF)) {
            return true;
        }
    }
    return false;
}

} // namespace LanguageGuess
