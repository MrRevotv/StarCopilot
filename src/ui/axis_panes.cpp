#include "axis_panes.h"
#include "../text_utils.h"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <map>
#include <regex>

namespace Gdiplus {
    using std::max;
    using std::min;
}
#include <objidl.h>
#include <gdiplus.h>

using namespace Gdiplus;
using json = nlohmann::json;

namespace AxisDraw {

namespace {
    constexpr float PAD = 10, CORNER = 7, TITLE_GAP = 8;
    constexpr float ROW_GAP = 6, CELL_GAP = 5, HAT_SIZE = 46, HAT_GAP = 10;
    constexpr int TRAIL_SAMPLES = 26;

    struct Rgb {
        BYTE r = 0, g = 0, b = 0;
    };

    Rgb parseColor(const std::string& hex) {
        Rgb c;
        if (hex.size() >= 7 && hex[0] == '#') {
            const unsigned long v = std::strtoul(hex.c_str() + 1, nullptr, 16);
            c.r = (BYTE)((v >> 16) & 255);
            c.g = (BYTE)((v >> 8) & 255);
            c.b = (BYTE)(v & 255);
        }
        return c;
    }

    Color withAlpha(Rgb c, double alpha) { return Color((BYTE)std::lround(std::clamp(alpha, 0.0, 1.0) * 255), c.r, c.g, c.b); }

    Color contrast(Rgb c) {
        const double lum = (0.299 * c.r + 0.587 * c.g + 0.114 * c.b) / 255.0;
        return lum > 0.55 ? Color(255, 0x04, 0x18, 0x1C) : Color(255, 255, 255, 255);
    }

    // Gli oggetti GDI+ tenuti per thread non si distruggono MAI: alla fine del thread
    // principale GDI+ è già spento (Ui::shutdown), e distruggerli dopo fa cadere il
    // programma in chiusura. Sono pochi e li libera Windows con il processo.

    // Le scritte in monospazio, come Axis Visualizer. Un carattere per misura e peso, tenuto.
    const Font& monoFont(float px, bool bold) {
        thread_local auto* cache = new std::map<std::pair<int, bool>, Font*>();
        const auto key = std::make_pair((int)std::lround(px * 10), bold);
        auto it = cache->find(key);
        if (it != cache->end()) return *it->second;
        FontFamily consolas(L"Consolas");
        const FontFamily* family = consolas.IsAvailable() ? &consolas : FontFamily::GenericMonospace();
        Font* font = new Font(family, px, bold ? FontStyleBold : FontStyleRegular, UnitPixel);
        (*cache)[key] = font;
        return *font;
    }

    Graphics& measuring() {
        thread_local auto* bitmap = new Bitmap(1, 1, PixelFormat32bppARGB);
        thread_local auto* g = new Graphics(bitmap);
        return *g;
    }

    const StringFormat* typographic() {
        thread_local StringFormat* format = [] {
            auto* f = new StringFormat(StringFormat::GenericTypographic());
            f->SetFormatFlags(f->GetFormatFlags() | StringFormatFlagsMeasureTrailingSpaces | StringFormatFlagsNoWrap);
            return f;
        }();
        return format;
    }

    std::wstring w(const std::string& s) { return TextUtils::utf8ToWide(s); }

    std::wstring upper(std::wstring s) {
        for (auto& c : s) c = (wchar_t)std::towupper(c);
        return s;
    }

    std::wstring percent(double v) {
        const long n = std::lround(v * 100);
        return (n > 0 ? L"+" : L"") + std::to_wstring(n) + L"%";
    }

    double approach(double current, double target, double smoothing, double dt) {
        if (smoothing <= 0.001) return target;
        const double tau = smoothing * 0.12;
        return current + (target - current) * (1 - std::exp(-dt / tau));
    }

    double fade(double heat, bool active, double dt, double tau = 0.16) { return active ? 1 : std::max(0.0, heat * std::exp(-dt / tau) - dt * 0.2); }

    // --- Pennelli ---------------------------------------------------------------------------------

    void line(Graphics& g, float x1, float y1, float x2, float y2, Color c, bool dash = false) {
        Pen pen(c, 1.0f);
        if (dash) {
            const REAL pattern[] = { 4, 3 };
            pen.SetDashPattern(pattern, 2);
        }
        g.DrawLine(&pen, x1, y1, x2, y2);
    }

