#pragma once

#include <windows.h>

#include <string>
#include <vector>

// Aspetto comune di tutte le finestre: palette scura in stile "launcher da
// gioco" (la stessa della versione precedente), caratteri, pulsanti e
// interruttori disegnati da noi, adattamento alla scala dello schermo (DPI).
//
// Le misure si scrivono "a 96 DPI" (scala 100%) e si convertono con px():
// a 150% un pulsante alto 32 diventa alto 48 pixel veri.
namespace Ui {

    namespace Color {
        constexpr COLORREF Background = RGB(15, 17, 22);
        constexpr COLORREF Nav = RGB(10, 11, 15);
        constexpr COLORREF Panel = RGB(24, 27, 34);
        constexpr COLORREF PanelBorder = RGB(42, 47, 57);
        constexpr COLORREF Input = RGB(33, 37, 46);
        constexpr COLORREF Text = RGB(235, 237, 240);
        constexpr COLORREF TextDim = RGB(150, 155, 165);
        constexpr COLORREF Accent = RGB(30, 170, 220);
        constexpr COLORREF AccentHover = RGB(60, 192, 238);
        constexpr COLORREF AccentPressed = RGB(22, 138, 182);
        constexpr COLORREF Secondary = RGB(44, 49, 60);
        constexpr COLORREF SecondaryHover = RGB(58, 64, 78);
        constexpr COLORREF Danger = RGB(190, 45, 45);
        constexpr COLORREF DangerHover = RGB(215, 65, 65);
        constexpr COLORREF ToggleOff = RGB(68, 74, 88);
        constexpr COLORREF Ok = RGB(60, 200, 110);
        constexpr COLORREF Busy = RGB(230, 190, 40);
        constexpr COLORREF Error = RGB(220, 70, 70);
        constexpr COLORREF Idle = RGB(140, 145, 155);
    }

    enum class Font { Normal, Small, Bold, Header, Title, Status, Mono };

    // Avvio: tema scuro di Windows per menu e barre di scorrimento, GDI+.
    void initialize();
    void shutdown();

    inline int px(int value, UINT dpi) { return MulDiv(value, (int)dpi, 96); }
    UINT dpiOf(HWND hwnd);

    // Caratteri per una certa scala (creati una volta e tenuti).
    HFONT font(Font role, UINT dpi);

    // Pennelli in tinta unita (creati una volta e tenuti).
    HBRUSH brush(COLORREF color);

    void darkTitleBar(HWND window);
    // Tema scuro di Windows sul controllo (barre di scorrimento, menu a tendina).
    void darkControl(HWND control, const wchar_t* theme = L"DarkMode_Explorer");

    // --- Pulsanti disegnati da noi ------------------------------------------------
    enum class ButtonKind {
        Primary,   // azione principale (azzurro)
        Secondary, // azione normale (grigio)
        Danger,    // togli/cancella (rosso)
        Nav,       // voce del menu a sinistra (checked = pagina aperta)
        Toggle,    // interruttore sì/no con la scritta a sinistra (checked = sì)
    };

    // behind: il colore dietro al pulsante (pannello o sfondo), per gli angoli arrotondati.
    HWND createButton(HWND parent, int id, const std::wstring& text, ButtonKind kind, COLORREF behind);
    void setChecked(HWND button, bool checked);
    bool isChecked(HWND button);
    void setText(HWND control, const std::wstring& text);
    // Da chiamare in WM_DRAWITEM: true se era uno dei nostri pulsanti.
    bool drawButton(const DRAWITEMSTRUCT* dis);

    // --- Altri controlli ----------------------------------------------------------
    // Testo: colore e sfondo li decide ctlColor().
    HWND createLabel(HWND parent, const std::wstring& text, COLORREF color, COLORREF behind, int id = 0);
    void setLabelColor(HWND label, COLORREF color);
    // Colori di testo e sfondo per un controllo qualsiasi (letti da ctlColor()).
    void setColors(HWND control, COLORREF text, COLORREF background);
    HWND createCombo(HWND parent, int id, const std::vector<std::wstring>& items);
    HWND createEdit(HWND parent, int id, bool multiline, bool readOnly, int maxChars = 0);

    // Da chiamare in WM_CTLCOLORSTATIC / EDIT / LISTBOX: colori scuri. 0 se non è nostro.
    LRESULT ctlColor(UINT msg, HDC hdc, HWND control);

    // --- Disegno ------------------------------------------------------------------
    // Rettangolo arrotondato con bordi lisci (GDI+).
    void fillRound(HDC hdc, const RECT& r, int radius, COLORREF fill, COLORREF border = CLR_INVALID);
    void fillCircle(HDC hdc, int cx, int cy, int radius, COLORREF fill);
    // Testo con il carattere e il colore dati (sfondo trasparente).
    void drawText(HDC hdc, const std::wstring& text, RECT r, HFONT font, COLORREF color, UINT format);
    int textHeight(HDC hdc, const std::wstring& text, int width, HFONT font);

    // Disegno senza sfarfallio: si disegna su una bitmap e la si copia alla fine.
    // Le coordinate restano quelle della finestra (la bitmap parte da 0,0 e
    // arriva all'angolo in basso a destra di "area"): niente trasformazioni,
    // che GDI+ potrebbe non rispettare.
    class Buffered {
    public:
        Buffered(HDC target, const RECT& area);
        ~Buffered();
        HDC dc() const { return m_dc ? m_dc : m_target; }
    private:
        HDC m_target;
        RECT m_area;
        HDC m_dc = nullptr;
        HBITMAP m_bitmap = nullptr;
        HGDIOBJ m_old = nullptr;
    };

    // --- Posizioni che seguono la scala ---------------------------------------------
    // Ogni pagina registra i suoi controlli con le misure a 96 DPI; quando la
    // scala cambia (finestra spostata su un altro monitor) li riposiziona e
    // cambia i caratteri.
    class Form {
    public:
        void attach(HWND window) { m_window = window; }
        HWND place(HWND control, int x, int y, int w, int h, Font role = Font::Normal);
        void move(HWND control, int x, int y, int w, int h);
        // Pannello con un titolo (disegnato dalla pagina in WM_PAINT).
        void card(int x, int y, int w, int h, const std::wstring& title = {});
        void clearCards() { m_cards.clear(); }
        void applyDpi(UINT dpi);
        void paintCards(HDC hdc, UINT dpi) const;
        int bottom() const;

    private:
        struct Item {
            HWND hwnd;
            RECT r;
            Font role;
        };
        struct Card {
            RECT r;
            std::wstring title;
        };
        HWND m_window = nullptr;
        UINT m_dpi = 96;
        std::vector<Item> m_items;
        std::vector<Card> m_cards;
    };

} // namespace Ui
