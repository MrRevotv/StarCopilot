#include "orb.h"
#include "ui.h"

#include <algorithm>
#include <cmath>

namespace Gdiplus {
    using std::max;
    using std::min;
}
#include <objidl.h>
#include <gdiplus.h>

namespace {
    constexpr const wchar_t* kClassName = L"IrisOrb";
    constexpr float kPi = 3.14159265f;

    Orb::Animation* get(HWND h) { return reinterpret_cast<Orb::Animation*>(GetWindowLongPtrW(h, GWLP_USERDATA)); }

    // Volume (RMS) -> 0..1 su scala in decibel: la voce normale sta a metà.
    float loudness(float rms) {
        float db = 20.0f * std::log10(std::max(rms, 1e-5f));
        return std::clamp((db + 55.0f) / 45.0f, 0.0f, 1.0f);
    }

    void targetColor(Orb::Mode m, float out[3]) {
        COLORREF c;
        switch (m) {
        case Orb::Mode::Listening: c = RGB(70, 220, 255); break;
        case Orb::Mode::Thinking: c = RGB(240, 180, 60); break;
        case Orb::Mode::Speaking: c = RGB(60, 200, 255); break;
        case Orb::Mode::Error: c = RGB(230, 70, 70); break;
        case Orb::Mode::Starting: c = RGB(200, 160, 60); break;
        default: c = RGB(30, 150, 200); break;
        }
        out[0] = GetRValue(c);
        out[1] = GetGValue(c);
        out[2] = GetBValue(c);
    }

    Gdiplus::Color col(const float t[3], float alpha, float lighten = 0.0f) {
        auto ch = [&](float v) { return (BYTE)std::clamp(v + (255.0f - v) * lighten, 0.0f, 255.0f); };
        return Gdiplus::Color((BYTE)std::clamp(alpha * 255.0f, 0.0f, 255.0f), ch(t[0]), ch(t[1]), ch(t[2]));
    }

    void paint(HWND h, Orb::Animation* s) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        {
            Ui::Buffered buffer(hdc, rc);
            HDC dc = buffer.dc();
            FillRect(dc, &rc, Ui::brush(Ui::Color::Panel)); // lo stesso colore del pannello che la contiene
            Gdiplus::Graphics g(dc);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            Orb::draw(g, (float)(rc.right - rc.left), (float)(rc.bottom - rc.top), *s);
        }
        EndPaint(h, &ps);
    }

    LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
        case WM_NCCREATE:
            SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR) new Orb::Animation());
            break;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            if (Orb::Animation* s = get(h)) {
                paint(h, s);
                return 0;
            }
            break;
        case WM_NCDESTROY:
            delete get(h);
            SetWindowLongPtrW(h, GWLP_USERDATA, 0);
            break;
        }
        return DefWindowProcW(h, msg, wp, lp);
    }
}

