#pragma once

#include "screen_capture.h"

#include <cstdint>
#include <string>
#include <vector>

// La pulizia dell'immagine di mining_ocr (preprocessor.py), rifatta in C++
// senza OpenCV, per leggere la signature dello scanner di Star Citizen:
//  1. trova il "badge" (segnaposto + numero) dentro la zona catturata, con
//     qualsiasi colore dell'HUD e sfondi complicati (asteroidi, nebulose):
//     bordi (Canny) + pixel chiari (Otsu e 80° percentile), dilatazione
//     orizzontale che unisce icona e cifre, blob con il punteggio migliore
//     (proporzioni, densità di bordi, luminosità, posizione);
//  2. toglie l'icona del segnaposto a sinistra (proiezione delle colonne:
//     un blocco a sinistra separato dalle cifre da uno spazio largo);
//  3. altezza 120 px (bicubico), grigi, CLAHE, Otsu, apertura 2×2.
// Al posto di EasyOCR si usa l'OCR di Windows (vedi mining_scanner).
namespace MiningImage {

    struct Gray {
        std::vector<uint8_t> p;
        int w = 0, h = 0;
        bool empty() const { return p.empty() || w <= 0 || h <= 0; }
        uint8_t at(int x, int y) const { return p[(size_t)y * w + x]; }
    };

    struct Box {
        int x = 0, y = 0, w = 0, h = 0;
    };

    Gray toGray(const ScreenCapture::Image& bgra);
    ScreenCapture::Image toBgra(const Gray& g);
    Gray crop(const Gray& g, const Box& b);
    ScreenCapture::Image crop(const ScreenCapture::Image& img, const Box& b);

    Box findBadge(const Gray& gray);
    // Colonna da cui cominciano le cifre (0 = nessuna icona da togliere).
    int iconCut(const Gray& cropped);

    Gray resizeToHeight(const Gray& g, int height);
    Gray clahe(const Gray& g, double clipLimit, int grid);
    int otsuThreshold(const Gray& g);
    Gray binarize(const Gray& g, int threshold); // > soglia = 255
    Gray morphOpen(const Gray& g, int k);
    Gray invert(const Gray& g);
    Gray pad(const Gray& g, int margin, uint8_t value);
    // Testo scuro su fondo chiaro (come piace all'OCR di Windows): si inverte se serve.
    Gray darkTextOnLight(const Gray& g);
    // La pulizia di Mining Signature Calculator: tutta la zona, ingrandita, grigi, contrasto ×2.
    // Serve quando il badge non si trova (zona molto stretta o HUD insolito).
    Gray simpleContrast(const ScreenCapture::Image& img, int scale);

    struct Steps {
        Gray original;
        Box badge;
        int iconCut = 0;
        Gray cropped;  // badge senza icona
        Gray resized;  // altezza fissa
        Gray enhanced; // CLAHE
        Gray binary;   // Otsu + apertura
    };
    Steps process(const ScreenCapture::Image& bgra, int targetHeight = 120);

    bool saveBmp(const std::wstring& path, const Gray& g);
    bool saveBmp(const std::wstring& path, const ScreenCapture::Image& img);

    // Immagine da file (PNG), per le prove. Vuota se non si legge.
    ScreenCapture::Image loadPng(const std::wstring& path);
    // zlib (con intestazione) -> byte. false se i dati sono rovinati.
    bool zlibInflate(const std::string& data, std::string& out);

} // namespace MiningImage
