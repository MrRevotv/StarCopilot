#pragma once

#include <string>
#include <vector>

// Dal testo del modello alla voce. Funzioni pure: provate in tests/tests.cpp.
namespace SpeechText {

    // Il modello scrive la risposta un pezzetto alla volta: appena una frase è
    // completa la si manda alla voce, così Iris comincia a parlare mentre il
    // resto è ancora da scrivere.
    class SentenceSplitter {
    public:
        // Aggiunge un pezzo di testo; ritorna le frasi diventate complete.
        std::vector<std::string> push(const std::string& piece);
        // Fine della risposta: quello che resta (anche senza punto finale).
        std::string flush();
        void reset() { m_buffer.clear(); }

    private:
        std::string m_buffer;
    };

    // Testo da leggere ad alta voce: via markdown (**, #, elenchi), link,
    // indirizzi web ed emoji; "°C" diventa "gradi", "km/h" "chilometri orari".
    std::string speakable(const std::string& text);

} // namespace SpeechText