    void strokeRect(Graphics& g, float x, float y, float w, float h, Color c) {
        Pen pen(c, 1.0f);
        g.DrawRectangle(&pen, x, y, w, h);
    }

    void fillRect(Graphics& g, float x, float y, float w, float h, Color c) {
        SolidBrush b(c);
        g.FillRectangle(&b, x, y, w, h);
    }

    void circle(Graphics& g, float x, float y, float r, const Color* fill, const Color* stroke) {
        r = std::max(0.0f, r);
        if (fill) {
            SolidBrush b(*fill);
            g.FillEllipse(&b, x - r, y - r, 2 * r, 2 * r);
        }
        if (stroke) {
            Pen p(*stroke, 1.0f);
            g.DrawEllipse(&p, x - r, y - r, 2 * r, 2 * r);
        }
    }
}

PaneView::PaneView(const Axis::Pane& pane, const Axis::Settings& settings, const std::string& fallbackTitle)
    : m_pane(pane), m_settings(settings), m_source(pane, settings) {
    m_title = upper(w(TextUtils::trim(pane.label).empty() ? fallbackTitle : pane.label));
    m_scale = settings.textSize / 11.0f;
    m_source.update(0.0);
    layout();
}

float PaneView::measure(const std::wstring& text) const {
    if (text.empty()) return 0;
    RectF box;
    measuring().MeasureString(text.c_str(), (INT)text.size(), &monoFont((float)m_settings.textSize, m_settings.boldText), PointF(0, 0),
                              typographic(), &box);
    return box.Width;
}

std::wstring PaneView::axisTag(int index, const wchar_t* fallback) const {
    auto it = m_pane.axisLabels.find(index);
    return it != m_pane.axisLabels.end() ? upper(w(it->second)) : std::wstring(fallback);
}

std::wstring PaneView::rowName(int index) const {
    auto it = m_pane.axisLabels.find(index);
    if (it != m_pane.axisLabels.end()) return upper(w(it->second));
    const auto& names = m_source.state().axisNames;
    return index < (int)names.size() ? upper(w(names[index])) : L"ASSE " + std::to_wstring(index + 1);
}

std::wstring PaneView::caption(const std::string& name, int index, int labelKey) const {
    auto it = m_pane.buttonLabels.find(labelKey);
    if (it != m_pane.buttonLabels.end()) return upper(w(it->second));
    const std::string trimmed = TextUtils::trim(name);
    static const std::regex generic(R"(^(pulsante|button|btn|bouton|taste)\s*\d*$)", std::regex::icase);
    if (trimmed.empty() || std::regex_match(trimmed, generic)) return std::to_wstring(index + 1);
    return upper(w(trimmed));
}

void PaneView::layout() {
    const float innerWidth = std::max(40.0f, m_pane.size - 2 * PAD);
    const float titleHeight = std::round(m_settings.textSize + 2.0f);
    m_titleBand = m_settings.showTitles && !m_title.empty() ? titleHeight + TITLE_GAP : 0;
    m_titleWidth = m_titleBand > 0 ? measure(m_title) : 0;
    float content = 0;
    const auto& state = m_source.state();

    if (m_pane.kind == Axis::Kind::Assi) {
        const float barRow = std::round(26 * m_scale), valueRow = std::round(16 * m_scale);
        const bool hasBar = m_pane.axisZ >= 0;
        content = innerWidth + (hasBar ? barRow : 0) + (m_pane.showValues ? valueRow : 0);
        const float readout = measure(L"-100%"), tag = measure(axisTag(m_pane.axisZ, L"Z"));
        m_gutter = std::max(readout, std::min(tag, innerWidth * 0.30f));
    } else if (m_pane.kind == Axis::Kind::Barre) {
        const int available = (int)state.axes.size();
        m_barAxes.clear();
        if (!m_pane.barAxes.empty()) {
            for (int i : m_pane.barAxes) {
                if (i >= 0 && i < available) m_barAxes.push_back(i);
            }
        } else {
            for (int i = 0; i < available; ++i) m_barAxes.push_back(i);
        }
        m_barValues.assign(m_barAxes.size(), 0.0);
        const float rowH = std::round(18 * m_scale);
        content = m_barAxes.empty() ? rowH : m_barAxes.size() * rowH + (m_barAxes.size() - 1) * ROW_GAP;
    } else {
        const auto& names = state.buttonNames;
        const bool keyboard = m_pane.deviceId == "keyboard";
        m_buttonIndices.clear();
        m_labelKeys.clear();
        if (!keyboard && !m_pane.buttons.empty()) {
            for (int i : m_pane.buttons) {
                if (i >= 0 && i < (int)names.size()) m_buttonIndices.push_back(i);
            }
        } else {
            for (int i = 0; i < (int)names.size(); ++i) m_buttonIndices.push_back(i);
        }
        if (keyboard) {
            std::vector<int> keys;
            for (int k : m_pane.buttons) {
                if (k > 0) keys.push_back(k);
            }
            for (int i : m_buttonIndices) m_labelKeys.push_back(i < (int)keys.size() ? keys[i] : i);
        } else {
            m_labelKeys = m_buttonIndices;
        }
        m_heat.assign(m_buttonIndices.size(), 0.0);
        m_hatCount = m_pane.showHat ? (int)state.hats.size() : 0;
        m_hasWheel = m_pane.deviceId == "mouse";

        const float cellH = std::round(24 * m_scale), minCellW = std::round(26 * m_scale);
        m_chips.clear();
        m_rowsHeight = 0;
        if (!m_buttonIndices.empty()) {
            float fixedWidth = 0;
            if (m_pane.buttonColumns > 0) {
                const int cols = m_pane.buttonColumns;
                fixedWidth = std::max(minCellW, (innerWidth - CELL_GAP * (cols - 1)) / cols);
            }
            float x = 0, y = 0;
            for (size_t i = 0; i < m_buttonIndices.size(); ++i) {
                const int index = m_buttonIndices[i];
                Chip chip;
                chip.source = index;
                chip.text = caption(index < (int)names.size() ? names[index] : "", index, m_labelKeys[i]);
                const float cellW = fixedWidth > 0 ? fixedWidth : std::max(minCellW, measure(chip.text) + 14);
                if (x > 0 && x + cellW > innerWidth + 0.5f) {
                    x = 0;
                    y += cellH + CELL_GAP;
                }
                chip.x = x;
                chip.y = y;
                chip.w = cellW;
                chip.h = cellH;
                m_chips.push_back(chip);
                x += cellW + CELL_GAP;
            }
            m_rowsHeight = y + cellH;
        }
        content = m_rowsHeight;
        if (m_hatCount > 0 || m_hasWheel) content += (content > 0 ? CELL_GAP : 0) + HAT_SIZE;
        content = std::max(cellH, content);
    }
    m_width = innerWidth + 2 * PAD;
    m_height = 2 * PAD + m_titleBand + content;
}

void PaneView::tick(double dt) {
    const size_t buttonsBefore = m_source.state().buttonNames.size(), axesBefore = m_source.state().axes.size();
    m_source.update(dt);
    const auto& s = m_source.state();
    // Periferica appena collegata (o scollegata): cambiano assi e pulsanti, si rifanno le misure.
    if (s.buttonNames.size() != buttonsBefore || s.axes.size() != axesBefore) layout();
    const double smoothing = m_settings.smoothing;
    if (m_pane.kind == Axis::Kind::Assi) {
        m_x = approach(m_x, s.x, smoothing, dt);
        m_y = approach(m_y, s.y, smoothing, dt);
        m_z = approach(m_z, s.z.value_or(0.0), smoothing, dt);
        if (m_pane.trailSeconds > 0.01) {
            // La scia campiona a intervalli fissi: stessa lunghezza a qualsiasi frequenza.
            m_trailClock += dt;
            if (m_trailClock >= m_pane.trailSeconds / TRAIL_SAMPLES) {
                m_trailClock = 0;
                m_trail.insert(m_trail.begin(), { m_x, m_y });
                if (m_trail.size() > TRAIL_SAMPLES) m_trail.resize(TRAIL_SAMPLES);
            }
        }
    } else if (m_pane.kind == Axis::Kind::Barre) {
        for (size_t i = 0; i < m_barAxes.size(); ++i) {
            const int index = m_barAxes[i];
            m_barValues[i] = approach(m_barValues[i], index < (int)s.axes.size() ? s.axes[index] : 0.0, smoothing, dt);
        }
    } else {
        for (size_t i = 0; i < m_buttonIndices.size(); ++i) {
            const int index = m_buttonIndices[i];
            m_heat[i] = fade(m_heat[i], index < (int)s.buttons.size() && s.buttons[index], dt);
        }
        if (m_hasWheel) {
            m_wheelUp = std::max(std::max(0.0, s.wheel), fade(m_wheelUp, false, dt));
            m_wheelDown = std::max(std::max(0.0, -s.wheel), fade(m_wheelDown, false, dt));
        }
    }
}

void PaneView::values(std::vector<double>& out) const {
    out.clear();
    if (m_pane.kind == Axis::Kind::Assi) {
        out = { m_x, m_y, m_z };
    } else if (m_pane.kind == Axis::Kind::Barre) {
        out = m_barValues;
    } else {
        out = m_heat;
        const auto& hats = m_source.state().hats;
        for (int i = 0; i < m_hatCount; ++i) out.push_back(i < (int)hats.size() ? hats[i] : -1);
        if (m_hasWheel) {
            out.push_back(m_wheelUp);
            out.push_back(m_wheelDown);
        }
    }
}

json PaneView::describe() const {
    const Rgb bg = parseColor(m_settings.backgroundColor);
    (void)bg;
    json j;
    j["kind"] = m_pane.kind == Axis::Kind::Assi ? "assi" : m_pane.kind == Axis::Kind::Barre ? "barre" : "pulsanti";
    j["w"] = m_width;
    j["h"] = m_height;
    j["title"] = m_titleBand > 0 ? TextUtils::wideToUtf8(m_title) : "";
    j["align"] = m_pane.align == "Destra" ? "right" : m_pane.align == "Centro" ? "center" : "left";
    j["frame"] = m_settings.showFrames;
    j["grid"] = m_settings.showGrid;
    j["titleBand"] = m_titleBand;
    j["titleWidth"] = m_titleWidth;
    j["bg"] = m_settings.backgroundColor;
    j["bgAlpha"] = 1.0;
    j["accent"] = m_settings.accentColor;
    j["dot"] = m_pane.dotColor;
    j["barColor"] = m_pane.barColor;
    if (m_pane.kind == Axis::Kind::Assi) {
        const bool hasBar = m_pane.axisZ >= 0;
        j["dotRadius"] = m_settings.dotRadius;
        j["hasBar"] = hasBar;
        j["showValues"] = m_pane.showValues;
        j["trailSeconds"] = m_pane.trailSeconds > 0.01 ? m_pane.trailSeconds : 0.0;
        j["barGutter"] = hasBar ? m_gutter : 0.0f;
        if (hasBar) j["barTag"] = TextUtils::wideToUtf8(axisTag(m_pane.axisZ, L"Z"));
        j["tags"] = { TextUtils::wideToUtf8(axisTag(m_pane.axisX, L"X")), TextUtils::wideToUtf8(axisTag(m_pane.axisY, L"Y")) };
    } else if (m_pane.kind == Axis::Kind::Barre) {
        json rows = json::array();
        for (int index : m_barAxes) rows.push_back(TextUtils::wideToUtf8(rowName(index)));
        j["rows"] = rows;
    } else {
        json chips = json::array();
        for (const auto& c : m_chips) chips.push_back({ { "x", c.x }, { "y", c.y }, { "w", c.w }, { "h", c.h }, { "text", TextUtils::wideToUtf8(c.text) } });
        j["chips"] = chips;
        j["rowsHeight"] = m_rowsHeight;
        j["hats"] = m_hatCount;
        j["wheel"] = m_hasWheel;
    }
    return j;
}

// --- Disegno ----------------------------------------------------------------------------------------

namespace {
    struct Theme {
        Color bg, dot, dotSoft, bar, accent, text, muted, inverted, frame, rule, track, cell, grid, barDash;
    };

