#pragma once

#include "screen_capture.h"

#include <string>
#include <vector>

// OCR integrato in Windows (Windows.Media.Ocr, via C++/WinRT): offline, niente
// da scaricare. Serve il pacchetto della lingua installato in Windows
// (Impostazioni -> Ora e lingua -> Lingua e area geografica).
namespace OcrEngineWrapper {

    // Una riga di testo con la sua posizione nell'immagine (pixel).
    struct Line {
        std::string text;
        int x = 0, y = 0, width = 0, height = 0;
    };

    // Le lingue OCR installate in Windows (codici come "it", "en-US").
    std::vector<std::string> installedLanguages();

    // C'è un OCR per questa lingua? (es. "pl", "ru"). Se no, si usa quello
    // delle lingue del profilo di Windows.
    bool isLanguageSupported(const std::string& code);

    int maxImageDimension();

    // Le righe in ordine di lettura (dall'alto). Le parti di una stessa riga
    // che l'OCR separa (per esempio "[SQUADRA]" e "Nome: testo" se c'è uno
    // spazio largo) vengono riunite. error riceve il motivo se fallisce.
    std::vector<Line> recognize(const std::string& languageCode, const ScreenCapture::Image& image, std::string& error);

    // Righe -> testo con un "a capo" tra una riga e l'altra.
    std::string joinLines(const std::vector<Line>& lines);

} // namespace OcrEngineWrapper
