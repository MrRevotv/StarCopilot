#pragma once

#include "../axis_config.h"
#include "../axis_input.h"

#include <windows.h>

#include <nlohmann/json.hpp>

#include <memory>
#include <string>
#include <vector>

namespace Gdiplus {
    class Graphics;
}

// Axis Visualizer dentro Iris: un modulo (ASSI, BARRE o PULSANTI) con la sua
// periferica, il movimento (smorzamento, scia, pulsanti che sfumano) e il
// disegno. Le misure sono quelle di Axis Visualizer (PaneVisual e
// overlay.html): lo stesso modulo viene uguale a schermo, nell'anteprima della
// pagina e nella sorgente Browser di OBS.
namespace AxisDraw {

    class PaneView {
    public:
        PaneView(const Axis::Pane& pane, const Axis::Settings& settings, const std::string& fallbackTitle);

        // Legge la periferica e fa avanzare il movimento.
        void tick(double deltaSeconds);
        // Disegna il modulo con l'angolo in alto a sinistra in (x, y), senza l'opacità globale.
        void draw(Gdiplus::Graphics& g, float x, float y) const;
        // La descrizione per la pagina (OBS e anteprima): PaneLayout di Axis Visualizer.
        nlohmann::json describe() const;
        // I numeri del fotogramma, nell'ordine che la pagina si aspetta.
        void values(std::vector<double>& out) const;

        float width() const { return m_width; }
        float height() const { return m_height; }
        const Axis::InputState& input() const { return m_source.state(); }

    private:
        struct Chip {
            int source = 0; // indice del pulsante nella periferica
            float x = 0, y = 0, w = 0, h = 0;
            std::wstring text;
        };

        void layout();
        float measure(const std::wstring& text) const;
        std::wstring axisTag(int index, const wchar_t* fallback) const;
        std::wstring rowName(int index) const;
        std::wstring caption(const std::string& name, int index, int labelKey) const;

        void drawAxes(Gdiplus::Graphics& g, float x, float y, float w) const;
        void drawBars(Gdiplus::Graphics& g, float x, float y, float w) const;
        void drawButtons(Gdiplus::Graphics& g, float x, float y, float w) const;

        Axis::Pane m_pane;
        const Axis::Settings m_settings;
        Axis::Source m_source;
        std::wstring m_title;
        float m_width = 0, m_height = 0, m_titleBand = 0, m_titleWidth = 0;
        float m_scale = 1; // misura delle scritte / 11

        // ASSI
        double m_x = 0, m_y = 0, m_z = 0;
        std::vector<std::pair<double, double>> m_trail;
        double m_trailClock = 0;
        float m_gutter = 0;
        // BARRE
        std::vector<int> m_barAxes;
        std::vector<double> m_barValues;
        // PULSANTI
        std::vector<int> m_buttonIndices, m_labelKeys;
        std::vector<Chip> m_chips;
        std::vector<double> m_heat;
        float m_rowsHeight = 0;
        int m_hatCount = 0;
        bool m_hasWheel = false;
        double m_wheelUp = 0, m_wheelDown = 0;
    };

} // namespace AxisDraw