    Theme themeFor(const Axis::Settings& s, const Axis::Pane& p) {
        const Rgb bg = parseColor(s.backgroundColor), accent = parseColor(s.accentColor), dot = parseColor(p.dotColor), bar = parseColor(p.barColor);
        Theme t;
        t.bg = withAlpha(bg, 1.0);
        t.dot = withAlpha(dot, 1.0);
        t.dotSoft = withAlpha(dot, 0.2);
        t.bar = withAlpha(bar, 1.0);
        t.accent = withAlpha(accent, 1.0);
        t.text = Color(255, 255, 255, 255);
        t.muted = Color(153, 255, 255, 255);
        t.inverted = contrast(dot);
        t.frame = withAlpha(accent, 0.4);
        t.rule = withAlpha(accent, 0.2);
        t.track = Color(61, 255, 255, 255);
        t.cell = withAlpha(accent, 0.35);
        t.grid = Color(77, 255, 255, 255);
        t.barDash = withAlpha(bar, 0.5);
        return t;
    }

    void text(Graphics& g, const std::wstring& s, float x, float y, const Font& font, Color c, bool middle = false) {
        if (s.empty()) return;
        SolidBrush brush(c);
        if (middle) y -= font.GetHeight(&g) / 2.0f;
        g.DrawString(s.c_str(), (INT)s.size(), &font, PointF(x, y), typographic(), &brush);
    }

