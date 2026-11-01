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

    // --- Zona ruotata ------------------------------------------------------------
    // L'HUD di Star Citizen segue la curva del casco: la chat può essere un po'
    // storta. La zona è il rettangolo dritto x,y,w,h girato di `degrees` attorno
    // al suo centro (positivo = senso orario sullo schermo).
    struct Point {
        double x = 0, y = 0;
    };
    struct Bounds {
        int x = 0, y = 0, width = 0, height = 0;
    };
    // I quattro angoli: in alto a sinistra, in alto a destra, in basso a destra, in basso a sinistra.
    std::vector<Point> rotatedCorners(int x, int y, int width, int height, double degrees);
    // Il rettangolo dritto che contiene tutta la zona ruotata.
    Bounds rotatedBounds(int x, int y, int width, int height, double degrees);
    // Il contenuto della zona ruotata, raddrizzato (largo width, alto height).
    // Con un angolo quasi nullo è la cattura normale.
    Image captureRotated(int x, int y, int width, int height, double degrees);
    // Raddrizza: il rettangolo width×height centrato in (centerX, centerY) di
    // source e girato di degrees, campionato (bilineare) in un'immagine dritta.
    // Fuori da source: nero.
    Image straighten(const Image& source, double centerX, double centerY, int width, int height, double degrees);

    // Per l'OCR: ingrandisce (il testo di chat è piccolo: ingrandito si legge
    // molto meglio, parentesi quadre comprese), lo porta in scala di grigi, ne
    // alza il contrasto e toglie lo sfondo dietro le lettere (pianeti, luci).
    // maxDimension: lato massimo accettato dall'OCR.
    Image prepareForOcr(const Image& source, int scale, int maxDimension);

    // Impronta veloce del contenuto: se la zona non è cambiata, l'OCR si salta.
    uint64_t hash(const Image& image);

} // namespace ScreenCapture
