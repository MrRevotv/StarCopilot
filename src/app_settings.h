#pragma once

#include "hotkey_listener.h"

#include <string>
#include <vector>

// Impostazioni salvate in %appdata%\Iris\settings.cfg (testo, "chiave=valore", UTF-8).
struct AppSettings {
    // --- Tu ---------------------------------------------------------------------
    std::string userName;     // come ti chiama Iris (vuoto = non usa il nome)

    // --- Ascolto ----------------------------------------------------------------
    bool wakeWordEnabled = true;          // sempre in ascolto: si sveglia con "Iris"
    std::vector<TalkTrigger> triggers;    // tasti per parlare (anche senza "Iris")
    std::vector<std::string> deviceNames; // microfoni (vuoto = predefinito di Windows); per l'ascolto continuo il primo
    float micGain = 1.0f;
    double vadThreshold = 0.5;            // probabilità di voce (0..1) sopra la quale "qualcuno parla"
    int endSilenceMs = 700;               // pausa che chiude una frase
    int followUpSeconds = 6;              // dopo una risposta resta in ascolto senza bisogno di "Iris"
    // Quanto parla: "intere" (frasi complete), "gioco" (brevi mentre il gioco è aperto), "brevi" (sempre).
    // Brevi = poche parole, niente cortesie; i comandi di gioco confermano con una parola o un suono.
    std::string replyStyle = "gioco";
    int conversationTimeoutMin = 10;      // dopo tanti minuti di silenzio si ricomincia da capo

    // --- Voce ---------------------------------------------------------------------
    bool speechEnabled = true;
    std::string voiceEngine = "neurale"; // "neurale" (Qwen3-TTS, sulla scheda video) o "windows"
    std::string voiceName = "Elsa";       // parte del nome della voce di Windows
    std::string voiceEffect = "nessuno";  // effetto da macchina, apposta: "nessuno", "ia", "androide", "robot"
    std::string outputDeviceName;         // dove parla Iris (vuoto = uscita predefinita di Windows)
    double speechRate = 1.1;              // 0.5 - 2.0

    // --- Cervello -------------------------------------------------------------------
    std::string llmModelFile;             // vuoto = Qwen2.5-7B-Instruct-Q4_K_M.gguf
    int gpuMemoryLimitMb = -1;            // -1 = senza limite, 0 = tutto sul processore
    bool gameEnabled = true;              // comandi di Star Citizen quando il gioco è aperto

    // --- Overlay e finestra -------------------------------------------------------------
    bool overlayEnabled = true;
    int overlayOpacity = 230;
    int overlaySize = 160; // la sfera fluttuante: lato in pixel (a 96 DPI)
    int overlayX = -1, overlayY = -1;
    int windowX = -1, windowY = -1;
    int windowW = -1, windowH = -1;       // dimensione della finestra (a 96 DPI; -1 = quella di partenza)

    // --- Aggiornamenti ------------------------------------------------------------------
    bool autoUpdate = true;               // controlla e scarica da solo le versioni nuove (GitHub)

    // --- Avvio e area di notifica (l'avvio con Windows sta nel registro: vedi autostart.h) ---
    bool greetOnStart = false;            // "Ciao, sono online e pronta" quando i modelli sono caricati
    bool closeToTray = true;              // la X nasconde la finestra nell'area di notifica (Esci dal menu dell'icona)

    // --- Barra rapida nel gioco (come la barra di citizen-assistant) -----------------------
    bool quickBarEnabled = true;
    std::vector<TalkTrigger> quickBarTriggers; // mostra/nascondi la barra (predefinito: Num /)
    std::vector<TalkTrigger> hideAllTriggers;  // nasconde tutto quello di Iris sullo schermo (predefinito: Num *)
    // I pannelli sopra il gioco: aperti o no, e dove (-1 = sotto la barra).
    bool panelMissionsOpen = false, panelRefineryOpen = false, panelLogOpen = false;
    int panelMissionsX = -1, panelMissionsY = -1;
    int panelRefineryX = -1, panelRefineryY = -1;
    int panelLogX = -1, panelLogY = -1;

