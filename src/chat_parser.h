#pragma once

#include <string>
#include <vector>

// Un messaggio della chat di gioco letto dall'OCR.
struct ChatMessage {
    std::vector<std::string> tags; // canali/organizzazioni tra quadre, es. {"TUTTO", "SDR"}
    std::string playerName;        // es. "Tadzyk"; vuoto = non letto (con tags) o coda di un messaggio cominciato sopra la zona (senza)
    std::string rawPrefix;         // l'inizio della riga come l'ha letto l'OCR, es. "[TUTTO][SDR]Tadzyk:"
    std::string body;              // il testo, con le righe andate a capo già riunite

    // "[TUTTO][SDR] Tadzyk: " (ripulito), "[GLOBALE] " se il nome non si legge, "" per una coda.
    std::string displayPrefix() const;
};

// Divide le righe lette dall'OCR in messaggi. Formato della chat di Star
// Citizen (e di molti altri giochi), con o senza pallino davanti:
//   • [SQUADRA]Trime98: testo
//   [TUTTO][SDR]Tadzyk: testo
//   [TEAM][ANZ] Invictus: testo
//   Invictus: testo
// Una riga che non comincia così è il seguito (andato a capo) del messaggio
// sopra. I canali con nome noto (GLOBALE, GLOBAL, PARTY, GRUPPO, GROUP, SQUAD,
// LSE) si riconoscono anche con le quadre lette male ("tGLOBALEl Foxbat:")
// e, se le quadre si vedono, anche senza i due punti dopo il nome. A sinistra
// della "[" nella chat non c'è niente: quello che l'OCR legge lì si taglia.
// Funzioni pure: provate in tests/tests.cpp.
namespace ChatParser {

    // Se la riga è l'inizio di un messaggio, riempie out (body = il resto della riga).
    bool parseHeader(const std::string& line, ChatMessage& out);

    // Nessun messaggio riconosciuto = non è una chat (un menu, un cartello...):
    // chi chiama tradurrà il testo così com'è.
    std::vector<ChatMessage> parse(const std::vector<std::string>& lines);

    // Il messaggio è del canale/organizzazione scritto dall'utente (es. "LSE")?
    // Tollerante agli errori dell'OCR sulle quadre ("tLSEJ", "(LSE)").
    bool matchesFilter(const ChatMessage& message, const std::string& filterTag);

    // Quante parole di una lettura sono rovinate dall'OCR: simboli dentro la
    // parola ("wh*o", "perse&ring"), maiuscole in mezzo ("sTlts"), cifre tra
    // lettere ("fy3afft"), lettere accentate dentro parole inglesi ("flightèuits").
    // Se words non è nullo riceve quante parole ci sono.
    int garbledWords(const std::string& text, int* words = nullptr);

    // Così rovinata che tradurla darebbe solo altre parole a caso.
    bool unreadable(const std::string& text);

} // namespace ChatParser