    float widthOf(Graphics& g, const std::wstring& s, const Font& font) {
        if (s.empty()) return 0;
        RectF box;
        g.MeasureString(s.c_str(), (INT)s.size(), &font, PointF(0, 0), typographic(), &box);
        return box.Width;
    }

    std::wstring ellipsis(Graphics& g, const std::wstring& s, const Font& font, float max) {
        if (widthOf(g, s, font) <= max) return s;
        std::wstring cut = s;
        while (cut.size() > 1 && widthOf(g, cut + L"…", font) > max) cut.pop_back();
        return cut + L"…";
    }
}

void PaneView::draw(Graphics& g, float ox, float oy) const {
    const Theme t = themeFor(m_settings, m_pane);
    const Font& font = monoFont((float)m_settings.textSize, m_settings.boldText);
    fillRect(g, ox, oy, m_width, m_height, t.bg);

    if (m_settings.showFrames) {
        strokeRect(g, ox + 0.5f, oy + 0.5f, std::max(1.0f, m_width - 1), std::max(1.0f, m_height - 1), t.rule);
        const float x0 = ox + 0.5f, y0 = oy + 0.5f, x1 = ox + m_width - 0.5f, y1 = oy + m_height - 0.5f;
        line(g, x0, y0, x0 + CORNER, y0, t.frame);
        line(g, x0, y0, x0, y0 + CORNER, t.frame);
        line(g, x1, y0, x1 - CORNER, y0, t.frame);
        line(g, x1, y0, x1, y0 + CORNER, t.frame);
        line(g, x0, y1, x0 + CORNER, y1, t.frame);
        line(g, x0, y1, x0, y1 - CORNER, t.frame);
        line(g, x1, y1, x1 - CORNER, y1, t.frame);
        line(g, x1, y1, x1, y1 - CORNER, t.frame);
    }

    float top = oy + PAD;
    const float areaW = m_width - 2 * PAD;
    if (m_titleBand > 0) {
        const float left = ox + PAD;
        float x = left;
        if (m_pane.align == "Destra") x = left + areaW - m_titleWidth;
        else if (m_pane.align == "Centro") x = left + (areaW - m_titleWidth) / 2;
        x = std::max(left, x);
        text(g, m_title, x, top, font, t.accent);
        const float ruleY = std::round(top + font.GetHeight(&g) / 2) + 0.5f;
        if (x > left + 6) line(g, left, ruleY, x - 6, ruleY, t.rule);
        if (x + m_titleWidth + 6 < left + areaW) line(g, x + m_titleWidth + 6, ruleY, left + areaW, ruleY, t.rule);
        top += m_titleBand;
    }
    if (areaW <= 0) return;
    if (m_pane.kind == Axis::Kind::Assi) drawAxes(g, ox + PAD, top, areaW);
    else if (m_pane.kind == Axis::Kind::Barre) drawBars(g, ox + PAD, top, areaW);
    else drawButtons(g, ox + PAD, top, areaW);
}

void PaneView::drawAxes(Graphics& g, float ax, float ay, float side) const {
    const Theme t = themeFor(m_settings, m_pane);
    const Font& font = monoFont((float)m_settings.textSize, m_settings.boldText);
    const float dotR = (float)m_settings.dotRadius;
    const float cx = ax + side / 2, cy = ay + side / 2, reach = side / 2 - dotR;
    strokeRect(g, ax + 0.5f, ay + 0.5f, side - 1, side - 1, t.rule);
    if (m_settings.showGrid) {
        line(g, cx, ay + 4, cx, ay + side - 4, t.grid, true);
        line(g, ax + 4, cy, ax + side - 4, cy, t.grid, true);
    }
    // Tacche a un quarto, a metà e a tre quarti: la scala del riquadro.
    for (int i = 1; i <= 3; ++i) {
        const float f = i / 4.0f, len = i == 2 ? 6.0f : 3.0f;
        const float x = ax + side * f, y = ay + side * f;
        line(g, x, ay, x, ay + len, t.frame);
        line(g, x, ay + side, x, ay + side - len, t.frame);
        line(g, ax, y, ax + len, y, t.frame);
        line(g, ax + side, y, ax + side - len, y, t.frame);
    }
    const float dotX = cx + (float)m_x * reach, dotY = cy + (float)m_y * reach;
    if (m_pane.trailSeconds > 0.01) {
        for (int i = (int)m_trail.size() - 1; i >= 0; --i) {
            const float weight = 1.0f - i / (float)TRAIL_SAMPLES, r = dotR * 0.9f * weight;
            if (r < 0.7f) continue;
            circle(g, cx + (float)m_trail[i].first * reach, cy + (float)m_trail[i].second * reach, r, &t.dotSoft, nullptr);
        }
    }
    line(g, ax + 1, dotY, ax + side - 1, dotY, t.track);
    line(g, dotX, ay + 1, dotX, ay + side - 1, t.track);
    circle(g, dotX, dotY, dotR * 1.9f, &t.dotSoft, nullptr);
    circle(g, dotX, dotY, dotR, &t.dot, nullptr);

    float next = ay + side;
    if (m_pane.axisZ >= 0) {
        const float barRow = std::round(26 * m_scale);
        const std::wstring readout = percent(m_z);
        const float readoutW = widthOf(g, readout, font);
        const std::wstring tag = ellipsis(g, axisTag(m_pane.axisZ, L"Z"), font, m_gutter);
        const float center = ax + side / 2, half = std::max(5.0f, side / 2 - m_gutter - 8);
        const float x0 = center - half, x1 = center + half, y = std::round(next + barRow / 2) + 0.5f;
        line(g, x0, y, x1, y, t.barDash, true);
        for (float x : { x0, center, x1 }) line(g, x, y - 4, x, y + 4, t.frame);
        const float value = center + (float)m_z * half, fillW = std::fabs(value - center);
        if (fillW > 0) fillRect(g, std::min(center, value), y - 2, fillW, 4, t.bar);
        circle(g, value, y, 5, &t.bar, nullptr);
        text(g, tag, ax, y, font, t.muted, true);
        text(g, readout, ax + side - readoutW, y, font, t.muted, true);
        next += barRow;
    }
    if (m_pane.showValues) {
        const std::wstring content = axisTag(m_pane.axisX, L"X") + L" " + percent(m_x) + L"   " + axisTag(m_pane.axisY, L"Y") + L" " + percent(m_y);
        text(g, content, ax, next, font, t.muted);
    }
}

void PaneView::drawBars(Graphics& g, float ax, float ay, float aw) const {
    const Theme t = themeFor(m_settings, m_pane);
    const Font& font = monoFont((float)m_settings.textSize, m_settings.boldText);
    if (m_barAxes.empty()) {
        text(g, L"NESSUN ASSE", ax, ay, font, t.muted);
        return;
    }
    const float rowH = std::round(18 * m_scale), valueW = std::round(44 * m_scale), nameW = std::round(96 * m_scale);
    const float nameWidth = std::min(nameW, std::max(40.0f, aw * 0.35f));
    const float trackLeft = ax + nameWidth + 8, trackWidth = std::max(20.0f, ax + aw - valueW - 6 - trackLeft);
    for (size_t i = 0; i < m_barAxes.size(); ++i) {
        const float top = ay + i * (rowH + ROW_GAP), value = (float)m_barValues[i];
        text(g, ellipsis(g, rowName(m_barAxes[i]), font, nameWidth), ax, top + rowH / 2, font, t.muted, true);
        const float y = std::round(top + rowH / 2) + 0.5f, center = trackLeft + trackWidth / 2;
        line(g, trackLeft, y, trackLeft + trackWidth, y, t.track);
        line(g, center, y - 5, center, y + 5, t.frame);
        line(g, trackLeft, y - 3, trackLeft, y + 3, t.frame);
        line(g, trackLeft + trackWidth, y - 3, trackLeft + trackWidth, y + 3, t.frame);
        const float end = center + value * (trackWidth / 2), fillW = std::fabs(end - center);
        if (fillW >= 1) fillRect(g, std::min(center, end), y - 3, fillW, 6, t.dot);
        fillRect(g, end - 1.5f, y - 6, 3, 12, t.dot);
        const std::wstring readout = percent(value);
        text(g, readout, ax + aw - widthOf(g, readout, font), top + rowH / 2, font, t.text, true);
    }
}

void PaneView::drawButtons(Graphics& g, float ax, float ay, float aw) const {
    const Theme t = themeFor(m_settings, m_pane);
    const Font& font = monoFont((float)m_settings.textSize, m_settings.boldText);
    if (m_chips.empty() && m_hatCount == 0 && !m_hasWheel) {
        text(g, L"NESSUN PULSANTE SCELTO", ax, ay, font, t.muted);
        return;
    }
    for (size_t i = 0; i < m_chips.size(); ++i) {
        const Chip& c = m_chips[i];
        const float x = ax + c.x, y = ay + c.y;
        strokeRect(g, x + 0.5f, y + 0.5f, c.w - 1, c.h - 1, t.cell);
        const float tw = widthOf(g, c.text, font);
        text(g, c.text, x + (c.w - tw) / 2, y + c.h / 2, font, t.muted, true);
        const double heat = i < m_heat.size() ? m_heat[i] : 0.0;
        if (heat <= 0.001) continue;
        // La casella accesa sta sopra a quella spenta e sfuma nel rilascio.
        const BYTE a = (BYTE)std::lround(std::min(1.0, heat) * 255);
        fillRect(g, x, y, c.w, c.h, Color(a, t.dot.GetR(), t.dot.GetG(), t.dot.GetB()));
        text(g, c.text, x + (c.w - tw) / 2, y + c.h / 2, font, Color(a, t.inverted.GetR(), t.inverted.GetG(), t.inverted.GetB()), true);
    }
    const float top = ay + m_rowsHeight + (m_rowsHeight > 0 ? CELL_GAP : 0);
    float left = ax;
    const auto& hats = m_source.state().hats;
    for (int i = 0; i < m_hatCount; ++i) {
        const int angle = i < (int)hats.size() ? hats[i] : -1;
        const float cx = left + HAT_SIZE / 2, cy = top + HAT_SIZE / 2, ring = HAT_SIZE / 2 - 6;
        circle(g, cx, cy, ring, nullptr, &t.rule);
        circle(g, cx, cy, 3, nullptr, &t.cell);
        for (int step = 0; step < 8; ++step) {
            const int degrees = step * 45;
            const double rad = (degrees - 90) * 3.14159265358979 / 180.0;
            const float x = cx + (float)std::cos(rad) * ring, y = cy + (float)std::sin(rad) * ring;
            const bool active = angle >= 0 && std::abs(((angle - degrees + 540) % 360) - 180) < 1;
            if (active) circle(g, x, y, 5, &t.dot, nullptr);
            else circle(g, x, y, 3, nullptr, &t.cell);
        }
        left += HAT_SIZE + HAT_GAP;
    }
    if (m_hasWheel) {
        const float cx = left + HAT_SIZE / 2, cy = top + HAT_SIZE / 2;
        strokeRect(g, cx - 7, cy - 11, 14, 22, t.cell);
        line(g, cx, cy - 4, cx, cy + 4, t.cell);
        auto arrow = [&](float tipY, bool up, double heat) {
            const float dir = up ? 1.0f : -1.0f;
            PointF pts[3] = { PointF(cx, tipY), PointF(cx - 6, tipY + 8 * dir), PointF(cx + 6, tipY + 8 * dir) };
            Pen pen(t.cell, 1.0f);
            g.DrawPolygon(&pen, pts, 3);
            if (heat <= 0.001) return;
            SolidBrush b(Color((BYTE)std::lround(std::min(1.0, heat) * 255), t.dot.GetR(), t.dot.GetG(), t.dot.GetB()));
            g.FillPolygon(&b, pts, 3);
        };
        arrow(top + 1, true, m_wheelUp);
        arrow(top + HAT_SIZE - 1, false, m_wheelDown);
    }
}

} // namespace AxisDraw
