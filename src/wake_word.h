#pragma once

#include <string>
#include <vector>

// La parola di attivazione "Iris" nel testo trascritto da whisper.
// Funzioni pure (niente stato, niente Windows a parte il minuscolo): provate in tests/tests.cpp.
//
// Si accetta:
//  - all'inizio: "Iris, che tempo fa?", "Ehi Iris apri Spotify", "Ok Iris...";
//  - alla fine: "Che ore sono, Iris?" (come si parla a una persona).
// NON a metà frase: "ieri ho parlato con Iris di..." detto a un amico non la sveglia.
// E nemmeno alla fine quando si parla DI lei: "ho aggiornato Iris", "parlavo con Iris",
// "la mia Iris" (senza virgola prima del nome).
// "Iris, Iris, carrello": il nome ripetuto si toglie.
namespace WakeWord {

    struct Match {
        bool found = false;
        std::string command; // il resto della frase, ripulito (vuoto = solo "Iris")
    };

    Match detect(const std::string& transcript);

    // "iris", "Iris!", "airis" (come la sente whisper a volte)...: la parola è "Iris"?
    bool isWakeToken(const std::string& word);

    // "basta", "stop", "zitta", "stai zitta!"...: smetti di parlare, senza chiedere al modello.
    bool isStopCommand(const std::string& command);

    // "Ehm.", "Mmm", "Boh": solo esitazioni, niente da chiedere.
    bool isHesitation(const std::string& text);
    // La frase è rimasta a metà: whisper la chiude con "..." ("Imposta la rotta per...").
    bool isUnfinished(const std::string& text);
    // Quante parole (dopo normalize).
    size_t wordCount(const std::string& text);

    // Minuscolo, solo lettere e cifre separate da uno spazio ("Ehi, Iris!" -> "ehi iris").
    std::string normalize(const std::string& text);

} // namespace WakeWord
