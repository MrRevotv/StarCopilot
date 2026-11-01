#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Un tasto per parlare: un tasto di tastiera/mouse (codice virtuale di
// Windows) o un pulsante di joystick (id stabile della periferica + numero).
struct TalkTrigger {
    enum class Type { Keyboard, Joystick };
    Type type = Type::Keyboard;
    int vk = 0;
    std::string joystickId;   // GUID di istanza (vedi joystick_input.h)
    std::string joystickName; // per mostrarlo anche se è scollegato
    int buttonIndex = 0;      // 0-based

    bool operator==(const TalkTrigger& o) const {
        return type == o.type && (type == Type::Keyboard ? vk == o.vk : joystickId == o.joystickId && buttonIndex == o.buttonIndex);
    }
};

// Ascolta, su un thread suo, i tasti per parlare (finché ALMENO UNO è premuto
// si registra: per esempio il mouse a piedi e il joystick in nave) e i tasti
// "interruttore" (pressione singola: F10 = invio automatico sì/no).
// I callback partono dal thread di ascolto: devono essere brevi.
class HotkeyListener {
public:
    HotkeyListener() = default;
    ~HotkeyListener();

    void setOnTalkStart(std::function<void()> cb) { m_onTalkStart = std::move(cb); }
    void setOnTalkStop(std::function<void()> cb) { m_onTalkStop = std::move(cb); }
    void registerToggleKey(int vk, std::function<void()> cb);

    void setTalkTriggers(const std::vector<TalkTrigger>& triggers);
    std::vector<TalkTrigger> getTalkTriggers();
    bool isAnyTalkTriggerDown();

    // Il secondo gruppo di tasti (Iris: "scrivi in chat", come Voice to Chat).
    // Un tasto può stare in tutti e due i gruppi: partono tutti e due i callback.
    void setOnChatStart(std::function<void()> cb) { m_onChatStart = std::move(cb); }
    void setOnChatStop(std::function<void()> cb) { m_onChatStop = std::move(cb); }
    void setChatTriggers(const std::vector<TalkTrigger>& triggers);
    bool isAnyChatTriggerDown();

    // Durante la scelta di un tasto nuovo il push-to-talk è sospeso: premere
    // il tasto da aggiungere non deve far partire una registrazione. Alla
    // ripresa si aspetta che tutti i tasti siano rilasciati (il tasto appena
    // scelto, di solito, è ancora premuto).
    void setPaused(bool paused) {
        if (!paused && m_paused) m_waitRelease = true;
        m_paused = paused;
    }

    static std::string describeTrigger(const TalkTrigger& t);
    static std::string keyName(int vk);

    // Aspetta il prossimo tasto/pulsante premuto (non uno già tenuto giù).
    // allowJoystick=false: solo tastiera (per i tasti della chat). Il tasto
    // sinistro del mouse non si accetta: lo si preme per usare la finestra, e
    // come push-to-talk registrerebbe a ogni clic. Esc annulla.
    static bool captureNextInput(TalkTrigger& out, std::atomic<bool>& cancel, int timeoutMs, bool allowJoystick);

    // Azioni a pressione singola con tasti di tastiera O pulsanti di joystick
    // (il mining: "leggi la signature", "inserimento manuale"...). Il callback
    // parte quando un tasto della lista viene premuto (non mentre lo tieni giù).
    struct Action {
        std::vector<TalkTrigger> triggers;
        std::function<void()> callback;
    };
    void setActions(std::vector<Action> actions);

    void start();
    void stop();

private:
    void loop();

    struct Toggle {
        int vk;
        bool wasDown = false;
        std::function<void()> callback;
    };

    static bool anyDown(const std::vector<TalkTrigger>& triggers);

    std::mutex m_mutex;
    std::vector<TalkTrigger> m_triggers;
    std::vector<TalkTrigger> m_chatTriggers;
    std::vector<Toggle> m_toggles;
    std::vector<Action> m_actions;
    std::vector<bool> m_actionWasDown;
    std::atomic<bool> m_running{ false };
    std::atomic<bool> m_paused{ false };
    std::atomic<bool> m_waitRelease{ false };
    std::thread m_thread;
    bool m_wasDown = false;
    bool m_chatWasDown = false;
    std::function<void()> m_onTalkStart;
    std::function<void()> m_onTalkStop;
    std::function<void()> m_onChatStart;
    std::function<void()> m_onChatStop;
};
