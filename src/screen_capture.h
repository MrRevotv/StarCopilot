#pragma once

#include <cstdint>
#include <vector>

// Cattura di una zona dello schermo (GDI), pronta per l'OCR di Windows
// (pixel BGRA, righe dall'alto verso il basso).
namespace ScreenCapture {

    struct Image {
        std::vector<uint8_t> pixels; // BGRA
        int width = 0;
        int height = 0;
        bool empty() const { return pixels.empty(); }
    };

    // x,y,w,h in coordinate schermo assolute (anche su più monitor).
    Image capture(int x, int y, int width, int height);

    // Per l'OCR: ingrandisce (il testo di chat è piccolo: ingrandito si legge
    // molto meglio, parentesi quadre comprese), lo porta in scala di grigi, ne
    // alza il contrasto e toglie lo sfondo dietro le lettere (pianeti, luci).
    // maxDimension: lato massimo accettato dall'OCR.
    Image prepareForOcr(const Image& source, int scale, int maxDimension);

    // Impronta veloce del contenuto: se la zona non è cambiata, l'OCR si salta.
    uint64_t hash(const Image& image);

} // namespace ScreenCapture
