#pragma once

#include <string>
#include <vector>

// Controlli sull'audio e sul testo trascritto, prima e dopo whisper.
// Funzioni pure (niente stato, niente Windows): provate in tests/tests.cpp.
namespace SpeechUtils {

    constexpr int kSampleRate = 16000;
    constexpr int kFrameSamples = kSampleRate / 50; // blocchi da 20 ms

    // Volume medio (RMS) di tutto il buffer.
    double rms(const std::vector<float>& samples);

    // Quanti secondi di "voce" ci sono: blocchi da 20 ms sopra la soglia.
    // Più affidabile della media: in una pressione lunga con una parola sola,
    // la media scende sotto la soglia anche se la parola c'è.
    double voicedSeconds(const std::vector<float>& samples, double threshold);

    // Toglie il silenzio prima e dopo il parlato (tenendo un margine di
    // marginMs per non tagliare l'inizio/la fine delle parole): whisper è più
    // veloce e inventa meno.
    std::vector<float> trimSilence(const std::vector<float>& samples, double threshold, int marginMs = 200);

    // Frasi che whisper "inventa" sul silenzio o sul rumore (le ha imparate
    // dai sottotitoli dei video: "Sottotitoli a cura di...", "Grazie per la
    // visione", "Thanks for watching"). true = da scartare.
    bool isLikelyHallucination(const std::string& text);

    // Pulisce la trascrizione: spazi, annotazioni tra parentesi ("[Musica]",
    // "(ride)"), simboli musicali. Ritorna stringa vuota se non resta nulla.
    std::string cleanTranscript(const std::string& text);

    // La frase sentita è la voce di Iris tornata dalle casse al microfono? heard e said
    // (le ultime frasi dette) già normalizzati: minuscolo, parole separate da uno spazio.
    // Vale anche con qualche parola capita male ("ciao rebo sono iris" per "Ciao REVO, sono
    // Iris"), purché le parole siano in fila come le ha dette lei. strict: sentita DOPO che
    // aveva finito di parlare (potrebbe essere la tua risposta con le sue parole: "sì,
    // imposta la rotta"), quindi solo se è proprio uguale.
    bool soundsLikeEcho(const std::string& heard, const std::vector<std::string>& said, bool strict);

} // namespace SpeechUtils
