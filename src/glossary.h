#pragma once

#include <string>
#include <vector>
#include <utility>

// Glossario personale: gergo della community con la traduzione da usare
// sempre. File %appdata%\VoiceToChat\glossary.txt, una riga per termine:
//   termine originale=traduzione desiderata
// Righe vuote o che iniziano con # sono ignorate. Si rilegge a ogni frase:
// le modifiche valgono subito, senza riavviare.
namespace Glossary {

    using Entry = std::pair<std::string, std::string>; // {termine, traduzione}

    // Se il file non c'è, lo crea con un esempio commentato. Accetta file
    // salvati da Blocco note in UTF-8 (anche con BOM) o in ANSI.
    std::vector<Entry> load();

    // Solo le voci il cui termine compare nella frase (parola intera, senza
    // distinzione di maiuscole): al modello diamo solo quello che serve,
    // così il prompt resta corto e veloce.
    std::vector<Entry> relevantTo(const std::vector<Entry>& entries, const std::string& text);

    // I termini, separati da virgola, come suggerimento per whisper.
    std::string asWhisperHint(const std::vector<Entry>& entries);

    std::wstring filePath();

} // namespace Glossary
