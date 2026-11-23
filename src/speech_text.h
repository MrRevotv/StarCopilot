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

    // L'ultimo passo prima della voce (lo fa TtsEngine, per ogni motore): numeri in
    // lettere ("16.960" -> "sedicimila novecentosessanta", "2,5" -> "due virgola
    // cinque", "15%" -> "quindici per cento", "-12"), unità ("km", "m/s") e le sigle
    // che la voce neurale legge male ("SCU", "QT", "ATC", "x4"). I codici come "F7C",
    // "Area18", "UEC" li legge bene da sé e restano. La conversazione scritta (e il
    // controllo dell'eco) restano con le cifre.
    std::string forVoice(const std::string& text);

    // Un intero in lettere, in italiano ("ventitré", "duemila trecento", "un milione").
    std::string numberToWords(long long n);

} // namespace SpeechText
