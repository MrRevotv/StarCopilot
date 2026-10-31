#pragma once

#include "../assistant.h"
#include "ui.h"

#include <memory>
#include <string>
#include <vector>

// Messaggi tra la finestra principale e le pagine.
constexpr UINT WM_APP_ENGINE = WM_APP + 1;          // dall'assistente: wParam = Assistant::Event
constexpr UINT WM_APP_CONTENT_CHANGED = WM_APP + 2; // una pagina ha cambiato altezza
constexpr UINT WM_APP_CAPTURED = WM_APP + 3;        // tasto scelto: wParam = 1 se ok, lParam = TalkTrigger*
constexpr UINT WM_APP_REBUILD = WM_APP + 4;         // ricostruisci la pagina (righe aggiunte/tolte)

// Larghezza delle pagine (a 96 DPI).
constexpr int kPageWidth = 600;

// Stato in parole e colori (finestra, overlay).
std::wstring stateLabel(Assistant::State state);
COLORREF stateColor(Assistant::State state);

// Una pagina della finestra (le voci del menu a sinistra). Ogni pagina è una
// finestra figlia, alta quanto il suo contenuto: la finestra principale la fa
// scorrere se lo schermo è più basso. (Come in Voice to Chat.)
class Page {
public:
    Page(Assistant& assistant, const wchar_t* title);
    virtual ~Page();
    Page(const Page&) = delete;
    Page& operator=(const Page&) = delete;

    HWND create(HWND parent);
    HWND hwnd() const { return m_hwnd; }
    const wchar_t* title() const { return m_title; }
    int contentHeight() const; // a 96 DPI
    void applyDpi(UINT dpi);

    virtual void onShow() {}
    virtual void onEngineEvent(Assistant::Event) {}
    virtual void onTimer() {}

protected:
    virtual void build() = 0;
    virtual void onCommand(int id, int code, HWND control) {}
    virtual bool onDrawItem(const DRAWITEMSTRUCT*) { return false; }
    virtual LRESULT onAppMessage(UINT msg, WPARAM w, LPARAM l) { return 0; }
    virtual void paintExtra(HDC hdc) {}

    void rebuild();
    void contentChanged();

    // Mattoncini per costruire le pagine (misure a 96 DPI).
    int beginCard(int y);
    int endCard(int top, int y, const std::wstring& title);
    HWND label(const std::wstring& text, int x, int y, int w, int h, COLORREF color = Ui::Color::Text,
               Ui::Font font = Ui::Font::Normal, int id = 0);
    int note(const std::wstring& text, int x, int y, int w, COLORREF color = Ui::Color::TextDim, HWND* out = nullptr);
    HWND button(int id, const std::wstring& text, Ui::ButtonKind kind, int x, int y, int w, int h = 32);
    HWND toggle(int id, const std::wstring& text, int x, int y, int w);
    HWND combo(int id, const std::vector<std::wstring>& items, int x, int y, int w);

    Assistant& m_assistant;
    Ui::Form m_form;
    HWND m_hwnd = nullptr;
    UINT m_dpi = 96;
    const wchar_t* m_title;

private:
    static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM w, LPARAM l);
};

// Le pagine, nell'ordine del menu.
std::vector<std::unique_ptr<Page>> createPages(Assistant& assistant);

// Le pagine della chat di gioco (le funzioni di Voice to Chat): ui/chat_pages.cpp.
std::unique_ptr<Page> createChatPage(Assistant& assistant);
std::unique_ptr<Page> createChatScreenPage(Assistant& assistant);
std::unique_ptr<Page> createFilteredPage(Assistant& assistant);