    // --- Diario di bordo: lettura del Game.log ----------------------------------------------
    // ATTENZIONE: la lettura del Game.log è vietata dai Termini di Servizio di Star Citizen.
    // Spenta in partenza; si accende solo dopo l'avviso, consapevolmente e a rischio dell'utente.
    bool gameLogEnabled = false;
    bool gameLogAccepted = false;          // avviso letto e accettato (quando l'ha accesa)
    std::string gameLogPath;               // vuoto = trovalo da solo (LIVE, PTU...)
    bool gameLogSpeakContracts = true;     // a voce: contratti accettati, completati, falliti
    bool gameLogSpeakHealth = true;        // a voce: ferite, stati (disidratato...), soccorsi
    bool gameLogSpeakZones = false;        // a voce: armistizio, spazio monitorato
    bool gameLogSpeakParty = true;         // a voce: un compagno in quantum verso di te

    // --- Raffineria (timer) ------------------------------------------------------------------
    bool refinerySpeak = true;             // avvisa a voce quando un ordine è pronto

    // --- Chat di gioco (le funzioni di Voice to Chat, stessi nomi delle sue impostazioni) ---
    bool chatEnabled = true;                // detta in chat col suo tasto
    std::vector<TalkTrigger> chatTriggers;  // tasti "scrivi in chat" (uno in comune con Iris: decide "Iris" detto o no)
    int sourceLanguageIndex = 0;            // vedi languages.h
    int targetLanguageIndex = 1;
    bool autoSend = true;
    bool gamingTone = true;
    double silenceThreshold = 0.01;
    bool chatOpenEnabled = true;
    int chatOpenKeyVk = 0x0D;   // Invio
    int chatSendKeyVk = 0x0D;   // Invio
    int chatOpenDelayMs = 180;
    int typingDelayMs = 12;
    bool unicodeFallback = false;
    // Traduzione della chat degli altri (OCR)
    bool ocrEnabled = false;
    int ocrTargetLanguageIndex = 0;
    std::string chatFilterTag;
    int ocrRegionX = -1, ocrRegionY = -1, ocrRegionW = -1, ocrRegionH = -1;
    double ocrRegionAngle = 0.0;            // zona girata (gradi, + = senso orario): l'HUD segue la curva del casco
    int ocrOverlayX = -1, ocrOverlayY = -1; // -1 = sotto la zona letta
    int ocrOverlayOpacity = 235;
    int ocrMaxMessages = 6;
    bool voiceToChatImported = false; // impostazioni di Voice to Chat già importate (una volta sola)

    // --- Mining (Mining Signature Calculator + mining_ocr dentro Iris) ----------------
    bool miningEnabled = true;                     // letture col tasto, a voce, risultato a schermo
    std::vector<TalkTrigger> miningScanTriggers;   // leggi la signature (predefinito: Num +)
    std::vector<TalkTrigger> miningManualTriggers; // inserimento manuale dal tastierino (predefinito: Num -)
    std::vector<TalkTrigger> miningBorderTriggers; // contorno della zona sì/no
    std::vector<TalkTrigger> miningToggleTriggers; // mining acceso/spento
    int miningRegionX = -1, miningRegionY = -1, miningRegionW = -1, miningRegionH = -1;
    int miningOverlayX = -1, miningOverlayY = -1;  // -1 = accanto alla zona letta
    int miningOverlayOpacity = 235;                // opacità dello sfondo del riquadro (0 = solo le scritte)
    int miningOverlayTextOpacity = 255;            // opacità delle scritte
    int miningOverlaySeconds = 20;                 // quanto resta a schermo il risultato
    bool miningOverlayBasic = false;               // informazioni base: solo materiale e quantità
    bool miningShowBorder = true;                  // contorno della zona (fuori dalla zona, non letto)
    bool miningAuto = false;                       // lettura continua con Star Citizen in primo piano
    int miningIntervalMs = 1000;
    bool miningSaveImages = false;                 // immagini dell'ultima lettura in %appdata%\Iris\debug\mining
    int miningMaxQuantity = 40;                    // rocce/pezzi al massimo in una signature
    bool miningSpeak = false;                      // dice a voce il minerale trovato (una volta per roccia)
    bool miningLogCsv = false;                     // registro delle scansioni: anche il CSV per Excel, oltre al JSON

    static AppSettings load();
    bool save() const;

    // Il contenuto del file (per load/save e per le prove automatiche).
    static AppSettings parse(const std::string& content);
    std::string serialize() const;

    // Valori fuori misura (file modificato a mano o rovinato) tornano sensati.
    void sanitize();

    // Prende le impostazioni della chat da quelle di Voice to Chat (il contenuto
    // del suo settings.cfg): lingue, tasti (i suoi "trigger" diventano chatTriggers),
    // chat, zona letta. Microfono, overlay e finestra restano quelli di Iris.
    void importVoiceToChat(const std::string& voiceToChatContent);
};
