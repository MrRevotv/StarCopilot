#pragma once

#include <string>
#include <vector>
#include <utility>

// Come si chiede una traduzione al modello (Qwen2.5 Instruct, formato ChatML).
// Il prompt è diviso in due:
//  - il PREFISSO (istruzioni + esempi): dipende solo da lingue e tono, quindi
//    il motore lo elabora una volta sola e lo tiene in memoria;
//  - la RICHIESTA: la frase da tradurre, più i nomi da non toccare e le voci
//    del glossario che compaiono in QUELLA frase.
// Gli esempi insegnano al modello che una domanda va tradotta, non risposta
// ("come stai?" -> "how are you?", mai "I'm fine").
// Funzioni pure: provate in tests/tests.cpp.

struct TranslationRequest {
    std::string text;
    std::string sourceLanguage;  // nome inglese (es. "Italian"); vuoto = riconoscila tu
    std::string targetLanguage;  // nome inglese (es. "English")
    bool gamingTone = true;      // gergo da videogiocatore (voce); false = neutro
    bool fromScreen = false;     // messaggio letto dalla chat (OCR), scritto da un altro giocatore
    std::vector<std::pair<std::string, std::string>> glossary; // solo le voci presenti nel testo
    std::vector<std::string> keepNames;                         // nomi propri presenti nel testo
};

namespace TranslationPrompt {

    // Istruzioni ed esempi (cambiano solo se cambiano lingue/tono/modalità).
    std::string prefix(const TranslationRequest& request);

    // Il turno con la frase da tradurre, e l'inizio della risposta.
    std::string request(const TranslationRequest& request);

    // Ripulisce la risposta del modello: una riga sola, senza virgolette,
    // senza "Traduzione:" davanti, senza i segni <text>. Se non resta nulla,
    // ritorna il testo originale (meglio l'originale che un messaggio vuoto).
    std::string cleanOutput(const std::string& raw, const std::string& original);

} // namespace TranslationPrompt
