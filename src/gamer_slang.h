#pragma once

#include <string>
#include <utility>
#include <vector>

// Il vocabolario del videogiocatore: sigle e gergo delle chat di gioco
// (gg, ty, omw, afk, push, feeddare...), usato in tutte e due le direzioni.
// - Mandare un messaggio: quello che si dice in italiano diventa la sigla
//   ("torno subito" -> "brb", solo col gergo da videogiocatore acceso), il
//   gergo italiano diventa quello inglese ("pushiamo" -> "push", sempre) e le
//   sigle dette restano sigle. Voci di glossario: il modello le segue alla lettera.
// - Leggere la chat: le sigle che il modello ha lasciato nella traduzione
//   diventano il loro significato. Non si suggeriscono al modello: provato,
//   le lasciava lo stesso, e in polacco ("ty" = "tu") sbagliava la frase.
// Funzioni pure: provate in tests/tests.cpp.
namespace GamerSlang {

    using Entry = std::pair<std::string, std::string>;

    // Per la voce: {come è detto nel testo, come scriverlo in chat}.
    // Le frasi italiane si riconoscono solo se si parla italiano.
    std::vector<Entry> forSending(const std::string& text, const std::string& sourceLanguage, bool gamingTone);

    // Per la chat degli altri, sulla traduzione già fatta: "omw, ty per l'aiuto"
    // -> "sto arrivando, grazie per l'aiuto". Solo verso l'italiano, e solo le
    // sigle che non sono anche parole normali ("mia", "push").
    std::string explain(const std::string& translated, const std::string& targetLanguage);

} // namespace GamerSlang
