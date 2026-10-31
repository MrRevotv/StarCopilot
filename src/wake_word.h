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
namespace WakeWord {

    struct Match {
        bool found = false;
        std::string command; // il resto della frase, ripulito (vuoto = solo "Iris")
    };

    Match detect(const std::string& transcript);

    // "iris", "Iris!", "airis" (come la sente whisper a volte)...: la parola è "Iris"?
    bool isWakeToken(const std::string& word);

    // "basta", "stop", "zitta"...: smetti di parlare, senza chiedere al modello.
    bool isStopCommand(const std::string& command);

    // Minuscolo, solo lettere e cifre separate da uno spazio ("Ehi, Iris!" -> "ehi iris").
    std::string normalize(const std::string& text);

} // namespace WakeWord
