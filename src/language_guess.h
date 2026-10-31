#pragma once

#include <string>

// Il messaggio letto dallo schermo è in inglese? Si traducono solo quelli:
// gli altri (italiano, polacco, tedesco...) restano come sono. Il modello,
// lasciato a indovinare, traduceva male le lingue che conosce poco
// ("ty jesteś pilotem?" -> "ti è stato assegnato un pilota?").
// Si contano le parole tipiche dell'inglese contro quelle delle lingue più
// frequenti nei server, più le lettere che l'inglese non ha (ą, ł, ñ, ß...).
// Senza indizi (es. "faaaah", "o7") vale inglese: è la lingua dei server.
// Funzioni pure: provate in tests/tests.cpp.
namespace LanguageGuess {

    bool isEnglish(const std::string& text);

    // Ideogrammi o caratteri giapponesi/coreani. Il modello a volte, davanti a
    // una lettura senza senso, finisce la risposta in cinese.
    bool hasCjk(const std::string& text);

} // namespace LanguageGuess
