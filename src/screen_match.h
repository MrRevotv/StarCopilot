#pragma once

#include <string>
#include <vector>

// Riconoscere lo stesso messaggio della chat in due letture dell'OCR. Un
// messaggio resta a schermo per minuti e ogni lettura ha errori diversi:
// lettere sbagliate ("tolling", "tolljng", "tolliog"), il nome finito nel
// testo ("Kleran3464} hope thats you") o perso con la prima parola, una riga
// andata a capo in meno. Le chiavi sono il testo del messaggio passato da
// TextUtils::normalizeForMatch.
namespace ScreenMatch {

    // Lo stesso testo con al più qualche lettera letta male. Nei messaggi
    // cortissimi nessuna: "same" e "rude", "ok" e "on" sono messaggi diversi.
    bool sameText(const std::string& a, const std::string& b);

    // Le stesse cifre, nello stesso ordine: "need 2 guys" non è "need 3 guys".
    bool sameNumbers(const std::string& a, const std::string& b);

    // Quanto due letture sembrano lo stesso messaggio (0..1), contando anche
    // una lettura che è la fine dell'altra (il nome finito nel testo) o
    // l'inizio (una riga persa). Da 0.7 in su: probabilmente lo stesso.
    double likeness(const std::string& a, const std::string& b);

    // Abbina i messaggi letti ora a quelli della lettura prima, nell'ordine
    // della chat: i messaggi nuovi arrivano solo in fondo, quindi tra due
    // messaggi riconosciuti ci sono gli stessi di prima, anche se letti
    // troppo male per riconoscerli. Per ogni messaggio di current, l'indice
    // in previous, o -1 se è nuovo.
    std::vector<int> align(const std::vector<std::string>& previous, const std::vector<std::string>& current);

} // namespace ScreenMatch
