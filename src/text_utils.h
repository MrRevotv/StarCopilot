#pragma once

#include <string>
#include <vector>

// Piccoli attrezzi per il testo, usati ovunque. Tutte le stringhe del
// programma sono UTF-8 (std::string); le API di Windows vogliono UTF-16
// (std::wstring): si converte SEMPRE con queste due funzioni, mai con
// "std::wstring(s.begin(), s.end())", che rompe accenti e ideogrammi.
namespace TextUtils {

    std::wstring utf8ToWide(const std::string& utf8);
    std::string wideToUtf8(const std::wstring& wide);

    std::string trim(const std::string& s);
    // Spazi, a capo e tabulazioni consecutivi diventano uno spazio solo.
    std::string collapseSpaces(const std::string& s);
    // Minuscolo (anche per le lettere accentate, via Windows).
    std::string toLower(const std::string& s);

    bool startsWith(const std::string& s, const std::string& prefix);
    bool containsIgnoreCase(const std::string& haystack, const std::string& needle);

    // Il termine compare nel testo come parola intera (non dentro un'altra:
    // "Ita" non deve combaciare con "Italia"), senza distinzione di maiuscole.
    bool containsWord(const std::string& text, const std::string& term);

    // Se l'LLM restituisce tutta la frase in MAIUSCOLO (difetto occasionale),
    // la riporta a "Prima lettera maiuscola, resto minuscolo". Le frasi già
    // normali (anche con acronimi come "UEE") restano com'erano.
    std::string fixShoutingCase(const std::string& text);

    // Lettere accentate/speciali che la tastiera potrebbe non avere, sostituite
    // con quella "semplice" più vicina (ñ -> n, ü -> u, ß -> ss, « -> ").
    // Per i caratteri senza equivalente (ideogrammi...) ritorna stringa vuota.
    std::wstring transliterate(wchar_t c);

    // Chiave per riconoscere lo stesso messaggio letto due volte dall'OCR con
    // piccole differenze: minuscolo, solo lettere e cifre, spazi compressi.
    std::string normalizeForMatch(const std::string& s);

    // Somiglianza 0..1 tra due stringhe (1 = identiche), da distanza di edit.
    double similarity(const std::string& a, const std::string& b);

    std::vector<std::string> splitLines(const std::string& s);

} // namespace TextUtils
