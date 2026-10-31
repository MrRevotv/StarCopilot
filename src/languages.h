#pragma once

#include <string>
#include <vector>

// Lingue supportate: codice per whisper e per l'OCR di Windows, nome nella
// finestra (in italiano) e nome per l'LLM (in inglese: i modelli seguono le
// istruzioni in inglese in modo più affidabile, qualunque sia la coppia).
// L'ORDINE NON VA CAMBIATO: le impostazioni salvate usano la posizione.
struct LanguageOption {
    const char* code;           // es. "it"
    const wchar_t* displayName; // es. "Italiano"
    const char* englishName;    // es. "Italian"
};

inline const std::vector<LanguageOption>& supportedLanguages() {
    static const std::vector<LanguageOption> languages = {
        { "it", L"Italiano",    "Italian" },
        { "en", L"Inglese",     "English" },
        { "es", L"Spagnolo",    "Spanish" },
        { "fr", L"Francese",    "French" },
        { "de", L"Tedesco",     "German" },
        { "pt", L"Portoghese",  "Portuguese" },
        { "ru", L"Russo",       "Russian" },
        { "ja", L"Giapponese",  "Japanese" },
        { "zh", L"Cinese",      "Chinese" },
        { "pl", L"Polacco",     "Polish" },
        { "nl", L"Olandese",    "Dutch" },
        { "tr", L"Turco",       "Turkish" },
    };
    return languages;
}

// Indice valido (quelli salvati da versioni vecchie o file rovinati tornano a 0).
inline int clampLanguageIndex(int index) {
    int n = (int)supportedLanguages().size();
    return (index >= 0 && index < n) ? index : 0;
}