namespace Orb {

void draw(Gdiplus::Graphics& g, float w, float hgt, const Animation& a, float radius) {
    const float cx = w / 2.0f, cy = hgt / 2.0f;
    const float R = std::min(w, hgt) * radius;
    const float t = (float)(GetTickCount64() % 1000000ULL) / 1000.0f;
    const float e = a.energy;
    const float scale = 1.0f + e * 0.12f;

    // Alone: cerchi sempre più grandi e trasparenti.
    for (int i = 0; i < 10; ++i) {
        float r = R * (0.9f + i * 0.12f) * scale;
        Gdiplus::SolidBrush b(col(a.tint, (0.035f + e * 0.03f) * (1.0f - i / 10.0f)));
        g.FillEllipse(&b, cx - r, cy - r, 2 * r, 2 * r);
    }

    // Barre radiali: la voce (tua o sua) le fa allungare.
    const int bars = 64;
    for (int i = 0; i < bars; ++i) {
        float ang = i * 2.0f * kPi / bars + a.spin * 0.15f;
        float wobble = 0.55f + 0.45f * std::sin(i * 1.7f + t * 6.0f) * std::sin(i * 0.6f - t * 3.1f);
        float len = R * (0.05f + e * 0.30f * (0.4f + 0.6f * std::abs(wobble)));
        float r0 = R * 0.66f * scale, r1 = r0 + len;
        Gdiplus::Pen pen(col(a.tint, 0.55f + e * 0.4f, 0.15f), std::max(1.5f, R * 0.018f));
        pen.SetStartCap(Gdiplus::LineCapRound);
        pen.SetEndCap(Gdiplus::LineCapRound);
        g.DrawLine(&pen, cx + std::cos(ang) * r0, cy + std::sin(ang) * r0, cx + std::cos(ang) * r1, cy + std::sin(ang) * r1);
    }

    // Anelli che girano (gradi per GDI+).
    const float deg = a.spin * 180.0f / kPi;
    auto ring = [&](float rad, float width, float alpha, float start, float sweep, int count) {
        Gdiplus::Pen pen(col(a.tint, alpha, 0.1f), width);
        pen.SetStartCap(Gdiplus::LineCapRound);
        pen.SetEndCap(Gdiplus::LineCapRound);
        float r = rad * scale;
        for (int k = 0; k < count; ++k) g.DrawArc(&pen, cx - r, cy - r, 2 * r, 2 * r, start + k * 360.0f / count, sweep);
    };
    ring(R * 1.02f, std::max(2.0f, R * 0.025f), 0.85f, deg * 1.0f, 70.0f, 3);
    ring(R * 1.16f, std::max(1.5f, R * 0.012f), 0.55f, -deg * 0.6f, 250.0f, 1);
    ring(R * 1.30f, std::max(2.5f, R * 0.035f), 0.35f + e * 0.3f, deg * 0.35f, 8.0f, 24);
    ring(R * 0.84f, std::max(1.0f, R * 0.010f), 0.45f, -deg * 1.4f, 40.0f, 6);

    // Il nucleo: più chiaro al centro.
    for (int i = 0; i < 6; ++i) {
        float r = R * (0.56f - i * 0.075f) * (1.0f + e * 0.08f);
        Gdiplus::SolidBrush b(col(a.tint, 0.20f + i * 0.13f, 0.12f * i));
        g.FillEllipse(&b, cx - r, cy - r, 2 * r, 2 * r);
    }
    // Riflesso
    float hr = R * 0.18f;
    Gdiplus::SolidBrush shine(Gdiplus::Color((BYTE)(60 + e * 80), 255, 255, 255));
    g.FillEllipse(&shine, cx - hr, cy - hr, 2 * hr, 2 * hr);
}

void step(Animation& a, Mode mode, float micLevel, float voiceLevel, bool userSpeaking) {
    const ULONGLONG now = GetTickCount64();
    const float dt = a.lastTick ? std::min(0.2f, (now - a.lastTick) / 1000.0f) : 0.033f;
    a.lastTick = now;
    const float t = now / 1000.0f;

    // Quanta "energia" mostrare, e quanto veloce girano gli anelli.
    float target = 0.0f, speed = 0.25f;
    switch (mode) {
    case Mode::Idle:
        target = 0.08f + 0.06f * std::sin(t * 1.3f) + (userSpeaking ? loudness(micLevel) * 0.25f : 0.0f);
        speed = 0.25f;
        break;
    case Mode::Listening:
        target = 0.25f + loudness(micLevel) * 0.75f;
        speed = 0.8f;
        break;
    case Mode::Thinking:
        target = 0.35f + 0.2f * std::sin(t * 5.0f);
        speed = 3.2f;
        break;
    case Mode::Speaking:
        target = 0.2f + loudness(voiceLevel) * 0.8f;
        speed = 1.2f;
        break;
    case Mode::Starting:
        target = 0.2f + 0.15f * std::sin(t * 3.0f);
        speed = 1.5f;
        break;
    case Mode::Error:
        target = 0.15f;
        speed = 0.1f;
        break;
    }
    // Sale in fretta, scende piano.
    const float k = target > a.energy ? std::min(1.0f, dt * 18.0f) : std::min(1.0f, dt * 4.0f);
    a.energy += (target - a.energy) * k;
    a.spin += speed * dt;

    float want[3];
    targetColor(mode, want);
    for (int i = 0; i < 3; ++i) a.tint[i] += (want[i] - a.tint[i]) * std::min(1.0f, dt * 5.0f);
    a.mode = mode;
}

HWND create(HWND parent, int id) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kClassName;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassW(&wc);
        registered = true;
    }
    return CreateWindowExW(0, kClassName, L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, parent, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
}

void update(HWND orb, Mode mode, float micLevel, float voiceLevel, bool userSpeaking) {
    Animation* s = get(orb);
    if (!s) return;
    step(*s, mode, micLevel, voiceLevel, userSpeaking);
    InvalidateRect(orb, nullptr, FALSE);
}

} // namespace Orb
