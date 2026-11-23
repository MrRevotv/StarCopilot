// Prove automatiche di Iris.
//   iris_tests.exe                 prove veloci (niente modelli, niente rete, nessun file toccato)
//   iris_tests.exe --completi      anche il modello di linguaggio vero e la rete
//
// Prove a mano dei singoli pezzi:
//   iris_tests.exe --chiedi "ricordati che..."       una domanda a Iris (modello + strumenti), senza voce
//   iris_tests.exe --voce "testo"                     la voce di Windows
//   iris_tests.exe --ascolto                          l'ascolto continuo con frasi dette dalla voce di Windows
//                                                     (Silero + whisper + "Iris", come dal microfono)

#include "agent.h"
#include "app_paths.h"
#include "app_settings.h"
#include "assistant.h"
#include "audio_capture.h"
#include "autostart.h"
#include "axis_config.h"
#include "axis_server.h"
#include "chat_format.h"
#include "chat_parser.h"
#include "command_matcher.h"
#include "game_chat.h"
#include "game_controller.h"
#include "game_input.h"
#include "game_log.h"
#include "game_profile.h"
#include "gamer_slang.h"
#include "glossary.h"
#include "gpu.h"
#include "http_client.h"
#include "input_sender.h"
#include "language_guess.h"
#include "languages.h"
#include "llm_engine.h"
#include "mining_decoder.h"
#include "mining_image.h"
#include "mining_log.h"
#include "mining_scanner.h"
#include "ppocr.h"
#include "refinery.h"
#include "neural_voice.h"
#include "ocr_engine.h"
#include "sc_locations.h"
#include "screen_capture.h"
#include "screen_match.h"
#include "screen_ocr.h"
#include "sc_telemetry.h"
#include "speech_utils.h"
#include "translation_prompt.h"
#include "speech_text.h"
#include "ship_toggle.h"
#include "text_utils.h"
#include "tools.h"
#include "tts_engine.h"
#include "updater.h"
#include "vad_segmenter.h"
#include "voice_detector.h"
#include "voice_effect.h"
#include "wake_word.h"
#include "whisper_engine.h"
#include "ui/axis_panes.h"
#include "ui/orb.h"
#include "ui/quick_bar.h"
#include "ui/ui.h"

#include <windows.h>
#include <objbase.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

// GDI+ (la sfera, --sfera): vuole min/max, che NOMINMAX toglie.
namespace Gdiplus {
    using std::max;
    using std::min;
}
#include <objidl.h>
#include <gdiplus.h>

namespace {
    int g_checks = 0;
    int g_failures = 0;

    void check(bool ok, const char* what, const char* file, int line) {
        g_checks++;
        if (!ok) {
            g_failures++;
            std::printf("  FALLITO %s:%d: %s\n", file, line, what);
        }
    }

    void checkStr(const std::string& got, const std::string& want, const char* what, const char* file, int line) {
        g_checks++;
        if (got != want) {
            g_failures++;
            std::printf("  FALLITO %s:%d: %s\n    atteso: \"%s\"\n    avuto:  \"%s\"\n", file, line, what, want.c_str(), got.c_str());
        }
    }

    template <typename A, typename B>
    void checkEq(const A& a, const B& b, const char* what, const char* file, int line) {
        g_checks++;
        if (!(a == b)) {
            g_failures++;
            std::printf("  FALLITO %s:%d: %s\n", file, line, what);
        }
    }

#define CHECK(x) check((x), #x, __FILE__, __LINE__)
#define CHECK_STR(a, b) checkStr((a), (b), #a, __FILE__, __LINE__)
#define CHECK_EQ(a, b) checkEq((a), (b), #a " == " #b, __FILE__, __LINE__)

    void section(const char* name) { std::printf("- %s\n", name); }

    // Per gli strumenti: memoria finta.
    class FakeHost : public ToolHost {
    public:
        void remember(const std::string& fact) override { std::printf("  [memoria: %s]\n", fact.c_str()); }
    };

    // Come Iris col gioco aperto (--prova-domande): game_command con i comandi del profilo
    // predefinito, ma senza premere niente (il risultato dice solo cosa avrebbe fatto).
    class GameFakeHost : public FakeHost {
    public:
        explicit GameFakeHost(std::string definition) : m_definition(std::move(definition)) {}
        std::vector<std::string> extraToolDefinitions() override { return { m_definition }; }
        bool runExtraTool(const std::string& name, const std::string& argumentsJson, const std::atomic<bool>*, std::string& result) override {
            if (name != "game_command") return false;
            std::printf("  [game_command %s]\n", argumentsJson.c_str());
            const nlohmann::json a = nlohmann::json::parse(argumentsJson, nullptr, false);
            const std::string command = a.is_object() && a.contains("command") && a["command"].is_string() ? a["command"].get<std::string>() : "";
            result = !command.empty() && m_definition.find("\"" + command + "\"") != std::string::npos ? "Fatto: " + command
                                                                                                       : "Errore: comando sconosciuto.";
            return true;
        }

    private:
        std::string m_definition;
    };

    // ------------------------------------------------------------------------------------
    void testWakeWord() {
        std::printf("Parola di attivazione\n");
        auto m = WakeWord::detect("Iris, che tempo fa domani?");
        CHECK(m.found);
        CHECK_STR(m.command, "che tempo fa domani?");
        m = WakeWord::detect("Ehi Iris apri Spotify");
        CHECK(m.found);
        CHECK_STR(m.command, "apri Spotify");
        m = WakeWord::detect("Iris.");
        CHECK(m.found);
        CHECK_STR(m.command, "");
        m = WakeWord::detect("iris");
        CHECK(m.found && m.command.empty());
        m = WakeWord::detect("Che ore sono, Iris?");
        CHECK(m.found);
        CHECK_STR(m.command, "Che ore sono?");
        m = WakeWord::detect("Airis, metti la musica.");
        CHECK(m.found);
        CHECK_STR(m.command, "metti la musica.");
        m = WakeWord::detect("E Iris, apri Spotify."); // whisper a volte sente "E" al posto di "Ehi"
        CHECK(m.found);
        CHECK_STR(m.command, "apri Spotify.");
        m = WakeWord::detect("Allora, Iris: accendi le luci");
        CHECK(m.found);
        CHECK_STR(m.command, "accendi le luci");
        CHECK(!WakeWord::detect("Ieri ho parlato con Iris di lavoro e poi sono uscito").found);
        CHECK(!WakeWord::detect("Crisi economica in vista").found);
        CHECK(!WakeWord::detect("Che ore sono?").found);
        CHECK(!WakeWord::detect("").found);
        // Dal registro vero: il nome ripetuto ("Iris, Iris.") è solo una chiamata, non una domanda al modello.
        m = WakeWord::detect("Iris, Iris.");
        CHECK(m.found && m.command.empty());
        m = WakeWord::detect("Iris, Iris, chiamala come vuoi.");
        CHECK(m.found);
        CHECK_STR(m.command, "chiamala come vuoi.");
        m = WakeWord::detect("Rotta per Crusader, Iris, Iris.");
        CHECK(m.found);
        CHECK_STR(m.command, "Rotta per Crusader");
        // Parlare DI lei con un amico, col nome in fondo, non la sveglia.
        CHECK(!WakeWord::detect("Ho aggiornato Iris.").found);
        CHECK(!WakeWord::detect("Ieri ho parlato con Iris.").found);
        CHECK(!WakeWord::detect("Ti piace la mia Iris?").found);
        CHECK(!WakeWord::detect("Ciao Rebo, sono Iris.").found);
        CHECK(!WakeWord::detect("Hai visto dell'Iris?").found);
        // Con la virgola, o dopo un comando, sì.
        m = WakeWord::detect("Accendi i motori Iris");
        CHECK(m.found);
        CHECK_STR(m.command, "Accendi i motori");
        m = WakeWord::detect("Hai finito, Iris?");
        CHECK(m.found);
        CHECK_STR(m.command, "Hai finito?");
        CHECK(WakeWord::isStopCommand("basta"));
        CHECK(WakeWord::isStopCommand("Stop!"));
        CHECK(WakeWord::isStopCommand("Zitta."));
        CHECK(WakeWord::isStopCommand("Basta così, grazie"));
        CHECK(!WakeWord::isStopCommand("basta con la pasta per stasera, cosa cucino?"));
        CHECK(!WakeWord::isStopCommand("che tempo fa"));
        // Dal registro vero: andavano al modello invece di zittirla.
        CHECK(WakeWord::isStopCommand("Zitta, sai zitta, basta, non mi interessa, zitta."));
        CHECK(WakeWord::isStopCommand("Madonna, stai zitta!"));
        CHECK(WakeWord::isStopCommand("No, stop."));
        CHECK(WakeWord::isStopCommand("Eh, basta!"));
        CHECK(!WakeWord::isStopCommand("annulla la rotta"));
        CHECK(!WakeWord::isStopCommand("Squamno, guarda che basta un tuo cenno, scendo con un FS9 e lo trito."));
        CHECK(WakeWord::isHesitation("Ehm."));
        CHECK(WakeWord::isHesitation("Mmm, boh..."));
        CHECK(!WakeWord::isHesitation("Sì."));
        CHECK(!WakeWord::isHesitation("Ah, ok"));
        CHECK(WakeWord::isUnfinished("Imposta la rotta per..."));
        CHECK(WakeWord::isUnfinished("Integra\xE2\x80\xA6"));
        CHECK(!WakeWord::isUnfinished("Rotta per Crusader."));
        CHECK(WakeWord::wordCount("Ehi, Iris! Carrello.") == 3);

        // L'eco della sua voce: dal registro vero, "Ciao Rebo, sono Iris" (detto da lei: "Ciao REVO, sono Iris...")
        // era finito al modello come richiesta.
        auto n = [](const char* s) { return WakeWord::normalize(s); };
        const std::vector<std::string> said = { n("Ciao REVO, sono Iris. Questa è la mia voce.") };
        CHECK(SpeechUtils::soundsLikeEcho(n("Ciao Rebo, sono Iris."), said, false));
        CHECK(SpeechUtils::soundsLikeEcho(n("Questa è la mia voce."), said, false));
        CHECK(!SpeechUtils::soundsLikeEcho(n("Iris, basta."), said, false));
        const std::vector<std::string> said2 = { n("Ho cancellato il timer. Se ne avessi avuto più di uno, li avrei tutti annullati.") };
        CHECK(SpeechUtils::soundsLikeEcho(n("Se ne avessi avuto più di uno, li avrei confiato."), said2, false));
        // Dopo che ha finito di parlare: la tua risposta con le sue parole non è eco.
        const std::vector<std::string> said3 = { n("Vuoi che imposti la rotta per Seraphim?") };
        CHECK(!SpeechUtils::soundsLikeEcho(n("Sì, imposta la rotta."), said3, true));
        CHECK(!SpeechUtils::soundsLikeEcho(n("Sì."), { n("Dimmi sì o no.") }, true));
        CHECK(SpeechUtils::soundsLikeEcho(n("imposti la rotta per Seraphim"), said3, true));
        CHECK_STR(WakeWord::normalize("Ehi, Iris!"), "ehi iris");
        CHECK_STR(WakeWord::normalize("Perché  NO?"), "perché no");
    }

    void testSpeechText() {
        std::printf("Frasi per la voce\n");
        SpeechText::SentenceSplitter s;
        auto out = s.push("Ciao! Come");
        CHECK(out.size() == 1 && out[0] == "Ciao!");
        out = s.push(" stai? Bene");
        CHECK(out.size() == 1 && out[0] == "Come stai?");
        CHECK_STR(s.flush(), "Bene");
        out = s.push("Vedi es. la pagina. Poi");
        CHECK(out.size() == 1 && out[0] == "Vedi es. la pagina.");
        s.reset();
        out = s.push("Sono 3.5 gradi. Ok");
        CHECK(out.size() == 1 && out[0] == "Sono 3.5 gradi.");
        s.reset();
        out = s.push("Prima riga\nSeconda");
        CHECK(out.size() == 1 && out[0] == "Prima riga");
        s.reset();
        out = s.push("J. R. R. Tolkien scrisse libri. Fine");
        CHECK(out.size() == 1 && out[0] == "J. R. R. Tolkien scrisse libri.");

        CHECK_STR(SpeechText::speakable("**Ciao**, ecco: [link](http://x.it) e https://www.google.com/search?q=1."), "Ciao, ecco: link e .");
        CHECK_STR(SpeechText::speakable("- primo\n- secondo"), "primo. secondo");
        CHECK_STR(SpeechText::speakable("1. Uno\n2. Due"), "Uno. Due");
        CHECK_STR(SpeechText::speakable("Ci sono 21\xC2\xB0" "C e vento a 10 km/h"), "Ci sono 21 gradi e vento a 10 chilometri orari");
        CHECK_STR(SpeechText::speakable("# Titolo\nTesto normale."), "Titolo. Testo normale.");
        CHECK_STR(SpeechText::speakable("Fatto \xE2\x9C\x85 subito \xF0\x9F\x98\x80"), "Fatto subito");

        // Dopo il punto una minuscola non è una frase nuova ("Allora... vediamo").
        s.reset();
        out = s.push("Allora... vediamo la rotta. Fatto");
        CHECK(out.size() == 1 && out[0] == "Allora... vediamo la rotta.");

        // Numeri e sigle come li deve dire la voce.
        CHECK_STR(SpeechText::numberToWords(0), "zero");
        CHECK_STR(SpeechText::numberToWords(21), "ventuno");
        CHECK_STR(SpeechText::numberToWords(23), "ventitré");
        CHECK_STR(SpeechText::numberToWords(28), "ventotto");
        CHECK_STR(SpeechText::numberToWords(103), "centotré");
        CHECK_STR(SpeechText::numberToWords(108), "centotto");
        CHECK_STR(SpeechText::numberToWords(180), "centottanta");
        CHECK_STR(SpeechText::numberToWords(1000), "mille");
        CHECK_STR(SpeechText::numberToWords(2023), "duemila ventitré");
        CHECK_STR(SpeechText::numberToWords(23000), "ventitremila");
        CHECK_STR(SpeechText::numberToWords(16960), "sedicimila novecentosessanta");
        CHECK_STR(SpeechText::numberToWords(1000000), "un milione");
        CHECK_STR(SpeechText::numberToWords(2500000), "due milioni cinquecentomila");
        CHECK_STR(SpeechText::forVoice("Hai 16.960 UEC e 32 SCU di Laranite."), "Hai sedicimila novecentosessanta UEC e trentadue esse ci u di Laranite.");
        CHECK_STR(SpeechText::forVoice("Il salto dura circa 2,5 minuti."), "Il salto dura circa due virgola cinque minuti.");
        CHECK_STR(SpeechText::forVoice("Sono 3.5 gradi, poi 1,05."), "Sono tre virgola cinque gradi, poi uno virgola zero cinque.");
        CHECK_STR(SpeechText::forVoice("Carburante al 15%"), "Carburante al quindici per cento");
        CHECK_STR(SpeechText::forVoice("A 1.200.000 km da Hurston"), "A un milione duecentomila chilometri da Hurston");
        CHECK_STR(SpeechText::forVoice("Signature 16,960: Borase x4"), "Signature sedicimila novecentosessanta: Borase per quattro");
        CHECK_STR(SpeechText::forVoice("Vai a CRU-L1, poi Area18"), "Vai a CRU-L1, poi Area18");
        CHECK_STR(SpeechText::forVoice("Alle 14:30 fa -5 gradi"), "Alle quattordici e trenta fa meno cinque gradi");
        CHECK_STR(SpeechText::forVoice("Ciao, sono Iris. Tutto a posto?"), "Ciao, sono Iris. Tutto a posto?");
        CHECK_STR(SpeechText::forVoice("QT pronto, contatta l'ATC: 1.250 m/s, 10km"), "quantum pronto, contatta l'a ti ci: mille duecentocinquanta metri al secondo, dieci chilometri");
        CHECK_STR(SpeechText::forVoice("Scudi al 100% e 2 F7C pronti"), "Scudi al cento per cento e due F7C pronti");

        // Coda della voce: un frammento corto si dice insieme alla frase dopo, se c'è già.
        std::deque<std::string> q = { "Certo!", "Imposto la rotta per Hurston.", "Poi controllo il carburante." };
        CHECK_STR(TtsEngine::takeNext(q), "Certo! Imposto la rotta per Hurston.");
        CHECK(q.size() == 1);
        q = { "Questa frase è lunga abbastanza per stare da sola.", "Ok." };
        CHECK_STR(TtsEngine::takeNext(q), "Questa frase è lunga abbastanza per stare da sola.");
        q = { "Ok", "Va bene." };
        CHECK_STR(TtsEngine::takeNext(q), "Ok. Va bene.");
        CHECK(q.empty());
        q = { "Sì.", std::string(230, 'a') };
        CHECK_STR(TtsEngine::takeNext(q), "Sì.");
        CHECK(q.size() == 1);
        q = { "Fatto." };
        CHECK_STR(TtsEngine::takeNext(q), "Fatto.");
    }

    void testVad() {
        std::printf("Ritaglio delle frasi\n");
        VadParams p;
        VadSegmenter seg(p);
        std::vector<float> frame(512, 0.1f);    // voce
        std::vector<float> quiet(512, 0.0005f); // silenzio della stanza
        bool done = false;
        for (int i = 0; i < 10; ++i) done |= seg.push(quiet.data(), 0.0f);
        CHECK(!done && !seg.inSpeech());
        for (int i = 0; i < 20; ++i) done |= seg.push(frame.data(), 0.9f);
        CHECK(!done && seg.inSpeech());
        int frames = 0;
        while (!done && frames < 100) {
            done = seg.push(quiet.data(), 0.0f);
            frames++;
        }
        CHECK(done);
        CHECK(frames == 22); // 22 x 32 ms = 704 ms di silenzio
        std::vector<float> u = seg.takeUtterance();
        CHECK(u.size() == 37 * 512); // 12 di pre-ascolto + 18 di voce + 7 di coda
        CHECK(seg.takeUtterance().empty());

        // Un colpo breve non è una frase.
        VadSegmenter seg2(p);
        done = false;
        for (int i = 0; i < 3; ++i) done |= seg2.push(frame.data(), 0.9f);
        for (int i = 0; i < 40; ++i) done |= seg2.push(quiet.data(), 0.0f);
        CHECK(!done);
        // Un blocco solo non fa nemmeno cominciare.
        VadSegmenter seg3(p);
        seg3.push(frame.data(), 0.9f);
        seg3.push(frame.data(), 0.1f);
        CHECK(!seg3.inSpeech());

        // Il guasto con la Scarlett: dopo la voce, il rumore di fondo (volume 0,0008) che
        // Silero valuta 0,45. Senza il controllo del volume la frase non finiva mai.
        VadSegmenter seg4(p);
        std::vector<float> hum(512, 0.0008f);
        for (int i = 0; i < 30; ++i) seg4.push(hum.data(), 0.30f);
        for (int i = 0; i < 20; ++i) seg4.push(frame.data(), 0.9f);
        done = false;
        frames = 0;
        while (!done && frames < 200) {
            done = seg4.push(hum.data(), 0.45f);
            frames++;
        }
        CHECK(done && frames == 22);
        // E lo stesso rumore, per quanto Silero esiti, non fa cominciare una frase.
        VadSegmenter seg5(p);
        for (int i = 0; i < 100; ++i) seg5.push(hum.data(), 0.7f);
        CHECK(!seg5.inSpeech());
        // Una voce bassa (volume 0,005, come la tua Scarlett) passa.
        VadSegmenter seg6(p);
        std::vector<float> soft(512, 0.005f);
        for (int i = 0; i < 30; ++i) seg6.push(hum.data(), 0.1f);
        for (int i = 0; i < 5; ++i) seg6.push(soft.data(), 0.9f);
        CHECK(seg6.inSpeech());
    }

    void testChatFormat() {
        std::printf("Formato della conversazione\n");
        using M = ChatFormat::Message;
        std::vector<M> msgs = { { M::Role::User, "che tempo fa?" },
                                { M::Role::Assistant, "<tool_call>\n{\"name\": \"get_weather\", \"arguments\": {}}\n</tool_call>" },
                                { M::Role::Tool, "sole" },
                                { M::Role::Tool, "caldo" } };
        std::string p = ChatFormat::buildPrompt("Sei Iris.", { "{\"type\":\"function\"}" }, msgs);
        CHECK(TextUtils::startsWith(p, "<|im_start|>system\nSei Iris."));
        CHECK(p.find("<tools>\n{\"type\":\"function\"}\n</tools>") != std::string::npos);
        CHECK(p.find("<|im_start|>user\n<tool_response>\nsole\n</tool_response>\n<tool_response>\ncaldo\n</tool_response><|im_end|>\n") != std::string::npos);
        CHECK(p.size() > 20 && p.compare(p.size() - 22, 22, "<|im_start|>assistant\n") == 0);
        std::string noTools = ChatFormat::buildPrompt("S", {}, { { M::Role::User, "ciao" } }, true);
        CHECK(noTools.find("<tools>") == std::string::npos);
        CHECK(noTools.find("<think>\n\n</think>\n\n") != std::string::npos);

        std::string before;
        auto calls = ChatFormat::parseToolCalls("<tool_call>\n{\"name\": \"get_weather\", \"arguments\": {\"location\": \"Roma\"}}\n</tool_call>", &before);
        CHECK(calls.size() == 1);
        if (!calls.empty()) {
            CHECK_STR(calls[0].name, "get_weather");
            CHECK(calls[0].argumentsJson.find("Roma") != std::string::npos);
        }
        CHECK_STR(before, "");
        calls = ChatFormat::parseToolCalls("Controllo subito.\n<tool_call>{\"name\":\"web_search\",\"arguments\":\"{\\\"query\\\":\\\"x\\\"}\"}</tool_call>", &before);
        CHECK(calls.size() == 1 && calls[0].argumentsJson == "{\"query\":\"x\"}");
        CHECK_STR(before, "Controllo subito.\n");
        calls = ChatFormat::parseToolCalls("{\"name\": \"open_app\", \"arguments\": {\"name\": \"Spotify\"}}");
        CHECK(calls.size() == 1 && calls[0].name == "open_app");
        calls = ChatFormat::parseToolCalls("```json\n{\"name\": \"open_app\", \"arguments\": {\"name\": \"Steam\"}}\n```");
        CHECK(calls.size() == 1 && calls[0].argumentsJson.find("Steam") != std::string::npos);
        CHECK(ChatFormat::parseToolCalls("Ciao, come stai? Tutto bene.").empty());
        CHECK(ChatFormat::parseToolCalls("<tool_call>{rotto</tool_call>").empty());

        // Dal registro vero: il 7B (soprattutto con le risposte brevi) scrive la richiesta come testo.
        // Prima la voce la leggeva ("game_command graffa command...") e il comando non partiva.
        const std::vector<std::string> names = { "remember", "game_command" };
        calls = ChatFormat::parseToolCalls("game_command {\"command\": \"press:power\"} game_command {\"command\": \"press:engines\"} motori spenti",
                                           &before, names);
        CHECK(calls.size() == 2 && calls[0].name == "game_command" && calls[1].argumentsJson.find("press:engines") != std::string::npos);
        CHECK_STR(before, "");
        calls = ChatFormat::parseToolCalls("remember{\"fact\": \"il mio wingman si chiama Luca\"}", &before, names);
        CHECK(calls.size() == 1 && calls[0].name == "remember" && calls[0].argumentsJson.find("Luca") != std::string::npos);
        calls = ChatFormat::parseToolCalls("Ricevuto. game_command({'command': 'press:shields'})", &before, names);
        CHECK(calls.size() == 1 && calls[0].argumentsJson.find("press:shields") != std::string::npos);
        CHECK_STR(before, "Ricevuto. ");
        calls = ChatFormat::parseToolCalls("open_website {\"url\": \"facebook.com\"}", &before, names); // strumento che non c'è: si riconosce lo stesso
        CHECK(calls.size() == 1 && calls[0].name == "open_website");
        calls = ChatFormat::parseToolCalls("{\"name\": \"game_command\", \"command\": \"press:vtol\"}", &before, names); // argomenti accanto al nome
        CHECK(calls.size() == 1 && calls[0].argumentsJson.find("press:vtol") != std::string::npos);
        calls = ChatFormat::parseToolCalls("<tool_call>\n{\"name\": \"game_command\", \"arguments\": {\"command\": \"set_route\", \"destination\": \"Seraphim\"", &before, names);
        CHECK(calls.size() == 1 && calls[0].argumentsJson.find("Seraphim") != std::string::npos); // troncata: si chiude
        CHECK(ChatFormat::parseToolCalls("Ricorda di alzare gli scudi {prima} di partire.", &before, names).empty());
        CHECK(ChatFormat::parseToolCalls("remember che domani si gioca.", &before, names).empty());
        calls = ChatFormat::parseToolCalls("remember: fact=\"Il mio wingman si chiama Luca\"", &before, names); // altra forma vista nelle prove
        CHECK(calls.size() == 1 && calls[0].name == "remember" && calls[0].argumentsJson.find("Luca") != std::string::npos);
        calls = ChatFormat::parseToolCalls("game_command(command=\"press:shields\")", &before, names);
        CHECK(calls.size() == 1 && calls[0].argumentsJson.find("press:shields") != std::string::npos);
        CHECK(ChatFormat::toolCallStart("remember: fact=", names) == 0);
        CHECK(ChatFormat::possibleToolCallTail("Ok. remember: fact", names) == 14);
        CHECK(ChatFormat::plainToolCallComplete("game_command(command=\"press:shields\")", names));
        // Con i nomi degli argomenti (toolSignature) anche gli argomenti in fila: game_command("set_route", "Crusader").
        CHECK_STR(ChatFormat::toolSignature("{\"type\":\"function\",\"function\":{\"name\":\"game_command\",\"parameters\":{\"type\":\"object\","
                                            "\"properties\":{\"command\":{},\"destination\":{}}}}}"),
                  "game_command(command,destination)");
        const std::vector<std::string> sigs = { "remember(fact)", "game_command(command,destination)" };
        calls = ChatFormat::parseToolCalls("Ok, imposto. game_command(\"set_route\", \"Crusader\")", &before, sigs);
        CHECK(calls.size() == 1 && calls[0].argumentsJson.find("\"destination\":\"Crusader\"") != std::string::npos &&
              calls[0].argumentsJson.find("set_route") != std::string::npos);
        calls = ChatFormat::parseToolCalls("game_command(\"press:shields\") game_command(\"press:engines\")", &before, sigs);
        CHECK(calls.size() == 2);
        CHECK(ChatFormat::parseToolCalls("game_command {\"command\": \"press:power\"}", &before, sigs).size() == 1);
        CHECK(ChatFormat::mentionedTool("Ok, faccio. game_command[rotto", sigs) == 12);
        // Solo gli argomenti ({"command": ...}) e una parola a caso prima ("ronics", visto davvero al posto di <tool_call>).
        calls = ChatFormat::parseToolCalls("ronics\n{\"command\":\"press:shields\"} Scudi su.", &before, sigs);
        CHECK(calls.size() == 1 && calls[0].name == "game_command");
        calls = ChatFormat::parseToolCalls("ronics\n{\"name\": \"game_command\", \"arguments\": {\"command\": \"cancel_route\"}}", &before, sigs);
        CHECK(calls.size() == 1 && calls[0].argumentsJson.find("cancel_route") != std::string::npos);
        CHECK(ChatFormat::parseToolCalls("Il valore è {\"prezzo\": 3}", &before, sigs).empty());
        // "Game_command: press:shields" (maiuscola in cima, valori nudi dopo i due punti), visto nelle prove.
        calls = ChatFormat::parseToolCalls("Game_command: press:shields\ngame_command: set_route Port Tressler", &before, sigs);
        CHECK(calls.size() == 2 && calls[0].name == "game_command" && calls[0].argumentsJson.find("press:shields") != std::string::npos &&
              calls[1].argumentsJson.find("\"destination\":\"Port Tressler\"") != std::string::npos);
        calls = ChatFormat::parseToolCalls("Remember: il mio wingman si chiama Luca", &before, sigs);
        CHECK(calls.size() == 1 && calls[0].argumentsJson.find("il mio wingman si chiama Luca") != std::string::npos);
        CHECK(ChatFormat::mentionedTool("Game_command press:shields press:engines", sigs) == 0);
        CHECK(ChatFormat::parseToolCalls("Rotta: impostata per Crusader.", &before, sigs).empty());
        CHECK(ChatFormat::mentionedTool("Rotta impostata.", sigs) == std::string::npos);
        // Dove comincia, per non dirla a voce; e cosa tenere da parte mentre arriva a pezzi.
        CHECK(ChatFormat::toolCallStart("Fatto. game_command {\"command\"", names) == 7);
        CHECK(ChatFormat::toolCallStart("Scudi su.", names) == std::string::npos);
        CHECK(ChatFormat::possibleToolCallTail("Ok. game_comm", names) == 9);
        CHECK(ChatFormat::possibleToolCallTail("Ok. remember ", names) == 9);
        CHECK(ChatFormat::possibleToolCallTail("Ok. <tool_c", names) == 7);
        CHECK(ChatFormat::possibleToolCallTail("Scudi su.", names) == 0);
        CHECK(ChatFormat::possibleToolCallTail("Motori ", names) == 0);
        CHECK(ChatFormat::plainToolCallComplete("game_command {\"command\": \"press:power\"}", names));
        CHECK(!ChatFormat::plainToolCallComplete("game_command {\"command\": \"press:po", names));
        // Nella conversazione resta sempre la forma giusta.
        CHECK_STR(ChatFormat::formatToolCall({ "game_command", "{\"command\":\"press:power\"}" }),
                  "<tool_call>\n{\"name\": \"game_command\", \"arguments\": {\"command\":\"press:power\"}}\n</tool_call>");
        CHECK_STR(ChatFormat::toolName("{\"type\":\"function\",\"function\":{\"name\":\"remember\"}}"), "remember");
        CHECK_STR(ChatFormat::cleanAnswer("Ciao!<|im_end|>"), "Ciao!");
        CHECK_STR(ChatFormat::cleanAnswer("<think>\nhmm\n</think>\n\nRisposta."), "Risposta.");
        CHECK_STR(ChatFormat::cleanAnswer("Ok. <tool_call>{\"name\""), "Ok.");
    }

    void testGame() {
        std::printf("Comandi di gioco\n");
        const GameProfile p = GameProfile::defaults();
        auto m = Cmd::match(p, "carrello");
        CHECK(m.found && m.command.id() == "press:landing_gear");
        m = Cmd::match(p, "Imposta la rotta per Seraphim.");
        CHECK(m.found && m.command.intent == "set_route");
        CHECK_STR(m.slots["destination"], "Stazione Orbitale Seraphim");
        m = Cmd::match(p, "rotta per sera a film"); // come lo sentiva whisper
        CHECK(m.found);
        CHECK_STR(m.slots["destination"], "Stazione Orbitale Seraphim");
        m = Cmd::match(p, "a nulla rotta");
        CHECK(m.found && m.command.intent == "cancel_route");
        m = Cmd::match(p, "cancella la rotta");
        CHECK(m.found && m.command.intent == "cancel_route");
        m = Cmd::match(p, "portami a Lorville");
        CHECK(m.found);
        CHECK_STR(m.slots["destination"], "Lorville");
        m = Cmd::match(p, "rotta per Pippo Station"); // luogo che il profilo non conosce: si prova lo stesso
        CHECK(m.found);
        CHECK_STR(m.slots["destination"], "Pippo Station");
        m = Cmd::match(p, "alza gli scudi");
        CHECK(m.found && m.command.id() == "press:shields");
        m = Cmd::match(p, "Modalità VTOL!");
        CHECK(m.found && m.command.id() == "press:vtol");
        CHECK(!Cmd::match(p, "che tempo fa domani a Roma?").found);
        CHECK(!Cmd::match(p, "raccontami una barzelletta").found);
        CHECK_STR(Cmd::normalize("Modalità VTOL!"), "modalita vtol");
        CHECK_STR(Cmd::applyCorrections("a nulla rotta", p.corrections), "annulla rotta");
        CHECK(Cmd::bestText({ "CERCA LOCALITA", "STAZIONE ORBITALE SERAPHIM 12 KM", "CRUSADER" }, "Stazione Orbitale Seraphim", 0.68) == 1);
        CHECK(Cmd::bestText({ "CRUSADER", "HURSTON" }, "microTech", 0.68) == -1);

        CHECK(GameInput::parseCombo("LAlt+N").size() == 2 && GameInput::parseCombo("LAlt+N")[0] == 0x38);
        CHECK(GameInput::parseCombo("Pippo").empty());
        CHECK(GameInput::scanCode("F2") == 0x3C && GameInput::scanCode("Delete") == 0xE053 && GameInput::scanCode("0x3C") == 0x3C);

        // La vecchia Iris: JSON con commenti e virgole in più.
        const std::string dictionary =
            "{ // commento\n \"matchThreshold\": 0.7,\n \"corrections\": {\"olisa\": \"olisar\",},\n"
            " \"commands\": [ {\"intent\": \"press\", \"args\": {\"key\": \"landing_gear\"}, \"phrases\": [\"giù il carrello\"]}, ],\n"
            " /* alias */ \"aliases\": {\"destination\": {\"Port Olisar\": [\"porto oliza\"]}} }";
        const std::string settings = "{\"game\": {\"keys\": {\"landing_gear\": \"LAlt+N\"}, \"starMap\": {\"searchBoxX\": 0.24}}, \"dryRun\": true}";
        GameProfile imported;
        std::string err;
        CHECK(GameProfile::importOldIris(settings, dictionary, imported, err));
        CHECK(imported.keys["landing_gear"] == "LAlt+N");
        CHECK(imported.keys["map"] == "F2"); // i tasti che non avevi: predefiniti
        CHECK(imported.dryRun);
        CHECK(std::abs(imported.starMap.searchBoxX - 0.24) < 1e-9);
        auto m2 = Cmd::match(imported, "giù il carrello");
        CHECK(m2.found && m2.command.id() == "press:landing_gear");
        auto m3 = Cmd::match(imported, "annulla la rotta"); // comando nuovo aggiunto ai tuoi
        CHECK(m3.found && m3.command.intent == "cancel_route");
        auto m4 = Cmd::match(imported, "rotta per porto oliza");
        CHECK(m4.found && m4.slots["destination"] == "Port Olisar");
        GameProfile back;
        CHECK(GameProfile::parse(imported.toJson(), back, err));
        CHECK(back.commands.size() == imported.commands.size() && back.keys == imported.keys && back.dryRun == imported.dryRun);
        CHECK(!GameProfile::parse("{ rotto", back, err));

        // Dalla frase al tasto, in modalità prova, con Esplora risorse al posto del gioco (non si preme niente).
        if (GameInput::findGameWindow("explorer", "")) {
            GameController game;
            game.load(); // nella cartella delle prove: profilo predefinito
            GameProfile fake = game.profile();
            fake.processName = "explorer";
            fake.windowTitle.clear();
            fake.dryRun = true;
            CHECK(game.saveProfile(fake));
            CHECK(game.status().running);
            std::vector<std::string> said;
            auto sayFn = [&](const std::string& t) { said.push_back(t); };
            GameController::Result r = game.execute(game.match("carrello"), sayFn, nullptr);
            CHECK(r.ok && r.shown.find("(N)") != std::string::npos && r.spoken == "Carrello.");
            r = game.execute(game.match("ancora"), sayFn, nullptr); // ripete l'ultimo
            CHECK(r.ok && r.shown.find("landing_gear") != std::string::npos);
            r = game.execute(game.match("rotta per Crusader"), sayFn, nullptr);
            CHECK(r.ok && !said.empty() && said.back() == "Calcolo la rotta per Crusader.");
            CHECK_STR(r.brief, "Rotta di prova."); // risposte brevi: due parole invece della frase
            r = game.executeById("press:shields", "", sayFn, nullptr);
            CHECK(r.ok && r.shown.find("(O)") != std::string::npos);
            CHECK(game.toolDefinition().find("press:landing_gear") != std::string::npos);
            CHECK(game.recentActions().size() >= 4);
            r = game.execute(game.match("annulla la rotta"), sayFn, nullptr);
            CHECK(r.ok);
            CHECK_STR(r.brief, "Annullo di prova.");
            CHECK(game.profile().defaultsVersion == GameProfile::kDefaultsVersion);
        }
    }

    GameCommand testCommand(const std::string& intent, std::vector<std::string> phrases, const std::string& key) {
        GameCommand c;
        c.intent = intent;
        c.phrases = std::move(phrases);
        if (!key.empty()) c.args["key"] = key;
        return c;
    }

    void testGameMatching() {
        section("comandi: i contrari non si confondono");
        const GameProfile p = GameProfile::defaults();
        CHECK(Cmd::polarity("disattiva gli scudi") == -1 && Cmd::polarity("alza gli scudi") == 1 && Cmd::polarity("scudi") == 0);
        auto m = Cmd::match(p, "Disattiva gli scudi.");
        CHECK(m.found && m.command.id() == "press:shields" && m.phrase != "alza gli scudi");
        // Un profilo di prima, senza le frasi nuove: "disattiva" trova "abbassa" (stesso verso), mai "alza".
        GameProfile older = p;
        for (auto& c : older.commands) {
            if (c.id() == "press:shields") c.phrases = { "scudi", "alza gli scudi", "abbassa gli scudi" };
        }
        m = Cmd::match(older, "disattiva gli scudi");
        CHECK(m.found && m.command.id() == "press:shields" && m.phrase != "alza gli scudi");
        // Solo la frase contraria: niente comando (prima era "alza gli scudi" al 63%, a un passo dalla soglia).
        GameProfile only;
        only.keys = p.keys;
        only.commands = { testCommand("press", { "alza gli scudi" }, "shields") };
        CHECK(!Cmd::match(only, "disattiva gli scudi").found);
        CHECK(!Cmd::match(only, "abbassa gli scudi").found);
        // Comandi tuoi con tasti diversi per aprire e chiudere: mai uno al posto dell'altro.
        GameProfile doors = p;
        doors.keys["doors_open"] = "LAlt+O";
        doors.keys["doors_close"] = "LAlt+P";
        doors.commands.push_back(testCommand("press", { "apri le porte" }, "doors_open"));
        doors.commands.push_back(testCommand("press", { "chiudi le porte" }, "doors_close"));
        m = Cmd::match(doors, "chiudi le porte");
        CHECK(m.found && m.command.id() == "press:doors_close");
        m = Cmd::match(doors, "chiudi la porta");
        CHECK(!(m.found && m.command.id() == "press:doors_open"));
        m = Cmd::match(doors, "apri le porte");
        CHECK(m.found && m.command.id() == "press:doors_open");
        m = Cmd::match(p, "chiudi la mappa");
        CHECK(m.found && m.command.intent == "close_map");
        m = Cmd::match(p, "spegni la nave");
        CHECK(m.found && m.phrase == "spegni la nave");
        // Spegnere il quantum non è mai un salto quantico (non è un interruttore).
        m = Cmd::match(p, "spegni il quantum");
        CHECK(!(m.found && m.command.intent == "quantum_travel"));

        section("comandi: come li storpia whisper");
        for (const char* said : { "Mi posso una rotta per Seraphim?", "Un po' sono rotta per Seraphim.", "posta una rotta per Seraphim",
                                  "Imposta una rotta per Seraphim." }) {
            m = Cmd::match(p, said);
            CHECK(m.found && m.command.intent == "set_route");
            CHECK_STR(m.slots["destination"], "Stazione Orbitale Seraphim");
        }
        m = Cmd::match(p, "Imposto una rotta per Microtech.");
        CHECK(m.found && m.slots["destination"] == "microTech");
        // "annulla" prima di "rotta per": non è mai un'impostazione della rotta.
        m = Cmd::match(p, "annulla la rotta per Crusader");
        CHECK(!(m.found && m.command.intent == "set_route"));
        m = Cmd::match(p, "Spegni le cose, come si chiamano? Spegni i motori.");
        CHECK(m.found && m.command.id() == "press:engines");
        m = Cmd::match(p, "Apri l'hangar.");
        CHECK(m.found && m.command.id() == "press:request_landing");
        m = Cmd::match(p, "Chiamala TC.");
        CHECK(m.found && m.command.id() == "press:request_landing");
        m = Cmd::match(p, "Alzati dal posto di guida.");
        CHECK(m.found && m.command.id() == "hold:exit_seat");
        m = Cmd::match(p, "Disattiva la modalità Capoled.");
        CHECK(m.found && m.command.id() == "press:decoupled");
        CHECK(!Cmd::match(p, "Qual è il tempo domani a Bagnarola?").found);
        CHECK(!Cmd::match(p, "Nessuna.").found);

        section("luoghi: i numeri contano");
        m = Cmd::match(p, "Imposta la rotta per Cru L2.");
        // Prima finiva su Cru-L1: rotta sbagliata. Né su Arc-L2 (nome corto, due lettere diverse).
        CHECK(m.found && m.slots["destination"] != "Cru-L1" && m.slots["destination"] != "Arc-L2");
        m = Cmd::match(p, "rotta per cru l4");
        CHECK_STR(m.slots["destination"], "Cru-L4");
        m = Cmd::match(p, "Imposta una rotta per Cruel 1.");
        CHECK_STR(m.slots["destination"], "Cru-L1");
        m = Cmd::match(p, "rotta per hurston l2");
        CHECK_STR(m.slots["destination"], "Hur-L2");
        m = Cmd::match(p, "rotta per arc l uno");
        CHECK_STR(m.slots["destination"], "Arc-L1");
        m = Cmd::match(p, "rotta per area diciotto");
        CHECK_STR(m.slots["destination"], "Area18");

        section("testo letto dall'OCR della mappa");
        CHECK(Cmd::bestText({ "CRU-LI" }, "Cru-L1", 0.68) == 0);
        CHECK(Cmd::bestText({ "ANNULIA" }, "ANNULLA", 0.75) == 0);
        CHECK(Cmd::bestText({ "STAZONE ORBITALE SERAPHIM" }, "Stazione Orbitale Seraphim", 0.68) == 0);
        CHECK(Cmd::ocrContains("ORISONORISON", "Orison") >= 0.99);
        CHECK(Cmd::ocrContains("CRU-L1 STAZIONE AMBITIOUS", "Cru-L1") >= 0.7);
        CHECK(Cmd::ocrContains("DNORISONI", "Orison") >= 0.7);
        CHECK(Cmd::ocrContains("Li STAZIONE AMBITIOUS", "Cru-L1") < 0.7); // lì decide il campo cambiato, non l'OCR
        CHECK(Cmd::ocrContains("q CERCA LOCALITÅ...", "CERCA LOCALITA") >= 0.75);
        CHECK(Cmd::ocrContains("", "Orison") == 0.0);
        CHECK_STR(Cmd::ocrFold("CRU-L1"), Cmd::ocrFold("cru li"));
    }

    void testProfileByHand() {
        section("profilo scritto a mano");
        std::string err;
        GameProfile h;
        // Commenti di tre tipi, virgole in più, una virgola dimenticata a fine riga, virgolette “tipografiche”.
        const std::string hand =
            "// il mio profilo\n{\n  \"name\": \"Star Citizen\"\n  # commento\n  \"dryRun\": true, /* prova */\n"
            "  \"keys\": { \"landing_gear\": \xE2\x80\x9CLAlt+N\xE2\x80\x9D, },\n}\n";
        CHECK(GameProfile::parse(hand, h, err));
        CHECK(h.dryRun && h.keys["landing_gear"] == "LAlt+N" && h.name == "Star Citizen");
        CHECK(h.defaultsVersion == 0); // scritto senza il numero: riceverà i predefiniti nuovi
        // L'errore dice dove: riga e colonna del file com'è scritto (i commenti contano).
        CHECK(!GameProfile::parse("// commento\n{\n  \"name\": \"x\",\n  \"dryRun\": tru\n}\n", h, err));
        CHECK(err.find("riga 4") != std::string::npos);
        std::printf("  errore: %s\n", err.c_str());
        CHECK(!GameProfile::parse("{\n  \"name\" \"x\"\n}", h, err));
        CHECK(err.find("riga 2") != std::string::npos);
        CHECK(!GameProfile::parse("{\n  \"keys\": {\"map\": \"F2\"\n", h, err));
        CHECK(err.find("finisce prima del tempo") != std::string::npos);
        CHECK(!GameProfile::parse("{ rotto", h, err));
        CHECK(err.find("riga 1") != std::string::npos);
        // Le posizioni del testo ripulito restano quelle del file (per riga e colonna).
        std::vector<size_t> where;
        const std::string relaxed = relaxedJson("{ \"a\": 1, // x\n \"b\": [1,2,], }", &where);
        CHECK(relaxed.size() == where.size());
        CHECK(nlohmann::json::parse(relaxed, nullptr, false).is_object());

        section("predefiniti nuovi nei profili di prima");
        GameProfile old = GameProfile::defaults();
        old.defaultsVersion = 0;
        old.keys.erase("mining_mode");
        old.keys["decoupled"] = "C"; // il tuo tasto resta il tuo
        old.aliases["destination"].erase("Cru-L4");
        old.commands.erase(std::remove_if(old.commands.begin(), old.commands.end(), [](const GameCommand& c) { return c.id() == "press:mobiglas"; }),
                           old.commands.end());
        for (auto& c : old.commands) {
            if (c.id() == "press:request_landing") c.phrases = { "richiedi atterraggio", "chiama la torre" };
        }
        // Una frase nuova di Iris già usata da un tuo comando: resta tua.
        old.keys["hangar_door"] = "LAlt+H";
        old.commands.push_back(testCommand("press", { "apri l'hangar" }, "hangar_door"));
        const int added = old.addMissingDefaults();
        std::printf("  aggiunte %d cose al profilo di prima\n", added);
        CHECK(added >= 5);
        CHECK(old.defaultsVersion == GameProfile::kDefaultsVersion);
        CHECK(old.keys["decoupled"] == "C" && old.keys["mining_mode"] == "M");
        CHECK(old.aliases["destination"].count("Cru-L4") == 1);
        auto m = Cmd::match(old, "apri il mobiglas");
        CHECK(m.found && m.command.id() == "press:mobiglas");
        m = Cmd::match(old, "chiedi il decollo");
        CHECK(m.found && m.command.id() == "press:request_landing");
        m = Cmd::match(old, "apri l'hangar");
        CHECK(m.found && m.command.id() == "press:hangar_door");
        CHECK(old.addMissingDefaults() == 0); // la seconda volta niente
        GameProfile back;
        CHECK(GameProfile::parse(GameProfile::defaults().toJson(), back, err) && back.defaultsVersion == GameProfile::kDefaultsVersion);
        CHECK(GameProfile::parse("{}", back, err) && back.defaultsVersion == 0);

        section("tasti finti e finestra del gioco");
        Sleep(200);
        CHECK(!GameInput::injectingInput());
        {
            GameInput::InjectionScope scope;
            CHECK(GameInput::injectingInput());
        }
        CHECK(GameInput::injectingInput()); // ancora un attimo dopo: il thread dei tasti legge ogni 8 ms
        Sleep(200);
        CHECK(!GameInput::injectingInput());
        // Esplora risorse ha finestre di servizio 0x0 prima di quelle vere: si prende la più grande.
        if (HWND w = GameInput::findGameWindow("explorer", "")) {
            RECT r{};
            CHECK(GameInput::clientRect(w, r));
        }
    }

    // La casella di ricerca della mappa stellare come nelle catture vere (551x51 su 3440x1440):
    // bordi scuri, vetro sfumato (chiaro 185->215 o scuro), la lente e il testo bianco (240).
    ScreenCapture::Image renderSearchBox(const wchar_t* text, bool brightGlass, bool caret) {
        const int w = 551, h = 51;
        HDC screen = GetDC(nullptr);
        HDC dc = CreateCompatibleDC(screen);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        HGDIOBJ oldBmp = SelectObject(dc, bmp);
        for (int x = 0; x < w; ++x) {
            int lum = 22;
            if (x >= 40 && x < 520) lum = brightGlass ? 185 + 30 * (x - 40) / 480 : 45 + 10 * (x - 40) / 480;
            RECT col{ x, 0, x + 1, h };
            HBRUSH b = CreateSolidBrush(RGB(std::max(0, lum - 8), lum, std::min(255, lum + 8)));
            FillRect(dc, &col, b);
            DeleteObject(b);
        }
        HPEN pen = CreatePen(PS_SOLID, 2, RGB(205, 210, 215));
        HGDIOBJ oldPen = SelectObject(dc, pen);
        HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Ellipse(dc, 72, 12, 92, 32);
        MoveToEx(dc, 90, 30, nullptr);
        LineTo(dc, 97, 37);
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(pen);
        HFONT font = CreateFontW(-22, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        HGDIOBJ oldFont = SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(238, 242, 248));
        SIZE size{};
        GetTextExtentPoint32W(dc, text, (int)wcslen(text), &size);
        TextOutW(dc, 115, (h - size.cy) / 2, text, (int)wcslen(text));
        if (caret) {
            RECT c{ 115 + size.cx + 2, 12, 115 + size.cx + 4, 39 };
            HBRUSH white = CreateSolidBrush(RGB(240, 244, 250));
            FillRect(dc, &c, white);
            DeleteObject(white);
        }
        GdiFlush();
        ScreenCapture::Image img;
        img.width = w;
        img.height = h;
        img.pixels.assign((uint8_t*)bits, (uint8_t*)bits + (size_t)w * h * 4);
        for (size_t i = 3; i < img.pixels.size(); i += 4) img.pixels[i] = 255;
        SelectObject(dc, oldFont);
        DeleteObject(font);
        SelectObject(dc, oldBmp);
        DeleteObject(bmp);
        DeleteDC(dc);
        ReleaseDC(nullptr, screen);
        return img;
    }

    void testMapOcr() {
        section("mappa stellare: il nome è entrato nel campo?");
        for (bool bright : { true, false }) {
            const ScreenOcr::Shot empty = ScreenOcr::grayOf(renderSearchBox(L"", bright, false));
            const double name = ScreenOcr::textChange(empty, ScreenOcr::grayOf(renderSearchBox(L"ORISON", bright, false)));
            const double two = ScreenOcr::textChange(empty, ScreenOcr::grayOf(renderSearchBox(L"U.A.", bright, false)));
            const double caret = ScreenOcr::textChange(empty, ScreenOcr::grayOf(renderSearchBox(L"", bright, true)));
            std::printf("  vetro %s: nome %.2f%%, due lettere %.2f%%, cursore %.2f%%\n", bright ? "chiaro" : "scuro", name * 100, two * 100, caret * 100);
            CHECK(name >= 0.0025); // il nome scritto si vede anche se l'OCR non lo legge
            CHECK(two >= 0.0025);  // anche "U.A." (l'OCR non lo legge mai)
            CHECK(caret < 0.0025); // il cursore che lampeggia no
            CHECK(ScreenOcr::textChange(empty, empty) == 0.0);
            // Il vetro un po' più chiaro (la mappa che si muove dietro): non è testo.
            ScreenCapture::Image shifted = renderSearchBox(L"", bright, false);
            for (size_t i = 0; i < shifted.pixels.size(); ++i) {
                if (i % 4 != 3) shifted.pixels[i] = (uint8_t)std::min(255, shifted.pixels[i] + 6);
            }
            CHECK(ScreenOcr::textChange(empty, ScreenOcr::grayOf(shifted)) < 0.0025);
        }
        CHECK(ScreenOcr::textChange(ScreenOcr::Shot{}, ScreenOcr::grayOf(renderSearchBox(L"", true, false))) == 1.0);

        if (OcrEngineWrapper::installedLanguages().empty()) {
            std::printf("  (OCR di Windows non installato: lettura saltata)\n");
            return;
        }
        OcrSettings o;
        o.language = "en";
        o.upscale = 2;
        o.saveDebugShots = false;
        const RECT region{ 0, 0, 551, 51 };
        for (bool bright : { true, false }) {
            for (const wchar_t* text : { L"ORISON", L"CRU-L1", L"SERAPHIM", L"LORVILLE" }) {
                const std::string want = TextUtils::wideToUtf8(text);
                const ScreenOcr::Result res = ScreenOcr::readImage(renderSearchBox(text, bright, false), region, o, "", [&](const ScreenOcr::Result& x) {
                    return Cmd::ocrContains(ScreenOcr::linesText(x), want) >= 0.7;
                });
                const std::string seen = TextUtils::collapseSpaces(ScreenOcr::linesText(res));
                std::printf("  %s su vetro %s -> \"%s\" (%s)\n", want.c_str(), bright ? "chiaro" : "scuro", seen.c_str(), res.variant.c_str());
                CHECK(Cmd::ocrContains(seen, want) >= 0.7);
            }
        }
    }

    // ===== Chat di gioco: le prove di Voice to Chat per i suoi moduli (copiate così come sono) =====

    // --- Lettura della chat ----------------------------------------------------------

    void testChatParser() {
        section("chat letta dallo schermo");
        ChatMessage m;
        CHECK(ChatParser::parseHeader("\xE2\x80\xA2 Trime98: mr. dat theres is 2-5 guys", m));
        CHECK_STR(m.playerName, "Trime98");
        CHECK(m.tags.empty());
        CHECK_STR(m.body, "mr. dat theres is 2-5 guys");

        CHECK(ChatParser::parseHeader("\xE2\x80\xA2 [SQUADRA]Trime98: mr. dat theres is 2-3 guys", m));
        CHECK_STR(m.playerName, "Trime98");
        CHECK_EQ(m.tags.size(), (size_t)1);
        if (!m.tags.empty()) CHECK_STR(m.tags[0], "SQUADRA");
        CHECK_STR(m.displayPrefix(), "[SQUADRA] Trime98: ");

        CHECK(ChatParser::parseHeader("[TUTTO][SDR]Tadzyk: Keemon dawaj arte nastepna", m));
        CHECK_STR(m.playerName, "Tadzyk");
        CHECK_EQ(m.tags.size(), (size_t)2);
        CHECK_STR(m.body, "Keemon dawaj arte nastepna");

        CHECK(ChatParser::parseHeader("[TEAM][ANZ] Invictus: crazy how different comms are in unofficial servers", m));
        CHECK_STR(m.playerName, "Invictus");
        CHECK_EQ(m.tags.size(), (size_t)2);

        CHECK(ChatParser::parseHeader("Invictus: no one bothering to give any tactical updates at all", m));
        CHECK_STR(m.playerName, "Invictus");

        // Quadre lette male dall'OCR.
        CHECK(ChatParser::parseHeader("tLSEJ Pippo_92: ci vediamo a Lorville", m));
        CHECK_STR(m.playerName, "Pippo_92");
        CHECK(ChatParser::matchesFilter(m, "LSE"));
        CHECK(ChatParser::parseHeader("(LSE) Pippo: ciao", m));
        CHECK(ChatParser::matchesFilter(m, "[lse]"));
        CHECK(!ChatParser::matchesFilter(m, "SDR"));
        CHECK(ChatParser::parseHeader("SQUADRA]Trime98: ok", m));
        CHECK_STR(m.playerName, "Trime98");
        CHECK(ChatParser::matchesFilter(m, "squadra"));

        // Come li ha letti davvero l'OCR di Windows: "]" diventa "I", il nome si spezza.
        CHECK(ChatParser::parseHeader("[SQUADRAI Trim e98: there are 2-3 gws at the hangar", m));
        CHECK_STR(m.playerName, "Trime98");
        CHECK(m.tags.size() == 1 && m.tags[0] == "SQUADRA");
        CHECK(ChatParser::parseHeader("[TUTTOIISDRI Tadzyk: kaska sie przyda", m));
        CHECK(m.tags.size() == 2 && m.tags[0] == "TUTTO" && m.tags[1] == "SDR");
        CHECK_STR(m.displayPrefix(), "[TUTTO][SDR] Tadzyk: ");
        CHECK(ChatParser::parseHeader("[TUTTOI[SDRI Tadzyk: kaska sie przyda", m));
        CHECK(m.tags.size() == 2 && m.tags[1] == "SDR");
        CHECK(ChatParser::parseHeader("ITUTTOIISDRI Tadzyk: kaska sie przyda", m)); // anche "[" letta come "I"
        CHECK_STR(m.displayPrefix(), "[TUTTO][SDR] Tadzyk: ");
        CHECK(ChatParser::parseHeader("tLSEJ Pippo_92: ok", m));
        CHECK(m.tags.size() == 1 && m.tags[0] == "LSE");
        CHECK(!ChatParser::parseHeader("Ma dai: non ci credo", m)); // due parole senza canale: non è un nome

        // Canali con nome noto, come li ha letti davvero l'OCR di Windows in gioco.
        for (const char* line : { "tGLOBALEl Foxbat: yeah thats not how", "tGLOBALE1 Foxbat: yeah thats not how",
                                  "IGLOBALEI Foxbat: yeah thats not how", "GLOBALE) Foxbat: yeah thats not how",
                                  "(GLOBALE) Foxbat: yeah thats not how", "[GLOBALE) Foxbat: yeah thats not how",
                                  "[GLOBALE] Foxbat: yeah thats not how", "\xE2\x80\xA2 [GL0BALE]Foxbat: yeah thats not how" }) {
            const bool ok = ChatParser::parseHeader(line, m);
            CHECK(ok);
            if (!ok) std::printf("    riga: \"%s\"\n", line);
            CHECK_STR(m.displayPrefix(), "[GLOBALE] Foxbat: ");
            CHECK_STR(m.body, "yeah thats not how");
        }
        CHECK(ChatParser::parseHeader("tGLOBALEl Foxbat: hi", m));
        CHECK_STR(m.rawPrefix, "tGLOBALEl Foxbat:"); // per il file di debug: com'era scritto
        CHECK(ChatParser::parseHeader("IGLOBALI ZMAD29: what is best fighter", m));
        CHECK_STR(m.displayPrefix(), "[GLOBAL] ZMAD29: ");
        CHECK(ChatParser::parseHeader("lPARTYl Bob: on my way", m));
        CHECK_STR(m.displayPrefix(), "[PARTY] Bob: ");
        CHECK(ChatParser::parseHeader("[GRUPPOI Bob: arrivo", m));
        CHECK_STR(m.displayPrefix(), "[GRUPPO] Bob: ");
        CHECK(ChatParser::parseHeader("tGROUP1 Bob: coming", m));
        CHECK_STR(m.displayPrefix(), "[GROUP] Bob: ");
        CHECK(ChatParser::parseHeader("ISQUADI Bob: hangar 3", m));
        CHECK_STR(m.displayPrefix(), "[SQUAD] Bob: ");
        CHECK(ChatParser::parseHeader("ELSEI Pippo: ciao", m));
        CHECK_STR(m.displayPrefix(), "[LSE] Pippo: ");
        CHECK(ChatParser::parseHeader("[GLOBALEIILSEI Pippo: ciao", m));
        CHECK_STR(m.displayPrefix(), "[GLOBALE][LSE] Pippo: ");
        CHECK(ChatParser::matchesFilter(m, "LSE"));
        CHECK(ChatParser::parseHeader("tGLOBALEl ISDRI Tadzyk: kaska sie przyda", m)); // poi un'organizzazione qualunque
        CHECK_STR(m.displayPrefix(), "[GLOBALE][SDR] Tadzyk: ");
        CHECK(ChatParser::parseHeader("[SQUADRAI Trime98: ok", m)); // "SQUADRA" non è "SQUAD"
        CHECK_STR(m.displayPrefix(), "[SQUADRA] Trime98: ");
        // Senza i due punti (persi o tagliati dal bordo), se le quadre si vedono.
        CHECK(ChatParser::parseHeader("tGLOBALEl Foxbat yeah thats not h", m));
        CHECK_STR(m.playerName, "Foxbat");
        CHECK_STR(m.body, "yeah thats not h");
        CHECK(ChatParser::parseHeader("[GLOBALE] Foxbat:12 guys at the hangar", m));
        CHECK_STR(m.body, "12 guys at the hangar");
        CHECK(!ChatParser::parseHeader("LSE members meet at Lorville", m));
        CHECK(!ChatParser::parseHeader("PARTY so we can go together", m));
        CHECK(!ChatParser::parseHeader("Party time: lets go", m));
        // A sinistra della "[" non c'è niente: quello che l'OCR legge lì si taglia.
        for (const char* line : { "1.1? [GLOBALE] Foxbat: hi", "* tGLOBALEl Foxbat: hi", "'|[GLOBALE] Foxbat: hi",
                                  "\xC2\xBB GLOBALE) Foxbat: hi", "\xE2\x80\xA2tGLOBALEl Foxbat: hi" }) {
            const bool ok = ChatParser::parseHeader(line, m);
            CHECK(ok);
            if (!ok) std::printf("    riga: \"%s\"\n", line);
            CHECK_STR(m.displayPrefix(), "[GLOBALE] Foxbat: ");
            CHECK_STR(m.body, "hi");
        }
        CHECK(ChatParser::parseHeader("1.1? tGLOBALEl Foxbat yeah thats not h", m)); // anche senza i due punti
        CHECK_STR(m.body, "yeah thats not h");
        CHECK(!ChatParser::parseHeader("anymore [GLOBALE] Fox: hi", m)); // prima c'è una parola: è una frase
        CHECK(!ChatParser::parseHeader("1 GLOBALE Fox hi", m));           // rumore e niente quadre: non si taglia
        auto cut = ChatParser::parse({ "tGLOBALEl Mothman", "anyone selling a cutlass", "IGLOBALEI Fox: no" });
        CHECK_EQ(cut.size(), (size_t)2);
        if (cut.size() == 2) {
            CHECK_STR(cut[0].playerName, "Mothman");
            CHECK_STR(cut[0].body, "anyone selling a cutlass");
        }

        // Righe vere dell'OCR in gioco. Il canale da solo su una riga, con il resto
        // sotto: è un messaggio nuovo, non il seguito di quello sopra.
        auto real = ChatParser::parse({ "tGLOBALEl Cedorus: nahh just go boom :D", "GLOBALE)",
                                        "Fuses and repair page o' display and thats it XD", "tGLOBALE1 BigManStian:",
                                        "Okay sweet, wont bother too much with it then" });
        CHECK_EQ(real.size(), (size_t)3);
        if (real.size() == 3) {
            CHECK_STR(real[0].body, "nahh just go boom :D");
            CHECK(real[1].playerName.empty());
            CHECK_STR(real[1].displayPrefix(), "[GLOBALE] ");
            CHECK_STR(real[1].body, "Fuses and repair page o' display and thats it XD");
            CHECK_STR(real[2].body, "Okay sweet, wont bother too much with it then");
        }
        CHECK(ChatParser::parseHeader("ILSEI", m));
        CHECK(ChatParser::matchesFilter(m, "LSE")); // il filtro vale anche senza nome
        // La "_" dei nomi letta come lineetta, anche con i due punti in mezzo.
        CHECK(ChatParser::parseHeader("tGLOBALEl Cpt\xE2\x80\x94" "Andromeda: faaaah", m));
        CHECK_STR(m.playerName, "Cpt_Andromeda");
        CHECK(ChatParser::parseHeader("[GLOBALE] Cpt: \xE2\x80\x94" "Andromeda: faaaah", m));
        CHECK_STR(m.playerName, "Cpt_Andromeda");
        CHECK_STR(m.body, "faaaah");
        // Due messaggi finiti sulla stessa riga.
        real = ChatParser::parse({ "[GLOBALE] LaffyJaff: What is grief if not love persevering tGLOBALEl Foxbat: i thought the undersuits would be flight suits?" });
        CHECK_EQ(real.size(), (size_t)2);
        if (real.size() == 2) {
            CHECK_STR(real[0].body, "What is grief if not love persevering");
            CHECK_STR(real[1].playerName, "Foxbat");
        }
        CHECK_EQ(ChatParser::parse({ "1.1? [GLOBALE] Foxbat: hi" }).size(), (size_t)1);       // prima solo rumore
        CHECK_EQ(ChatParser::parse({ "[GLOBALE] Bob: join [PARTY] chat now" }).size(), (size_t)1); // un canale nel testo
        // "I'm" letto male: il modello lasciava la frase in inglese.
        for (const char* line : { "[GLOBALE] JAMRAGS: l'm looking for bummers", "[GLOBALE] JAMRAGS: l'rn looking for bummers",
                                  "[GLOBALE] JAMRAGS: Ilm looking for bummers" }) {
            real = ChatParser::parse({ line });
            CHECK(real.size() == 1 && real[0].body == "I'm looking for bummers");
        }
        real = ChatParser::parse({ "[GLOBALE] Bob: l think so, l'amico mio" });
        CHECK(real.size() == 1 && real[0].body == "I think so, l'amico mio");

        // Letture rovinate dall'OCR (righe vere): se ne sceglie la più pulita, e
        // quelle illeggibili non si traducono.
        CHECK_EQ(ChatParser::garbledWords("i thought the undersuits would be flight suits?"), 0);
        CHECK(ChatParser::garbledWords("i thought the undersui\xC3\x88uould be flight suits?") > 0); // "undersuiÈuould"
        CHECK(ChatParser::garbledWords("i thought the undersuits would be flight\xC3\xA8uits?") > 0); // "flightèuits"
        CHECK(ChatParser::garbledWords("who looking for gunners wh*o") > 0);
        CHECK(ChatParser::unreadable("\xE2\x80\xA2" "ef%\xC3\xB2ot LaffVJ wh*o ba\xE2\x80\xA2\xC3\xBBrgG-s")); // "•ef%òot LaffVJ wh*o ba•ûrgG-s"
        CHECK(ChatParser::unreadable("Whati$ grief perse&ring sTlts fy3afft"));
        // Frasi normali, anche con nomi, sigle, numeri e altre lingue: si traducono.
        for (const char* ok : { "LaffyJaff send a friend", "o7 C8X ready at 12:30, 50% fuel", "need 2-3 guys at the hangar",
                                "perch\xC3\xA9 no? \xC3\xA8 gi\xC3\xA0 finito", "kaska si\xC4\x99 przyda, dzi\xC4\x99ki",
                                "pr\xC3\xA9" "f\xC3\xA9r\xC3\xA9 de tous", "I'm looking for bummers :D", "ok!!! xD", "w/ the C2 to Area18" }) {
            const bool bad = ChatParser::unreadable(ok);
            CHECK(!bad);
            if (bad) std::printf("    scartata per errore: \"%s\"\n", ok);
        }

        // Non sono messaggi.
        CHECK(!ChatParser::parseHeader("Quantum travel ready", m));
        CHECK(!ChatParser::parseHeader("Arrivo alle 12:30 circa", m));
        CHECK(!ChatParser::parseHeader("vedi https://robertsspaceindustries.com", m));
        CHECK(!ChatParser::parseHeader("questa frase ha troppe parole prima: dei due punti", m));

        // Righe andate a capo e coda di un messaggio cominciato sopra la zona.
        auto msgs = ChatParser::parse({ "of the station and then we go", "[SQUADRA] Trime98: hello there", "how are you doing",
                                        "[TUTTO][SDR]Tadzyk: kaska sie przyda" });
        CHECK_EQ(msgs.size(), (size_t)3);
        if (msgs.size() == 3) {
            CHECK(msgs[0].playerName.empty());
            CHECK_STR(msgs[1].body, "hello there how are you doing");
            CHECK_STR(msgs[2].playerName, "Tadzyk");
        }
        CHECK(ChatParser::parse({ "MENU", "Opzioni" }).empty());

        // I due punti letti come un altro segno attaccato al nome (righe vere).
        const std::pair<const char*, const char*> misreadColon[] = {
            { "[GLOBALE] Kleran3464} hope thats you", "Kleran3464" },
            { "[GLOBALE] RogueWave3} are the server dying here ?", "RogueWave3" },
            { "[GLOBALE] Synethc\xC2\xBB" "a good luck kiwa 07", "Synethc" }, // "Synethc»a"
            { "[GLOBALE] Kieran346'E the computers where you take out your ships", "Kieran346" },
            { "[GLOBALE] Swashpiates% server change", "Swashpiates" },
        };
        for (const auto& c : misreadColon) {
            CHECK(ChatParser::parseHeader(c.first, m));
            CHECK_STR(m.playerName, c.second);
        }
        CHECK(ChatParser::parseHeader("[GLOBALE] Kleran3464} hope thats you", m));
        CHECK_STR(m.body, "hope thats you");
        CHECK(ChatParser::parseHeader("[GLOBALE] 1<1eran3464} hope thats you", m)); // nome illeggibile: resta nel testo
        CHECK(m.playerName.empty());
        // Il pallino dei messaggi nuovi tra il canale e il nome (righe vere).
        CHECK(ChatParser::parseHeader("[GLOBALE] \xE2\x80\xA2" "Bearprime303a: reshare combat mission too pls", m));
        CHECK_STR(m.playerName, "Bearprime303a");
        CHECK_STR(m.body, "reshare combat mission too pls");
        real = ChatParser::parse({ "[GLOBALE] DarkFloomC: where? tGLOBALEl\xE2\x80\xA2travissgrey: tolling grim hex 500k" });
        CHECK_EQ(real.size(), (size_t)2);
        if (real.size() == 2) {
            CHECK_STR(real[0].body, "where?");
            CHECK_STR(real[1].playerName, "travissgrey");
            CHECK_STR(real[1].body, "tolling grim hex 500k");
        }
    }

    // --- Lo stesso messaggio in due letture ------------------------------------------

    std::vector<std::string> keys(const std::vector<const char*>& texts) {
        std::vector<std::string> out;
        for (const char* t : texts) out.push_back(TextUtils::normalizeForMatch(t));
        return out;
    }

    void testChatChannels() {
        section("chat letta: canali storpiati, righe fuse, messaggi corti");
        // Letture vere del registro.
        for (const char* tag : { "GLCBALE", "CLOBALE", "BALE", "'BALE", "BALE'", "tGLOBA", "CGI-OBALE", "tel-OBALEJ", "tGLCBALEJx", "GLOD3.LE",
                                 "CLOCALE", "GLOBALE" }) {
            CHECK_STR(ChatParser::canonicalChannel(tag), "GLOBALE");
        }
        CHECK_STR(ChatParser::canonicalChannel("LSE"), "LSE");
        CHECK_STR(ChatParser::canonicalChannel("PARTY"), "PARTY");
        for (const char* tag : { "GLOBEX", "SDR", "TEAM", "ANZ", "SQUADRA", "TUTTO" }) CHECK(ChatParser::canonicalChannel(tag).empty());

        auto msgs = ChatParser::parse({ "[GLCBALE] Cybercomet: geme absolutely ready for IAE free fly event", "[BALE] BloodHeaven: Got nothing in",
                                        "[tGLOBA][E] DarkstaR1st: MKS Connie is amazing" });
        CHECK(msgs.size() == 3);
        if (msgs.size() == 3) {
            CHECK_STR(msgs[0].displayPrefix(), "[GLOBALE] Cybercomet: ");
            CHECK_STR(msgs[1].displayPrefix(), "[GLOBALE] BloodHeaven: ");
            CHECK_STR(msgs[2].displayPrefix(), "[GLOBALE] DarkstaR1st: ");
        }
        // Il filtro dell'utente su un'organizzazione resta com'era.
        msgs = ChatParser::parse({ "[GLOBALE][LSE] Pilota: ci vediamo a Lorville" });
        CHECK(msgs.size() == 1 && ChatParser::matchesFilter(msgs[0], "LSE"));
        // Due righe fuse con il canale della seconda letto male in mezzo: il messaggio finisce lì.
        msgs = ChatParser::parse({ "[BALE] xCJ-6x: rone in need of an extra seto ands? IBALEJ BloodHeavenCResend me te S y han" });
        CHECK(msgs.size() == 1 && msgs[0].body == "rone in need of an extra seto ands?");
        msgs = ChatParser::parse({ "[GLOBALE] Foo: is GLOBAL chat down for everyone?" });
        CHECK(msgs.size() == 1 && msgs[0].body == "is GLOBAL chat down for everyone?");
        CHECK(ChatParser::tooShortToTranslate("o7") && ChatParser::tooShortToTranslate("W") && ChatParser::tooShortToTranslate("?"));
        CHECK(ChatParser::tooShortToTranslate("101") && !ChatParser::tooShortToTranslate("lol") && !ChatParser::tooShortToTranslate("thx m8"));
        // La stessa riga riletta male mezzo minuto dopo somiglia abbastanza da non tradurla di nuovo.
        const std::string good = TextUtils::normalizeForMatch("I was stuck in server error. could you give lead to bayhlen and have him invite?");
        const std::string bad = TextUtils::normalizeForMatch("—-1 wasstuck In servererror:tould you give lead to ay en and have him invite?");
        CHECK(ScreenMatch::likeness(good, bad) >= 0.75);
    }

    void testScreenMatch() {
        section("stesso messaggio in due letture");
        auto key = [](const char* t) { return TextUtils::normalizeForMatch(t); };
        // Letture vere dello stesso messaggio, una dopo l'altra.
        CHECK(ScreenMatch::sameText(key("tolling grim hex 500k"), key("tolljng grim hex 500k")));
        CHECK(ScreenMatch::sameText(key("tolling grim hex 500k"), key("tolliog grim hex 500k")));
        CHECK(ScreenMatch::sameText(key("you Witt keep all your stuff"), key("you will eep all your stuff")));
        CHECK(ScreenMatch::sameText(key("OK"), key("ok.")));
        CHECK(!ScreenMatch::sameText(key("same"), key("rude")));
        CHECK(!ScreenMatch::sameText(key("ok"), key("on")));
        CHECK(!ScreenMatch::sameText(key("where are you"), key("who cares?")));
        CHECK(ScreenMatch::sameNumbers(key("tolling grim hex 500k"), key("tolljng grim hex 500k")));
        CHECK(!ScreenMatch::sameNumbers(key("need 2 guys"), key("need 3 guys")));
        // Il nome finito nel testo, o perso con la prima parola.
        CHECK(ScreenMatch::likeness(key("hope thats you"), key("Kleran3464} hope thats you")) >= 0.7);
        CHECK(ScreenMatch::likeness(key("thats you"), key("hope thats you")) >= 0.7);
        CHECK(ScreenMatch::likeness(key("same"), key("rude")) < 0.7);
        CHECK(ScreenMatch::likeness(key("you"), key("hope thats you")) < 0.7); // pezzi troppo corti no

        // La chat scorre: un messaggio nuovo in fondo, quelli di prima letti diversi.
        auto match = ScreenMatch::align(keys({ "right im back", "if you guuys are here", "i just arrived" }),
                                        keys({ "if yog guuys are bere", "i just arrived", "gonna grab smth to eat and im ready" }));
        CHECK(match == std::vector<int>({ 1, 2, -1 }));
        // Tra due messaggi riconosciuti, una lettura troppo rovinata: è quello di prima.
        match = ScreenMatch::align(keys({ "ok your new", "not sure where that is", "the computers where you take out your ships" }),
                                   keys({ "ok your new", "certo dove", "the computers vvhere you take out your ships", "invite me again please" }));
        CHECK(match == std::vector<int>({ 0, 1, 2, -1 }));
        // Lo stesso testo detto di nuovo in fondo è un messaggio nuovo.
        match = ScreenMatch::align(keys({ "lol", "same", "ok" }), keys({ "same", "ok", "ok" }));
        CHECK(match ==std::vector<int>({ 1, 2, -1 }) || match == std::vector<int>({ 1, -1, 2 }));
        // Niente in comune, o nessuna lettura prima: tutti nuovi.
        CHECK(ScreenMatch::align(keys({ "who cares?" }), keys({ "where are you" })) == std::vector<int>({ -1 }));
        CHECK(ScreenMatch::align({}, keys({ "hi", "there" })) == std::vector<int>({ -1, -1 }));
    }

    // --- Audio e trascrizione ---------------------------------------------------------

    std::vector<float> tone(double seconds, double amplitude) {
        std::vector<float> s((size_t)(seconds * SpeechUtils::kSampleRate));
        for (size_t i = 0; i < s.size(); ++i) s[i] = (float)(amplitude * std::sin(2 * 3.14159265 * 220.0 * i / SpeechUtils::kSampleRate));
        return s;
    }

    // --- Prompt di traduzione ------------------------------------------------------------

    void testTranslationPrompt() {
        section("prompt di traduzione");
        TranslationRequest r;
        r.text = "come stai?";
        r.sourceLanguage = "Italian";
        r.targetLanguage = "English";
        r.keepNames = { "Lorville" };
        r.glossary = { { "QT", "quantum travel" } };
        std::string prefix = TranslationPrompt::prefix(r);
        CHECK(prefix.find("from Italian into English") != std::string::npos);
        CHECK(prefix.find("how are you?") != std::string::npos);  // esempio: la domanda si traduce
        CHECK(prefix.find("I'm down") != std::string::npos);      // tono da giocatore
        std::string req = TranslationPrompt::request(r);
        CHECK(req.find("<text>come stai?</text>") != std::string::npos);
        CHECK(req.find("Lorville") != std::string::npos);
        CHECK(req.find("\"QT\" -> \"quantum travel\"") != std::string::npos);
        CHECK(req.size() > 22 && req.compare(req.size() - 22, 22, "<|im_start|>assistant\n") == 0);

        CHECK(prefix.find("gg everyone") == std::string::npos); // esempio solo per la chat degli altri
        r.gamingTone = false;
        CHECK(TranslationPrompt::prefix(r).find("I'm down") == std::string::npos);
        TranslationRequest screen;
        screen.targetLanguage = "Italian";
        screen.fromScreen = true;
        const std::string screenPrefix = TranslationPrompt::prefix(screen);
        CHECK(screenPrefix.find("lol that was close, gg everyone") != std::string::npos);
        CHECK(screenPrefix.find("gg a tutti") != std::string::npos);
        r.sourceLanguage.clear();
        CHECK(TranslationPrompt::prefix(r).find("already in English") != std::string::npos);

        CHECK_STR(TranslationPrompt::cleanOutput("Translation: \"how are you?\"", "come stai?"), "how are you?");
        CHECK_STR(TranslationPrompt::cleanOutput("<text>where are you?</text>\nsecond line", "dove sei?"), "where are you?");
        CHECK_STR(TranslationPrompt::cleanOutput("  ", "originale"), "originale");
        CHECK_STR(TranslationPrompt::cleanOutput("\xC2\xAB" "ciao" "\xC2\xBB", "hi"), "ciao");
        CHECK_STR(TranslationPrompt::cleanOutput("\"quoted\"", "\"quoted\""), "\"quoted\"");
    }

    // --- Scrittura con la tastiera -------------------------------------------------------

    void testInputPlan() {
        section("scrittura con la tastiera");
        HKL layouts[32];
        int n = GetKeyboardLayoutList(32, layouts);
        HKL italian = nullptr;
        for (int i = 0; i < n; ++i) {
            if (LOWORD((UINT_PTR)layouts[i]) == 0x0410) italian = layouts[i];
        }
        if (!italian) {
            std::printf("  (tastiera italiana non installata: prova saltata)\n");
            return;
        }
        std::wstring dropped;
        auto plan = InputSender::planText(L"a@A", italian, InputSender::Unmappable::Transliterate, &dropped);
        CHECK_EQ(plan.size(), (size_t)3);
        if (plan.size() == 3) {
            CHECK(!plan[0].shift && !plan[0].ctrl && !plan[0].alt);
            CHECK(plan[1].ctrl && plan[1].alt); // @ = AltGr + ò
            CHECK(plan[2].shift);
        }
        plan = InputSender::planText(L"\u00F1\u4F60", italian, InputSender::Unmappable::Transliterate, &dropped);
        CHECK_EQ(plan.size(), (size_t)1); // ñ -> n, l'ideogramma si perde
        CHECK(dropped == L"\u4F60");
        plan = InputSender::planText(L"\u00F1", italian, InputSender::Unmappable::Unicode);
        CHECK(plan.size() == 1 && plan[0].unicode);
        plan = InputSender::planText(L"\u00E8\u00F2", italian, InputSender::Unmappable::Transliterate);
        CHECK(plan.size() == 2 && !plan[0].unicode && !plan[1].unicode); // è e ò ci sono sulla tastiera italiana
    }

    void testGlossary() {
        section("glossario");
        std::vector<Glossary::Entry> entries = { { "QT", "quantum travel" }, { "caricatore", "ammo" }, { "Ita", "Ita" } };
        auto relevant = Glossary::relevantTo(entries, "faccio il QT verso Italia");
        CHECK_EQ(relevant.size(), (size_t)1);
        CHECK_STR(Glossary::asWhisperHint(entries), "QT, caricatore, Ita");
    }

    void testLanguageGuess() {
        section("solo i messaggi in inglese");
        // Righe vere della chat.
        for (const char* en : { "i thought the undersuits would be flight suits?", "nahh just go boom :D",
                                "what buttons to undock the skylark", "Fuses and repair page o' display and thats it XD",
                                "LaffyJaff send a friend", "faaaah", "o7", "gg wp", "come on guys", "ok",
                                "i thought the undersui\xC3\x88uould be flight suits?", "clipper can fit a medbed" }) {
            const bool ok = LanguageGuess::isEnglish(en);
            CHECK(ok);
            if (!ok) std::printf("    non riconosciuto come inglese: \"%s\"\n", en);
        }
        for (const char* other : { "ty jeste\xC5\x9B pilotem?", "kaska sie przyda", "Keemon dawaj arte nastepna",
                                   "ciao, sono Enrico Papi, vuoi sapere quanto vale la tua astronave?", "ok raga andiamo",
                                   "gg a tutti", "danke f\xC3\xBCr die Hilfe", "hola amigos, como estan", "merci pour l'aide",
                                   "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82 \xD0\xB2\xD1\x81\xD0\xB5\xD0\xBC" }) { // "Привет всем"
            const bool en = LanguageGuess::isEnglish(other);
            CHECK(!en);
            if (en) std::printf("    preso per inglese: \"%s\"\n", other);
        }
        // Una risposta vera del modello finita in cinese.
        CHECK(LanguageGuess::hasCjk("certo dove \xC3\xA8\xE9\x82\xA3\xE4\xB8\xAA\xE6\xB6\x88\xE6\x81\xAF")); // "certo dove è那个消息"
        CHECK(!LanguageGuess::hasCjk("perch\xC3\xA9 no? \xC3\xA8 gi\xC3\xA0 finito"));
        CHECK(!LanguageGuess::hasCjk("\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82")); // il cirillico non è cinese
    }

    // Una voce {da, a} c'è?
    bool hasEntry(const std::vector<GamerSlang::Entry>& entries, const std::string& from, const std::string& to) {
        return std::any_of(entries.begin(), entries.end(), [&](const auto& e) { return e.first == from && e.second == to; });
    }

    void testGamerSlang() {
        section("vocabolario del videogiocatore");
        // Mandare un messaggio: frasi italiane -> sigle, solo col gergo da videogiocatore.
        auto send = GamerSlang::forSending("grazie ragazzi, torno subito", "Italian", true);
        CHECK(hasEntry(send, "grazie", "ty"));
        CHECK(hasEntry(send, "torno subito", "brb"));
        CHECK(GamerSlang::forSending("grazie ragazzi, torno subito", "Italian", false).empty());
        CHECK(GamerSlang::forSending("grazie ragazzi, torno subito", "English", true).empty()); // frasi solo in italiano
        // Il gergo italiano si traduce sempre; le sigle dette restano sigle.
        send = GamerSlang::forSending("pushiamo B, il nabbo sta feeddando", "Italian", false);
        CHECK(hasEntry(send, "pushiamo", "push"));
        CHECK(hasEntry(send, "nabbo", "noob"));
        CHECK(hasEntry(send, "feeddando", "feeding"));
        CHECK(hasEntry(GamerSlang::forSending("sono AFK cinque minuti", "Italian", false), "afk", "afk"));
        // Solo la frase più lunga: "bella partita ben giocata", non anche "bella partita".
        send = GamerSlang::forSending("bella partita ben giocata", "Italian", true);
        CHECK_EQ(send.size(), (size_t)1);
        CHECK(hasEntry(send, "bella partita ben giocata", "gg wp"));
        // Parole italiane normali uguali a una sigla: niente.
        CHECK(GamerSlang::forSending("la mia nave è al pad 3", "Italian", true).empty());

        // Feeddando -> "feeding", non "feed".
        CHECK(hasEntry(GamerSlang::forSending("il nabbo sta feeddando", "Italian", false), "feeddando", "feeding"));
        CHECK(hasEntry(GamerSlang::forSending("ha throwato la partita", "Italian", false), "throwato", "threw"));

        // Leggere la chat: le sigle rimaste nella traduzione diventano il loro significato.
        CHECK_STR(GamerSlang::explain("omw, ty per l'aiuto", "Italian"), "sto arrivando, grazie per l'aiuto");
        CHECK_STR(GamerSlang::explain("Brb, devo mangiare", "Italian"), "Torno subito, devo mangiare");
        CHECK_STR(GamerSlang::explain("TY", "Italian"), "Grazie");
        CHECK_STR(GamerSlang::explain("gg wp a tutti, lol", "Italian"), "gg wp a tutti, ahah"); // gg resta gg
        CHECK_STR(GamerSlang::explain("la mia nave", "Italian"), "la mia nave");              // "mia" è una parola normale
        CHECK_STR(GamerSlang::explain("typo, omwx", "Italian"), "typo, omwx");                // solo parole intere
        CHECK_STR(GamerSlang::explain("omw, ty", "German"), "omw, ty");                       // solo verso l'italiano
    }

    void testScreenCapture() {
        section("immagine per l'OCR");
        ScreenCapture::Image img;
        img.width = 10;
        img.height = 5;
        img.pixels.assign(10 * 5 * 4, 0);
        for (size_t i = 0; i < img.pixels.size(); i += 4) img.pixels[i] = img.pixels[i + 1] = img.pixels[i + 2] = (uint8_t)(i % 200);
        auto big = ScreenCapture::prepareForOcr(img, 2, 1000);
        CHECK_EQ(big.width, 20);
        CHECK_EQ(big.height, 10);
        auto clamped = ScreenCapture::prepareForOcr(img, 4, 25);
        CHECK(clamped.width <= 25 && clamped.height <= 25);
        CHECK(ScreenCapture::hash(img) != ScreenCapture::hash(big));
    }

    // ===== Chat di gioco dentro Iris =====
    void testGameChatInIris() {
        std::printf("Chat di gioco dentro Iris\n");
        // "Iris, scrivi in chat: ..." (il nome è già tolto da WakeWord).
        CHECK_STR(Assistant::chatCommandText("scrivi in chat: arrivo tra due minuti"), "arrivo tra due minuti");
        CHECK_STR(Assistant::chatCommandText("Scrivi in chat che sto arrivando"), "sto arrivando");
        CHECK_STR(Assistant::chatCommandText("manda in chat \"grazie a tutti\""), "grazie a tutti");
        // "Ricordati che...": in memoria senza modello (il 7B a volte leggeva lo strumento a voce invece di usarlo).
        CHECK_STR(Assistant::memoryCommandText("Ricordati che il mio wingman si chiama Luca."), "il mio wingman si chiama Luca");
        CHECK_STR(Assistant::memoryCommandText("ricorda: la base è a Lorville"), "la base è a Lorville");
        CHECK_STR(Assistant::memoryCommandText("Ricordati di alzare il carrello"), "");
        CHECK_STR(Assistant::memoryCommandText("ricordati che"), "");
        CHECK_STR(Assistant::memoryCommandText("ricordami la rotta"), "");
        CHECK_STR(Assistant::chatCommandText("di' in chat, ciao ragazzi"), "ciao ragazzi");
        CHECK_STR(Assistant::chatCommandText("che tempo fa a Roma?"), "");
        CHECK_STR(Assistant::chatCommandText("scrivi in chat"), "");

        // Le impostazioni di Voice to Chat (il suo settings.cfg vero, di oggi) diventano quelle della chat di Iris.
        const std::string vtc =
            "sourceLang=0\ntargetLang=1\nautoSend=1\ngamingTone=1\ndeviceName=Microfono (Scarlett Solo USB)\nsilenceThreshold=0.003\n"
            "micGain=1.2\ngpuMemoryLimitMb=4096\nchatOpenEnabled=1\nchatOpenKeyVk=13\nchatSendKeyVk=13\nchatOpenDelayMs=180\ntypingDelayMs=12\n"
            "unicodeFallback=0\noverlayEnabled=1\noverlayOpacity=205\nocrEnabled=1\nocrTargetLanguageIndex=0\nchatFilterTag=LSE\n"
            "ocrRegionX=88\nocrRegionY=593\nocrRegionW=530\nocrRegionH=259\nocrMaxMessages=6\nwindowX=-1251\nwindowY=460\n"
            "trigger=keyboard,6\ntrigger=keyboard,114\n";
        AppSettings s;
        s.micGain = 1.5f;
        s.gpuMemoryLimitMb = -1;
        s.overlayOpacity = 230;
        TalkTrigger irisKey;
        irisKey.vk = 6;
        s.triggers = { irisKey };
        s.importVoiceToChat(vtc);
        CHECK(s.voiceToChatImported);
        CHECK(s.chatTriggers.size() == 2 && s.chatTriggers[0].vk == 6 && s.chatTriggers[1].vk == 114);
        CHECK(s.triggers.size() == 1 && s.triggers[0].vk == 6); // i tasti di Iris restano
        CHECK(s.ocrEnabled && s.ocrRegionX == 88 && s.ocrRegionW == 530 && s.ocrRegionH == 259);
        CHECK(std::abs(s.silenceThreshold - 0.003) < 1e-9 && s.autoSend && s.gamingTone && s.targetLanguageIndex == 1);
        CHECK_STR(s.chatFilterTag, "LSE");
        // Microfono, memoria video e overlay restano quelli di Iris.
        CHECK(s.micGain == 1.5f && s.gpuMemoryLimitMb == -1 && s.overlayOpacity == 230);
        // E si salvano e rileggono (i tasti della chat a parte da quelli di Iris).
        AppSettings back = AppSettings::parse(s.serialize());
        CHECK(back.chatTriggers.size() == 2 && back.triggers.size() == 1 && back.voiceToChatImported && back.ocrRegionY == 593);

        // Il suggerimento per whisper della chat: luoghi di Star Citizen.
        CHECK(GameChat::whisperHint().find("Port Tressler") != std::string::npos || GameChat::whisperHint().find("Lorville") != std::string::npos);
    }

    // ===== Mining (Mining Signature Calculator + mining_ocr) =====

    // Un badge come quello dello scanner (segnaposto + numero) disegnato con GDI su uno sfondo scuro.
    ScreenCapture::Image renderBadge(const wchar_t* text, int w, int h) {
        ScreenCapture::Image img;
        HDC screen = GetDC(nullptr);
        HDC dc = CreateCompatibleDC(screen);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        HGDIOBJ oldBmp = SelectObject(dc, bmp);
        for (int y = 0; y < h; ++y) {
            RECT row{ 0, y, w, y + 1 };
            HBRUSH b = CreateSolidBrush(RGB(28 + y / 8, 38 + y / 6, 52 + y / 5)); // sfondo sfumato, come lo spazio
            FillRect(dc, &row, b);
            DeleteObject(b);
        }
        HBRUSH panel = CreateSolidBrush(RGB(58, 60, 44));
        RECT badge{ w / 2 - 95, h / 2 - 22, w / 2 + 95, h / 2 + 22 };
        FillRect(dc, &badge, panel);
        DeleteObject(panel);
        HBRUSH pin = CreateSolidBrush(RGB(240, 200, 60));
        HGDIOBJ oldBrush = SelectObject(dc, pin);
        HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
        Ellipse(dc, badge.left + 10, badge.top + 6, badge.left + 30, badge.top + 26); // il segnaposto
        RECT foot{ badge.left + 8, badge.top + 30, badge.left + 32, badge.top + 36 };
        FillRect(dc, &foot, pin);
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(pin);
        HFONT font = CreateFontW(-30, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        HGDIOBJ oldFont = SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(235, 240, 255));
        RECT tr{ badge.left + 48, badge.top, badge.right, badge.bottom };
        DrawTextW(dc, text, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        GdiFlush();
        img.width = w;
        img.height = h;
        img.pixels.assign((uint8_t*)bits, (uint8_t*)bits + (size_t)w * h * 4);
        for (size_t i = 3; i < img.pixels.size(); i += 4) img.pixels[i] = 255;
        SelectObject(dc, oldFont);
        DeleteObject(font);
        SelectObject(dc, oldBmp);
        DeleteObject(bmp);
        DeleteDC(dc);
        ReleaseDC(nullptr, screen);
        return img;
    }

    // Un badge "difficile" per confrontare gli OCR: carattere, colore dell'HUD, testo
    // piccolo (disegnato grande e poi rimpicciolito, quindi sfocato) e rumore.
    struct BadgeStyle {
        const wchar_t* face;
        int fontPx;
        COLORREF text;
        int shrink;  // 1 = nessuno, 2 = metà, 3 = un terzo...
        int noise;   // ampiezza del rumore (0-255)
    };

    ScreenCapture::Image renderStyledBadge(const wchar_t* text, const BadgeStyle& st, unsigned seed) {
        const int w = 320 * st.shrink, h = 130 * st.shrink;
        HDC screen = GetDC(nullptr);
        HDC dc = CreateCompatibleDC(screen);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        HGDIOBJ oldBmp = SelectObject(dc, bmp);
        for (int y = 0; y < h; ++y) {
            RECT row{ 0, y, w, y + 1 };
            HBRUSH b = CreateSolidBrush(RGB(18 + y / (6 * st.shrink), 24 + y / (5 * st.shrink), 36 + y / (4 * st.shrink)));
            FillRect(dc, &row, b);
            DeleteObject(b);
        }
        const int s = st.shrink;
        RECT badge{ w / 2 - 100 * s, h / 2 - 24 * s, w / 2 + 100 * s, h / 2 + 24 * s };
        HBRUSH panel = CreateSolidBrush(RGB(40, 46, 40));
        FillRect(dc, &badge, panel);
        DeleteObject(panel);
        HBRUSH pin = CreateSolidBrush(st.text);
        HGDIOBJ oldBrush = SelectObject(dc, pin);
        HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
        Ellipse(dc, badge.left + 10 * s, badge.top + 8 * s, badge.left + 30 * s, badge.top + 28 * s);
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(pin);
        HFONT font = CreateFontW(-st.fontPx * s, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, st.face);
        HGDIOBJ oldFont = SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, st.text);
        RECT tr{ badge.left + 46 * s, badge.top, badge.right, badge.bottom };
        DrawTextW(dc, text, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        GdiFlush();
        const uint8_t* src = (const uint8_t*)bits;
        ScreenCapture::Image img;
        img.width = w / s;
        img.height = h / s;
        img.pixels.resize((size_t)img.width * img.height * 4);
        unsigned rnd = seed * 2654435761u + 1;
        for (int y = 0; y < img.height; ++y) {
            for (int x = 0; x < img.width; ++x) {
                for (int c = 0; c < 3; ++c) {
                    int sum = 0;
                    for (int dy = 0; dy < s; ++dy)
                        for (int dx = 0; dx < s; ++dx) sum += src[(((size_t)(y * s + dy)) * w + (x * s + dx)) * 4 + c];
                    rnd = rnd * 1103515245u + 12345u;
                    const int noise = st.noise ? (int)((rnd >> 16) % (2 * st.noise + 1)) - st.noise : 0;
                    img.pixels[((size_t)y * img.width + x) * 4 + c] = (uint8_t)std::clamp(sum / (s * s) + noise, 0, 255);
                }
                img.pixels[((size_t)y * img.width + x) * 4 + 3] = 255;
            }
        }
        SelectObject(dc, oldFont);
        DeleteObject(font);
        SelectObject(dc, oldBmp);
        DeleteObject(bmp);
        DeleteDC(dc);
        ReleaseDC(nullptr, screen);
        return img;
    }

    // PP-OCR contro l'OCR di Windows sugli stessi badge (serve PP-OCR scaricato: lo scarica qui).
    int comparePpOcr() {
        std::printf("PP-OCR (Citizen Assistant) contro OCR di Windows\n");
        std::string error;
        if (!PpOcr::filesPresent()) {
            std::printf("  scarico runtime e modello...\n");
            if (!PpOcr::ensureFiles(nullptr, nullptr, error)) {
                std::printf("  non scaricato: %s\n", error.c_str());
                return 1;
            }
        }
        if (!PpOcr::load(error)) {
            std::printf("  PP-OCR non si carica: %s\n", error.c_str());
            return 1;
        }
        const auto mats = Mining::defaultMaterials();
        const BadgeStyle styles[] = {
            { L"Segoe UI", 30, RGB(235, 240, 255), 1, 0 },   { L"Bahnschrift", 26, RGB(90, 220, 255), 1, 6 },
            { L"Consolas", 24, RGB(255, 190, 70), 1, 10 },  { L"Arial", 28, RGB(120, 255, 170), 2, 8 },
            { L"Bahnschrift", 30, RGB(200, 230, 255), 3, 12 }, { L"Segoe UI", 34, RGB(90, 220, 255), 3, 18 },
            { L"Consolas", 32, RGB(255, 140, 90), 2, 20 },
        };
        int samples = 0, okPp = 0, okWin = 0;
        long long msPp = 0, msWin = 0;
        unsigned seed = 7;
        for (const auto& style : styles) {
            int styleOkPp = 0, styleOkWin = 0, styleN = 0;
            for (int i = 0; i < 12; ++i) {
                const auto& m = mats[(seed = seed * 1664525u + 1013904223u) % (mats.size() - 3)];
                const int q = 1 + (int)((seed >> 8) % 6);
                const int value = m.base * q;
                wchar_t text[32];
                swprintf(text, 32, L"%d,%03d", value / 1000, value % 1000);
                const ScreenCapture::Image img = renderStyledBadge(text, style, seed);
                PpOcr::setEnabled(true);
                const auto a = MiningScanner::readImage(img, mats, 40, {});
                PpOcr::setEnabled(false);
                const auto b = MiningScanner::readImage(img, mats, 40, {});
                PpOcr::setEnabled(true);
                ++samples;
                ++styleN;
                if (a.decoded.hasValue && a.decoded.value == value) ++okPp, ++styleOkPp;
                if (b.decoded.hasValue && b.decoded.value == value) ++okWin, ++styleOkWin;
                msPp += a.durationMs;
                msWin += b.durationMs;
            }
            std::wprintf(L"  %-12ls %2dpx /%d rumore %2d:  PP-OCR %2d/%d   Windows %2d/%d\n", style.face, style.fontPx, style.shrink, style.noise,
                         styleOkPp, styleN, styleOkWin, styleN);
        }
        std::printf("  Totale: PP-OCR %d/%d (%.0f ms a lettura)   OCR di Windows %d/%d (%.0f ms a lettura)\n", okPp, samples,
                    (double)msPp / samples, okWin, samples, (double)msWin / samples);
        return 0;
    }

    bool hasMatch(const Mining::Decoded& d, const char* namePart, int quantity) {
        for (const auto& m : d.matches) {
            if (m.name.find(namePart) != std::string::npos && m.quantity == quantity) return true;
        }
        return false;
    }

    void testMining() {
        // Le prove veloci leggono solo con l'OCR di Windows: niente modelli, risultati sempre uguali.
        PpOcr::setEnabled(false);
        std::printf("Mining\n");
        const auto mats = Mining::defaultMaterials();

        section("signature dal testo dell'OCR");
        auto d = Mining::decodeText("2 16,960", mats); // il segnaposto letto come "2"
        CHECK(d.hasValue && d.value == 16960 && !d.corrected && hasMatch(d, "Copper", 4) && d.matches.size() == 1);
        d = Mining::decodeText("216,960", mats); // segnaposto incollato al numero
        CHECK(d.value == 16960 && d.corrected && hasMatch(d, "Copper", 4));
        d = Mining::decodeText("16.960", mats);
        CHECK(d.value == 16960 && !d.corrected);
        d = Mining::decodeText("16 960", mats);
        CHECK(d.value == 16960 && !d.corrected);
        d = Mining::decodeText("l6,96O", mats); // lettere scambiate per cifre
        CHECK(d.value == 16960 && hasMatch(d, "Copper", 4));
        d = Mining::decodeText("7,080", mats);
        CHECK(d.value == 7080 && hasMatch(d, "Beryl", 2));
        d = Mining::decodeText("16,968", mats); // una cifra confusa (8 al posto di 0): una sola correzione possibile
        CHECK(d.value == 16960 && d.corrected && d.readValue == 16968);
        d = Mining::decodeText("2 700", mats); // NON diventa 2700 per forza: niente in tabella
        CHECK(d.hasValue && d.matches.empty());
        // Letture vere dello scanner (catture di Mining Signature Calculator) con lettere al posto delle cifre.
        d = Mining::decodeText("7,OCO", mats);
        CHECK(d.value == 7080 && d.corrected && hasMatch(d, "Beryl", 2));
        d = Mining::decodeText("Q 10,C20", mats);
        CHECK(d.value == 10620 && d.corrected && hasMatch(d, "Beryl", 3));
        d = Mining::decodeText("2 li,565", mats);
        CHECK(d.value == 11565 && hasMatch(d, "Titanium", 3));
        d = Mining::decodeText("SHIP PORTS", mats);
        CHECK(!d.hasValue);
        d = Mining::decodeText("17,740", mats); // la prima cifra NON si toglie (5 cifre, virgola dopo il 17)
        CHECK(d.readValue == 17740 && d.value != 7740);
        d = Mining::decodeText("245m 9.0km", mats); // unità di misura dell'HUD: niente cifre inventate
        CHECK(d.matches.empty());
        d = Mining::decodeText("2 10,820 80 100", mats); // numeri dell'HUD accanto: vince quello con la virgola
        CHECK(d.readValue == 10820 && d.value == 10620 && hasMatch(d, "Beryl", 3));
        d = Mining::decodeText("NESSUN NUMERO", mats);
        CHECK(!d.hasValue);
        d = Mining::decodeText("6,000", mats);
        CHECK(hasMatch(d, "Scrap", 3)); // il salvage
        d = Mining::decodeValue(16000, mats);
        // Tre possibilità: prima la roccia, poi i rottami, in fondo il deposito a terra.
        CHECK(d.matches.size() == 3 && hasMatch(d, "Savrilium", 5) && hasMatch(d, "Scrap", 8) && hasMatch(d, "ROC", 4));
        CHECK(d.matches.size() == 3 && d.matches[0].name == "Savrilium" && d.matches[2].kind == "roc");
        d = Mining::decodeValue(12345, mats);
        CHECK(d.hasValue && d.matches.empty());
        CHECK_STR(Mining::formatValue(16960), "16.960");
        CHECK_STR(Mining::summary(Mining::decodeValue(8541, mats)), "8.541: sconosciuta");
        CHECK_STR(Mining::summary(Mining::decodeValue(8540, mats)), "8.540 = Iron (Ferro) × 2");
        CHECK_STR(Mining::summary(Mining::decodeValue(4240, mats)), "4.240 = Copper (Rame) × 1");

        section("letture vere dell'HUD (dal log di Iris)");
        // La distanza incollata dietro la signature, e "S" al posto del 6: si cambia solo dove c'era una lettera.
        d = Mining::decodeText("SCONOSCII 0 11,5SS9.OkM", mats); // prima: "5.559: sconosciuta"
        CHECK(d.value == 11565 && d.corrected && d.matches.size() == 1 && hasMatch(d, "Titanium", 3));
        d = Mining::decodeText("11,5SS", mats); // con ogni cifra libera sarebbe anche Agricium × 3 (11.655)
        CHECK(d.value == 11565 && d.corrected && d.matches.size() == 1);
        // La distanza dell'HUD non è una signature (prima: "19.500 = Torite × 5 (probabile)").
        CHECK(Mining::decodeText("SCONOSCIUTO 19.5kM", mats).matches.empty());
        CHECK(Mining::decodeText("Q 17,200 scoNosc 30.4kP", mats).value == 17200);
        // Numeri dell'HUD più piccoli di ogni firma base, e tacche unite: niente "340: sconosciuta".
        for (const char* t : { "280 300 320 340 sot", "A.B. 100%", "L88Y", "240", "1622 5.0M IIS", "PRONTO 24\xC3\xB8 260 3\xC3\xB8\xC3\xB8 269" }) {
            CHECK(!Mining::decodeText(t, mats).hasValue);
        }
        // Lo zero col taglio del carattere dell'HUD ("ø"): uno zero vero, la lettura è certa.
        d = Mining::decodeText("16.96\xC3\xB8", mats);
        CHECK(d.value == 16960 && !d.corrected && hasMatch(d, "Copper", 4));
        // Una lettera che potrebbe essere due cifre in tabella: "1S,850" = Quantanium × 5 o Ouratite × 5 (16.850).
        d = Mining::decodeText("1S,850", mats);
        CHECK(d.value == 15850 && d.corrected && d.note.find("16.850") != std::string::npos);
        // Solo un numero letto pulito, nella forma del badge, è una "sconosciuta" da annotare.
        d = Mining::decodeText("32,300", mats);
        CHECK(d.hasValue && d.matches.empty() && d.wellFormed);
        CHECK(!Mining::decodeText("021565", mats).wellFormed);
        CHECK(!Mining::decodeText("2 700", mats).wellFormed);
        CHECK(Mining::decodeValue(32300, mats).wellFormed);
        CHECK(Mining::decodeText("2 16,960", mats).wellFormed);
        // Un nome con la virgola si salva e si rilegge uguale.
        const auto commaTable = Mining::parseTable(Mining::formatTable({ { "Rame, puro", 4240, "minerale" } }));
        CHECK(commaTable.size() == 1 && commaTable[0].name == "Rame, puro" && commaTable[0].base == 4240);

        section("numeri a voce e comandi");
        CHECK_EQ(Mining::parseItalianNumber("sedicimila novecentosessanta"), 16960LL);
        CHECK_EQ(Mining::parseItalianNumber("settemilaottanta"), 7080LL);
        CHECK_EQ(Mining::parseItalianNumber("duemila"), 2000LL);
        CHECK_EQ(Mining::parseItalianNumber("centotto"), 108LL);
        CHECK_EQ(Mining::parseItalianNumber("mille e cento"), 1100LL);
        CHECK_EQ(Mining::parseItalianNumber("ventitré"), 23LL);
        CHECK_EQ(Mining::parseItalianNumber("trentaquattromila"), 34000LL);
        CHECK_EQ(Mining::parseItalianNumber("ciao"), -1LL);
        auto v = Mining::parseVoice("che minerale è?");
        CHECK(v.kind == Mining::VoiceKind::Scan);
        v = Mining::parseVoice("Scansiona la roccia");
        CHECK(v.kind == Mining::VoiceKind::Scan);
        v = Mining::parseVoice("signature 16960");
        CHECK(v.kind == Mining::VoiceKind::Value && v.value == 16960);
        v = Mining::parseVoice("signature 16.960");
        CHECK(v.kind == Mining::VoiceKind::Value && v.value == 16960);
        v = Mining::parseVoice("firma sedicimila novecentosessanta");
        CHECK(v.kind == Mining::VoiceKind::Value && v.value == 16960);
        v = Mining::parseVoice("che minerale è settemilaottanta?");
        CHECK(v.kind == Mining::VoiceKind::Value && v.value == 7080);
        v = Mining::parseVoice("settemila ottanta");
        CHECK(v.kind == Mining::VoiceKind::Value && v.value == 7080);
        CHECK(Mining::parseVoice("che tempo fa domani?").kind == Mining::VoiceKind::None);
        CHECK(Mining::parseVoice("quanto vale un minerale da 3000 aUEC?").kind == Mining::VoiceKind::None);
        CHECK(Mining::parseVoice("metti un timer di 10 minuti").kind == Mining::VoiceKind::None);
        CHECK(Mining::parseVoice("alza il volume").kind == Mining::VoiceKind::None);

        section("tabella dei materiali");
        std::vector<std::string> errors;
        auto t = Mining::parseTable("name,base_signature\nCOPPER,4240\nBERYL;3540\nSCRAP,2000,salvage\nrotta,abc\n# commento\n", &errors);
        CHECK(t.size() == 3 && t[0].base == 4240 && t[1].name == "BERYL" && t[2].kind == "salvage");
        CHECK(errors.size() == 1); // "rotta,abc"; l'intestazione non è un errore
        auto round = Mining::parseTable(Mining::formatTable(mats));
        CHECK(round.size() == mats.size());
        bool same = true;
        for (const auto& m : mats) same = same && std::find(round.begin(), round.end(), m) != round.end();
        CHECK(same);
        std::string csv;
        CHECK(!Mining::decodeMscDatabase("non è un database", csv));

        section("impostazioni del mining");
        AppSettings fresh = AppSettings::parse("");
        CHECK(fresh.miningScanTriggers.size() == 1 && fresh.miningScanTriggers[0].vk == VK_ADD);
        CHECK(fresh.miningManualTriggers.size() == 1 && fresh.miningManualTriggers[0].vk == VK_SUBTRACT);
        fresh.miningRegionX = 1147;
        fresh.miningRegionY = 331;
        fresh.miningRegionW = 293;
        fresh.miningRegionH = 126;
        fresh.miningManualTriggers.clear(); // tolto: deve restare tolto
        fresh.miningAuto = true;
        AppSettings back = AppSettings::parse(fresh.serialize());
        back.sanitize();
        CHECK(back.miningRegionX == 1147 && back.miningRegionW == 293 && back.miningAuto);
        CHECK(back.miningScanTriggers.size() == 1 && back.miningManualTriggers.empty());
        // Opacità di sfondo e scritte separate: lo sfondo può sparire, le scritte no.
        CHECK(fresh.miningOverlayOpacity == 235 && fresh.miningOverlayTextOpacity == 255);
        fresh.miningOverlayOpacity = 0;
        fresh.miningOverlayTextOpacity = 179;
        back = AppSettings::parse(fresh.serialize());
        back.sanitize();
        CHECK(back.miningOverlayOpacity == 0 && back.miningOverlayTextOpacity == 179);
        back = AppSettings::parse("miningOverlayOpacity=-40\nminingOverlayTextOpacity=3\n");
        back.sanitize();
        CHECK(back.miningOverlayOpacity == 0 && back.miningOverlayTextOpacity == 51);
        // Informazioni base (solo materiale e quantità) e zona della chat girata.
        CHECK(!fresh.miningOverlayBasic && fresh.ocrRegionAngle == 0.0);
        fresh.miningOverlayBasic = true;
        fresh.ocrRegionX = 100;
        fresh.ocrRegionY = 200;
        fresh.ocrRegionW = 600;
        fresh.ocrRegionH = 220;
        fresh.ocrRegionAngle = -3.5;
        back = AppSettings::parse(fresh.serialize());
        CHECK(back.miningOverlayBasic && std::fabs(back.ocrRegionAngle + 3.5) < 1e-9);
        back = AppSettings::parse("ocrRegionX=1\nocrRegionY=1\nocrRegionW=300\nocrRegionH=100\nocrRegionAngle=47.26\n");
        back.sanitize();
        CHECK(back.ocrRegionAngle == 20.0);
        back = AppSettings::parse("ocrRegionX=1\nocrRegionY=1\nocrRegionW=300\nocrRegionH=100\nocrRegionAngle=2.34\n");
        back.sanitize();
        CHECK(std::fabs(back.ocrRegionAngle - 2.3) < 1e-9);
        back = AppSettings::parse("ocrRegionAngle=5\n"); // senza zona non c'è niente da girare
        back.sanitize();
        CHECK(back.ocrRegionAngle == 0.0);

        section("zona girata: la chat storta si raddrizza");
        {
            // Una riga chiara che scende verso destra di 8 gradi (come la chat sull'HUD curvo).
            ScreenCapture::Image src;
            src.width = 240;
            src.height = 140;
            src.pixels.assign((size_t)src.width * src.height * 4, 0);
            const double slope = std::tan(8.0 * 3.14159265358979323846 / 180.0);
            for (int x = 0; x < src.width; ++x) {
                const int yc = (int)std::lround(70 + (x + 0.5 - 120) * slope - 0.5);
                for (int y = yc - 1; y <= yc + 1; ++y) {
                    if (y < 0 || y >= src.height) continue;
                    uint8_t* p = &src.pixels[((size_t)y * src.width + x) * 4];
                    p[0] = p[1] = p[2] = 255;
                }
            }
            auto worstRowError = [&](double degrees) {
                const ScreenCapture::Image out = ScreenCapture::straighten(src, 120.0, 70.0, 180, 40, degrees);
                int worst = 0;
                for (int u = 10; u < out.width - 10; ++u) {
                    int bestRow = 0, bestValue = -1;
                    for (int v = 0; v < out.height; ++v) {
                        const int value = out.pixels[((size_t)v * out.width + u) * 4];
                        if (value > bestValue) {
                            bestValue = value;
                            bestRow = v;
                        }
                    }
                    worst = std::max(worst, std::abs(bestRow - out.height / 2));
                }
                return worst;
            };
            CHECK(worstRowError(8.0) <= 1);  // girata come la chat: la riga è dritta, al centro
            CHECK(worstRowError(0.0) >= 8);  // senza girarla resta storta
            CHECK(worstRowError(-8.0) >= 8); // girata dalla parte sbagliata: ancora di più
            // Angolo zero: lo stesso ritaglio della cattura dritta.
            const ScreenCapture::Image same = ScreenCapture::straighten(src, 120.0, 70.0, 180, 40, 0.0);
            bool identical = same.width == 180 && same.height == 40;
            for (int v = 0; identical && v < 40; ++v) {
                for (int u = 0; identical && u < 180; ++u) {
                    identical = same.pixels[((size_t)v * 180 + u) * 4] == src.pixels[((size_t)(v + 50) * src.width + (u + 30)) * 4];
                }
            }
            CHECK(identical);
            // Il rettangolo che contiene la zona girata: più grande, stesso centro.
            const ScreenCapture::Bounds b = ScreenCapture::rotatedBounds(100, 200, 600, 220, 10.0);
            CHECK(b.width > 600 && b.height > 220);
            CHECK(std::abs((b.x + b.width / 2) - 400) <= 1 && std::abs((b.y + b.height / 2) - 310) <= 1);
            const auto corners = ScreenCapture::rotatedCorners(100, 200, 600, 220, 10.0);
            CHECK(corners.size() == 4 && corners[1].y > corners[0].y); // senso orario: il lato di destra scende
        }

        section("PP-OCR (Citizen Assistant): parti pure");
        {
            // CTC: vuoti (classe 0) e ripetizioni saltati; la sicurezza è la media dei tenuti.
            const std::vector<std::string> chars = { "blank", "1", "6", ",", "9", " " };
            // passi: 1 1 _ 6 , 9 9 _ 6  ->  "16,96"
            const int seq[] = { 1, 1, 0, 2, 3, 4, 4, 0, 2 };
            std::vector<float> probs(9 * chars.size(), 0.0f);
            for (int t = 0; t < 9; ++t) probs[(size_t)t * chars.size() + seq[t]] = t == 0 ? 0.5f : 0.9f;
            const PpOcr::Line line = PpOcr::ctcDecode(probs.data(), 9, (int)chars.size(), chars);
            CHECK_STR(line.text, "16,96");
            CHECK(std::fabs(line.confidence - (0.5f + 0.9f * 4) / 5) < 1e-4);
            // Il tensore: altezza 48, larghezza in proporzione, valori tra -1 e 1, tre canali uguali.
            MiningImage::Gray g;
            g.w = 100;
            g.h = 25;
            g.p.assign(2500, 0);
            for (int x = 50; x < 100; ++x)
                for (int y = 0; y < 25; ++y) g.p[(size_t)y * 100 + x] = 255;
            int width = 0;
            const auto tensor = PpOcr::toTensor(g, width);
            CHECK(width == 192 && tensor.size() == (size_t)3 * 48 * 192);
            CHECK(tensor[(size_t)10 * 192 + 10] < -0.99f && tensor[(size_t)10 * 192 + 150] > 0.99f);
            CHECK(tensor[(size_t)2 * 48 * 192 + 10 * 192 + 150] == tensor[(size_t)10 * 192 + 150]);
            // Il ritaglio stretto attorno alle cifre chiare.
            MiningImage::Gray box;
            box.w = 200;
            box.h = 80;
            box.p.assign(16000, 20);
            for (int y = 30; y < 50; ++y)
                for (int x = 60; x < 140; ++x) box.p[(size_t)y * 200 + x] = 230;
            const MiningImage::Gray t = PpOcr::tight(box);
            CHECK(t.w > 80 && t.w < 100 && t.h > 20 && t.h < 30);
        }

        section("mining: rarità, depositi a terra, annuncio a voce");
        {
            const auto mats = Mining::defaultMaterials();
            const Mining::OreInfo* borase = Mining::oreInfo("Borase");
            CHECK(borase && borase->rarity == 4 && std::string(borase->secondary).find("Bexalite") != std::string::npos);
            CHECK(Mining::oreInfo("Copper (Rame)") && Mining::oreInfo("Copper (Rame)")->rarity == 2);
            CHECK(Mining::oreInfo("Deposito ROC") == nullptr);
            CHECK_STR(Mining::rarityName(5), "LEGGENDARIO");
            CHECK(hasMatch(Mining::decodeValue(12000, mats, 40), "ROC", 3));       // 3 depositi ROC (o 6 rottami)
            CHECK(hasMatch(Mining::decodeValue(9000, mats, 40), "FPS", 3));
            CHECK(!hasMatch(Mining::decodeValue(36000, mats, 40), "ROC", 9));      // a terra al massimo 8
            CHECK(hasMatch(Mining::decodeValue(36000, mats, 40), "Scrap", 18));    // i rottami no
            CHECK(Mining::groundDeposits().size() == 2);
            // Si dice solo una lettura certa e senza alternative.
            CHECK_STR(MiningScanner::spokenText(Mining::decodeValue(10710, mats, 40)), "Borase per 3");
            CHECK_STR(MiningScanner::spokenText(Mining::decodeValue(4240, mats, 40)), "Rame");
            CHECK(MiningScanner::spokenText(Mining::decodeValue(16000, mats, 40)).empty()); // Savrilium × 5 o Scrap × 8
            CHECK(MiningScanner::spokenText(Mining::decodeValue(13337, mats, 40)).empty()); // sconosciuta
            // La tabella salvata conosce i nuovi tipi.
            const auto back = Mining::parseTable(Mining::formatTable(mats));
            CHECK(back.size() == mats.size() && std::any_of(back.begin(), back.end(), [](const Mining::Material& m) { return m.kind == "fps"; }));
        }

        section("immagine: badge e pulizia");
        const ScreenCapture::Image badge = renderBadge(L"16,960", 320, 130);
        const MiningImage::Steps st = MiningImage::process(badge, 120);
        // Il badge sta al centro (pannello x 65..255, y 43..87; segnaposto da x 73, cifre da x 113 a ~200):
        // il ritaglio deve contenere le cifre, e l'icona va tolta.
        std::printf("  badge trovato: x=%d y=%d w=%d h=%d, icona tolta fino a %d\n", st.badge.x, st.badge.y, st.badge.w, st.badge.h, st.iconCut);
        CHECK(st.badge.x <= 113 && st.badge.x + st.badge.w >= 195 && st.badge.y <= 55 && st.badge.y + st.badge.h >= 75);
        CHECK(st.iconCut > 0 && st.badge.x + st.iconCut <= 113 && st.badge.x + st.iconCut >= 95);
        CHECK(st.badge.w < 320); // non tutta la zona
        CHECK(st.resized.h == 120 && st.binary.h == 120);
        if (OcrEngineWrapper::installedLanguages().empty()) {
            std::printf("  (OCR di Windows non installato: lettura saltata)\n");
        } else {
            for (const wchar_t* text : { L"16,960", L"7,080", L"38,250", L"6,000" }) {
                const auto r = MiningScanner::readImage(renderBadge(text, 320, 130), mats, 40, {});
                std::printf("  %ls -> \"%s\" -> %s (%lld ms)\n", text, r.ocrText.c_str(), Mining::summary(r.decoded).c_str(), r.durationMs);
                std::wstring digits;
                for (const wchar_t* p = text; *p; ++p) {
                    if (*p >= L'0' && *p <= L'9') digits += *p;
                }
                CHECK(r.decoded.hasValue && r.decoded.value == _wtoi(digits.c_str()) && !r.decoded.matches.empty());
            }
        }
    }

    void testVoiceEffect() {
        std::printf("Effetti sulla voce\n");
        // Una "voce" finta: vocale a 180 Hz con armoniche, a sillabe, più un fruscio da consonante.
        const int sr = TtsEngine::kSampleRate;
        std::vector<float> voice(sr * 2);
        unsigned noise = 1;
        for (size_t i = 0; i < voice.size(); ++i) {
            const double t = (double)i / sr;
            const double syllable = 0.5 + 0.5 * std::sin(2 * 3.14159265 * 3 * t);
            double v = 0;
            for (int h = 1; h <= 12; ++h) v += std::sin(2 * 3.14159265 * 180 * h * t) / h;
            noise = noise * 1664525u + 1013904223u;
            v = 0.12 * v * syllable + 0.01 * ((int)(noise >> 9) / 4194304.0 - 1.0);
            voice[i] = (float)v;
        }
        auto rms = [](const std::vector<float>& s, size_t from, size_t to) {
            double e = 0;
            for (size_t i = from; i < to; ++i) e += (double)s[i] * s[i];
            return std::sqrt(e / std::max<size_t>(1, to - from));
        };
        const double dryRms = rms(voice, voice.size() / 2, voice.size());

        CHECK(VoiceEffect::presets().size() >= 4 && std::string(VoiceEffect::presets()[0].id) == "nessuno");
        CHECK(VoiceEffect::isValid("robot") && !VoiceEffect::isValid("boh"));
        VoiceEffect fx(sr);
        CHECK(!fx.active());
        std::vector<float> same = voice;
        fx.process(same.data(), same.size());
        CHECK(same == voice); // "nessuno": identica
        fx.setPreset("boh");
        CHECK(fx.preset() == "nessuno");

        for (const char* id : { "ia", "androide", "robot" }) {
            fx.setPreset(id);
            CHECK(fx.active());
            std::vector<float> out = voice;
            // A pezzi, come arriva dalla voce neurale.
            for (size_t at = 0; at < out.size(); at += 1000) fx.process(out.data() + at, std::min<size_t>(1000, out.size() - at));
            bool finite = true;
            float peak = 0;
            for (float x : out) {
                finite = finite && std::isfinite(x);
                peak = std::max(peak, std::fabs(x));
            }
            const double wetRms = rms(out, out.size() / 2, out.size());
            CHECK(finite && peak <= 1.0f);
            CHECK(wetRms > dryRms * 0.4 && wetRms < dryRms * 2.5); // volume simile a quello di partenza
            CHECK(out != voice);
            const std::vector<float> t = fx.tail();
            CHECK(!t.empty() && std::fabs(t.back()) < 1e-3f); // la coda sfuma
            // Dopo la coda si riparte dal silenzio: niente eco rimasta che salta fuori (clic).
            std::vector<float> after(400, 0.0f);
            fx.process(after.data(), after.size());
            float leftover = 0;
            for (float x : after) leftover = std::max(leftover, std::fabs(x));
            CHECK(leftover < 1e-6f);
            std::printf("  %-9s volume %.2f del originale, picco %.2f\n", id, wetRms / dryRms, peak);
        }

        // Le misure della voce (per --voce-coerenza) e i file di riferimento della voce neurale.
        std::vector<float> tone(sr * 6, 0.0f);
        for (size_t i = sr; i < (size_t)sr * 5; ++i) {
            const double t = (double)i / sr;
            tone[i] = (float)(0.3 * std::sin(2 * 3.14159265 * 200 * t) + 0.1 * std::sin(2 * 3.14159265 * 400 * t));
        }
        const double f0 = VoiceMeasure::median(VoiceMeasure::pitch(tone.data(), tone.size(), sr));
        CHECK(f0 > 197 && f0 < 203);
        CHECK(std::fabs(VoiceMeasure::cosine({ 1, 2, 3 }, { 2, 4, 6 }) - 1.0) < 1e-9 && std::fabs(VoiceMeasure::cosine({ 1, 0 }, { 0, 1 })) < 1e-9);
        CHECK(VoiceMeasure::resample(tone.data(), 24000, 24000, 16000).size() == 16000);
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        const std::wstring wav = std::wstring(tmp) + L"iris_voce_prova.wav";
        CHECK(NeuralVoice::writeWav(wav, tone, sr));
        std::vector<float> back;
        int rate = 0;
        std::string error;
        CHECK(NeuralVoice::readWav(wav, back, rate, &error) && rate == sr && back.size() == tone.size());
        CHECK(std::fabs(back[sr * 2] - tone[sr * 2]) < 1e-4f);
        CHECK(NeuralVoice::cleanReference(back, rate, &error));
        CHECK(back.size() > (size_t)sr * 4 && back.size() < (size_t)sr * 4.5); // via il silenzio ai lati
        std::vector<float> shortOne(tone.begin() + sr, tone.begin() + sr * 2);
        CHECK(!NeuralVoice::cleanReference(shortOne, sr, &error) && error.find("corto") != std::string::npos);
        CHECK(!NeuralVoice::readWav(std::wstring(tmp) + L"non_esiste_proprio.wav", back, rate, &error));
        DeleteFileW(wav.c_str());
        CHECK(NeuralVoice::isPreset(NeuralVoice::presets().front().id) && !NeuralVoice::isPreset("boh"));
        AppSettings vs = AppSettings::parse("neuralVoice=boh\n");
        vs.sanitize();
        CHECK(vs.neuralVoice == NeuralVoice::presets().front().id);
        vs = AppSettings::parse("neuralVoice=file\nneuralVoiceFile=C:\\voce.wav\n");
        vs.sanitize();
        AppSettings vb = AppSettings::parse(vs.serialize());
        CHECK(vb.neuralVoice == "file" && vb.neuralVoiceFile == "C:\\voce.wav");
    }

    void testUpdater() {
        std::printf("Aggiornamento automatico\n");
        section("versioni");
        CHECK(Updater::compareVersions("V1.1", "1.1.0") == 0);
        CHECK(Updater::compareVersions("V1.1", "1.2.0") < 0);
        CHECK(Updater::compareVersions("1.10.0", "1.9.9") > 0); // numeri, non lettere
        CHECK(Updater::compareVersions("v2", "1.99.99") > 0);
        CHECK(Updater::compareVersions("1.2.0-beta", "1.2.0") == 0);
        CHECK(Updater::compareVersions(Updater::currentVersion(), "0.0.1") > 0);

        section("release di GitHub");
        const std::string json = R"({"tag_name":"V1.3.0","name":"Iris V1.3.0","body":"Novità","html_url":"https://github.com/x/y/releases/tag/V1.3.0",
            "draft":false,"prerelease":false,"assets":[
              {"name":"note.txt","browser_download_url":"https://github.com/x/y/releases/download/V1.3.0/note.txt","size":5},
              {"name":"iris.exe","browser_download_url":"https://github.com/x/y/releases/download/V1.3.0/iris.exe","size":49396736,
               "digest":"sha256:B417BBBC4028DBD809FFFF1FB0F6584E29E7121E183DE6BF53453B69F5C18E39"}]})";
        Updater::Release r;
        std::string error;
        CHECK(Updater::parseRelease(json, r, error));
        CHECK_STR(r.version, "1.3.0");
        CHECK_STR(r.assetUrl, "https://github.com/x/y/releases/download/V1.3.0/iris.exe");
        CHECK(r.size == 49396736);
        CHECK_STR(r.sha256, "b417bbbc4028dbd809ffff1fb0f6584e29e7121e183de6bf53453b69f5c18e39");
        CHECK_STR(r.notes, "Novità");
        CHECK(!Updater::parseRelease(R"({"tag_name":"V1.4","assets":[{"name":"x.zip","browser_download_url":"https://a/x.zip"}]})", r, error));
        CHECK(!Updater::parseRelease(R"({"tag_name":"V1.4","draft":true,"assets":[]})", r, error));
        CHECK(!Updater::parseRelease("non è json", r, error));

        section("impronta SHA-256");
        {
            const std::wstring path = AppPaths::getAppDataDir() + L"\\prova_sha256.txt";
            std::ofstream(path, std::ios::binary | std::ios::trunc) << "abc";
            std::string hex;
            CHECK(Updater::sha256File(path, hex));
            CHECK_STR(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
            DeleteFileW(path.c_str());
        }
    }

    void testSettingsAndTools() {
        std::printf("Impostazioni e strumenti\n");
        AppSettings s;
        s.userName = "Roberto";
        s.wakeWordEnabled = false;
        s.followUpSeconds = 9;
        s.voiceName = "Cosimo";
        TalkTrigger t;
        t.vk = VK_XBUTTON1;
        s.triggers.push_back(t);
        AppSettings back = AppSettings::parse(s.serialize());
        CHECK_STR(back.userName, "Roberto");
        CHECK(!back.wakeWordEnabled);
        CHECK(back.followUpSeconds == 9);
        CHECK_STR(back.voiceName, "Cosimo");
        CHECK(back.triggers.size() == 1 && back.triggers[0].vk == VK_XBUTTON1);
        AppSettings bad = AppSettings::parse("speechRate=99\nvadThreshold=-3\nfollowUpSeconds=500\n");
        bad.sanitize();
        CHECK(bad.speechRate == 2.0 && bad.vadThreshold == 0.1 && bad.followUpSeconds == 30);

        // Solo gioco e memoria: niente internet, programmi, volume, musica o timer.
        CHECK_STR(Tools::describe("remember", "{\"fact\":\"x\"}"), "Me lo segno");
        CHECK_STR(Tools::describe("game_command", "{\"command\":\"carrello\"}"), "Star Citizen: carrello");
        FakeHost host;
        Tools tools(host);
        CHECK(tools.definitions().size() == 1);
        for (const char* gone : { "web_search", "get_weather", "get_news", "read_webpage", "open_app", "open_website", "set_timer",
                                  "cancel_timers", "set_volume", "media_control" }) {
            CHECK(TextUtils::startsWith(tools.run(gone, "{}", nullptr), "Errore"));
        }
        CHECK(TextUtils::startsWith(tools.run("remember", "{\"fact\": \"gioca in squadra con Luca\"}", nullptr), "Salvato in memoria"));

        LlmEngine llm;
        Agent agent(llm, tools);
        Agent::Persona persona;
        persona.userName = "Roberto";
        persona.memory = { "Gioca a Star Citizen" };
        agent.setPersona(persona);
        std::string sys = agent.systemPrompt();
        CHECK(sys.find("di Roberto") != std::string::npos);
        CHECK(sys.find("Gioca a Star Citizen") != std::string::npos);
        CHECK(sys.find("web_search") == std::string::npos && sys.find("open_website") == std::string::npos);
        CHECK(sys.find("solo del gioco") != std::string::npos);
        CHECK(sys.find("MODALITÀ BREVE") == std::string::npos);
        persona.brief = true;
        agent.setPersona(persona);
        sys = agent.systemPrompt();
        CHECK(sys.find("MODALITÀ BREVE") != std::string::npos && sys.find("una o due frasi") == std::string::npos);

        // Quanto parla ed effetto sulla voce: si salvano, i valori strani tornano ai predefiniti.
        AppSettings r;
        CHECK_STR(r.replyStyle, "gioco");
        r.replyStyle = "brevi";
        r.voiceEffect = "robot";
        AppSettings r2 = AppSettings::parse(r.serialize());
        CHECK_STR(r2.replyStyle, "brevi");
        CHECK_STR(r2.voiceEffect, "robot");
        AppSettings r3 = AppSettings::parse("replyStyle=chiacchierona\nvoiceEffect=dalek\n");
        r3.sanitize();
        CHECK_STR(r3.replyStyle, "gioco");
        CHECK_STR(r3.voiceEffect, "nessuno");
    }

    // --- Con i modelli veri ------------------------------------------------------------------

    bool loadLlm(LlmEngine& llm) {
        Gpu::prepare();
        AppSettings s = AppSettings::load();
        const std::string path = AppPaths::toLibraryPath(AppPaths::llmModelPath(s.llmModelFile));
        auto t0 = std::chrono::steady_clock::now();
        if (!llm.init(path, 8, -1)) {
            std::printf("  Modello non caricato: %s\n", path.c_str());
            return false;
        }
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        std::printf("  Modello: %s, %d strati su %d sulla scheda, caricato in %lld ms\n", llm.modelName().c_str(), llm.gpuLayers(),
                    llm.totalLayers(), (long long)ms);
        return true;
    }

    std::string askOnce(Agent& agent, const std::string& question, std::vector<std::string>* toolsUsed = nullptr) {
        Agent::Callbacks cb;
        auto t0 = std::chrono::steady_clock::now();
        bool first = true;
        cb.onSentence = [&](const std::string& s) {
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
            if (first) std::printf("  (prima frase dopo %lld ms)\n", (long long)ms);
            first = false;
            std::printf("  VOCE: %s\n", SpeechText::speakable(s).c_str());
        };
        cb.onTool = [&](const std::string& what) {
            std::printf("  STRUMENTO: %s\n", what.c_str());
            if (toolsUsed) toolsUsed->push_back(what);
        };
        std::string answer;
        std::atomic<bool> cancel{ false };
        Agent::Result r = agent.ask(question, cb, &cancel, answer);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        std::printf("  RISPOSTA (%s, %lld ms): %s\n", r == Agent::Result::Ok ? "ok" : "ERRORE", (long long)ms, answer.c_str());
        return answer;
    }

    void testFull() {
        std::printf("Con il modello vero\n");
        LlmEngine llm;
        if (!loadLlm(llm)) {
            CHECK(false);
            return;
        }
        FakeHost host;
        Tools tools(host);
        Agent agent(llm, tools);
        Agent::Persona persona;
        persona.userName = "Roberto";
        agent.setPersona(persona);

        std::vector<std::string> used;
        std::string a = askOnce(agent, "Quanto fa 12 per 7? Dimmi solo il risultato.", &used);
        CHECK(a.find("84") != std::string::npos);
        CHECK(used.empty());

        // Fuori dal gioco: niente strumenti, lo dice e basta.
        used.clear();
        a = askOnce(agent, "Che tempo fa domani a Roma?", &used);
        CHECK(used.empty() && !a.empty());

        used.clear();
        a = askOnce(agent, "Ricordati che il mio wingman si chiama Luca", &used);
        CHECK(!used.empty() && used[0].find("segno") != std::string::npos);
    }
}

// --- Axis Visualizer dentro Iris --------------------------------------------------------------------------
static void testAxis() {
    std::printf("\n[axis]\n");
    // Il settings.json di Axis Visualizer, così com'è (campi con l'iniziale maiuscola).
    const std::string av = R"({
      "Mode": "Streamer", "ServerEnabled": true, "ServerPort": 8777,
      "Profiles": [ { "Name": "STAR CITIZEN", "Triggers": ["StarCitizen.exe"], "Panes": [
        { "Kind": "Assi", "DeviceId": "dinput:5ab9b330-8904-11f1-800f-444553540000", "Label": "TRASLAZIONALI", "Align": "Centro",
          "Size": 200, "Group": 1, "OverlayLeft": 1560, "OverlayTop": 1160, "AxisX": 0, "AxisY": 1, "AxisZ": 5,
          "TrailSeconds": 0.35, "ShowValues": true, "AxisLabels": { "5": "ACCEL.", "0": "L/R" }, "DotColor": "#2AA198" },
        { "Kind": "Assi", "DeviceId": "dinput:5ab9b330-8904-11f1-800d-444553540000", "Label": "ROTAZIONALI", "Group": 1 },
        { "Kind": "Pulsanti", "DeviceId": "keyboard", "Buttons": [87, 65], "ButtonLabels": { "87": "AVANTI" }, "Group": 0 },
        { "Kind": "Barre", "DeviceId": "", "Group": 0 } ] },
        { "Name": "DCS", "Panes": [] } ],
      "ActiveProfile": 0, "Opacity": 0.8, "DotRadius": 12, "TextSize": 12, "Columns": 2, "Gap": 8 })";
    Axis::Settings s = Axis::parse(av);
    CHECK_EQ((int)s.profiles.size(), 2);
    CHECK_STR(s.active().name, "STAR CITIZEN");
    CHECK_EQ((int)s.active().panes.size(), 4);
    const Axis::Pane& first = s.active().panes[0];
    CHECK(first.kind == Axis::Kind::Assi);
    CHECK_STR(first.deviceId, "dinput:5ab9b330-8904-11f1-800f-444553540000");
    CHECK_STR(first.align, "Centro");
    CHECK_EQ(first.group, 1);
    CHECK_EQ(first.axisZ, 5);
    CHECK(first.left && *first.left == 1560 && first.top && *first.top == 1160);
    CHECK(first.axisLabels.count(5) && first.axisLabels.at(5) == "ACCEL.");
    CHECK(s.active().panes[2].kind == Axis::Kind::Pulsanti);
    CHECK(s.active().panes[2].buttons == std::vector<int>({ 87, 65 }));
    CHECK(s.active().panes[2].buttonLabels.count(87));
    CHECK(!s.active().panes[3].enabled());
    CHECK(s.active().triggers == std::vector<std::string>({ "StarCitizen.exe" }));
    CHECK_EQ(s.serverPort, 8777); // parse lascia la porta; è l'importazione che la cambia
    CHECK_EQ(s.columns, 2);

    // Andata e ritorno: niente si perde.
    const Axis::Settings back = Axis::parse(Axis::serialize(s));
    CHECK_STR(Axis::serialize(back), Axis::serialize(s));

    // Valori fuori misura riportati nei limiti.
    Axis::Settings wild = Axis::parse(R"({"ServerPort": 80, "Opacity": 9, "Profiles": [ { "Panes": [ { "Kind": "Barre", "DeviceId": "mouse", "Size": 5000, "Group": 40 } ] } ]})");
    wild.sanitize();
    CHECK_EQ(wild.serverPort, 1024);
    CHECK(wild.opacity <= 1.0);
    CHECK_EQ(wild.active().panes[0].size, 600);
    CHECK_EQ(wild.active().panes[0].group, 9);
    // Senza profili (o con i moduli alla radice, come le prime versioni): un profilo c'è sempre.
    Axis::Settings legacy = Axis::parse(R"({"Panes": [ { "Kind": "Barre", "DeviceId": "mouse" } ]})");
    legacy.sanitize();
    CHECK_EQ((int)legacy.profiles.size(), 1);
    CHECK_EQ((int)legacy.active().panes.size(), 1);
    Axis::Settings empty = Axis::parse("{}");
    empty.sanitize();
    CHECK_EQ((int)empty.profiles.size(), 1);

    // Le finestre: i due moduli del gruppo 1 insieme, la tastiera da sola, quello spento no.
    const auto groups = Axis::windowGroups(s.active());
    CHECK_EQ((int)groups.size(), 2);
    if (groups.size() == 2) {
        CHECK_STR(groups[0].key, "g1");
        CHECK(groups[0].panes == std::vector<int>({ 0, 1 }));
        CHECK(groups[0].name.find("TRASLAZIONALI + ROTAZIONALI") != std::string::npos);
        CHECK_STR(groups[1].key, "m3");
        CHECK_STR(groups[1].name, "PULSANTI");
    }

    CHECK_STR(Axis::processName("C:\\Games\\RSI\\StarCitizen.exe"), "starcitizen");
    CHECK_STR(Axis::processName("  \"DCS.EXE\" "), "dcs");
    CHECK_STR(Axis::processName("elite"), "elite");

    // Il server per OBS: pagine, chiave nella pagina, e la porta occupata detta per bene
    // (era il guaio di Axis Visualizer: due copie aperte, la seconda diceva "server spento").
    {
        AxisServer server;
        server.setPages("<html>KEY=__KEY__</html>", "<html>__ROWS__</html>");
        std::string error;
        int port = 0;
        for (int p = 18787; p < 18800 && !port; ++p) {
            if (server.start(p, error)) port = p;
        }
        CHECK(port != 0);
        if (port) {
            CHECK(server.running());
            server.publishLayout({ { "g1", "GRUPPO 1", R"({"panes":[]})" } });
            const Http::Response page = Http::get("http://127.0.0.1:" + std::to_string(port) + "/g1");
            CHECK_EQ(page.status, 200);
            CHECK(page.body.find("KEY=g1") != std::string::npos);
            const Http::Response index = Http::get("http://127.0.0.1:" + std::to_string(port) + "/");
            CHECK_EQ(index.status, 200);
            CHECK(index.body.find("/g1") != std::string::npos);
            const Http::Response icon = Http::get("http://127.0.0.1:" + std::to_string(port) + "/favicon.ico");
            CHECK_EQ(icon.status, 404);

            AxisServer second;
            std::string busy;
            CHECK(!second.start(port, busy));
            CHECK(!second.running());
            CHECK(busy.find(std::to_string(port)) != std::string::npos);
            server.stop();
            CHECK(!server.running());
            std::string again;
            CHECK(second.start(port, again)); // liberata: ora parte
            second.stop();
        }
    }

    // Il disegno di un modulo: misure come Axis Visualizer, numeri nell'ordine della pagina.
    {
        Gdiplus::GdiplusStartupInput input;
        ULONG_PTR token = 0;
        Gdiplus::GdiplusStartup(&token, &input, nullptr);
        Axis::Settings look;
        look.sanitize();
        Axis::Pane axes;
        axes.kind = Axis::Kind::Assi;
        axes.deviceId = "mouse";
        axes.size = 220;
        axes.axisZ = 2;
        {
            AxisDraw::PaneView view(axes, look, "MOUSE");
            view.tick(1.0 / 60);
            CHECK_EQ((int)view.width(), 220);
            CHECK(view.height() > 220); // riquadro quadrato + titolo + barra
            const nlohmann::json d = view.describe();
            CHECK_STR(d.value("kind", std::string()), "assi");
            CHECK_EQ(d.value("w", 0), 220);
            std::vector<double> values;
            view.values(values);
            CHECK_EQ((int)values.size(), 3);
        }
        Axis::Pane keys;
        keys.kind = Axis::Kind::Pulsanti;
        keys.deviceId = "keyboard";
        keys.buttons = { 'W', 'A', 'S', 'D' };
        {
            AxisDraw::PaneView view(keys, look, "TASTIERA");
            view.tick(1.0 / 60);
            const nlohmann::json d = view.describe();
            CHECK_STR(d.value("kind", std::string()), "pulsanti");
            CHECK_EQ((int)d["chips"].size(), 4);
            std::vector<double> values;
            view.values(values);
            CHECK((int)values.size() >= 4);
        }
        Gdiplus::GdiplusShutdown(token);
    }
}

// --- Diario di bordo (Game.log), raffineria, avvio con Windows -------------------------------------------
static void testGameLog() {
    std::printf("\n[diario di bordo]\n");
    using GameLog::Kind;
    const std::string me = "MrRevo";
    // Righe vere del Game.log (gioco in italiano), accorciate.
    const std::string accepted =
        "<2026-07-28T19:57:39.119Z> [Notice] <SHUDEvent_OnNotification> Added notification \"Contratto accettato: Localizzare Clienti Scomparsi: \" [21] "
        "to queue. New queue size: 1, MissionId: [430c6f2c-acb9-499a-98a1-23e2534141ca], ObjectiveId: [] [Team_CoreGameplayFeatures][Missions][Comms]";
    const std::string objective =
        "<2026-07-28T19:57:39.119Z> [Notice] <SHUDEvent_OnNotification> Added notification \"Nuovo obiettivo: Vai All'Ultima Posizione Nota: \" [22] "
        "to queue. New queue size: 2, MissionId: [430c6f2c-acb9-499a-98a1-23e2534141ca], ObjectiveId: [18f90169-e8ad-1342-aaaa-000000000001] [Team]";
    const std::string objectiveDone =
        "<2026-07-28T20:04:50.538Z> [Notice] <SHUDEvent_OnNotification> Added notification \"Obiettivo completato: Vai All'Ultima Posizione Nota: \" [3] "
        "to queue. New queue size: 1, MissionId: [430c6f2c-acb9-499a-98a1-23e2534141ca], ObjectiveId: [18f90169-e8ad-1342-aaaa-000000000001] [Team]";
    const std::string completed =
        "<2026-07-28T20:10:00.000Z> [Notice] <SHUDEvent_OnNotification> Added notification \"Contratto completato: Localizzare Clienti Scomparsi: \" [4] "
        "to queue. New queue size: 1, MissionId: [430c6f2c-acb9-499a-98a1-23e2534141ca], ObjectiveId: [] [Team]";
    const std::string echo = "<2026-11-01T08:56:36.140Z> [Notice] <UpdateNotificationItem> Notification \"Sei entrato in Crusader Industries: \" [0], Action: Next [Team]";
    const std::string jurisdiction =
        "<2026-11-01T08:56:36.139Z> [Notice] <SHUDEvent_OnNotification> Added notification \"Sei entrato in Crusader Industries: \" [0] to queue. New queue "
        "size: 1, MissionId: [00000000-0000-0000-0000-000000000000], ObjectiveId: [] [Team]";
    const std::string armistice =
        "<2026-11-01T09:00:00.000Z> [Notice] <SHUDEvent_OnNotification> Added notification \"Sei in una zona di armistizio: combattimento proibito!!: \" [1] "
        "to queue. New queue size: 1, MissionId: [00000000-0000-0000-0000-000000000000], ObjectiveId: [] [Team]";
    const std::string injury =
        "<2026-10-29T21:57:05.832Z> [Notice] <SHUDEvent_OnNotification> Added notification \"Rilevato infortunio grave - Braccio sinistro - Trattamento di "
        "livello 1 richiesto: \" [33] to queue. New queue size: 1, MissionId: [00000000-0000-0000-0000-000000000000], ObjectiveId: [] [Team]";
    const std::string qt =
        "<2026-10-10T19:16:27.684Z> [Notice] <SHUDEvent_OnNotification> Added notification \"Wingman_77 ha iniziato a QT verso la tua posizione attuale.: \" [5] "
        "to queue. New queue size: 1, MissionId: [00000000-0000-0000-0000-000000000000], ObjectiveId: [] [Team]";
    const std::string login = "<2026-11-01T08:55:16.067Z> [Notice] <Legacy login response> [CIG-net] User Login Success - Handle[MrRevo] - Time[182380939] [Team_GameServices][Login]";
    const std::string seat = "<2026-08-02T18:42:07.292Z> [Notice] <Vehicle Control Flow> CVehicleMovementBase::ClearDriver: Local client node [123456789] releasing "
                             "control token for 'AEGS_Gladius_749027674818' [749027674818] [Team_CGP4][Vehicle]";
    const std::string location = "<2026-11-01T09:29:17.387Z> [Notice] <RequestLocationInventory> Player[MrRevo] requested inventory for Location[RR_CRU_LEO] [Team][Inventory]";
    const std::string mine = "<2026-11-01T09:31:24.099Z> Logged a start of a status effect! nickname: MrRevo, status effect: Dehydrated";
    const std::string theirs = "<2026-11-01T09:47:54.903Z> Logged a start of a status effect! nickname: Katyusha_pravda, status effect: Dehydrated";
    const std::string tips =
        "<2026-11-01T09:00:00.000Z> [Notice] <SHUDEvent_OnNotification> Added notification \"Suggerimenti: Puoi disattivare i suggerimenti in qualsiasi momento: \" [1] "
        "to queue. New queue size: 1, MissionId: [00000000-0000-0000-0000-000000000000], ObjectiveId: [] [Team]";
    const std::string english =
        "<2026-03-26T17:15:41.684Z> [Notice] <SHUDEvent_OnNotification> Added notification \"Contract Accepted: Shipment Delivery: \" [23] to queue. "
        "New queue size: 1, MissionId: [aaaaaaaa-0000-0000-0000-000000000001], ObjectiveId: [] [Team]";
    const std::string blueprint =
        "<2026-03-26T17:15:41.684Z> [Notice] <SHUDEvent_OnNotification> Added notification \"Received Blueprint: Defiance Helmet Tactical: \" [23] to queue.";
    const std::string kill =
        "<2025-01-02T10:00:00.000Z> [Notice] <Actor Death> CActor::Kill: 'Pirate_01' [111] in zone 'Stanton' killed by 'MrRevo' [222] using 'GATS_Ballistic' "
        "[Class GATS_BallisticGatling_S2] with damage type 'Bullet' from direction x: 0, y: 0, z: 0 [Team_ActorTech][Actor]";

    auto a = GameLog::parseLine(accepted, me);
    CHECK(a && a->kind == Kind::Contract && a->action == "accepted");
    if (a) {
        CHECK_STR(a->subject, "Localizzare Clienti Scomparsi");
        CHECK_STR(a->missionId, "430c6f2c-acb9-499a-98a1-23e2534141ca");
        CHECK_STR(a->text, "Contratto accettato: Localizzare Clienti Scomparsi");
        CHECK(a->epoch == GameLog::lineEpoch(accepted) && a->epoch > 1700000000LL);
    }
    CHECK(!GameLog::parseLine(echo, me));  // l'eco della stessa notifica non conta due volte
    CHECK(!GameLog::parseLine(tips, me));  // i suggerimenti non interessano
    auto j = GameLog::parseLine(jurisdiction, me);
    CHECK(j && j->kind == Kind::Jurisdiction && j->subject == "Crusader Industries" && j->missionId.empty());
    auto z = GameLog::parseLine(armistice, me);
    CHECK(z && z->kind == Kind::Zone && z->action == "armistice_in");
    auto i = GameLog::parseLine(injury, me);
    CHECK(i && i->kind == Kind::Health && i->action == "injury" && i->level == GameLog::Level::Bad);
    if (i) CHECK_STR(i->text, "Infortunio grave · braccio sinistro (trattamento di livello 1 richiesto)");
    auto q = GameLog::parseLine(qt, me);
    CHECK(q && q->kind == Kind::Party && q->subject == "Wingman_77");
    auto l = GameLog::parseLine(login, "");
    CHECK(l && l->kind == Kind::Login && l->subject == "MrRevo");
    auto s = GameLog::parseLine(seat, me);
    CHECK(s && s->kind == Kind::Ship && s->subject == "Aegis Gladius");
    auto loc = GameLog::parseLine(location, me);
    CHECK(loc && loc->subject == "Seraphim Station");
    auto eff = GameLog::parseLine(mine, me);
    CHECK(eff && eff->kind == Kind::Health && eff->action == "effect" && eff->subject == "disidratato");
    CHECK(!GameLog::parseLine(theirs, me)); // lo stato di un altro giocatore no
    CHECK(!GameLog::parseLine(mine, ""));   // finché non si sa chi sei, nemmeno il tuo
    auto en = GameLog::parseLine(english, me);
    CHECK(en && en->kind == Kind::Contract && en->action == "accepted" && en->subject == "Shipment Delivery");
    auto bp = GameLog::parseLine(blueprint, me);
    CHECK(bp && bp->kind == Kind::Blueprint && bp->subject == "Defiance Helmet Tactical");
    auto k = GameLog::parseLine(kill, me);
    CHECK(k && k->kind == Kind::Combat && k->action == "kill" && k->subject == "Pirate_01");
    CHECK(!GameLog::parseLine(kill, "Qualcuno")); // uccisioni tra altri: non ci riguardano

    CHECK_STR(GameLog::shipName("RSI_Constellation_Mk5_Centaurus_200000446458"), "RSI Constellation Mk5 Centaurus");
    CHECK_STR(GameLog::shipName("ANVL_C8R_Pisces_737378971160"), "Anvil C8R Pisces");
    CHECK_STR(GameLog::locationName("Stanton2_Orison"), "Orison");
    CHECK_STR(GameLog::locationName("RR_ARC_L1"), "ARC-L1");
    CHECK_STR(GameLog::locationName("RR_JP_StantonMagnus"), "Portale Stanton\xE2\x80\x93Magnus");

    // Lo stato: contratto accettato -> obiettivo -> obiettivo fatto -> contratto completato.
    GameLog::State st;
    for (const auto& line : { login, accepted, objective, jurisdiction, armistice, location, location }) {
        if (auto e = GameLog::parseLine(line, st.handle)) GameLog::apply(st, *e);
    }
    CHECK_STR(st.handle, "MrRevo");
    CHECK_EQ((int)st.contracts.size(), 1);
    CHECK_EQ((int)st.objectives.size(), 1);
    CHECK_EQ(st.armistice, 1);
    CHECK_STR(st.jurisdiction, "Crusader Industries");
    CHECK_STR(st.location, "Seraphim Station");
    const size_t before = st.recent.size();
    if (auto e = GameLog::parseLine(accepted, st.handle)) GameLog::apply(st, *e); // stesso contratto due volte: uno solo
    CHECK_EQ((int)st.contracts.size(), 1);
    CHECK_EQ((int)st.recent.size(), (int)before + 1);
    for (const auto& line : { objectiveDone, completed }) {
        if (auto e = GameLog::parseLine(line, st.handle)) GameLog::apply(st, *e);
    }
    CHECK(st.contracts.empty());
    CHECK(st.objectives.empty());
    CHECK_EQ(st.completed, 1);
    CHECK_EQ(st.accepted, 2);

    // I contratti ricordati: quelli di più di 3 giorni si dimenticano, quelli senza data contano da adesso.
    {
        const std::wstring missions = AppPaths::getAppDataDir() + L"\\missioni.json";
        const long long now = (long long)time(nullptr);
        {
            std::ofstream f(missions, std::ios::binary | std::ios::trunc);
            f << "[{\"id\":\"a\",\"name\":\"Vecchio\",\"since\":" << (now - 4 * 86400) << "},{\"id\":\"b\",\"name\":\"Senza data\",\"since\":0}]";
        }
        GameLog::Watcher w;
        w.start(L"C:\\nessuna-cartella-iris\\Game.log", nullptr, nullptr);
        const GameLog::State remembered = w.state();
        w.stop();
        CHECK(remembered.contracts.size() == 1 && remembered.contracts[0].name == "Senza data" && remembered.contracts[0].since >= now);
        DeleteFileW(missions.c_str());
    }

    // Un Game.log grande (una sessione lunga): la rilettura iniziale è veloce e non perde righe.
    {
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        const std::wstring path = std::wstring(tmp) + L"IrisTests\\GameGrande.log";
        const int kLines = 60000;
        {
            std::ofstream f(path, std::ios::binary | std::ios::trunc);
            f << login << "\r\n";
            for (int n = 0; n < kLines; ++n) {
                f << "<2026-11-01T09:" << std::setw(2) << std::setfill('0') << (n / 1000) % 60 << ":00.000Z> [Notice] <CEntityComponentPhysics> "
                  << "qualcosa di lungo che il gioco scrive di continuo, riga " << n << " [Team_Physics][Entity]\r\n";
                if (n % 10000 == 0) f << jurisdiction << "\r\n";
            }
            f << accepted << "\r\n";
        }
        GameLog::Watcher w;
        std::atomic<int> events{ 0 };
        const auto t0 = std::chrono::steady_clock::now();
        w.start(path, [&](const GameLog::Event&, bool) { events++; }, nullptr);
        for (int t = 0; t < 400 && (w.status().catchingUp || !w.status().found); ++t) Sleep(25);
        const long long ms = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        std::printf("  Game.log di %d righe riletto in %lld ms\n", w.status().lines, ms);
        CHECK(!w.status().catchingUp && w.status().lines >= kLines);
        CHECK_EQ(events.load(), 2 + kLines / 10000); // login, contratto, giurisdizioni
        CHECK(ms < 8000);
        w.stop();
        DeleteFileW(path.c_str());
        DeleteFileW((AppPaths::getAppDataDir() + L"\\missioni.json").c_str());
    }

    // Il file vero: un Game.log finto, letto dal thread come farebbe con quello del gioco.
    {
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        const std::wstring path = std::wstring(tmp) + L"IrisTests\\Game.log";
        {
            std::ofstream f(path, std::ios::binary | std::ios::trunc);
            f << login << "\r\n" << accepted << "\r\n" << jurisdiction << "\r\n";
        }
        GameLog::Watcher w;
        std::atomic<int> live{ 0 }, old{ 0 };
        w.start(path, [&](const GameLog::Event&, bool isLive) { (isLive ? live : old)++; }, nullptr);
        for (int t = 0; t < 60 && (w.status().catchingUp || !w.status().found); ++t) Sleep(50);
        CHECK(w.status().found);
        CHECK_EQ(old.load(), 3); // la sessione già scritta: senza avvisi
        {
            std::ofstream f(path, std::ios::binary | std::ios::app);
            f << completed << "\r\n";
        }
        for (int t = 0; t < 60 && live.load() == 0; ++t) Sleep(50);
        CHECK_EQ(live.load(), 1); // la riga nuova: avviso
        CHECK(w.state().contracts.empty());
        CHECK_STR(w.state().handle, "MrRevo");
        w.stop();
        DeleteFileW(path.c_str());
    }

    // Raffineria: le durate come si scrivono e come si dicono.
    auto dur = [](const char* t) { auto v = Refinery::parseDuration(t); return v ? *v : -1LL; };
    CHECK_EQ(dur("3:20"), 12000LL);
    CHECK_EQ(dur("3h20"), 12000LL);
    CHECK_EQ(dur("3h 20m"), 12000LL);
    CHECK_EQ(dur("200"), 12000LL);
    CHECK_EQ(dur("3 ore e 20 minuti"), 12000LL);
    CHECK_EQ(dur("tre ore e venti"), 12000LL);
    CHECK_EQ(dur("un'ora e mezza"), 5400LL);
    CHECK_EQ(dur("mezz'ora"), 1800LL);
    CHECK_EQ(dur("1g 2h"), 93600LL);
    CHECK_EQ(dur("45 min"), 2700LL);
    CHECK_EQ(dur("1,5 ore"), 5400LL);
    CHECK_EQ(dur("tra due ore"), 7200LL);
    CHECK_EQ(dur("boh"), -1LL);
    CHECK_EQ(dur(""), -1LL);
    CHECK_STR(Refinery::formatRemaining(12000), "3 h 20 min");
    CHECK_STR(Refinery::formatRemaining(125), "2 min 05 s");
    CHECK_STR(Refinery::formatRemaining(0), "pronto");
    CHECK_STR(Refinery::spokenDuration(12000), "3 ore e 20 minuti");
    CHECK_STR(Refinery::spokenDuration(3600), "un'ora");
    // Detti e scritti come vengono: apostrofo tipografico, quarti, numeri composti, unità intere.
    CHECK_EQ(dur("mezz\xE2\x80\x99ora"), 1800LL);      // prima valeva un'ora
    CHECK_EQ(dur("un\xE2\x80\x99ora e mezza"), 5400LL);
    CHECK_EQ(dur("un'ora e un quarto"), 4500LL);
    CHECK_EQ(dur("tre quarti d'ora"), 2700LL);
    CHECK_EQ(dur("un'ora e ventidue"), 4920LL);
    CHECK_EQ(dur("45 minuti"), 2700LL);
    CHECK_EQ(dur("1 ora e 20 di raffinazione"), 4800LL); // "di" non sono giorni
    CHECK_EQ(dur("sette ore"), 25200LL);
    // Niente eccezioni né numeri assurdi (prima "3: " e "99999999999999999999" facevano cadere Iris).
    CHECK_EQ(dur("3: "), 10800LL);
    CHECK_EQ(dur("99999999999999999999"), -1LL);
    CHECK_EQ(dur("99999999999999999999 ore"), -1LL);
    CHECK_EQ(dur("0"), -1LL);
    CHECK_EQ(dur("::"), -1LL);
    CHECK_STR(Refinery::spokenDuration(3585), "un'ora"); // prima: "meno di un minuto"
    CHECK_STR(Refinery::spokenDuration(14390), "4 ore"); // prima: "3 ore"
    CHECK_STR(Refinery::spokenDuration(20), "meno di un minuto");
    {
        // Finito con il PC sospeso (Iris già partita): si annuncia a voce al risveglio, non è "di prima dell'avvio".
        Refinery::Timers awake;
        awake.load();
        const long long realNow = (long long)time(nullptr);
        const std::string id = awake.add("Sospeso", "", 60, realNow);
        const auto finished = awake.takeFinished(realNow + 3 * 3600);
        CHECK(finished.size() == 1 && !finished[0].beforeStart);
        awake.remove(id);
    }
    {
        Refinery::Timers timers;
        timers.load();
        for (const auto& j0 : timers.jobs()) timers.remove(j0.id);
        const long long now = 1700000000;
        const std::string id = timers.add("ARC-L1", "Quantanium", 600, now);
        timers.add("Levski", "", 60, now);
        CHECK_EQ((int)timers.jobs().size(), 2);
        CHECK_STR(timers.jobs().front().place, "Levski"); // in ordine di fine
        CHECK(timers.takeFinished(now + 30).empty());
        CHECK_EQ((int)timers.takeFinished(now + 61).size(), 1);
        CHECK(timers.takeFinished(now + 62).empty()); // annunciato una volta sola
        Refinery::Timers again;
        again.load(); // dal file
        CHECK_EQ((int)again.jobs().size(), 2);
        again.clearDone(now + 100);
        CHECK_EQ((int)again.jobs().size(), 1);
        CHECK(again.remove(id));
        CHECK(again.jobs().empty());
    }

    // Avvio con Windows: il comando nel registro (non si tocca il registro vero nelle prove).
    CHECK(Autostart::commandFor(L"C:\\Giochi\\Iris\\iris.exe") == L"\"C:\\Giochi\\Iris\\iris.exe\" --avvio-windows");

    // Impostazioni nuove: la ruota con Shift+Tab (solo in gioco), saluto spento, diario spento finché non lo accetti.
    AppSettings fresh = AppSettings::parse("");
    CHECK(!fresh.greetOnStart);
    CHECK(fresh.closeToTray);
    CHECK(!fresh.gameLogEnabled && !fresh.gameLogAccepted);
    CHECK(fresh.quickBarTriggers.size() == 1 && fresh.quickBarTriggers[0].vk == VK_TAB && fresh.quickBarTriggers[0].mods == TalkTrigger::Shift);
    CHECK(fresh.wheelGameOnly);
    CHECK(fresh.hideAllTriggers.size() == 1 && fresh.hideAllTriggers[0].vk == VK_MULTIPLY);
    CHECK_STR(HotkeyListener::describeTrigger(fresh.quickBarTriggers[0]), "Shift+Tab");
    fresh.quickBarTriggers.clear(); // tolto dall'utente: deve restare tolto
    fresh.gameLogEnabled = fresh.gameLogAccepted = true;
    fresh.gameLogPath = "D:\\SC\\LIVE\\Game.log";
    fresh.panelRefineryOpen = true;
    fresh.panelLogX = 1200;
    fresh.wheelGameOnly = false;
    const AppSettings back = AppSettings::parse(fresh.serialize());
    CHECK(back.quickBarTriggers.empty());
    CHECK(!back.wheelGameOnly);
    CHECK(back.gameLogEnabled && back.gameLogAccepted);
    CHECK_STR(back.gameLogPath, "D:\\SC\\LIVE\\Game.log");
    CHECK(back.panelRefineryOpen);
    CHECK_EQ(back.panelLogX, 1200);

    // Le impostazioni della barra di prima (Num /): la ruota passa a Shift+Tab, "nascondi tutto" resta com'era.
    const AppSettings old = AppSettings::parse("quickBarConfigured=1\nquickBarTrigger=keyboard,111\nhideAllTrigger=keyboard,120\n");
    CHECK(old.quickBarTriggers.size() == 1 && old.quickBarTriggers[0].vk == VK_TAB && old.quickBarTriggers[0].mods == TalkTrigger::Shift);
    CHECK(old.hideAllTriggers.size() == 1 && old.hideAllTriggers[0].vk == VK_F9);

    // Combinazioni con i modificatori: si salvano, si rileggono, si distinguono dal tasto da solo.
    AppSettings combo = AppSettings::parse("");
    TalkTrigger ctrlF1;
    ctrlF1.vk = VK_F1;
    ctrlF1.mods = TalkTrigger::Ctrl | TalkTrigger::Shift;
    TalkTrigger plainF1;
    plainF1.vk = VK_F1;
    combo.quickBarTriggers = { ctrlF1 };
    combo.triggers = { plainF1 };
    const AppSettings comboBack = AppSettings::parse(combo.serialize());
    CHECK(comboBack.quickBarTriggers.size() == 1 && comboBack.quickBarTriggers[0] == ctrlF1);
    CHECK(comboBack.triggers.size() == 1 && comboBack.triggers[0] == plainF1 && comboBack.triggers[0].mods == 0);
    CHECK(!(ctrlF1 == plainF1));
    CHECK_STR(HotkeyListener::describeTrigger(ctrlF1), "Ctrl+Shift+F1");
    CHECK(combo.serialize().find("quickBarTrigger=keyboard,112,3\n") != std::string::npos);
    CHECK(combo.serialize().find("trigger=keyboard,112\n") != std::string::npos);
}

// --- La ruota: quale voce sta nella direzione del mouse ----------------------------------------------------
static void testWheel() {
    std::printf("\n[ruota]\n");
    // 9 voci da 40 gradi, la prima in alto, poi in senso orario (y verso il basso, come lo schermo).
    CHECK_EQ(QuickBar::sectorAt(0, -100, 9, 50), 0);   // su
    CHECK_EQ(QuickBar::sectorAt(100, 0, 9, 50), 2);    // destra: 90 gradi -> la terza voce (80-120 = 2)
    CHECK_EQ(QuickBar::sectorAt(5, 100, 9, 50), 4);    // giù, appena a destra: 177 gradi, voce 4 (140-180)
    CHECK_EQ(QuickBar::sectorAt(-5, 100, 9, 50), 5);   // giù, appena a sinistra: 183 gradi, voce 5 (180-220)
    CHECK_EQ(QuickBar::sectorAt(-100, 0, 9, 50), 7);   // sinistra: 270 gradi (260-300 = 7)
    CHECK_EQ(QuickBar::sectorAt(-50, -100, 9, 50), 8); // a sinistra dell'alto (-27 gradi): l'ultima voce
    CHECK_EQ(QuickBar::sectorAt(-30, -100, 9, 50), 0); // -17 gradi: ancora la prima (-20..20)
    CHECK_EQ(QuickBar::sectorAt(30, -100, 9, 50), 0);  // poco a destra dell'alto: ancora la prima
    CHECK_EQ(QuickBar::sectorAt(10, 10, 9, 50), -1);   // nel centro
    CHECK_EQ(QuickBar::sectorAt(0, -100, 0, 50), -1);  // nessuna voce
    // Al bordo tra due voci (20 gradi) vince quella dopo, senza buchi.
    const double a = 20.0 * 3.14159265358979 / 180.0;
    CHECK_EQ(QuickBar::sectorAt(100 * std::sin(a) + 0.01, -100 * std::cos(a), 9, 50), 1);
}

// --- Editor dei comandi di gioco (pagina Star Citizen) ----------------------------------------------------
static void testCommandEditor() {
    std::printf("\n[editor dei comandi]\n");
    CHECK_STR(GameInput::nameForVirtualKey('N'), "N");
    CHECK_STR(GameInput::nameForVirtualKey(VK_ADD), "NumPlus");
    CHECK_STR(GameInput::nameForVirtualKey(VK_LMENU), "LAlt");
    CHECK_STR(GameInput::nameForVirtualKey(VK_F5), "F5");
    CHECK_STR(GameInput::nameForVirtualKey(VK_NUMPAD7), "Num7");
    CHECK(GameInput::sameCombo("Alt+N", "LAlt+n"));
    CHECK(!GameInput::sameCombo("LAlt+N", "N"));

    const GameProfile base = GameProfile::defaults();
    std::string error;

    // Nuovo comando con un tasto nuovo: il nome lo prende dalla prima frase; "Iris," e i punti si tolgono.
    GameProfile p = base;
    CommandEdit e;
    e.intent = "press";
    e.phrases = { "Apri le porte", "Iris, porte.", "apri le porte" };
    e.steps = { { "", "LAlt+K" } };
    e.say = " Porte. ";
    CHECK(applyCommandEdit(p, e, error));
    CHECK_EQ((int)p.commands.size(), (int)base.commands.size() + 1);
    const GameCommand& added = p.commands.back();
    CHECK(added.phrases == std::vector<std::string>({ "Apri le porte", "porte" }));
    CHECK_STR(added.say, "Porte.");
    CHECK_STR(added.args.at("key"), "apri_le_porte");
    CHECK_STR(p.keys.at("apri_le_porte"), "LAlt+K");
    CHECK_STR(added.id(), "press:apri_le_porte");

    // Stessa frase di un altro comando: no, e il profilo non cambia.
    GameProfile q = p;
    CommandEdit dup;
    dup.intent = "press";
    dup.phrases = { "carrello" };
    dup.steps = { { "", "J" } };
    CHECK(!applyCommandEdit(q, dup, error));
    CHECK(error.find("carrello") != std::string::npos);
    CHECK_EQ((int)q.commands.size(), (int)p.commands.size());

    // Tasto non valido, frasi vuote, tasto mancante.
    CommandEdit bad = e;
    bad.phrases = { "accendi il frigo" };
    bad.steps = { { "", "Ctrl+Pippo" } };
    CHECK(!applyCommandEdit(q, bad, error));
    CHECK(error.find("Pippo") != std::string::npos);
    bad.steps = { { "", "" } };
    CHECK(!applyCommandEdit(q, bad, error));
    bad.phrases = { "  ", "." };
    bad.steps = { { "", "J" } };
    CHECK(!applyCommandEdit(q, bad, error));

    // Una combinazione già nel profilo: si riusa il suo nome, niente doppioni.
    const std::string gear = base.keys.count("landing_gear") ? base.keys.at("landing_gear") : "";
    if (!gear.empty()) {
        CommandEdit reuse;
        reuse.intent = "press";
        reuse.phrases = { "giù le ruote" };
        reuse.steps = { { "", gear } };
        GameProfile r = base;
        CHECK(applyCommandEdit(r, reuse, error));
        CHECK_STR(r.commands.back().args.at("key"), "landing_gear");
        CHECK_EQ((int)r.keys.size(), (int)base.keys.size());
    }

    // Tieni premuto (durata nei limiti) e sequenza con un tasto del profilo e uno nuovo.
    CommandEdit hold;
    hold.intent = "hold";
    hold.phrases = { "atterraggio automatico" };
    hold.steps = { { "", "N" } };
    hold.holdMs = 99999;
    GameProfile hp = base;
    CHECK(applyCommandEdit(hp, hold, error));
    CHECK_STR(hp.commands.back().args.at("ms"), "10000");
    CommandEdit seq;
    seq.intent = "sequence";
    seq.phrases = { "modalità combattimento" };
    seq.steps = { { "shields", "" }, { "", "LShift+F9" } };
    GameProfile sp = base;
    CHECK(applyCommandEdit(sp, seq, error));
    CHECK_STR(sp.commands.back().args.at("keys"), "shields,modalita_combattimento_2");
    const auto steps = commandSteps(sp, sp.commands.back());
    CHECK_EQ((int)steps.size(), 2);
    if (steps.size() == 2) CHECK_STR(steps[1].combo, "LShift+F9");

    // Cambiare il tasto di un comando che usa un nome del profilo cambia il tasto di quella funzione.
    int gearIndex = -1;
    for (size_t i = 0; i < p.commands.size(); ++i) {
        if (p.commands[i].intent == "press" && p.commands[i].args.count("key") && p.commands[i].args.at("key") == "landing_gear") gearIndex = (int)i;
    }
    CHECK(gearIndex >= 0);
    if (gearIndex >= 0) {
        CommandEdit change;
        change.index = gearIndex;
        change.intent = "press";
        change.phrases = p.commands[(size_t)gearIndex].phrases;
        change.steps = { { "landing_gear", "RAlt+N" } };
        GameProfile c = p;
        CHECK(applyCommandEdit(c, change, error));
        CHECK_STR(c.keys.at("landing_gear"), "RAlt+N");
        CHECK_EQ((int)c.commands.size(), (int)p.commands.size());
    }

    // I comandi speciali: si cambiano le frasi (la rotta con {destination}), non si tolgono.
    int route = -1;
    for (size_t i = 0; i < p.commands.size(); ++i) {
        if (p.commands[i].intent == "set_route") route = (int)i;
    }
    CHECK(route >= 0);
    if (route >= 0) {
        CommandEdit rt;
        rt.index = route;
        rt.intent = "press"; // ignorato: resta set_route
        rt.phrases = { "vola verso casa" };
        GameProfile c = p;
        CHECK(!applyCommandEdit(c, rt, error));
        CHECK(error.find("{destination}") != std::string::npos);
        rt.phrases = { "vola verso {destination}", "rotta per {destination}" };
        CHECK(applyCommandEdit(c, rt, error));
        CHECK_STR(c.commands[(size_t)route].intent, "set_route");
        CHECK_EQ((int)c.commands[(size_t)route].phrases.size(), 2);
        CHECK(!removeCommand(c, route, error));
    }
    const int last = (int)p.commands.size() - 1;
    CHECK(removeCommand(p, last, error));
    CHECK_EQ((int)p.commands.size(), (int)base.commands.size());
    CHECK(!removeCommand(p, 9999, error));

    // Il profilo con il comando nuovo si salva e si rilegge uguale.
    GameProfile saved = base, back;
    CHECK(applyCommandEdit(saved, e, error));
    CHECK(GameProfile::parse(saved.toJson(), back, error));
    CHECK_STR(back.keys.at("apri_le_porte"), "LAlt+K");
    CHECK(back.commands.back().phrases == std::vector<std::string>({ "Apri le porte", "porte" }));
}

// --- Interruttori della nave con la telemetria (ship_toggle) ------------------------------------------------
namespace ship_toggle_tests {
    namespace ST = ShipToggle;
    using ST::Field;
    using ST::Value;

    // Una nave seduta al posto di pilotaggio, tutto acceso (alimentazione, motori, scudi, armi), carrello giù.
    ScTelemetry::ShipState seatedShip() {
        ScTelemetry::ShipState s;
        s.hasVehicle = s.hasPower = s.hasFlightState = s.hasFlightSettings = true;
        s.className = "AEGS_Gladius";
        s.lightsOn = false;
        s.powerPlants = s.engines = s.shieldGenerators = s.weapons = true;
        s.gear = ScTelemetry::Deploy::Deployed;
        s.coupled = { true, true };
        s.cruise = { true, false };
        return s;
    }

    // Il gioco finto: lo stato, un orologio che va avanti solo con le attese, i tasti premuti e cosa fanno.
    struct FakeGame {
        ScTelemetry::ShipState s = seatedShip();
        int clock = 0, pauses = 0;
        bool lost = false;
        std::vector<std::string> pressed;
        std::vector<std::pair<int, std::function<void(ScTelemetry::ShipState&)>>> later; // tra quanti ms, cosa cambia
        std::function<void(const std::string&)> onPress;                                 // cosa fa un tasto
        std::map<std::string, std::string> keys = GameProfile::defaults().keys;

        void at(int delayMs, std::function<void(ScTelemetry::ShipState&)> change) { later.push_back({ clock + delayMs, std::move(change) }); }
        std::optional<ScTelemetry::ShipState> state() {
            for (auto it = later.begin(); it != later.end();) {
                if (it->first <= clock) {
                    it->second(s);
                    it = later.erase(it);
                } else {
                    ++it;
                }
            }
            if (lost) return std::nullopt;
            return s;
        }
        ST::Env env() {
            ST::Env e;
            e.state = [this]() { return state(); };
            e.press = [this](const std::string& logical, bool& ok) {
                pressed.push_back(logical);
                auto it = keys.find(logical);
                ok = it != keys.end() && !it->second.empty();
                if (ok && onPress) onPress(logical);
                return ok ? it->second : "nessun tasto per \"" + logical + "\" nel profilo";
            };
            e.wait = [this](int ms) {
                clock += ms;
                return true;
            };
            e.pause = [this]() {
                clock += 1000;
                pauses++;
                return true;
            };
            e.cancelled = []() { return false; };
            return e;
        }
    };

    void run() {
        std::printf("\n[interruttori della nave con la telemetria]\n");
        const GameProfile p = GameProfile::defaults();

        section("tasto -> campo della telemetria (tasti veri del profilo predefinito)");
        const std::vector<std::pair<std::string, Field>> fields = { { "lights", Field::Lights },       { "weapons", Field::Weapons },
                                                                    { "shields", Field::Shields },     { "engines", Field::Engines },
                                                                    { "power", Field::Power },         { "landing_gear", Field::Gear },
                                                                    { "decoupled", Field::Decoupled }, { "cruise", Field::Cruise } };
        for (const auto& [key, field] : fields) {
            CHECK(ST::fieldFor(key) == field);
            CHECK(p.keys.count(key) == 1 && !p.keys.at(key).empty()); // il tasto c'è nel profilo
            bool command = false;
            for (const auto& c : p.commands) command = command || c.id() == "press:" + key;
            CHECK(command); // e c'è il comando
        }
        CHECK_STR(p.keys.at("weapons"), "P");
        CHECK_STR(p.keys.at("landing_gear"), "N");
        CHECK_STR(p.keys.at("decoupled"), "LAlt+C");
        CHECK(ST::fieldFor("vtol") == Field::None); // la telemetria lo dà sbagliato: non si controlla
        CHECK(ST::fieldFor("missiles") == Field::None && ST::fieldFor("quantum_spool") == Field::None && ST::fieldFor("") == Field::None);

        section("lettura dello stato");
        ScTelemetry::ShipState s = seatedShip();
        CHECK(ST::read(Field::Weapons, s) == Value::On);
        CHECK(ST::read(Field::Lights, s) == Value::Off);
        CHECK(ST::read(Field::Gear, s) == Value::On);
        CHECK(ST::read(Field::Decoupled, s) == Value::Off); // coupled attivo = non disaccoppiato
        CHECK(ST::read(Field::Cruise, s) == Value::Off);
        s.gear = ScTelemetry::Deploy::Deploying;
        CHECK(ST::read(Field::Gear, s) == Value::TurningOn);
        s.gear = ScTelemetry::Deploy::Retracting;
        CHECK(ST::read(Field::Gear, s) == Value::TurningOff);
        s.gear = ScTelemetry::Deploy::Unknown;
        CHECK(ST::read(Field::Gear, s) == Value::Unknown);
        s.coupled.active = false;
        CHECK(ST::read(Field::Decoupled, s) == Value::On);
        s.cruise.enabled = false; // la nave non ce l'ha: il valore non vuol dire niente
        CHECK(ST::read(Field::Cruise, s) == Value::Unknown);
        // A piedi arriva solo l'attore: tutto sconosciuto.
        ScTelemetry::ShipState onFoot;
        onFoot.hasActor = true;
        for (const auto& [key, field] : fields) CHECK(ST::read(field, onFoot) == Value::Unknown);

        section("verso delle frasi del profilo predefinito");
        const std::map<std::string, int> expected = {
            // carrello: abbassare = fuori, alzare / tirare su = dentro (al contrario degli scudi)
            { "carrello", 0 }, { "carrello di atterraggio", 0 }, { "abbassa il carrello", 1 }, { "alza il carrello", -1 },
            { "tira su il carrello", -1 }, { "gear", 0 },
            { "luci", 0 }, { "accendi le luci", 1 }, { "spegni le luci", -1 }, { "fari", 0 }, { "attiva le luci", 1 }, { "disattiva le luci", -1 },
            { "alimentazione", 0 }, { "accendi la nave", 1 }, { "spegni la nave", -1 },
            { "scudi", 0 }, { "alza gli scudi", 1 }, { "abbassa gli scudi", -1 }, { "attiva gli scudi", 1 }, { "disattiva gli scudi", -1 },
            { "accendi gli scudi", 1 }, { "spegni gli scudi", -1 },
            { "motori", 0 }, { "accendi i motori", 1 }, { "spegni i motori", -1 }, { "attiva i motori", 1 }, { "disattiva i motori", -1 },
            { "armi", 0 }, { "armi in linea", 1 }, { "arma i cannoni", 1 }, { "disarma", -1 }, { "attiva le armi", 1 }, { "disattiva le armi", -1 },
            { "accendi le armi", 1 }, { "spegni le armi", -1 },
            // disaccoppiato = +1 (On), coupled / accoppiato = -1
            { "modalita disaccoppiata", 1 }, { "disaccoppiato", 1 }, { "decoupled", 1 }, { "modalita coupled", -1 },
            { "attiva la modalita coupled", -1 }, { "disattiva la modalita coupled", 1 }, { "modalita accoppiata", -1 }, { "accoppiato", -1 },
            { "cruise control", 0 }, { "velocita di crociera", 0 }, { "crociera", 0 },
        };
        int phrases = 0;
        for (const auto& c : p.commands) {
            if (c.intent != "press") continue;
            const Field f = ST::fieldFor(c.args.count("key") ? c.args.at("key") : "");
            if (f == Field::None) continue;
            for (const auto& ph : c.phrases) {
                auto it = expected.find(ph);
                CHECK(it != expected.end()); // una frase nuova nel profilo: aggiungila qui col suo verso
                if (it == expected.end()) {
                    std::printf("    frase senza verso atteso: \"%s\"\n", ph.c_str());
                    continue;
                }
                if (ST::wanted(f, "", ph) != it->second) std::printf("    verso sbagliato: \"%s\"\n", ph.c_str());
                CHECK_EQ(ST::wanted(f, "", ph), it->second);
                phrases++;
            }
        }
        CHECK_EQ(phrases, (int)expected.size());

        // Dalla frase detta (Cmd::Match::heard), prima di quella del dizionario.
        auto said = [&](const std::string& text, Field f) {
            const Cmd::Match m = Cmd::match(p, text);
            return m.found ? ST::wanted(f, m.heard, m.phrase) : 99;
        };
        CHECK_EQ(said("Tira su il carrello.", Field::Gear), -1);
        CHECK_EQ(said("Abbassa il carrello", Field::Gear), 1);
        CHECK_EQ(said("giù il carrello", Field::Gear), 1);
        CHECK_EQ(said("Carrello.", Field::Gear), 0);
        CHECK_EQ(said("Accendi le armi.", Field::Weapons), 1);
        CHECK_EQ(said("Spegni le armi", Field::Weapons), -1);
        CHECK_EQ(said("Armi.", Field::Weapons), 0);
        CHECK_EQ(said("Alza gli scudi!", Field::Shields), 1);
        CHECK_EQ(said("Disattiva la modalità Capoled.", Field::Decoupled), 1); // corretta in "coupled"
        CHECK_EQ(said("Modalità coupled", Field::Decoupled), -1);
        CHECK_EQ(said("cruise control", Field::Cruise), 0);
        CHECK_EQ(ST::wanted(Field::Lights, "luci spente", "luci"), -1);
        CHECK_EQ(ST::wanted(Field::Shields, "scudi su", "scudi"), 1);
        // Richieste al modello con più azioni: il verso solo se tutte le parole sono d'accordo.
        CHECK_EQ(ST::wanted(Field::Gear, "alza il carrello e accendi le luci", ""), -1);
        CHECK_EQ(ST::wanted(Field::Lights, "alza il carrello e accendi le luci", ""), 1);
        CHECK_EQ(ST::wanted(Field::Lights, "spegni le luci e alza gli scudi", ""), 0);
        // La richiesta seguita dal modello, a pezzi: il verso solo dal pezzo che parla di quell'interruttore.
        const std::vector<std::string> parts = ST::clauses("Alza gli scudi, poi carrello. Accendi le luci e spegni i motori");
        CHECK(parts == std::vector<std::string>({ "alza gli scudi", "carrello", "accendi le luci", "spegni i motori" }));
        CHECK_STR(ST::clauseAbout(Field::Gear, parts), "carrello");
        CHECK_EQ(ST::wanted(Field::Gear, ST::clauseAbout(Field::Gear, parts), ""), 0); // "alza" era per gli scudi
        CHECK_EQ(ST::wanted(Field::Shields, ST::clauseAbout(Field::Shields, parts), ""), 1);
        CHECK_EQ(ST::wanted(Field::Lights, ST::clauseAbout(Field::Lights, parts), ""), 1);
        CHECK_EQ(ST::wanted(Field::Engines, ST::clauseAbout(Field::Engines, parts), ""), -1);
        CHECK_STR(ST::clauseAbout(Field::Weapons, parts), "");
        CHECK_STR(ST::clauseAbout(Field::Decoupled, ST::clauses("passa in modalità disaccoppiata")), "passa in modalita disaccoppiata");
        // Dallo strumento del modello.
        CHECK_EQ(ST::wantedFromState(Field::Weapons, "on"), 1);
        CHECK_EQ(ST::wantedFromState(Field::Weapons, "OFF"), -1);
        CHECK_EQ(ST::wantedFromState(Field::Shields, "up"), 1);
        CHECK_EQ(ST::wantedFromState(Field::Shields, "down"), -1);
        CHECK_EQ(ST::wantedFromState(Field::Gear, "down"), 1);
        CHECK_EQ(ST::wantedFromState(Field::Gear, "up"), -1);
        CHECK_EQ(ST::wantedFromState(Field::Gear, "on"), 0); // non si indovina
        CHECK_EQ(ST::wantedFromState(Field::Decoupled, "on"), 1);
        CHECK_EQ(ST::wantedFromState(Field::Decoupled, "off"), -1);
        CHECK_EQ(ST::wantedFromState(Field::Lights, ""), 0);
        // Le sequenze: il verso delle frasi del comando ("accendi tutto").
        for (const auto& c : p.commands) {
            if (c.intent != "sequence") continue;
            CHECK_EQ(ST::commandDirection(Field::Weapons, c.phrases), 1);
            CHECK_EQ(ST::commandDirection(Field::Power, c.phrases), 1);
            CHECK_EQ(ST::commandDirection(Field::Gear, c.phrases), 0);
        }

        section("già a posto: niente tasto, testi in italiano");
        auto already = [](Field f, int want, Value now, const char* spoken, const char* brief) {
            const ST::Plan pl = ST::plan(f, want, now);
            CHECK(!pl.press);
            CHECK_STR(pl.spoken, spoken);
            CHECK_STR(pl.brief, brief);
        };
        already(Field::Weapons, 1, Value::On, "Le armi sono già accese.", "Già accese.");
        already(Field::Weapons, -1, Value::Off, "Le armi sono già spente.", "Già spente.");
        already(Field::Shields, 1, Value::On, "Gli scudi sono già alzati.", "Già alzati.");
        already(Field::Shields, -1, Value::Off, "Gli scudi sono già abbassati.", "Già abbassati.");
        already(Field::Lights, -1, Value::Off, "Le luci sono già spente.", "Già spente.");
        already(Field::Engines, 1, Value::On, "I motori sono già accesi.", "Già accesi.");
        already(Field::Power, 1, Value::On, "L'alimentazione è già accesa.", "Già accesa.");
        already(Field::Gear, 1, Value::On, "Il carrello è già abbassato.", "Già abbassato.");
        already(Field::Gear, -1, Value::Off, "Il carrello è già alzato.", "Già alzato.");
        already(Field::Decoupled, 1, Value::On, "Sei già in modalità disaccoppiata.", "Già disaccoppiato.");
        already(Field::Decoupled, -1, Value::Off, "Sei già in modalità accoppiata.", "Già accoppiato.");
        already(Field::Cruise, -1, Value::Off, "Il cruise control è già spento.", "Già spento.");
        // Stati di passaggio del carrello.
        already(Field::Gear, 1, Value::TurningOn, "Il carrello sta già scendendo.", "Sta già scendendo.");
        already(Field::Gear, -1, Value::TurningOff, "Il carrello sta già salendo.", "Sta già salendo.");
        ST::Plan pl = ST::plan(Field::Gear, -1, Value::TurningOn); // scende e lo vuoi su: si preme
        CHECK(pl.press && pl.target == Value::Off);
        pl = ST::plan(Field::Weapons, -1, Value::On);
        CHECK(pl.press && pl.target == Value::Off);
        pl = ST::plan(Field::Weapons, 0, Value::On); // senza verso: si preme, e deve cambiare
        CHECK(pl.press && pl.target == Value::Off);
        pl = ST::plan(Field::Gear, 0, Value::Off);
        CHECK(pl.press && pl.target == Value::On);
        pl = ST::plan(Field::Weapons, 1, Value::Unknown); // stato sconosciuto: si preme come prima
        CHECK(pl.press && pl.target == Value::Unknown);
        pl = ST::plan(Field::None, 1, Value::On);
        CHECK(pl.press && pl.target == Value::Unknown);

        section("dopo il tasto: è cambiato?");
        {
            FakeGame g;
            g.onPress = [&](const std::string&) { g.at(300, [](ScTelemetry::ShipState& x) { x.weapons = false; }); };
            ST::Env e = g.env();
            bool ok = false;
            const std::string combo = e.press("weapons", ok);
            const ST::Outcome o = ST::afterPress(Field::Weapons, Value::Off, combo, e);
            CHECK(o.used && o.ok);
            CHECK_STR(o.spoken, "Armi spente.");
            CHECK_STR(o.brief, "Armi spente.");
            CHECK(o.shown.find("telemetria") != std::string::npos);
            CHECK(g.clock >= 300 && g.clock < 500);
        }
        {
            FakeGame g; // il tasto non fa niente (gioco in pausa, tasto sbagliato...)
            ST::Env e = g.env();
            const ST::Outcome o = ST::afterPress(Field::Weapons, Value::Off, "P", e);
            CHECK(o.used && !o.ok);
            CHECK_STR(o.spoken, "Ho premuto P, ma le armi risultano ancora accese.");
            CHECK_STR(o.brief, "Non è cambiato.");
            CHECK(g.clock >= 2500 && g.clock <= 2600); // circa 2,5 s, non di più
        }
        {
            FakeGame g; // carrello su, lo abbassi: dopo il tasto scende
            g.s.gear = ScTelemetry::Deploy::Retracted;
            g.onPress = [&](const std::string&) { g.at(200, [](ScTelemetry::ShipState& x) { x.gear = ScTelemetry::Deploy::Deploying; }); };
            ST::Env e = g.env();
            bool ok = false;
            const ST::Outcome o = ST::afterPress(Field::Gear, Value::On, e.press("landing_gear", ok), e);
            CHECK(o.used && o.ok);
            CHECK_STR(o.spoken, "Carrello in discesa.");
            CHECK_STR(o.brief, "Carrello giù.");
        }
        {
            FakeGame g; // la telemetria si perde: si fa come sempre (nessun verdetto)
            g.lost = true;
            ST::Env e = g.env();
            CHECK(!ST::afterPress(Field::Weapons, Value::Off, "P", e).used);
            FakeGame d; // in prova non si aspetta niente
            ST::Env de = d.env();
            de.dryRun = true;
            CHECK(!ST::afterPress(Field::Weapons, Value::Off, "P", de).used && d.clock == 0);
        }
        CHECK_STR(ST::notChangedText(Field::Decoupled, Value::Off, "LAlt+C"), "Ho premuto LAlt+C, ma risulti ancora in modalità accoppiata.");
        CHECK_STR(ST::notChangedText(Field::Gear, Value::TurningOn, "N"), "Ho premuto N, ma il carrello sta ancora scendendo.");
        CHECK_STR(ST::doneText(Field::Shields, Value::On), "Scudi alzati.");
        CHECK_STR(ST::doneText(Field::Cruise, Value::On), "Cruise control acceso.");
        CHECK_STR(ST::doneBrief(Field::Cruise, Value::On), "Cruise acceso.");

        section("sequenze: passi saltati, alimentazione che si assesta");
        auto allOn = [](Field) { return 1; };
        const std::vector<std::string> steps = ST::splitSteps("power, engines,shields , weapons");
        CHECK(steps == std::vector<std::string>({ "power", "engines", "shields", "weapons" }));
        {
            // Nave spenta: motori e scudi tornano da soli quando torna l'alimentazione, le armi no.
            FakeGame g;
            g.s.powerPlants = g.s.engines = g.s.shieldGenerators = g.s.weapons = false;
            g.onPress = [&](const std::string& k) {
                if (k == "power") {
                    g.at(250, [](ScTelemetry::ShipState& x) { x.powerPlants = true; });
                    g.at(600, [](ScTelemetry::ShipState& x) { x.engines = true; });
                    g.at(900, [](ScTelemetry::ShipState& x) { x.shieldGenerators = true; });
                }
                if (k == "weapons") g.at(300, [](ScTelemetry::ShipState& x) { x.weapons = true; });
            };
            ST::Env e = g.env();
            CHECK(!ST::nothingToPress(steps, allOn, *g.state()));
            const ST::Outcome o = ST::sequence(steps, allOn, "Accendo tutto.", e);
            CHECK(o.used && o.ok && !o.interrupted);
            CHECK(g.pressed == std::vector<std::string>({ "power", "weapons" }));
            CHECK_STR(o.spoken, "Accendo tutto. Già a posto: motori e scudi.");
            CHECK_STR(o.brief, "Fatto.");
            CHECK(o.shown.find("power (U), weapons (P)") != std::string::npos);
            CHECK(o.shown.find("già a posto (telemetria): engines, shields") != std::string::npos);
            CHECK(g.clock >= 900 + 1000); // ha aspettato che la nave si assestasse
        }
        {
            // Tutto già acceso: niente tasti.
            FakeGame g;
            ST::Env e = g.env();
            CHECK(ST::nothingToPress(steps, allOn, *g.state()));
            const ST::Outcome o = ST::sequence(steps, allOn, "Accendo tutto.", e);
            CHECK(o.ok && g.pressed.empty());
            CHECK_STR(o.spoken, "È già tutto a posto: alimentazione, motori, scudi e armi.");
            CHECK_STR(o.brief, "Già a posto.");
        }
        {
            // Le armi non si accendono: lo dice.
            FakeGame g;
            g.s.weapons = false;
            g.s.shieldGenerators = false;
            g.onPress = [&](const std::string& k) {
                if (k == "shields") g.at(200, [](ScTelemetry::ShipState& x) { x.shieldGenerators = true; });
            };
            ST::Env e = g.env();
            const ST::Outcome o = ST::sequence(steps, allOn, "Accendo tutto.", e);
            CHECK(!o.ok);
            CHECK(g.pressed == std::vector<std::string>({ "shields", "weapons" }));
            CHECK_EQ(g.pauses, 1); // la pausa di sempre tra due tasti
            CHECK_STR(o.spoken, "Ho premuto O e P, ma le armi risultano ancora spente.");
            CHECK_STR(o.brief, "Non è cambiato.");
        }
        {
            // Senza verso (una tua sequenza "parcheggia"): si preme tutto, come prima.
            FakeGame g;
            auto none = [](Field) { return 0; };
            ST::Env e = g.env();
            g.onPress = [&](const std::string& k) {
                if (k == "landing_gear") g.at(100, [](ScTelemetry::ShipState& x) { x.gear = ScTelemetry::Deploy::Retracting; });
                if (k == "engines") g.at(100, [](ScTelemetry::ShipState& x) { x.engines = false; });
            };
            const ST::Outcome o = ST::sequence({ "landing_gear", "engines" }, none, "Parcheggio.", e);
            CHECK(o.ok && g.pressed.size() == 2);
            CHECK_STR(o.spoken, "Parcheggio.");
        }

        section("verbi col pronome e \"accendi tutti i sistemi\"");
        CHECK_EQ(Cmd::polarity("allora spegnile"), -1);
        CHECK_EQ(Cmd::polarity("riaccendile"), 1);
        CHECK_EQ(Cmd::polarity("alzali"), 1);
        CHECK_EQ(Cmd::polarity("abbassalo"), -1);
        CHECK_EQ(Cmd::polarity("riaccendi anche le armi va"), 1);
        CHECK_EQ(Cmd::polarity("passami le palle"), 0); // "palle" non è un verbo
        CHECK(Cmd::hasPronounVerb("allora spegnile"));
        CHECK(Cmd::hasPronounVerb("eh pero riaccendile"));
        CHECK(!Cmd::hasPronounVerb("spegni le armi"));
        CHECK(!Cmd::hasPronounVerb("alla fine"));
        {
            const GameProfile d = GameProfile::defaults();
            for (const char* said : { "accendi tutti i sistemi", "Iris accendi tutte le cose della nave", "attiva tutti i sistemi",
                                      "accendi tutto quanto" }) {
                const Cmd::Match m = Cmd::match(d, said);
                CHECK(m.found && m.command.intent == "sequence");
            }
            const Cmd::Match power = Cmd::match(d, "accendi la nave");
            CHECK(power.found && power.command.intent == "press" && power.command.args.at("key") == "power");
            // I profili di prima ricevono le frasi nuove.
            GameProfile old = d;
            for (auto& c : old.commands) {
                if (c.intent == "sequence") c.phrases = { "prepara la nave", "accendi tutto" };
            }
            old.defaultsVersion = 2;
            CHECK(old.addMissingDefaults() > 0);
            CHECK(Cmd::match(old, "accendi tutti i sistemi").command.intent == "sequence");
        }

        section("comandi veri (Esplora risorse al posto del gioco, modalità prova)");
        if (GameInput::findGameWindow("explorer", "")) {
            GameController game;
            game.load();
            GameProfile fake = game.profile();
            fake.processName = "explorer";
            fake.windowTitle.clear();
            fake.dryRun = true;
            CHECK(game.saveProfile(fake));
            auto sayFn = [](const std::string&) {};
            // Senza telemetria: esattamente come prima.
            GameController::Result r = game.execute(game.match("accendi le armi"), sayFn, nullptr);
            CHECK(r.ok && r.shown.find("(P)") != std::string::npos && r.spoken == "Armi.");
            CHECK(r.shown.find("telemetria") == std::string::npos);
            game.setShipStateProvider([]() -> std::optional<ScTelemetry::ShipState> { return std::nullopt; });
            r = game.execute(game.match("accendi le armi"), sayFn, nullptr);
            CHECK(r.ok && r.shown.find("(P)") != std::string::npos && r.spoken == "Armi.");

            ScTelemetry::ShipState ship = seatedShip();
            game.setShipStateProvider([&]() -> std::optional<ScTelemetry::ShipState> { return ship; });
            r = game.execute(game.match("accendi le armi"), sayFn, nullptr);
            CHECK(r.ok && r.shown.find("(P)") == std::string::npos); // niente tasto
            CHECK_STR(r.spoken, "Le armi sono già accese.");
            CHECK_STR(r.brief, "Già accese.");
            CHECK(r.shown.find("telemetria") != std::string::npos);
            {
                // "Allora spegnile!" dopo "le armi sono già accese": le armi, non un altro interruttore.
                const Cmd::Match follow = game.match("Allora spegnile!");
                CHECK(follow.found && follow.command.args.count("key") && follow.command.args.at("key") == "weapons");
                r = game.execute(follow, sayFn, nullptr);
                CHECK(r.ok && r.shown.find("(P)") != std::string::npos); // spegnere le armi accese: si preme
                r = game.execute(game.match("Eh, però riaccendile"), sayFn, nullptr);
                CHECK_STR(r.spoken, "Le armi sono già accese."); // (in prova lo stato non cambia)
                CHECK(!game.match("e quindi?").found); // niente verbo col pronome: non è un comando
            }
            r = game.execute(game.match("spegni le armi"), sayFn, nullptr); // in prova: si guarda lo stato, non si preme
            CHECK(r.ok && r.shown.find("[prova]") != std::string::npos && r.shown.find("(P)") != std::string::npos);
            CHECK(r.shown.find("telemetria: armi accese") != std::string::npos);
            r = game.execute(game.match("alza gli scudi"), sayFn, nullptr);
            CHECK_STR(r.spoken, "Gli scudi sono già alzati.");
            r = game.execute(game.match("abbassa il carrello"), sayFn, nullptr);
            CHECK_STR(r.spoken, "Il carrello è già abbassato.");
            ship.gear = ScTelemetry::Deploy::Deploying;
            r = game.execute(game.match("abbassa il carrello"), sayFn, nullptr);
            CHECK_STR(r.spoken, "Il carrello sta già scendendo.");
            ship.gear = ScTelemetry::Deploy::Retracted;
            r = game.execute(game.match("tira su il carrello"), sayFn, nullptr);
            CHECK_STR(r.spoken, "Il carrello è già alzato.");
            r = game.execute(game.match("carrello"), sayFn, nullptr); // senza verso: si preme
            CHECK(r.ok && r.shown.find("(N)") != std::string::npos);
            r = game.execute(game.match("accoppiato"), sayFn, nullptr);
            CHECK_STR(r.spoken, "Sei già in modalità accoppiata.");
            r = game.execute(game.match("accendi tutto"), sayFn, nullptr);
            CHECK(r.ok && r.shown.find("niente da premere") != std::string::npos);
            CHECK_STR(r.spoken, "È già tutto a posto: alimentazione, motori, scudi e armi.");
            ship.weapons = false;
            r = game.execute(game.match("prepara la nave"), sayFn, nullptr); // le armi sì, il resto no
            CHECK(r.ok && r.shown.find("weapons (P)") != std::string::npos && r.shown.find("power (U)") == std::string::npos);
            CHECK_STR(r.spoken, "Accendo tutto. Già a posto: alimentazione, motori e scudi.");
            ship.weapons = true;
            // Dal modello (strumento game_command): lo stesso, col verso dato dal modello o preso dalla richiesta.
            r = game.executeById("press:weapons", "", sayFn, nullptr, "on");
            CHECK_STR(r.spoken, "Le armi sono già accese.");
            r = game.executeById("press:landing_gear", "", sayFn, nullptr, "up");
            CHECK_STR(r.spoken, "Il carrello è già alzato.");
            r = game.executeById("press:landing_gear", "", sayFn, nullptr, "on", "Iris, alza il carrello per favore");
            CHECK_STR(r.spoken, "Il carrello è già alzato."); // "on" sul carrello non si capisce: vale la richiesta
            r = game.executeById("press:shields", "", sayFn, nullptr, "", "potresti alzare gli scudi?");
            CHECK_STR(r.spoken, "Gli scudi sono già alzati.");
            r = game.executeById("press:weapons", "", sayFn, nullptr);
            CHECK(r.ok && r.shown.find("(P)") != std::string::npos); // senza verso: si preme
            r = game.executeById("press:landing_gear", "", sayFn, nullptr, "", "alza gli scudi, poi carrello");
            CHECK(r.ok && r.shown.find("(N)") != std::string::npos); // "alza" era per gli scudi: il carrello si preme
            r = game.executeById("press:decoupled", "", sayFn, nullptr, "", "Disattiva la modalità Capoled");
            CHECK(r.ok && r.shown.find("(LAlt+C)") != std::string::npos); // accoppiato, lo vuoi disaccoppiato: si preme
            r = game.executeById("press:decoupled", "", sayFn, nullptr, "off");
            CHECK_STR(r.spoken, "Sei già in modalità accoppiata.");
            CHECK(game.toolDefinition().find("\"state\"") != std::string::npos);
            game.setShipStateProvider({});
        }

        section("impostazioni: telemetria e voce neurale");
        AppSettings fresh = AppSettings::parse("");
        CHECK(fresh.gameTelemetryEnabled); // predefinita: accesa
        fresh.gameTelemetryEnabled = false;
        AppSettings back = AppSettings::parse(fresh.serialize());
        CHECK(!back.gameTelemetryEnabled);
        CHECK(AppSettings::parse("gameTelemetryEnabled=1\n").gameTelemetryEnabled);
        CHECK(!AppSettings::parse("gameTelemetryEnabled=0\n").gameTelemetryEnabled);
        AppSettings v;
        CHECK(v.wantsNeuralVoice());
        v.speechEnabled = false; // risposte a voce spente: la voce neurale non si carica
        CHECK(!v.wantsNeuralVoice());
        v.speechEnabled = true;
        v.voiceEngine = "windows";
        CHECK(!v.wantsNeuralVoice());
        v.voiceEngine = "neurale";
        v.gpuMemoryLimitMb = 0; // tutto sul processore
        CHECK(!v.wantsNeuralVoice());
        CHECK(!AppSettings::parse("speechEnabled=0\n").wantsNeuralVoice());
    }
} // namespace ship_toggle_tests

static void testShipToggle() { ship_toggle_tests::run(); }

// --- Registro delle scansioni del mining ------------------------------------------------------------------
static void testMiningLog() {
    std::printf("\n[registro del mining]\n");
    CHECK_STR(MiningLog::gameName("Copper (Rame)"), "Copper");
    CHECK_STR(MiningLog::gameName("Quantanium"), "Quantanium");
    const std::string iso = MiningLog::isoLocal(1761990000);
    CHECK(iso.size() == 25 && iso[10] == 'T' && (iso[19] == '+' || iso[19] == '-') && iso[22] == ':');
    CHECK(TextUtils::startsWith(MiningLog::fileStem(1761990000), "Iris mining 20"));

    auto entry = [](long long t, int value, const char* outcome, const char* label, int qty, const char* source = "tasto") {
        MiningLog::Entry e;
        e.epoch = t;
        e.value = value;
        e.outcome = outcome;
        e.label = label;
        e.material = MiningLog::gameName(label);
        e.kind = "minerale";
        e.quantity = qty;
        e.source = source;
        return e;
    };

    // Il registro: spento non registra; la stessa roccia riletta conta una volta (e tiene la lettura migliore).
    MiningLog::Recorder rec;
    rec.load();
    if (rec.active()) rec.cancel();
    const long long t0 = 1761990000;
    CHECK(!rec.add(entry(t0, 16960, "certo", "Copper (Rame)", 4)));
    rec.start(t0);
    CHECK(rec.active());
    CHECK(rec.add(entry(t0 + 5, 16960, "probabile", "Copper (Rame)", 4, "continua")));
    CHECK(rec.add(entry(t0 + 20, 16950, "certo", "Copper (Rame)", 4, "continua"))); // stessa roccia (2%), più sicura
    CHECK(rec.add(entry(t0 + 60, 16960, "certo", "Copper (Rame)", 4)));             // riletta di continuo: ancora la stessa
    CHECK(rec.add(entry(t0 + 200, 16960, "certo", "Copper (Rame)", 4)));            // dopo un bel po': un'altra roccia
    MiningLog::Entry amb = entry(t0 + 260, 16000, "ambiguo", "", 0);
    amb.alternatives = { { "Savrilium", "Savrilium", "minerale", 5 }, { "Scrap", "Scrap", "salvage", 8 } };
    CHECK(rec.add(amb));
    CHECK(rec.add(entry(t0 + 300, 9510, "certo", "Quantanium", 3)));
    CHECK(rec.add(entry(t0 + 330, 13337, "sconosciuto", "", 0)));
    auto list = rec.entries();
    CHECK_EQ((int)list.size(), 5);
    if (list.size() == 5) {
        CHECK_STR(list[0].outcome, "certo");
        CHECK_EQ(list[0].reads, 3);
        CHECK_EQ((int)list[0].epoch, (int)(t0 + 5)); // l'ora della prima lettura
    }
    // Il registro in corso sopravvive a un riavvio.
    MiningLog::Recorder again;
    again.load();
    CHECK(again.active());
    CHECK_EQ((int)again.entries().size(), 5);

    // Riepilogo: solo i certi e i probabili nei totali.
    const MiningLog::Summary sum = MiningLog::summarize(list);
    CHECK_EQ(sum.finds, 5);
    CHECK_EQ(sum.sure, 3);
    CHECK_EQ(sum.toCheck, 2);
    CHECK_EQ((int)sum.materials.size(), 2);
    if (sum.materials.size() == 2) {
        CHECK_STR(sum.materials[0].label, "Copper (Rame)"); // la quantità più alta prima
        CHECK_EQ(sum.materials[0].rocks, 2);
        CHECK_EQ(sum.materials[0].quantity, 8);
    }

    // Il file per il sito: si rilegge, chiavi in italiano, totali giusti.
    MiningLog::Closing c;
    c.place = "Aaron Halo";
    c.quality = 640;
    c.operatorName = "MrRevo";
    c.program = "Iris 1.2.0";
    c.start = t0;
    c.end = t0 + 3600;
    const nlohmann::json j = nlohmann::json::parse(MiningLog::toJson(list, c), nullptr, false);
    CHECK(j.is_object());
    if (j.is_object()) {
        CHECK_STR(j.value("formato", std::string()), "iris-registro-mining");
        CHECK_EQ(j.value("versione", 0), 1);
        CHECK_STR(j.value("luogo", std::string()), "Aaron Halo");
        CHECK_EQ(j.value("qualitaMedia", -1), 640);
        CHECK_EQ(j.value("durataMinuti", 0), 60);
        CHECK_STR(j.value("operatore", std::string()), "MrRevo");
        CHECK_EQ(j["riepilogo"].value("rilevamenti", 0), 5);
        CHECK_EQ(j["riepilogo"].value("daVerificare", 0), 2);
        CHECK_STR(j["riepilogo"]["materiali"][0].value("materiale", std::string()), "Copper");
        CHECK_EQ(j["riepilogo"]["materiali"][0].value("quantita", 0), 8);
        CHECK_EQ((int)j["rilevamenti"].size(), 5);
        CHECK_STR(j["rilevamenti"][2].value("esito", std::string()), "ambiguo");
        CHECK_EQ((int)j["rilevamenti"][2]["alternative"].size(), 2);
        CHECK(!j["rilevamenti"][2].contains("materiale"));
        CHECK_EQ(j["rilevamenti"][0].value("letture", 0), 3);
    }
    const std::string csv = MiningLog::toCsv(list, c);
    CHECK(TextUtils::startsWith(csv, "\xEF\xBB\xBF" "Luogo;Aaron Halo"));
    CHECK(csv.find("Savrilium x5, Scrap x8") != std::string::npos);
    // Per Excel in italiano: data e ora che riconosce ("2025-11-01 10:40:00"), niente formule dai nomi.
    CHECK(csv.find("Inizio;" + MiningLog::isoLocal(t0).substr(0, 10) + " " + MiningLog::isoLocal(t0).substr(11, 8) + "\r\n") != std::string::npos);
    CHECK(csv.find("\r\n" + MiningLog::isoLocal(t0 + 5).substr(0, 10) + " ") != std::string::npos);
    {
        MiningLog::Closing evil = c;
        evil.place = "=HYPERLINK(\"x\");Halo";
        const std::string e2 = MiningLog::toCsv(list, evil);
        CHECK(e2.find("Luogo;\"'=HYPERLINK(\"\"x\"\");Halo\"") != std::string::npos);
        const nlohmann::json j2 = nlohmann::json::parse(MiningLog::toJson(list, evil), nullptr, false);
        CHECK(j2.is_object() && j2.value("luogo", std::string()) == evil.place);
    }

    // Chiusura: i file in una cartella di prova (non sul desktop vero), il registro si spegne, il luogo si ricorda.
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring folder = std::wstring(tmp) + L"IrisTests";
    std::wstring path;
    std::string error;
    CHECK(again.finish(c, folder, true, path, error));
    CHECK(GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES);
    const std::wstring csvPath = path.substr(0, path.size() - 5) + L".csv";
    CHECK(GetFileAttributesW(csvPath.c_str()) != INVALID_FILE_ATTRIBUTES);
    CHECK(!again.active());
    CHECK(again.entries().empty());
    CHECK(!again.recentPlaces().empty() && again.recentPlaces().front() == "Aaron Halo");
    CHECK(again.lastFile() == path);
    CHECK(!again.finish(c, folder, false, path, error)); // già chiuso
    DeleteFileW(path.c_str());
    DeleteFileW(csvPath.c_str());

    // Rocce vicine: Copper × 4 (16.960) e Iron × 4 (17.080) distano meno del 2% ma sono due rocce;
    // tornare sul rame dopo il ferro è la stessa roccia di prima; una cifra letta male va con la più vicina.
    {
        MiningLog::Recorder r2;
        r2.load();
        if (r2.active()) r2.cancel();
        r2.start(t0);
        CHECK(r2.add(entry(t0, 16960, "certo", "Copper (Rame)", 4, "continua")));
        CHECK(r2.add(entry(t0 + 5, 17080, "certo", "Iron (Ferro)", 4, "continua")));
        CHECK(r2.add(entry(t0 + 10, 16960, "certo", "Copper (Rame)", 4, "continua")));
        CHECK(r2.add(entry(t0 + 12, 16968, "sconosciuto", "", 0, "continua")));
        const auto l2 = r2.entries();
        CHECK_EQ((int)l2.size(), 2);
        if (l2.size() == 2) {
            CHECK_EQ(l2[0].reads, 3);
            CHECK_STR(l2[0].outcome, "certo");
            CHECK_STR(l2[1].label, "Iron (Ferro)");
            CHECK_EQ(l2[1].reads, 1);
        }
        r2.cancel();
    }
}

// --- Interfaccia e Windows: aggiornamento, file salvati, server per OBS, finestra -----------------------------
static void testInterfaceAndWindows() {
    std::printf("\n[interfaccia e windows]\n");
    // Aggiornamento: GitHub che limita i controlli (403/429) si lascia in pace un'ora, un suo errore o la
    // rete che manca mezz'ora, poi sempre più piano (al massimo 4 ore). Testo in italiano, non "ha risposto 503".
    Updater::Failure f = Updater::describeFailure(403, "");
    CHECK(f.retryMinutes == 60 && f.text.find("limitato") != std::string::npos);
    CHECK_EQ(Updater::describeFailure(429, "").retryMinutes, 60);
    f = Updater::describeFailure(503, "");
    CHECK(f.retryMinutes == 30 && f.text.find("non risponde") != std::string::npos && f.text.find("503") != std::string::npos);
    f = Updater::describeFailure(0, "tempo scaduto");
    CHECK(f.retryMinutes == 30 && f.text.find("connessione") != std::string::npos);
    CHECK_EQ(Updater::describeFailure(404, "").retryMinutes, 240);
    CHECK_EQ(Updater::retryDelayMinutes(30, 1), 30);
    CHECK_EQ(Updater::retryDelayMinutes(30, 2), 60);
    CHECK_EQ(Updater::retryDelayMinutes(30, 3), 120);
    CHECK_EQ(Updater::retryDelayMinutes(30, 12), 240);
    CHECK_EQ(Updater::retryDelayMinutes(60, 0), 60);

    // Una versione scaricata che, installata, non parte come quella versione: si scarta e non si
    // reinstalla a ogni avvio (niente aggiornamenti in cerchio).
    {
        CreateDirectoryW(AppPaths::getAppDataDir().c_str(), nullptr);
        CreateDirectoryW(Updater::updatesDir().c_str(), nullptr);
        DeleteFileW((Updater::updatesDir() + L"\\scartate.txt").c_str());
        const std::wstring exe = Updater::downloadedPath("99.0.0");
        std::ofstream(exe, std::ios::binary | std::ios::trunc) << "MZ finto exe di prova";
        std::string hash;
        CHECK(Updater::sha256File(exe, hash));
        std::ofstream(exe + L".ok", std::ios::binary | std::ios::trunc) << hash;
        std::string version;
        CHECK(Updater::pendingUpdate(&version) == exe);
        CHECK_STR(version, "99.0.0");
        CHECK(!Updater::isBroken("99.0.0"));
        Updater::confirmInstalled(); // c'è ancora uno scaricato più nuovo: l'installato non era quello
        CHECK(Updater::isBroken("99.0.0") && Updater::isBroken("V99.0") && !Updater::isBroken("99.0.1"));
        CHECK(GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES);
        CHECK(Updater::pendingUpdate().empty());
        DeleteFileW((Updater::updatesDir() + L"\\scartate.txt").c_str());
    }

    // File salvati "tutto o niente": il vecchio resta come .bak, e un file vuoto (PC spento mentre si
    // salvava) lascia il posto alla copia di sicurezza.
    {
        const std::wstring file = AppPaths::getAppDataDir() + L"\\prova_salvataggio.cfg";
        for (const wchar_t* ext : { L"", L".bak", L".tmp" }) DeleteFileW((file + ext).c_str());
        CHECK(AppPaths::writeFileSafely(file, "uno"));
        CHECK(AppPaths::writeFileSafely(file, "due"));
        std::string text;
        bool fromBackup = true;
        CHECK(AppPaths::readFileOrBackup(file, text, &fromBackup) && text == "due" && !fromBackup);
        HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        CloseHandle(h); // rovinato: vuoto
        CHECK(AppPaths::readFileOrBackup(file, text, &fromBackup) && text == "uno" && fromBackup);
        CHECK(AppPaths::writeFileSafely(file, "tre")); // sopra quello vuoto: la copia buona resta
        CHECK(AppPaths::readFileOrBackup(file, text, &fromBackup) && text == "tre" && !fromBackup);
        CHECK(AppPaths::readFileOrBackup(file + L".bak", text) && text == "uno");
        CHECK(GetFileAttributesW((file + L".tmp").c_str()) == INVALID_FILE_ATTRIBUTES);
        CHECK(!AppPaths::readFileOrBackup(file + L".non-esiste", text));
        for (const wchar_t* ext : { L"", L".bak", L".tmp" }) DeleteFileW((file + ext).c_str());
    }

    // Impostazioni scritte a mano con numeri impossibili; la finestra ingrandita si ricorda.
    AppSettings odd = AppSettings::parse("micGain=nan\nspeechRate=inf\nvadThreshold=-nan\nsilenceThreshold=nan\n");
    odd.sanitize();
    CHECK(odd.micGain == 1.0f && odd.speechRate == 1.1 && odd.vadThreshold == 0.5 && odd.silenceThreshold == 0.01);
    AppSettings big;
    CHECK(!big.windowMaximized && !AppSettings::parse("").windowMaximized);
    big.windowMaximized = true;
    CHECK(AppSettings::parse(big.serialize()).windowMaximized);

    // Server per OBS: solo questo computer e le sue pagine. Un sito aperto nel browser (Origin suo) o che
    // si fa passare per localhost (Host suo: "DNS rebinding") non legge assi e tasti dal WebSocket.
    CHECK(AxisServer::allowedRequest("localhost:8787", "", 8787));
    CHECK(AxisServer::allowedRequest("127.0.0.1:8787", "http://127.0.0.1:8787", 8787));
    CHECK(AxisServer::allowedRequest("[::1]:8787", "http://localhost:8787", 8787));
    CHECK(AxisServer::allowedRequest("LOCALHOST", "", 8787));
    CHECK(AxisServer::allowedRequest("", "", 8787));
    CHECK(!AxisServer::allowedRequest("localhost:8787", "https://sito-qualsiasi.example", 8787));
    CHECK(!AxisServer::allowedRequest("localhost:8787", "null", 8787));
    CHECK(!AxisServer::allowedRequest("localhost:8787", "http://localhost:8788", 8787));
    CHECK(!AxisServer::allowedRequest("sito.example:8787", "", 8787));
    CHECK(!AxisServer::allowedRequest("localhost.sito.example:8787", "", 8787));
    CHECK(!AxisServer::allowedRequest("localhost:9999", "", 8787));
    {
        AxisServer server;
        server.setPages("<html>KEY=__KEY__</html>", "<html>__ROWS__</html>");
        std::string error;
        int port = 0;
        for (int p = 18800; p < 18820 && !port; ++p) {
            if (server.start(p, error)) port = p;
        }
        CHECK(port != 0);
        if (port) {
            const std::string url = "http://127.0.0.1:" + std::to_string(port) + "/g1";
            CHECK_EQ(Http::get(url).status, 200);
            Http::Options foreign;
            foreign.headers = { L"Origin: https://sito-qualsiasi.example" };
            CHECK_EQ(Http::get(url, foreign).status, 403);
            server.stop();
        }
    }
}

// --- Coerenza della voce neurale (iris_tests --voce-coerenza) -----------------------------
// Non possiamo ascoltare: si misura. Per ogni frase F0 mediana, quanto la F0 si sposta
// dentro la frase (finestre da 0,5 s), centroide spettrale e il "timbro" (x-vector del
// codificatore di parlante del mmproj): stessa voce = timbri molto simili.
namespace voice_coherence {
    struct Row {
        std::string text;
        std::vector<float> pcm; // 24 kHz
        long long ms = 0, firstMs = -1;
        double seconds = 0, f0 = 0, f0Spread = 0, f0Jump = 0, centroid = 0, halves = -1, toMean = 0;
        std::vector<float> xvec;
        std::string heard;
    };

    double semitones(double f, double ref) { return f > 0 && ref > 0 ? 12.0 * std::log2(f / ref) : 0.0; }

    void writeWav(const std::wstring& path, const std::vector<float>& pcm, int rate) {
        FILE* out = nullptr;
        if (_wfopen_s(&out, path.c_str(), L"wb") != 0 || !out) return;
        const uint32_t r = (uint32_t)rate, dataBytes = (uint32_t)pcm.size() * 2, riff = 36 + dataBytes, fmtSize = 16, byteRate = r * 2;
        const uint16_t fmtPcm = 1, channels = 1, align = 2, bits = 16;
        fwrite("RIFF", 1, 4, out); fwrite(&riff, 4, 1, out); fwrite("WAVEfmt ", 1, 8, out); fwrite(&fmtSize, 4, 1, out);
        fwrite(&fmtPcm, 2, 1, out); fwrite(&channels, 2, 1, out); fwrite(&r, 4, 1, out); fwrite(&byteRate, 4, 1, out);
        fwrite(&align, 2, 1, out); fwrite(&bits, 2, 1, out); fwrite("data", 1, 4, out); fwrite(&dataBytes, 4, 1, out);
        for (float x : pcm) {
            const int16_t v = (int16_t)std::clamp(x * 32767.0f, -32768.0f, 32767.0f);
            fwrite(&v, 2, 1, out);
        }
        fclose(out);
    }

    // Le misure di una frase (il timbro a parte: serve la voce neurale caricata).
    void analyze(Row& r, int rate) {
        r.seconds = r.pcm.size() / (double)rate;
        const std::vector<float> f0 = VoiceMeasure::pitch(r.pcm.data(), r.pcm.size(), rate);
        r.f0 = VoiceMeasure::median(f0);
        r.centroid = VoiceMeasure::centroid(r.pcm.data(), r.pcm.size(), rate);
        // Finestre da 0,5 s (50 valori da 10 ms): mediana di ognuna, poi quanto si spostano.
        std::vector<double> windows;
        for (size_t at = 0; at + 25 <= f0.size(); at += 50) {
            std::vector<float> w(f0.begin() + at, f0.begin() + std::min(f0.size(), at + 50));
            const size_t voiced = (size_t)std::count_if(w.begin(), w.end(), [](float v) { return v > 0; });
            if (voiced >= 12) windows.push_back(semitones(VoiceMeasure::median(w), r.f0));
        }
        double mean = 0, var = 0, jump = 0;
        for (double v : windows) mean += v;
        if (!windows.empty()) mean /= windows.size();
        for (size_t i = 0; i < windows.size(); ++i) {
            var += (windows[i] - mean) * (windows[i] - mean);
            if (i > 0) jump = std::max(jump, std::fabs(windows[i] - windows[i - 1]));
        }
        r.f0Spread = windows.size() > 1 ? std::sqrt(var / windows.size()) : 0.0;
        r.f0Jump = jump;
    }

    // Timbro della frase intera e delle due metà (solo frasi da 3 s in su).
    void timbre(Row& r, NeuralVoice& voice, int rate) {
        voice.speakerEmbedding(r.pcm.data(), r.pcm.size(), rate, r.xvec);
        if (r.seconds >= 3.0) {
            std::vector<float> a, b;
            const size_t half = r.pcm.size() / 2;
            voice.speakerEmbedding(r.pcm.data(), half, rate, a);
            voice.speakerEmbedding(r.pcm.data() + half, r.pcm.size() - half, rate, b);
            r.halves = VoiceMeasure::cosine(a, b);
        }
    }

    struct Summary {
        double f0 = 0, f0Dev = 0, centroid = 0, centroidDev = 0, toMean = 0, minPair = 1, spread = 0, jump = 0, halves = 0;
    };

    Summary summarize(std::vector<Row>& rows) {
        Summary s;
        std::vector<float> f0s;
        for (const auto& r : rows) f0s.push_back((float)r.f0);
        const double ref = VoiceMeasure::median(f0s);
        s.f0 = ref;
        double m = 0, c = 0;
        for (const auto& r : rows) {
            m += semitones(r.f0, ref);
            c += r.centroid;
        }
        m /= rows.size();
        c /= rows.size();
        double v = 0, cv = 0;
        for (const auto& r : rows) {
            v += (semitones(r.f0, ref) - m) * (semitones(r.f0, ref) - m);
            cv += (r.centroid - c) * (r.centroid - c);
        }
        s.f0Dev = std::sqrt(v / rows.size());
        s.centroid = c;
        s.centroidDev = c > 0 ? 100.0 * std::sqrt(cv / rows.size()) / c : 0.0;
        // Timbro: il "centro" di tutte le frasi, poi quanto ognuna gli somiglia.
        std::vector<float> centre;
        for (const auto& r : rows) {
            if (r.xvec.empty()) continue;
            if (centre.empty()) centre.assign(r.xvec.size(), 0.0f);
            for (size_t i = 0; i < centre.size() && i < r.xvec.size(); ++i) centre[i] += r.xvec[i];
        }
        int n = 0, nh = 0;
        for (auto& r : rows) {
            if (r.xvec.empty()) continue;
            r.toMean = VoiceMeasure::cosine(r.xvec, centre);
            s.toMean += r.toMean;
            n++;
            for (const auto& o : rows) {
                if (&o != &r && !o.xvec.empty()) s.minPair = std::min(s.minPair, VoiceMeasure::cosine(r.xvec, o.xvec));
            }
            if (r.halves >= 0) {
                s.halves += r.halves;
                nh++;
            }
            s.spread += r.f0Spread;
            s.jump += r.f0Jump;
        }
        if (n > 0) {
            s.toMean /= n;
            s.spread /= n;
            s.jump /= n;
        }
        if (nh > 0) s.halves /= nh;
        return s;
    }

    void print(const char* title, std::vector<Row>& rows) {
        const Summary s = summarize(rows);
        std::printf("\n%s\n", title);
        std::printf("  #  durata  tempo   1o audio   F0    var.F0  salto  centroide  timbro  metà\n");
        for (size_t i = 0; i < rows.size(); ++i) {
            const Row& r = rows[i];
            std::printf(" %2zu  %5.1fs %6lldms %6lldms %5.0fHz %5.2fst %5.2fst %7.0fHz  %.3f  %s\n", i + 1, r.seconds, r.ms, r.firstMs, r.f0, r.f0Spread,
                        r.f0Jump, r.centroid, r.toMean, r.halves >= 0 ? (std::to_string(r.halves).substr(0, 5)).c_str() : "  -  ");
        }
        std::printf("  TRA LE FRASI:    F0 %.0f Hz, dev. std %.2f semitoni; centroide %.0f Hz, dev. std %.1f%%; timbro: somiglianza al centro %.3f, coppia peggiore %.3f\n",
                    s.f0, s.f0Dev, s.centroid, s.centroidDev, s.toMean, s.minPair);
        std::printf("  DENTRO LE FRASI: variazione F0 %.2f semitoni, salto massimo medio %.2f semitoni; timbro prima metà / seconda metà %.3f\n", s.spread,
                    s.jump, s.halves);
    }

    const char* const kSentences[] = {
        "Fatto.",
        "Carrello abbassato.",
        "Rotta impostata per Seraphim Station, in orbita attorno a Crusader.",
        "Scudi al massimo e motori accesi, pronta al decollo.",
        "Sul conto hai 16.960 UEC e nella stiva ci sono 32 SCU di Laranite.",
        "Il salto quantico verso Hurston dura circa 2,5 minuti.",
        "Domani a Roma il cielo sarà coperto al mattino, con qualche schiarita nel pomeriggio e una minima di 6 gradi, quindi ti conviene portare una giacca se esci presto.",
        "Ciao Rebo, sono online e pronta.",
        "Attenzione: carburante quantico al 15%, ti consiglio di fare rifornimento a Port Tressler prima di ripartire.",
        "L'ordine della raffineria di ArcCorp è pronto: ritiralo quando vuoi.",
    };

    bool g_forVoice = true;

    // "t=0.8 k=40 p=0.95 m=0.05 r=1.05 s=123 c=12 f=0": campionamento diverso da quello di Iris.
    NeuralVoice::Sampling parseSampling(int argc, char** argv, int from) {
        NeuralVoice::Sampling s;
        for (int i = from; i < argc; ++i) {
            const std::string a = argv[i];
            if (a.size() < 3 || a[1] != '=') continue;
            const double v = std::atof(a.c_str() + 2);
            switch (a[0]) {
            case 't': s.temperature = (float)v; break;
            case 'k': s.topK = (int)v; break;
            case 'p': s.topP = (float)v; break;
            case 'm': s.minP = (float)v; break;
            case 'r': s.repeatPenalty = (float)v; break;
            case 's': s.seed = (uint32_t)std::strtoul(a.c_str() + 2, nullptr, 10); break;
            case 'c': s.firstChunk = (int)v; break;
            case 'f': g_forVoice = v != 0; break; // f=0: senza numeri in lettere (come la Iris di prima)
            }
        }
        return s;
    }

    // La voce: "" = quella predefinita di Iris, un id ("aurora"), "nessuna" (la voce a caso di
    // prima) o un file WAV di riferimento.
    bool chooseVoice(NeuralVoice& voice, const std::string& choice) {
        if (choice.empty() || NeuralVoice::isPreset(choice)) {
            voice.setVoice(choice.empty() ? NeuralVoice::presets().front().id : choice);
            return voice.prepare();
        }
        if (choice == "nessuna") return voice.setReferenceAudio({}, 0);
        std::vector<float> pcm;
        int rate = 0;
        std::string error;
        if (!NeuralVoice::readWav(TextUtils::utf8ToWide(choice), pcm, rate, &error) || !NeuralVoice::cleanReference(pcm, rate, &error)) {
            std::printf("Riferimento non usabile: %s\n", error.c_str());
            return false;
        }
        return voice.setReferenceAudio(pcm, rate);
    }

    int run(const std::wstring& folder, const std::string& voiceChoice, const NeuralVoice::Sampling& sampling) {
        std::error_code dirError;
        std::filesystem::create_directories(folder, dirError);
        NeuralVoice voice;
        if (!voice.load(AppPaths::toLibraryPath(AppPaths::neuralVoiceModelPath()), AppPaths::toLibraryPath(AppPaths::neuralVoiceMmprojPath()), 8)) {
            std::printf("Voce neurale non caricata\n");
            return 1;
        }
        voice.setSampling(sampling);
        const int rate = voice.sampleRate();
        auto t0 = std::chrono::steady_clock::now();
        if (!chooseVoice(voice, voiceChoice)) std::printf("ATTENZIONE: voce \"%s\" non preparata\n", voiceChoice.c_str());
        voice.speak("Pronta.", [](const float*, size_t) { return true; }); // riscaldamento, come in Iris
        std::printf("Voce: %s. Campionamento: temperatura %.2f, top-k %d, top-p %.2f, min-p %.2f, ripetizione %.2f, seme %u. Preparazione: %lld ms\n",
                    voice.voiceName().c_str(), sampling.temperature, sampling.topK, sampling.topP, sampling.minP, sampling.repeatPenalty, sampling.seed,
                    (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());

        std::vector<Row> rows;
        std::vector<float> all;
        for (const char* sentence : kSentences) {
            Row r;
            r.text = g_forVoice ? SpeechText::forVoice(SpeechText::speakable(sentence)) : SpeechText::speakable(sentence); // come in Iris
            const auto start = std::chrono::steady_clock::now();
            voice.speak(r.text, [&](const float* p, size_t n) {
                if (r.firstMs < 0) r.firstMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
                r.pcm.insert(r.pcm.end(), p, p + n);
                return true;
            });
            r.ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
            analyze(r, rate);
            timbre(r, voice, rate);
            wchar_t name[32];
            swprintf_s(name, L"\\frase-%02zu.wav", rows.size() + 1);
            writeWav(folder + name, r.pcm, rate);
            all.insert(all.end(), r.pcm.begin(), r.pcm.end());
            all.resize(all.size() + rate * 4 / 10, 0.0f);
            rows.push_back(std::move(r));
        }
        writeWav(folder + L"\\tutte.wav", all, rate);

        // Per confronto: la voce di Windows (Elsa), che è sempre la stessa per costruzione.
        std::vector<Row> elsa;
        for (const char* sentence : kSentences) {
            Row r;
            r.text = SpeechText::speakable(sentence);
            std::thread([&]() { r.pcm = TtsEngine::synthesize(r.text, "Elsa"); }).join();
            if (r.pcm.empty()) break;
            analyze(r, TtsEngine::kSampleRate);
            timbre(r, voice, TtsEngine::kSampleRate);
            elsa.push_back(std::move(r));
        }
        voice.unload();

        // Whisper: cosa si capisce (nomi, numeri).
        WhisperEngine whisper;
        if (whisper.init(AppPaths::toLibraryPath(AppPaths::whisperModelPath()), AppPaths::toLibraryPath(AppPaths::vadModelPath()), 8, true)) {
            for (auto& r : rows) r.heard = whisper.transcribe(VoiceMeasure::resample(r.pcm.data(), r.pcm.size(), rate, 16000), "it", "");
        }

        print("VOCE NEURALE", rows);
        if (!elsa.empty()) print("VOCE DI WINDOWS (Elsa), per confronto: una voce sempre uguale", elsa);
        std::printf("\nCosa si capisce (whisper):\n");
        for (size_t i = 0; i < rows.size(); ++i) {
            std::printf(" %2zu  detto:   %s\n     capito:  %s\n", i + 1, rows[i].text.c_str(), rows[i].heard.c_str());
        }
        std::printf("\nFile in %s\n", TextUtils::wideToUtf8(folder).c_str());
        return 0;
    }

    // Le voci che il modello si inventa, seme per seme: la voce di riferimento (senza
    // riferimento) e tre frasi dette imitandola. Per scegliere le voci predefinite.
    int seeds(uint32_t first, uint32_t last, const std::wstring& folder) {
        std::error_code dirError;
        std::filesystem::create_directories(folder, dirError);
        NeuralVoice voice;
        if (!voice.load(AppPaths::toLibraryPath(AppPaths::neuralVoiceModelPath()), AppPaths::toLibraryPath(AppPaths::neuralVoiceMmprojPath()), 8)) return 1;
        const int rate = voice.sampleRate();
        voice.setReferenceAudio({}, 0);
        voice.speak("Pronta.", [](const float*, size_t) { return true; });
        const char* const probe[] = { "Rotta impostata per Hurston.", "Scudi al massimo e motori accesi, pronta al decollo.",
                                      "Domani a Roma il cielo sarà coperto, con una minima di 6 gradi." };
        struct Seed {
            uint32_t seed;
            Row ref;
            std::vector<Row> said;
        };
        std::vector<Seed> list;
        for (uint32_t seed = first; seed <= last; ++seed) {
            Seed s{ seed };
            const NeuralVoice::Preset p{ "prova", "prova", seed };
            if (!voice.makePresetReference(p, s.ref.pcm) || !NeuralVoice::cleanReference(s.ref.pcm, rate)) {
                std::printf("seme %u: niente voce\n", seed);
                continue;
            }
            analyze(s.ref, rate);
            timbre(s.ref, voice, rate);
            voice.setReferenceAudio(s.ref.pcm, rate);
            std::vector<float> all;
            for (const char* t : probe) {
                Row r;
                r.text = t;
                voice.speak(t, [&](const float* x, size_t n) {
                    r.pcm.insert(r.pcm.end(), x, x + n);
                    return true;
                });
                analyze(r, rate);
                timbre(r, voice, rate);
                r.toMean = VoiceMeasure::cosine(r.xvec, s.ref.xvec);
                all.insert(all.end(), r.pcm.begin(), r.pcm.end());
                all.resize(all.size() + rate * 4 / 10, 0.0f);
                s.said.push_back(std::move(r));
            }
            voice.setReferenceAudio({}, 0);
            wchar_t name[64];
            swprintf_s(name, L"\\seme-%u.wav", seed);
            writeWav(folder + name, s.ref.pcm, rate);
            swprintf_s(name, L"\\seme-%u-frasi.wav", seed);
            writeWav(folder + name, all, rate);
            std::printf("seme %u fatto\n", seed);
            list.push_back(std::move(s));
        }
        voice.unload();
        WhisperEngine whisper;
        const bool hear = whisper.init(AppPaths::toLibraryPath(AppPaths::whisperModelPath()), AppPaths::toLibraryPath(AppPaths::vadModelPath()), 8, true);
        std::printf("\n seme   durata  F0rif  var.F0  metà   | F0 frasi (st dal rif.)    timbro come il rif.  | whisper\n");
        for (auto& s : list) {
            if (hear) s.ref.heard = whisper.transcribe(VoiceMeasure::resample(s.ref.pcm.data(), s.ref.pcm.size(), rate, 16000), "it", "");
            double minSim = 1;
            std::string f0s;
            for (const auto& r : s.said) {
                minSim = std::min(minSim, r.toMean);
                char b[24];
                std::snprintf(b, sizeof(b), "%+5.1f ", semitones(r.f0, s.ref.f0));
                f0s += b;
            }
            std::printf(" %5u  %5.1fs %5.0fHz %5.2fst  %.3f | %s       min %.3f          | %s\n", s.seed, s.ref.seconds, s.ref.f0, s.ref.f0Spread,
                        s.ref.halves, f0s.c_str(), minSim, s.ref.heard.c_str());
        }
        return 0;
    }

    // Il primo pezzo chiesto prima (dopo 12 fotogrammi invece che 72) cambia l'audio? E dove
    // un pezzo finisce e l'altro comincia, il segnale salta (clic)?
    int seams() {
        NeuralVoice voice;
        if (!voice.load(AppPaths::toLibraryPath(AppPaths::neuralVoiceModelPath()), AppPaths::toLibraryPath(AppPaths::neuralVoiceMmprojPath()), 8)) return 1;
        voice.setVoice(NeuralVoice::presets().front().id);
        voice.prepare();
        const std::string text = SpeechText::speakable(kSentences[6]);
        auto once = [&](int firstChunk, std::vector<size_t>& cuts, long long& firstMs) {
            NeuralVoice::Sampling s;
            s.firstChunk = firstChunk;
            voice.setSampling(s);
            std::vector<float> pcm;
            const auto t0 = std::chrono::steady_clock::now();
            firstMs = -1;
            voice.speak(text, [&](const float* x, size_t n) {
                if (firstMs < 0) firstMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
                pcm.insert(pcm.end(), x, x + n);
                cuts.push_back(pcm.size());
                return true;
            });
            return pcm;
        };
        std::vector<size_t> cutsA, cutsB;
        long long msA = 0, msB = 0;
        const std::vector<float> a = once(12, cutsA, msA), b = once(0, cutsB, msB);
        double diff = 0, energy = 0, maxDiff = 0;
        for (size_t i = 0; i < std::min(a.size(), b.size()); ++i) {
            diff += (double)(a[i] - b[i]) * (a[i] - b[i]);
            energy += (double)b[i] * b[i];
            maxDiff = std::max(maxDiff, (double)std::fabs(a[i] - b[i]));
        }
        std::printf("Primo pezzo dopo 12 fotogrammi: %zu campioni, primo audio %lld ms, pezzi a", a.size(), msA);
        for (size_t c : cutsA) std::printf(" %.2fs", c / (double)voice.sampleRate());
        std::printf("\nBlocchi da 72 (libreria):       %zu campioni, primo audio %lld ms, pezzi a", b.size(), msB);
        for (size_t c : cutsB) std::printf(" %.2fs", c / (double)voice.sampleRate());
        std::printf("\nDifferenza: massima %.5f, rapporto segnale/differenza %.1f dB\n", maxDiff, diff > 0 ? 10 * std::log10(energy / diff) : 999.0);
        // Salti ai confini: |x[i]-x[i-1]| al confine rispetto al 99,9° percentile di tutto l'audio.
        for (const auto* pair : { &a, &b }) {
            const std::vector<float>& x = *pair;
            const std::vector<size_t>& cuts = pair == &a ? cutsA : cutsB;
            std::vector<float> steps;
            for (size_t i = 1; i < x.size(); ++i) steps.push_back(std::fabs(x[i] - x[i - 1]));
            std::vector<float> sorted = steps;
            std::sort(sorted.begin(), sorted.end());
            const float p999 = sorted.empty() ? 0 : sorted[sorted.size() * 999 / 1000];
            for (size_t c : cuts) {
                if (c > 0 && c < x.size()) std::printf("  %s: confine a %.2fs, salto %.4f (99,9%% dei salti sotto %.4f)\n", pair == &a ? "prima" : "72   ",
                                                       c / (double)voice.sampleRate(), steps[c - 1], p999);
            }
        }
        // Inizio e fine della frase: da quanto vicino a zero partono/arrivano.
        if (!a.empty()) std::printf("  primo campione %.4f, ultimo %.4f\n", a.front(), a.back());
        return 0;
    }
} // namespace voice_coherence

// --- Telemetria di Star Citizen (sc_telemetry) --------------------------------------------------
namespace telemetry_test {
    // Un piccolo codificatore protobuf per costruire i frame. Come il gioco (proto3) non scrive i valori
    // predefiniti (false, 0, ""); i sotto-messaggi invece ci sono anche vuoti.
    struct Pb {
        std::string b;

        static void putVarint(std::string& s, uint64_t v) {
            while (v >= 0x80) {
                s += (char)((v & 0x7F) | 0x80);
                v >>= 7;
            }
            s += (char)v;
        }
        static std::string le32(uint32_t v) {
            std::string s;
            for (int i = 0; i < 4; ++i) s += (char)((v >> (8 * i)) & 0xFF);
            return s;
        }
        static uint32_t bits(float f) {
            uint32_t u;
            std::memcpy(&u, &f, 4);
            return u;
        }
        Pb& key(uint32_t field, int wire) {
            putVarint(b, ((uint64_t)field << 3) | (uint64_t)wire);
            return *this;
        }
        Pb& always(uint32_t field, uint64_t v) { // anche 0 (i campi "optional")
            key(field, 0);
            putVarint(b, v);
            return *this;
        }
        Pb& varint(uint32_t field, uint64_t v) { return v ? always(field, v) : *this; }
        Pb& flag(uint32_t field, bool v) { return varint(field, v ? 1 : 0); }
        Pb& f32(uint32_t field, float v) {
            if (v != 0.0f) {
                key(field, 5);
                b += le32(bits(v));
            }
            return *this;
        }
        Pb& f64(uint32_t field, uint64_t v) {
            key(field, 1);
            b += le32((uint32_t)v) + le32((uint32_t)(v >> 32));
            return *this;
        }
        Pb& bytes(uint32_t field, const std::string& v) {
            key(field, 2);
            putVarint(b, v.size());
            b += v;
            return *this;
        }
        Pb& str(uint32_t field, const std::string& v) { return v.empty() ? *this : bytes(field, v); }
        Pb& msg(uint32_t field, const Pb& m) { return bytes(field, m.b); }
        Pb& packed(uint32_t field, const std::vector<float>& v) {
            std::string p;
            for (float f : v) p += le32(bits(f));
            return bytes(field, p);
        }
        Pb& raw(const std::string& s) {
            b += s;
            return *this;
        }
    };

    static Pb avail(bool enabled, bool active) { return Pb().flag(1, enabled).flag(2, active); }
    static Pb gauge(float current, float max) { return Pb().f32(1, current).f32(2, max); }

    // La Gladius di captures\session_20261121_120702.jsonl (Arena Commander, t = 1795259513.737): i valori
    // veri, campo per campo, anche quelli che Iris non legge (spie, impostazioni, ambiente...).
    static Pb gladiusVehicle() {
        Pb annunciators; // pwr NOMINAL, wpn MINOR_ON, thr/shld NOMINAL, cool/qdrv OFF, il resto NOMINAL
        annunciators.varint(1, 1).varint(2, 4).varint(3, 1).varint(4, 1).varint(5, 2).varint(6, 2).varint(7, 1).varint(8, 1).varint(9, 1).varint(10, 1);
        Pb flightState; // gear e vtol RETRACTED ("optional": scritti sempre), is_landed false
        flightState.always(2, 1).always(3, 1);
        Pb settings;
        settings.msg(1, avail(true, true)).msg(2, avail(true, true)).msg(3, avail(true, true)).msg(4, avail(true, true))
            .msg(5, avail(false, false)).flag(6, true).msg(7, avail(true, true)).msg(8, avail(true, false)).msg(9, avail(true, false))
            .msg(10, avail(true, true)).msg(11, avail(true, true)).flag(15, true).msg(16, avail(true, true)).flag(17, true)
            .msg(18, avail(true, true)).flag(19, false);
        Pb instruments; // speed 0, quantum 0; altitudine FLT_MAX = "non disponibile"
        instruments.f32(2, 1.0f).msg(4, gauge(0, 0)).msg(5, Pb().f32(3, 1.0f)).f32(6, 3.4028235e+38f).f32(7, 3.4028235e+38f).f32(9, 0.0026071696f);
        Pb power;
        power.flag(1, true).flag(2, true).flag(3, true).flag(4, true).flag(5, true).flag(6, false).flag(7, true).flag(8, true).flag(9, true);
        Pb shields;
        shields.f32(1, 0.42200407f).packed(2, { 0.42200407f });
        Pb state;
        state.str(1, "AEGS_Gladius_200000010670").str(2, "AEGS_Gladius").str(3, "").varint(4, 1).flag(5, false).msg(7, annunciators)
            .msg(8, Pb().msg(1, flightState).msg(2, settings).msg(3, instruments)).msg(9, power).msg(10, shields).f32(11, 1.0f)
            .msg(12, Pb().varint(1, 2).flag(2, true));
        return Pb().msg(2, state); // VehicleFrame
    }

    static Pb gladiusWeapons() {
        Pb w;
        w.msg(1, Pb().flag(8, true)); // gimbals_locked
        for (const char* name : { "GATS_BallisticGatling_S3_200000010719", "KLWE_LaserRepeater_S3_200000010721", "KLWE_LaserRepeater_S3_200000010723" })
            w.msg(4, Pb().str(1, name).flag(2, true).flag(3, true).flag(4, true));
        w.msg(5, Pb().varint(1, 1).varint(2, 48)).msg(5, Pb().varint(1, 2).varint(2, 5)); // 48 decoy, 5 noise
        return w;
    }

    static Pb gladiusActor() {
        Pb vitals;
        vitals.msg(1, gauge(100, 100)).msg(2, gauge(1, 1)).msg(3, gauge(0, 0)).msg(4, gauge(0, 0)).msg(5, gauge(0, 0))
            .f32(7, 60.797455f).f32(8, 1.0003417f).f32(9, 1.0f);
        Pb st;
        st.varint(1, 3).varint(2, 1).str(3, "AEGS_Gladius_SCItem_Seat_Pilot_200000010672").varint(4, 14984695);
        Pb env;
        env.f32(1, 20.999994f).flag(3, true);
        return Pb().msg(1, vitals).msg(2, st).msg(3, env);
    }

    // Il frame intero, nell'ordine del gioco: vehicle (3), weapons (4), actor (6). In ends le fine dei campi.
    static std::string gladiusFrame(std::vector<size_t>* ends = nullptr) {
        Pb frame;
        frame.msg(3, gladiusVehicle());
        if (ends) ends->push_back(frame.b.size());
        frame.msg(4, gladiusWeapons());
        if (ends) ends->push_back(frame.b.size());
        frame.msg(6, gladiusActor());
        if (ends) ends->push_back(frame.b.size());
        return frame.b;
    }

    // Lo stesso frame serializzato dal protobuf vero (Python, json_format.ParseDict della riga della cattura
    // e SerializeToString): 495 byte. Il codificatore delle prove deve dare gli stessi byte.
    static std::string gladiusCaptured() {
        static const char* hex =
            "1ae00112dd010a19414547535f476c61646975735f323030303030303130363730120c414547535f476c616469757320013a140801100418"
            "01200128023002380140014801500142700a041001180112490a04080110011204080110011a04080110012204080110012a0030013a0408"
            "011001420208014a0208015204080110015a0408011001780182010408011001880101920104080110011a1d150000803f22002a051d0000"
            "803f35ffff7f7f3dffff7f7f4d0cdd2a3b4a1008011001180120012801380140014801520b0deb10d83e1204eb10d83e5d0000803f620408"
            "0210012297010a024001222d0a25474154535f42616c6c69737469634761746c696e675f53335f3230303030303031303731391001180120"
            "01222a0a224b4c57455f4c6173657252657065617465725f53335f323030303030303130373231100118012001222a0a224b4c57455f4c61"
            "73657252657065617465725f53335f3230303030303031303732331001180120012a04080110302a040802100532700a2d0a0a0d0000c842"
            "150000c842120a0d0000803f150000803f1a0022002a003d9830734245320b803f4d0000803f1236080310011a2b414547535f476c616469"
            "75735f53434974656d5f536561745f50696c6f745f32303030303030313036373220f7cb92071a070dfdffa7411801";
        std::string out;
        for (size_t i = 0; hex[i] && hex[i + 1]; i += 2) out += (char)std::stoi(std::string(hex + i, 2), nullptr, 16);
        return out;
    }

    static bool decode(const std::string& bytes, ScTelemetry::ShipState& s) {
        return ScTelemetry::decodeFrame((const uint8_t*)bytes.data(), bytes.size(), s);
    }

    static const char* deployName(ScTelemetry::Deploy d) {
        using ScTelemetry::Deploy;
        return d == Deploy::Retracted ? "chiuso" : d == Deploy::Deploying ? "in apertura" : d == Deploy::Deployed ? "aperto" : d == Deploy::Retracting ? "in chiusura" : "?";
    }

    // Tutto lo stato in testo: per confrontare due decodifiche e per --telemetria.
    static std::string describe(const ScTelemetry::ShipState& s) {
        using namespace ScTelemetry;
        char buf[2048];
        std::string out;
        auto on = [](bool b) { return b ? "si" : "no"; };
        auto av = [](const Availability& a) { return std::string(a.enabled ? "c'e'" : "manca") + "/" + (a.active ? "acceso" : "spento"); };
        if (s.hasVehicle) {
            std::snprintf(buf, sizeof(buf), "  nave: %s (%s), luci %s, salute %.3f\n", s.className.c_str(), s.vehicleName.c_str(), on(s.lightsOn), s.health);
            out += buf;
        } else {
            out += "  nave: nessuna\n";
        }
        if (s.hasPower) {
            std::snprintf(buf, sizeof(buf), "  potenza: centrale %s, motori %s, armi %s, raffr. %s, vita %s, gravita' %s, sensori %s, scudi gen %s / emett %s\n",
                          on(s.powerPlants), on(s.engines), on(s.weapons), on(s.coolers), on(s.lifeSupport), on(s.gravityGenerators), on(s.sensors),
                          on(s.shieldGenerators), on(s.shieldEmitters));
            out += buf;
        }
        if (s.hasFlightState) {
            std::snprintf(buf, sizeof(buf), "  volo: atterrata %s, carrello %s, vtol %s\n", on(s.isLanded), deployName(s.gear), deployName(s.vtol));
            out += buf;
        }
        if (s.hasFlightSettings) {
            std::snprintf(buf, sizeof(buf), "  impostazioni: coupled %s, esp %s, g-safe %s, cruise %s, limitatore %s, prossimita' %s, visione notturna %s\n",
                          av(s.coupled).c_str(), av(s.esp).c_str(), av(s.gSafe).c_str(), av(s.cruise).c_str(), av(s.speedLimiter).c_str(),
                          av(s.proximityAssist).c_str(), on(s.lightAmplification));
            out += buf;
        }
        if (s.hasInstruments) {
            std::snprintf(buf, sizeof(buf), "  strumenti: velocita' %.1f m/s, idrogeno %.3f, quantico %.3f, boost %s\n", s.speed, s.hydrogenFuel, s.quantumFuel, on(s.boostEngaged));
            out += buf;
        }
        if (s.hasShields) {
            std::snprintf(buf, sizeof(buf), "  scudi: %.3f\n", s.shieldLevel);
            out += buf;
        }
        if (s.hasMasterMode) {
            const char* mode = s.masterMode == MasterMode::Navigation ? "NAV" : s.masterMode == MasterMode::Scm ? "SCM" : s.masterMode == MasterMode::Stealth ? "STEALTH" : "?";
            std::snprintf(buf, sizeof(buf), "  master mode: %s, pronta %s\n", mode, on(s.spooled));
            out += buf;
        }
        if (s.hasWeapons) {
            std::snprintf(buf, sizeof(buf), "  armi: %d/%d alimentate, gimbal bloccati %s, decoy %d, noise %d\n", s.weaponsPowered, s.weaponCount, on(s.gimbalsLocked), s.decoys, s.noise);
            out += buf;
        }
        if (s.hasActor) {
            const char* loc = s.locomotion == Locomotion::OnFoot ? "a piedi" : s.locomotion == Locomotion::Eva ? "EVA" : s.locomotion == Locomotion::Seated ? "seduto" : "?";
            const char* role = s.crewRole == CrewRole::Pilot ? "pilota" : s.crewRole == CrewRole::Gunner ? "artigliere" : s.crewRole == CrewRole::Crew ? "equipaggio" : "-";
            std::snprintf(buf, sizeof(buf), "  attore: %s, ruolo %s, posto \"%s\", salute %.1f/%.1f, ossigeno %.2f, aUEC %lld\n", loc, role, s.seatName.c_str(),
                          s.actorHealth, s.actorHealthMax, s.oxygen, s.uec);
            out += buf;
        }
        return out;
    }

    // HTTP/2 nelle prove: un frame, e i frame dentro un pezzo di byte.
    struct H2Frame {
        uint8_t type = 0, flags = 0;
        uint32_t stream = 0;
        std::string payload;
    };

    static std::string h2(uint8_t type, uint8_t flags, uint32_t stream, const std::string& payload) {
        std::string f;
        const size_t n = payload.size();
        f += (char)((n >> 16) & 0xFF);
        f += (char)((n >> 8) & 0xFF);
        f += (char)(n & 0xFF);
        f += (char)type;
        f += (char)flags;
        for (int i = 3; i >= 0; --i) f += (char)((stream >> (8 * i)) & 0xFF);
        return f + payload;
    }

    static uint32_t be32(const std::string& s, size_t at) {
        return ((uint32_t)(uint8_t)s[at] << 24) | ((uint32_t)(uint8_t)s[at + 1] << 16) | ((uint32_t)(uint8_t)s[at + 2] << 8) | (uint8_t)s[at + 3];
    }

    static std::vector<H2Frame> h2Frames(const std::string& bytes, size_t from = 0) {
        std::vector<H2Frame> out;
        while (bytes.size() - from >= 9) {
            const size_t n = ((size_t)(uint8_t)bytes[from] << 16) | ((size_t)(uint8_t)bytes[from + 1] << 8) | (uint8_t)bytes[from + 2];
            if (bytes.size() - from < 9 + n) break;
            H2Frame f;
            f.type = (uint8_t)bytes[from + 3];
            f.flags = (uint8_t)bytes[from + 4];
            f.stream = be32(bytes, from + 5) & 0x7FFFFFFF;
            f.payload = bytes.substr(from + 9, n);
            out.push_back(f);
            from += 9 + n;
        }
        return out;
    }

    // La somma degli incrementi dei WINDOW_UPDATE per uno stream.
    static uint64_t windowCredit(const std::string& reply, uint32_t stream) {
        uint64_t sum = 0;
        for (const H2Frame& f : h2Frames(reply))
            if (f.type == 8 && f.stream == stream && f.payload.size() == 4) sum += be32(f.payload, 0);
        return sum;
    }

    // Un messaggio gRPC: compressione (1 byte), lunghezza (4, big-endian), il messaggio.
    static std::string grpcMessage(const std::string& body, uint8_t compressed = 0) {
        std::string m(1, (char)compressed);
        for (int i = 3; i >= 0; --i) m += (char)((body.size() >> (8 * i)) & 0xFF);
        return m + body;
    }

    static bool feed(ScTelemetry::detail::Http2Receiver& rx, const std::string& bytes) {
        return rx.feed((const uint8_t*)bytes.data(), bytes.size());
    }
} // namespace telemetry_test

// Decodifica del protobuf, HTTP/2 e gRPC, flusso "congelato" al menu. Senza rete.
static void testTelemetry() {
    using namespace ScTelemetry;
    using namespace telemetry_test;
    using namespace std::string_literals; // "..."s: stringhe con dentro degli zeri
    std::printf("\n[telemetria di Star Citizen]\n");

    section("frame vero: la Gladius di una cattura");
    {
        ShipState s;
        const std::string frame = gladiusCaptured();
        CHECK_EQ(frame.size(), (size_t)495);
        CHECK(gladiusFrame() == frame); // il codificatore delle prove scrive come il protobuf vero
        CHECK(decode(frame, s));
        CHECK(s.hasVehicle);
        CHECK_STR(s.vehicleName, "AEGS_Gladius_200000010670");
        CHECK_STR(s.className, "AEGS_Gladius");
        CHECK(!s.lightsOn);
        CHECK(s.health == 1.0f);
        CHECK(s.hasPower);
        CHECK(s.powerPlants && s.engines && s.weapons && s.coolers && s.lifeSupport && s.sensors && s.shieldGenerators && s.shieldEmitters);
        CHECK(!s.gravityGenerators);
        CHECK(s.hasFlightState);
        CHECK(!s.isLanded);
        CHECK(s.gear == Deploy::Retracted);
        CHECK(s.vtol == Deploy::Retracted);
        CHECK(s.hasFlightSettings);
        CHECK(s.coupled.enabled && s.coupled.active);
        CHECK(s.esp.enabled && s.esp.active);
        CHECK(s.gSafe.enabled && s.gSafe.active);
        CHECK(!s.cruise.enabled && !s.cruise.active);
        CHECK(s.speedLimiter.enabled && s.speedLimiter.active);
        CHECK(s.proximityAssist.enabled && !s.proximityAssist.active);
        CHECK(!s.lightAmplification);
        CHECK(s.hasInstruments);
        CHECK(s.speed == 0.0f);
        CHECK(s.hydrogenFuel == 1.0f);
        CHECK(s.quantumFuel == 0.0f);
        CHECK(!s.boostEngaged);
        CHECK(s.hasShields);
        CHECK(s.shieldLevel == 0.42200407f);
        CHECK(s.hasMasterMode);
        CHECK(s.masterMode == MasterMode::Scm);
        CHECK(s.spooled);
        CHECK(s.hasWeapons);
        CHECK(s.gimbalsLocked);
        CHECK_EQ(s.weaponCount, 3);
        CHECK_EQ(s.weaponsPowered, 3);
        CHECK_EQ(s.decoys, 48);
        CHECK_EQ(s.noise, 5);
        CHECK(s.hasActor);
        CHECK(s.locomotion == Locomotion::Seated);
        CHECK(s.crewRole == CrewRole::Pilot);
        CHECK_STR(s.seatName, "AEGS_Gladius_SCItem_Seat_Pilot_200000010672");
        CHECK(s.actorHealth == 100.0f && s.actorHealthMax == 100.0f);
        CHECK(s.oxygen == 1.0f);
        CHECK_EQ(s.uec, 14984695LL);
    }

    section("a piedi, frame vuoto, sezioni che mancano");
    {
        // A piedi arriva solo actor (come all'inizio della cattura: attore segnaposto, uec "0").
        ShipState s;
        Pb actor;
        actor.msg(1, Pb().msg(1, gauge(100, 100)).msg(2, gauge(1, 1)).f32(7, 60.0f)).msg(2, Pb().varint(1, 1)).msg(3, Pb().f32(1, 21.0f));
        CHECK(decode(Pb().msg(6, actor).b, s));
        CHECK(s.hasActor && !s.hasVehicle && !s.hasWeapons && !s.hasPower && !s.hasFlightState && !s.hasFlightSettings);
        CHECK(!s.hasInstruments && !s.hasShields && !s.hasMasterMode);
        CHECK(s.locomotion == Locomotion::OnFoot);
        CHECK(s.crewRole == CrewRole::Unknown);
        CHECK_STR(s.seatName, "");
        CHECK_EQ(s.uec, 0LL);
        CHECK_EQ(s.decoys, -1);
        CHECK_EQ(s.noise, -1);

        ShipState e;
        e.hasActor = true; // deve tornare tutto ai valori iniziali
        CHECK(ScTelemetry::decodeFrame(nullptr, 0, e));
        CHECK(!e.hasActor && !e.hasVehicle);

        // vehicle senza power/flight/shields: la nave c'è, le sezioni no.
        ShipState v;
        CHECK(decode(Pb().msg(3, Pb().msg(2, Pb().str(2, "RSI_Aurora_MR"))).b, v));
        CHECK(v.hasVehicle && !v.hasPower && !v.hasFlightState && !v.hasShields && !v.hasMasterMode);
        CHECK_STR(v.className, "RSI_Aurora_MR");
        // Un sotto-messaggio presente ma vuoto accende comunque la sua sezione.
        CHECK(decode(Pb().msg(3, Pb().msg(2, Pb().msg(9, Pb()).msg(8, Pb().msg(1, Pb())))).b, v));
        CHECK(v.hasPower && v.hasFlightState && !v.hasFlightSettings && !v.powerPlants);
    }

    section("campi sconosciuti saltati");
    {
        ShipState clean, noisy;
        CHECK(decode(gladiusFrame(), clean));
        // Tutti i tipi di campo, in cima e dentro i messaggi: varint, fixed64, length-delimited, fixed32, gruppo.
        Pb group;
        group.key(103, 3).varint(1, 7).f32(2, 1.5f).key(5, 3).varint(1, 1).key(5, 4).key(103, 4);
        Pb unknown;
        unknown.always(99, 123456789).f64(100, 0x1122334455667788ull).bytes(101, "patch futura").f32(102, 2.5f).raw(group.b);
        Pb vehicle = gladiusVehicle(); // e un altro vehicle.state (si unisce al primo) con solo campi nuovi, anche nelle impostazioni di volo
        vehicle.raw(unknown.b).msg(2, Pb().raw(unknown.b).msg(8, Pb().raw(unknown.b).msg(2, Pb().raw(unknown.b))));
        Pb weapons = gladiusWeapons();
        weapons.msg(4, Pb().str(1, "arma_futura").flag(2, true).always(9, 1).f32(10, 1.0f)); // una quarta arma, con campi nuovi
        Pb actor = gladiusActor();
        actor.raw(unknown.b).msg(2, Pb().raw(unknown.b)).msg(1, Pb().msg(1, Pb().raw(unknown.b)));
        Pb frame;
        frame.raw(unknown.b).msg(3, vehicle).msg(4, weapons).msg(5, Pb().msg(1, Pb().varint(1, 4))).raw(unknown.b).msg(6, actor)
            .msg(7, Pb().f32(4, 0.5f)).msg(8, Pb().flag(1, true)).always(1, 5).bytes(2, "x").raw(unknown.b);
        CHECK(decode(frame.b, noisy));
        CHECK_EQ(noisy.weaponCount, 4);
        CHECK_EQ(noisy.weaponsPowered, 4);
        noisy.weaponCount = noisy.weaponsPowered = 3;
        CHECK_STR(describe(noisy), describe(clean));

        // Un campo noto col tipo sbagliato (una patch che lo cambia) si salta come uno sconosciuto.
        ShipState w;
        CHECK(decode(Pb().msg(3, Pb().msg(2, Pb().always(2, 5).f32(5, 1.0f).str(1, "AEGS_Gladius_1").always(11, 7))).b, w));
        CHECK_STR(w.className, "");
        CHECK_STR(w.vehicleName, "AEGS_Gladius_1");
        CHECK(!w.lightsOn);
        CHECK(w.health == 0.0f);

        // Messaggi ripetuti: si uniscono, e per i valori semplici vince l'ultimo.
        ShipState r;
        CHECK(decode(Pb().msg(3, Pb().msg(2, Pb().str(2, "A").flag(5, true))).msg(3, Pb().msg(2, Pb().str(2, "B").msg(9, Pb().flag(2, true)))).b, r));
        CHECK_STR(r.className, "B");
        CHECK(r.lightsOn && r.hasPower && r.engines);
    }

    section("frame troncato o rotto");
    {
        // Tagliato in qualunque punto: è valido solo dove finisce un campo intero del messaggio esterno.
        std::vector<size_t> ends;
        const std::string frame = gladiusFrame(&ends);
        int wrong = 0;
        for (size_t n = 0; n < frame.size(); ++n) {
            ShipState s;
            const bool boundary = n == 0 || std::find(ends.begin(), ends.end(), n) != ends.end();
            if (decode(frame.substr(0, n), s) != boundary) wrong++;
        }
        CHECK_EQ(wrong, 0);
        CHECK(frame == gladiusCaptured());

        ShipState s;
        CHECK(!decode(std::string("\x08") + std::string(10, '\xFF') + "\x01", s)); // varint di 11 byte
        CHECK(decode(std::string("\x08") + std::string(9, '\xFF') + "\x01", s));   // 10 byte: va bene
        CHECK(!decode("\x0E\x01", s));                     // tipo 6: non esiste
        CHECK(!decode("\x0F\x01", s));                     // tipo 7: non esiste
        CHECK(!decode(std::string("\x00\x01", 2), s));     // campo 0
        CHECK(!decode("\x1A\x05\x01", s));                 // lunghezza oltre la fine
        CHECK(!decode("\x1A\xFF\xFF\xFF\xFF\x0F", s));     // lunghezza enorme
        CHECK(!decode("\x1C", s));                         // fine di un gruppo mai aperto
        CHECK(!decode("\x1B\x08\x01", s));                 // gruppo senza fine
        CHECK(!decode("\x1B\x24", s));                     // gruppo 3 chiuso come 4
        CHECK(!decode("\x15\x01\x02", s));                 // fixed32 a metà
        CHECK(!decode("\x11\x01\x02\x03\x04\x05\x06\x07", s)); // fixed64 a metà
        // Dentro un sotto-messaggio: la lunghezza esterna torna ma quella interna no.
        CHECK(!decode(Pb().bytes(3, std::string("\x12\x09", 2) + "AEGS").b, s));
        CHECK(!decode(Pb().msg(6, Pb().bytes(2, "\x08")).b, s)); // varint a metà nello stato dell'attore
        CHECK(!decode(Pb().msg(4, Pb().bytes(5, "\x10")).b, s));  // contromisura a metà
    }

    section("enum");
    {
        auto gearOf = [](uint64_t v) {
            ShipState s;
            decode(Pb().msg(3, Pb().msg(2, Pb().msg(8, Pb().msg(1, Pb().always(2, v).always(3, v))))).b, s);
            return s.gear;
        };
        CHECK(gearOf(0) == Deploy::Unknown);
        CHECK(gearOf(1) == Deploy::Retracted);
        CHECK(gearOf(2) == Deploy::Deploying);
        CHECK(gearOf(3) == Deploy::Deployed);
        CHECK(gearOf(4) == Deploy::Retracting);
        CHECK(gearOf(5) == Deploy::Unknown);
        CHECK(gearOf((uint64_t)-1) == Deploy::Unknown); // un enum negativo: varint da 10 byte
        auto modeOf = [](uint64_t v) {
            ShipState s;
            decode(Pb().msg(3, Pb().msg(2, Pb().msg(12, Pb().always(1, v)))).b, s);
            return s.masterMode;
        };
        CHECK(modeOf(0) == MasterMode::Unknown);
        CHECK(modeOf(1) == MasterMode::Navigation);
        CHECK(modeOf(2) == MasterMode::Scm);
        CHECK(modeOf(3) == MasterMode::Stealth);
        CHECK(modeOf(4) == MasterMode::Unknown);
        auto actorOf = [](uint64_t loc, uint64_t role) {
            ShipState s;
            decode(Pb().msg(6, Pb().msg(2, Pb().always(1, loc).always(2, role))).b, s);
            return s;
        };
        CHECK(actorOf(0, 0).locomotion == Locomotion::Unknown && actorOf(0, 0).crewRole == CrewRole::Unknown);
        CHECK(actorOf(1, 1).locomotion == Locomotion::OnFoot && actorOf(1, 1).crewRole == CrewRole::Pilot);
        CHECK(actorOf(2, 2).locomotion == Locomotion::Eva && actorOf(2, 2).crewRole == CrewRole::Gunner);
        CHECK(actorOf(3, 3).locomotion == Locomotion::Seated && actorOf(3, 3).crewRole == CrewRole::Crew);
        CHECK(actorOf(9, 9).locomotion == Locomotion::Unknown && actorOf(9, 9).crewRole == CrewRole::Unknown);
    }

    section("armi, contromisure, aUEC");
    {
        ShipState s;
        // Armi senza contromisure: -1 (non arrivate). Due armi su quattro alimentate.
        Pb w;
        w.msg(4, Pb().flag(2, true)).msg(4, Pb()).msg(4, Pb().flag(2, true)).msg(4, Pb().flag(3, true));
        CHECK(decode(Pb().msg(4, w).b, s));
        CHECK(s.hasWeapons && !s.gimbalsLocked);
        CHECK_EQ(s.weaponCount, 4);
        CHECK_EQ(s.weaponsPowered, 2);
        CHECK_EQ(s.decoys, -1);
        CHECK_EQ(s.noise, -1);
        // Il primo frame in nave: DECOY con quantità 0 (non scritta) -> 0; NOISE non c'è -> -1.
        CHECK(decode(Pb().msg(4, Pb().msg(5, Pb().varint(1, 1))).b, s));
        CHECK_EQ(s.decoys, 0);
        CHECK_EQ(s.noise, -1);
        // Due lanciatori dello stesso tipo si sommano; un tipo sconosciuto (o UNSPECIFIED) si ignora.
        CHECK(decode(Pb().msg(4, Pb().msg(5, Pb().varint(1, 2).varint(2, 3)).msg(5, Pb().varint(1, 2).varint(2, 4)).msg(5, Pb().varint(2, 9)).msg(5, Pb().varint(1, 7).varint(2, 9))).b, s));
        CHECK_EQ(s.decoys, -1);
        CHECK_EQ(s.noise, 7);
        // Armi presenti ma vuote: zero armi.
        CHECK(decode(Pb().msg(4, Pb()).b, s));
        CHECK(s.hasWeapons);
        CHECK_EQ(s.weaponCount, 0);
        // uec_balance è int64: anche oltre i 4 miliardi, e negativo (varint da 10 byte).
        CHECK(decode(Pb().msg(6, Pb().msg(2, Pb().varint(4, 5000000000ull))).b, s));
        CHECK_EQ(s.uec, 5000000000LL);
        CHECK(decode(Pb().msg(6, Pb().msg(2, Pb().varint(4, (uint64_t)-25))).b, s));
        CHECK_EQ(s.uec, -25LL);
    }

    section("HPACK della richiesta");
    {
        // Letterale senza indicizzazione, nome nuovo: 0x00, lunghezza del nome, nome, lunghezza del valore, valore.
        auto lit = [](const std::string& name, const std::string& value) {
            return std::string(1, '\0') + (char)name.size() + name + (char)value.size() + value;
        };
        const std::string want = lit(":method", "POST") + lit(":scheme", "http") + lit(":path", "/sc.game.telemetry.v1.GameTelemetryService/TelemetryStream") +
                                 lit(":authority", "127.0.0.1:2942") + lit("content-type", "application/grpc") + lit("te", "trailers");
        CHECK(detail::requestHeaderBlock(2942) == want);
        CHECK(detail::requestHeaderBlock(29420).find(lit(":authority", "127.0.0.1:29420")) != std::string::npos);
        for (unsigned char c : detail::requestHeaderBlock(2942))
            if (c >= 0x80) { // niente Huffman (bit alto delle lunghezze) e niente byte strani
                CHECK(false);
                break;
            }
    }

    section("apertura HTTP/2");
    {
        const std::string open = detail::clientOpening(2942);
        const std::string preface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
        CHECK(open.compare(0, preface.size(), preface) == 0);
        const auto frames = h2Frames(open, preface.size());
        CHECK_EQ(frames.size(), (size_t)4);
        if (frames.size() == 4) {
            // SETTINGS: ENABLE_PUSH = 0 e INITIAL_WINDOW_SIZE grande.
            CHECK(frames[0].type == 4 && frames[0].stream == 0 && frames[0].flags == 0 && frames[0].payload.size() % 6 == 0);
            uint32_t push = 99, window = 0;
            for (size_t i = 0; i + 6 <= frames[0].payload.size(); i += 6) {
                const int id = ((uint8_t)frames[0].payload[i] << 8) | (uint8_t)frames[0].payload[i + 1];
                if (id == 2) push = be32(frames[0].payload, i + 2);
                if (id == 4) window = be32(frames[0].payload, i + 2);
            }
            CHECK_EQ(push, 0u);
            CHECK(window >= (1u << 20));
            // WINDOW_UPDATE della connessione: porta anche lei la finestra a quella degli stream.
            CHECK(frames[1].type == 8 && frames[1].stream == 0 && frames[1].payload.size() == 4);
            CHECK_EQ(be32(frames[1].payload, 0) + 65535u, window);
            // HEADERS sullo stream 1, intero (END_HEADERS), senza END_STREAM.
            CHECK(frames[2].type == 1 && frames[2].stream == 1 && frames[2].flags == 0x4);
            CHECK(frames[2].payload == detail::requestHeaderBlock(2942));
            // DATA: il messaggio gRPC vuoto, e la richiesta finisce (END_STREAM).
            CHECK(frames[3].type == 0 && frames[3].stream == 1 && frames[3].flags == 0x1);
            CHECK(frames[3].payload == std::string(5, '\0'));
        }
    }

    section("riassemblaggio gRPC");
    {
        const std::string frame = gladiusFrame();
        const std::string stream = grpcMessage(frame) + grpcMessage("") + grpcMessage("zzz", 1) + grpcMessage(std::string(50, 'x')) + grpcMessage("ab");
        // A pezzi di tutte le misure, anche un byte alla volta: sempre gli stessi messaggi.
        int wrong = 0;
        for (size_t piece : { (size_t)1, (size_t)2, (size_t)3, (size_t)5, (size_t)7, (size_t)64, (size_t)1000, stream.size() }) {
            detail::GrpcReassembler g;
            g.maxMessage = 40; // la Gladius e il messaggio da 50 sono "troppo grandi": saltati
            std::vector<std::string> got;
            for (size_t i = 0; i < stream.size(); i += piece)
                g.feed((const uint8_t*)stream.data() + i, std::min(piece, stream.size() - i), got);
            if (!(got.size() == 2 && got[0].empty() && got[1] == "ab" && g.skipped() == 3 && !g.midMessage())) wrong++;
        }
        CHECK_EQ(wrong, 0);
        detail::GrpcReassembler g;
        std::vector<std::string> got;
        for (size_t i = 0; i < stream.size(); i += 3) g.feed((const uint8_t*)stream.data() + i, std::min<size_t>(3, stream.size() - i), got);
        CHECK_EQ(got.size(), (size_t)4);
        CHECK_EQ(g.skipped(), 1); // solo il compresso
        if (got.size() == 4) {
            CHECK(got[0] == frame);
            CHECK(got[1].empty());
            CHECK(got[2] == std::string(50, 'x'));
            CHECK(got[3] == "ab");
        }
        // A metà di un messaggio lo sa.
        detail::GrpcReassembler half;
        std::vector<std::string> none;
        half.feed((const uint8_t*)stream.data(), 7, none);
        CHECK(none.empty() && half.midMessage());
    }

    section("HTTP/2 in arrivo: SETTINGS, PING, DATA a pezzi");
    {
        using detail::Http2Receiver;
        Http2Receiver rx;
        // SETTINGS del server -> ACK; il suo ACK ai nostri -> niente.
        CHECK(feed(rx, h2(4, 0, 0, std::string("\x00\x03\x00\x00\x00\x64\x00\x05\x00\x00\x40\x00", 12)) + h2(4, 1, 0, "")));
        auto out = h2Frames(rx.reply);
        CHECK(out.size() == 1 && out[0].type == 4 && out[0].flags == 1 && out[0].payload.empty() && out[0].stream == 0);
        rx.reply.clear();
        // PING -> PING con ACK e gli stessi 8 byte; un PING che è già un ACK -> niente.
        CHECK(feed(rx, h2(6, 0, 0, "12345678") + h2(6, 1, 0, "abcdefgh")));
        out = h2Frames(rx.reply);
        CHECK(out.size() == 1 && out[0].type == 6 && out[0].flags == 1 && out[0].payload == "12345678");
        rx.reply.clear();
        // Le intestazioni della risposta (PADDED + PRIORITY, divise in HEADERS + CONTINUATION): ignorate.
        CHECK(feed(rx, h2(1, 0x8 | 0x20, 1, "\x02\x00\x00\x00\x00\x10\x88\x5f\x00\x00"s) + h2(9, 0x4, 1, "\x40\x01")));
        CHECK(rx.reply.empty() && rx.messages.empty());
        // Un messaggio (la Gladius) spezzato su tre DATA, il secondo con riempimento, dati un byte alla volta.
        const std::string message = grpcMessage(gladiusFrame());
        const size_t a = 100, b = 400;
        const std::string padded = std::string(1, (char)10) + message.substr(a, b - a) + std::string(10, '\0');
        const std::string wire = h2(0, 0, 1, message.substr(0, a)) + h2(0, 0x8, 1, padded) + h2(0, 0, 1, message.substr(b));
        bool open = true;
        for (char c : wire) open = open && rx.feed((const uint8_t*)&c, 1);
        CHECK(open);
        CHECK_EQ(rx.messages.size(), (size_t)1);
        if (rx.messages.size() == 1) {
            ShipState s;
            CHECK(decode(rx.messages[0], s));
            CHECK_STR(s.className, "AEGS_Gladius");
        }
        // La finestra: tornano indietro tutti i byte dei DATA, riempimento compreso, per stream e connessione.
        const uint64_t dataBytes = message.size() + 1 + 10;
        CHECK_EQ(windowCredit(rx.reply, 0), dataBytes);
        CHECK_EQ(windowCredit(rx.reply, 1), dataBytes);
        rx.messages.clear();
        rx.reply.clear();
        // Due messaggi nello stesso DATA, e uno compresso (saltato) in mezzo.
        CHECK(feed(rx, h2(0, 0, 1, grpcMessage("uno") + grpcMessage("x", 1) + grpcMessage("due"))));
        CHECK(rx.messages.size() == 2 && rx.messages[0] == "uno" && rx.messages[1] == "due");
        CHECK_EQ(rx.skippedMessages(), 1);
        // Tipi sconosciuti, PRIORITY e WINDOW_UPDATE: ignorati.
        CHECK(feed(rx, h2(0xFA, 0xFF, 7, "boh") + h2(2, 0, 1, "\x80\x00\x00\x00\x10"s) + h2(8, 0, 0, "\x00\x00\x10\x00"s)));
        // DATA vuoto: nessun WINDOW_UPDATE (un incremento 0 sarebbe un errore).
        rx.reply.clear();
        CHECK(feed(rx, h2(0, 0, 1, "")));
        CHECK(rx.reply.empty());
    }

    section("HTTP/2 in arrivo: fine del flusso ed errori");
    {
        using detail::Http2Receiver;
        const std::string msg = grpcMessage("ciao");
        auto closes = [](const std::string& bytes, size_t wantMessages = 0) {
            Http2Receiver rx;
            const bool open = feed(rx, bytes);
            const bool ok = !open && !rx.closeReason.empty() && rx.messages.size() == wantMessages && !feed(rx, std::string(9, '\0'));
            return ok;
        };
        CHECK(closes(h2(0, 0x1, 1, msg), 1));                                  // DATA con END_STREAM: il messaggio arriva lo stesso
        CHECK(closes(h2(0, 0, 1, msg) + h2(1, 0x5, 1, "\x00\x01\x41\x01\x42"s), 1)); // trailer (grpc-status) con END_STREAM
        CHECK(closes(h2(1, 0x1, 1, "\x88") + h2(9, 0x4, 1, "\x40")));           // trailer diviso: si chiude a END_HEADERS
        CHECK(closes(h2(3, 0, 1, std::string("\x00\x00\x00\x08", 4))));           // RST_STREAM (CANCEL)
        CHECK(closes(h2(7, 0, 0, std::string("\x00\x00\x00\x01\x00\x00\x00\x00", 8)))); // GOAWAY
        CHECK(closes(h2(0, 0, 1, std::string(16385, 'x'))));                    // oltre la dimensione massima del frame
        CHECK(closes(std::string("\x00\x40\x01\x00\x00\x00\x00\x00\x01", 9)));  // ...anche solo dall'intestazione
        CHECK(closes(h2(0, 0x8, 1, std::string("\x05") + "abcd")));              // riempimento più lungo del frame
        CHECK(closes(h2(1, 0x8 | 0x4, 1, std::string("\x09") + "abc")));         // idem in HEADERS
        CHECK(closes(h2(1, 0x20 | 0x4, 1, "abc")));                             // PRIORITY più corta di 5 byte
        CHECK(closes(h2(0, 0, 0, msg)));                                        // DATA sullo stream 0
        CHECK(closes(h2(5, 0x4, 1, std::string("\x00\x00\x00\x02", 4))));        // PUSH_PROMISE col push spento
        CHECK(closes(h2(1, 0, 1, "\x88") + h2(0, 0, 1, msg)));                   // HEADERS senza CONTINUATION
        CHECK(closes(h2(9, 0x4, 1, "\x88")));                                   // CONTINUATION senza HEADERS
        CHECK(closes(h2(6, 0, 0, "1234")));                                     // PING corto
        CHECK(closes(h2(4, 0, 0, "12345")));                                    // SETTINGS non multiplo di 6
        CHECK(closes(std::string("HTTP/1.1 400 Bad Request\r\n\r\n")));         // non è HTTP/2
        // Le intestazioni normali (senza END_STREAM) e un RST_STREAM di un altro stream non chiudono.
        Http2Receiver rx;
        CHECK(feed(rx, h2(1, 0x4, 1, "\x88") + h2(3, 0, 3, std::string("\x00\x00\x00\x08", 4)) + h2(0, 0, 1, msg)));
        CHECK_EQ(rx.messages.size(), (size_t)1);
    }

    section("frame congelati (menu)");
    {
        detail::FreezeWatch w(2000);
        CHECK(!w.frozen());
        // In volo i frame cambiano sempre: mai congelato.
        for (int i = 0; i < 100; ++i) w.add("frame " + std::to_string(i), i * 50);
        CHECK(!w.frozen());
        // Al menu lo stesso frame, byte per byte: dopo 2 s non vale più.
        for (int t = 5000; t < 7000; t += 50) w.add("menu", t);
        CHECK(!w.frozen()); // 1,95 s
        w.add("menu", 7000);
        CHECK(w.frozen());
        w.add("menu", 9000);
        CHECK(w.frozen());
        // Il primo frame diverso (la partita dopo) sblocca subito.
        w.add("in gioco", 9050);
        CHECK(!w.frozen());
        // Identici per 1 s (in gioco capita): ancora buono.
        for (int t = 9100; t <= 10050; t += 50) w.add("in gioco", t);
        CHECK(!w.frozen());
        w.reset();
        CHECK(!w.frozen());
        w.add("x", 0);
        w.add("x", 1999);
        CHECK(!w.frozen());
    }

    section("client fermo");
    {
        Client c; // senza start(): niente rete, niente thread
        CHECK(!c.running());
        CHECK(!c.connected());
        CHECK(!c.current().has_value());
        CHECK_EQ(c.frameCount(), (uint64_t)0);
        CHECK_STR(c.statusText(), "Non attiva");
        c.stop(); // fermare un client mai partito non fa niente
        CHECK(!c.running());
    }
}

// iris_tests --telemetria [porta] [secondi]: si collega al gioco (o a un server finto) e stampa lo
// stato letto, secondo per secondo. Solo lettura.
static int telemetryMode(int argc, char** argv) {
    const int port = argc > 2 ? std::atoi(argv[2]) : 2942;
    const int seconds = argc > 3 ? std::atoi(argv[3]) : 10;
    std::printf("Telemetria di Star Citizen da 127.0.0.1:%d per %d s (solo lettura)\n", port, seconds);
    ScTelemetry::Client client;
    client.start(port);
    for (int i = 1; i <= seconds; ++i) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        std::printf("[%3d s] %s | frame %llu%s\n", i, client.statusText().c_str(), (unsigned long long)client.frameCount(),
                    client.connected() ? "" : " | non collegata");
        if (const auto s = client.current()) std::printf("%s", telemetry_test::describe(*s).c_str());
    }
    const auto t0 = std::chrono::steady_clock::now();
    client.stop();
    const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    std::printf("stop() in %lld ms\n", ms);
    return client.frameCount() > 0 ? 0 : 1;
}

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, nullptr, _IONBF, 0); // se una prova si schianta, quello che è già scritto resta
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    // Le prove non toccano le impostazioni e la memoria di tutti i giorni.
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    AppPaths::setDataDirOverride(std::wstring(tmp) + L"IrisTests");
    AppPaths::ensureDirectoriesExist();

    const std::string mode = argc > 1 ? argv[1] : "";
    const std::string arg = argc > 2 ? argv[2] : "";
    FakeHost host;
    Tools tools(host);

    if (mode == "--telemetria") return telemetryMode(argc, argv);
    if (mode == "--mining" && !arg.empty()) {
        // iris_tests --mining "2 16,960" : cosa capisce la decodifica da un testo dell'OCR (o un numero).
        const auto d = Mining::decodeText(arg, Mining::defaultMaterials());
        std::printf("%s\n", Mining::summary(d).c_str());
        if (!d.note.empty()) std::printf("  nota: %s\n", d.note.c_str());
        return 0;
    }

    if (mode == "--comando" && !arg.empty()) {
        // iris_tests --comando "frase" [star_citizen.json] : che comando di gioco capisce Iris (senza
        // premere niente). Con un profilo: quello, con i predefiniti nuovi aggiunti in memoria (il
        // file non si tocca).
        GameProfile p = GameProfile::defaults();
        if (argc > 3) {
            std::ifstream f(TextUtils::utf8ToWide(argv[3]), std::ios::binary);
            std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            if (TextUtils::startsWith(text, "\xEF\xBB\xBF")) text.erase(0, 3);
            std::string error;
            if (!GameProfile::parse(text, p, error)) {
                std::printf("Profilo non valido: %s\n", error.c_str());
                return 1;
            }
            if (p.defaultsVersion < GameProfile::kDefaultsVersion) std::printf("(predefiniti nuovi aggiunti in memoria: %d)\n", p.addMissingDefaults());
        }
        const Cmd::Match m = Cmd::match(p, arg);
        std::printf("%s  \"%s\" -> %s, frase \"%s\", %.0f%%", m.found ? "CAPITO" : "non capito", arg.c_str(),
                    m.command.intent.empty() ? "-" : m.command.id().c_str(), m.phrase.c_str(), m.score * 100);
        for (const auto& [k, v] : m.slots) std::printf(", %s = \"%s\"", k.c_str(), v.c_str());
        std::printf("\n");
        return 0;
    }

    if (mode == "--ocr-mappa" && !arg.empty()) {
        // iris_tests --ocr-mappa immagine.png ["testo atteso"] [lingua] : cosa legge l'OCR della mappa
        // stellare con ogni preparazione dell'immagine (le catture di %appdata%\Iris\debug, in PNG).
        const ScreenCapture::Image img = MiningImage::loadPng(TextUtils::utf8ToWide(arg));
        if (img.empty()) {
            std::printf("Non leggo l'immagine (serve un PNG).\n");
            return 1;
        }
        const std::string wanted = argc > 3 ? argv[3] : "";
        OcrSettings o;
        o.language = argc > 4 ? argv[4] : "en";
        if (argc > 5) o.upscale = std::atoi(argv[5]);
        o.saveDebugShots = false;
        const RECT r{ 0, 0, img.width, img.height };
        ScreenOcr::readImage(img, r, o, "", [&](const ScreenOcr::Result& x) {
            const std::string t = TextUtils::collapseSpaces(ScreenOcr::linesText(x));
            std::printf("  %-26s \"%s\"", x.variant.c_str(), t.c_str());
            if (!wanted.empty()) std::printf("   somiglianza %.2f", Cmd::ocrContains(t, wanted));
            std::printf("\n");
            return false;
        });
        return 0;
    }

    if (mode == "--mining-db" && !arg.empty()) {
        // iris_tests --mining-db materialsdb : la tabella di Mining Signature Calculator, in chiaro.
        std::ifstream f(TextUtils::utf8ToWide(arg), std::ios::binary);
        std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        std::string csv;
        if (!Mining::decodeMscDatabase(bytes, csv)) {
            std::printf("Non è un materialsdb di Mining Signature Calculator.\n");
            return 1;
        }
        std::printf("%s\n", csv.c_str());
        return 0;
    }

    if (mode == "--mining-immagini" && !arg.empty()) {
        // iris_tests --mining-immagini <cartella logs\debug di Mining Signature Calculator> [max]
        // Rilegge le catture salvate da MSC (a sinistra l'originale ×3) e confronta con il valore che aveva letto MSC.
        const std::wstring dir = TextUtils::utf8ToWide(arg);
        const int limit = argc > 3 ? std::atoi(argv[3]) : 100000;
        const auto mats = Mining::defaultMaterials();
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + L"\\*__val=*.png").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) {
            std::printf("Nessuna immagine *__val=*.png in %s\n", arg.c_str());
            return 1;
        }
        int total = 0, same = 0, differ = 0, oursOnly = 0, mscOnly = 0, none = 0, ourKnown = 0, mscKnown = 0;
        long long ms = 0;
        do {
            if (total >= limit) break;
            const std::wstring name = fd.cFileName;
            const size_t p = name.find(L"__val=");
            const std::wstring mscText = name.substr(p + 6, name.size() - p - 6 - 4);
            const int mscValue = mscText == L"NESSUN_NUMERO" ? 0 : _wtoi(mscText.c_str());
            const ScreenCapture::Image composite = MiningImage::loadPng(dir + L"\\" + name);
            if (composite.empty()) continue;
            // Larghezza = 3w + 8 + 3w, altezza = 3h + 32 (vedi _save_debug_image di MSC).
            const int ow = (composite.width - 8) / 6, oh = (composite.height - 32) / 3;
            if (ow < 8 || oh < 8) continue;
            ScreenCapture::Image original;
            original.width = ow;
            original.height = oh;
            original.pixels.resize((size_t)ow * oh * 4);
            for (int y = 0; y < oh; ++y) {
                for (int x = 0; x < ow; ++x) {
                    std::memcpy(&original.pixels[((size_t)y * ow + x) * 4], &composite.pixels[((size_t)(y * 3 + 1) * composite.width + x * 3 + 1) * 4], 4);
                }
            }
            const auto r = MiningScanner::readImage(original, mats, 40, {});
            ++total;
            ms += r.durationMs;
            const int ours = r.decoded.hasValue ? r.decoded.value : 0;
            const bool oursKnown = !r.decoded.matches.empty();
            const bool mscIsKnown = mscValue > 0 && !Mining::findMatches(mscValue, mats).empty();
            ourKnown += oursKnown ? 1 : 0;
            mscKnown += mscIsKnown ? 1 : 0;
            if (ours && mscValue && ours == mscValue) ++same;
            else if (ours && mscValue) {
                ++differ;
                std::printf("  diverso: %ls  MSC %d%s  Iris %s  [OCR \"%s\"]\n", name.c_str(), mscValue, mscIsKnown ? "" : " (sconosciuto)",
                            Mining::summary(r.decoded).c_str(), r.ocrText.c_str());
            } else if (ours) {
                ++oursOnly;
                std::printf("  solo Iris: %ls  -> %s  [OCR \"%s\"]\n", name.c_str(), Mining::summary(r.decoded).c_str(), r.ocrText.c_str());
            } else if (mscValue) {
                ++mscOnly;
                std::printf("  solo MSC: %ls  (Iris: %s, OCR \"%s\")\n", name.c_str(), r.error.c_str(), r.ocrText.c_str());
            } else {
                ++none;
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        std::printf("\n%d immagini: stesso valore %d, diverso %d, letto solo da Iris %d, letto solo da MSC %d, nessuno dei due %d\n", total,
                    same, differ, oursOnly, mscOnly, none);
        std::printf("Valori che tornano con la tabella: Iris %d, MSC %d. Tempo medio Iris: %lld ms\n", ourKnown, mscKnown, total ? ms / total : 0);
        return 0;
    }

    if (mode == "--ruota" && argc >= 3) {
        // iris_tests --ruota cartella : la ruota e i pannelli disegnati in PNG (stesso codice dello schermo).
        Ui::initialize();
        QuickBar::create(GetModuleHandleW(nullptr), {});
        QuickBar::setButtons({ { "mining", L"MINING", L"Leggi adesso la signature dello scanner", false, true, L"LEGGI ORA", 6 },
                               { "record", L"REGISTRO", L"Registro acceso (4 rilevamenti): clic per chiuderlo e salvarlo sul desktop", true, true,
                                 L"IN CORSO \u00B7 4", 1 },
                               { "refinery", L"RAFFINERIA", L"Gli ordini della raffineria con il conto alla rovescia", true, true, L"APERTO", 1 },
                               { "missions", L"MISSIONI", L"Contratti e obiettivi in corso (dal Game.log)", false, true, L"CHIUSO", 4 },
                               { "log", L"DIARIO", L"Attiva prima il diario di bordo nella finestra di Iris", false, false, L"DIARIO SPENTO", 2 },
                               { "chat", L"TRADUZIONE", L"Traduzione della chat accesa: clic per spegnerla", true, true, L"ACCESA", 1 },
                               { "axis", L"ASSI", L"Apri l'overlay dell'Axis Visualizer", false, true, L"CHIUSO", 4 },
                               { "orb", L"SFERA", L"Nascondi la sfera di Iris", true, true, L"VISIBILE", 1 },
                               { "iris", L"IRIS", L"Apri la finestra di Iris: impostazioni, mining, traduzione, diario", false, true, L"APRI", 6 } },
                             L"SHIFT+TAB", L"TI ASCOLTO", 1);
        QuickBar::setPanel("missions", L"MISSIONI",
                           { { L"IN CORSO (2)", L"", 5 }, { L"Mese Scoperta RSI: Liv. Giallo - Supporta Nave", L"15:21", 0 },
                             { L"Mese Scoperta RSI: Importante Trasporto Forniture", L"15:51", 0 }, { L"OBIETTIVI", L"", 5 },
                             { L"Vai alle Coordinate di Pattuglia", L"", 6 }, { L"SESSIONE", L"", 5 }, { L"Completati 2   \u00B7   Falliti 1", L"", 2 } });
        QuickBar::setPanel("refinery", L"RAFFINERIA",
                           { { L"ARC-L1 \u00B7 Quantanium, 32 SCU", L"2 h 16 min", 0, 0.33 }, { L"Levski", L"PRONTO", 1, 1.0 } });
        QuickBar::setPanel("log", L"DIARIO DI BORDO",
                           { { L"STATO", L"", 5 }, { L"Seraphim Station", L"Crusader Industries", 0 }, { L"Fuori dall'armistizio", L"", 2 },
                             { L"Spazio monitorato", L"", 1 }, { L"Stato: disidratato", L"16:14", 2 }, { L"ULTIMI EVENTI", L"", 5 },
                             { L"Contratto completato: Mese Scoperta RSI: Ordine UCM (S)", L"16:20", 1 },
                             { L"Wingman_77 \u00E8 in quantum verso di te", L"16:18", 0 },
                             { L"Contratto fallito: Liv. Arancione - Difendi Nave", L"16:09", 3 } });
        const bool ok = QuickBar::savePreview(TextUtils::utf8ToWide(argv[2]), 5);
        QuickBar::destroy();
        Ui::shutdown();
        std::printf("%s\n", ok ? "ruota e pannelli salvati" : "salvataggio non riuscito");
        return ok ? 0 : 1;
    }
    if (mode == "--sfera" && argc >= 3) {
        // iris_tests --sfera uscita.png : un fotogramma della sfera fluttuante, disegnato come nella sua
        // finestra (32 bit con alfa premoltiplicato), poi messo su una scacchiera per vedere la trasparenza.
        Ui::initialize();
        const int side = 320;
        int transparentCorners = 0;
        {
            Gdiplus::Bitmap frame(side, side, PixelFormat32bppPARGB);
            {
                Gdiplus::Graphics g(&frame);
                g.Clear(Gdiplus::Color(0, 0, 0, 0));
                g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                Orb::Animation a;
                for (int i = 0; i < 40; ++i) { // un po' di passi: colore ed energia arrivano a regime
                    Orb::step(a, Orb::Mode::Listening, 0.05f, 0.0f, true);
                    Sleep(5);
                }
                Orb::draw(g, (float)side, (float)side, a, 0.21f);
            }
            for (POINT p : { POINT{ 2, 2 }, POINT{ side - 3, 2 }, POINT{ 2, side - 3 }, POINT{ side - 3, side - 3 } }) {
                Gdiplus::Color c;
                frame.GetPixel(p.x, p.y, &c);
                if (c.GetA() == 0) transparentCorners++;
            }
            Gdiplus::Bitmap out(side, side, PixelFormat32bppARGB);
            {
                Gdiplus::Graphics g(&out);
                for (int y = 0; y < side; y += 16) {
                    for (int x = 0; x < side; x += 16) {
                        Gdiplus::SolidBrush b(((x + y) / 16) % 2 ? Gdiplus::Color(255, 200, 200, 200) : Gdiplus::Color(255, 120, 120, 120));
                        g.FillRectangle(&b, x, y, 16, 16);
                    }
                }
                g.DrawImage(&frame, 0, 0, side, side);
            }
            CLSID png{};
            UINT n = 0, bytes = 0;
            Gdiplus::GetImageEncodersSize(&n, &bytes);
            std::vector<BYTE> buf(bytes);
            auto* codecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buf.data());
            Gdiplus::GetImageEncoders(n, bytes, codecs);
            for (UINT i = 0; i < n; ++i) {
                if (std::wstring(codecs[i].MimeType) == L"image/png") png = codecs[i].Clsid;
            }
            out.Save(TextUtils::utf8ToWide(argv[2]).c_str(), &png, nullptr);
        }
        std::printf("Sfera disegnata: angoli trasparenti %d su 4\n", transparentCorners);
        Ui::shutdown();
        return transparentCorners == 4 ? 0 : 1;
    }
    if (mode == "--traduci" && !arg.empty()) {
        // iris_tests --traduci "frase" ... : la traduzione della chat di gioco con il modello di Iris
        // (canale voce, italiano -> inglese, gergo da videogiocatore), una domanda a Iris in mezzo
        // (stesso modello, contesto suo) e una riga di chat letta (canale schermo, inglese -> italiano).
        LlmEngine llm;
        if (!loadLlm(llm)) return 1;
        auto voice = [&](const std::string& text) {
            TranslationRequest r;
            r.text = text;
            r.sourceLanguage = "Italian";
            r.targetLanguage = "English";
            r.gamingTone = true;
            r.glossary = GamerSlang::forSending(text, r.sourceLanguage, true);
            for (const auto* list : { &scKnownLocations(), &scKnownShips() }) {
                for (const auto& name : *list) {
                    if (TextUtils::containsWord(text, name)) r.keepNames.push_back(name);
                }
            }
            std::string out;
            const auto t0 = std::chrono::steady_clock::now();
            const auto res = llm.translate(r, out, LlmEngine::Channel::Voice);
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
            std::printf("  VOCE   (%s, %lld ms): %s  ->  %s\n", res == LlmEngine::Result::Ok ? "ok" : "ERRORE", (long long)ms, text.c_str(),
                        TextUtils::fixShoutingCase(out).c_str());
        };
        for (int i = 2; i < argc; ++i) voice(argv[i]);
        // Iris usa lo stesso modello nel frattempo.
        Agent agent(llm, tools);
        agent.setPersona({});
        std::printf("\n  IRIS in mezzo:\n");
        askOnce(agent, "quanto fa 12 per 7?");
        // Riga letta dalla chat degli altri (canale schermo).
        TranslationRequest screen;
        screen.text = "omw to Port Tressler, ty for the help";
        screen.sourceLanguage = "English";
        screen.targetLanguage = "Italian";
        screen.gamingTone = false;
        screen.fromScreen = true;
        screen.keepNames = { "Port Tressler" };
        std::string out;
        const auto res = llm.translate(screen, out, LlmEngine::Channel::Screen);
        std::printf("\n  SCHERMO (%s): %s  ->  %s\n", res == LlmEngine::Result::Ok ? "ok" : "ERRORE", screen.text.c_str(),
                    GamerSlang::explain(out, "Italian").c_str());
        // E di nuovo la voce: il prefisso del suo canale è ancora in memoria (veloce).
        std::printf("\n  Di nuovo la voce, dopo Iris e lo schermo:\n");
        voice(argv[2]);
        return 0;
    }
    if (mode == "--ppocr") return comparePpOcr();
    if (mode == "--aggiornamento") {
        // Cosa vedrebbe l'aggiornamento automatico su GitHub (solo lettura: non scarica niente).
        Updater::Release r;
        std::string error;
        if (!Updater::fetchLatest(r, error, nullptr)) {
            std::printf("Controllo non riuscito: %s\n", error.c_str());
            return 1;
        }
        const int cmp = Updater::compareVersions(r.version, Updater::currentVersion());
        std::printf("Questa versione:   %s\nUltima pubblicata: %s (%s)\nFile: %s\n      %lld byte, SHA-256 %s\n%s\n", Updater::currentVersion().c_str(),
                    r.version.c_str(), r.tag.c_str(), r.assetUrl.c_str(), r.size, r.sha256.empty() ? "(non data da GitHub)" : r.sha256.c_str(),
                    cmp > 0 ? "-> Iris la scaricherebbe e la installerebbe." : "-> Niente da fare: questa è uguale o più nuova.");
        if (arg == "scarica") {
            // Prova del download controllato (nella cartella delle prove, poi si cancella).
            std::wstring path;
            const bool ok = Updater::download(r, [](long long done, long long total) {
                if (total > 0 && done == total) std::printf("  scaricati %lld byte\n", done);
            }, nullptr, path, error);
            std::printf("Download: %s%s\n", ok ? "ok, impronta controllata" : "NON riuscito: ", ok ? "" : error.c_str());
            if (ok) {
                std::string hex;
                Updater::sha256File(path, hex);
                std::printf("  %s\n  SHA-256 %s\n", TextUtils::wideToUtf8(path).c_str(), hex.c_str());
                DeleteFileW(path.c_str());
                DeleteFileW((path + L".ok").c_str());
            }
            return ok ? 0 : 1;
        }
        return 0;
    }
    if ((mode == "--chiedi" || mode == "--chiedi-breve") && !arg.empty()) {
        // --chiedi-breve: come Iris con le risposte brevi (mentre giochi).
        LlmEngine llm;
        if (!loadLlm(llm)) return 1;
        Agent agent(llm, tools);
        Agent::Persona persona;
        persona.brief = mode == "--chiedi-breve";
        agent.setPersona(persona);
        for (int i = 2; i < argc; ++i) {
            std::printf("\nDOMANDA: %s\n", argv[i]);
            askOnce(agent, argv[i]);
        }
        return 0;
    }
    if (mode == "--prova-domande") {
        // iris_tests --prova-domande [breve] : un giro di frasi vere (dal registro di chi gioca), col gioco
        // "aperto" (profilo predefinito, niente tasti premuti): prima il dizionario dei comandi, se no il
        // modello. Per ognuna: tempo alla prima frase (o al primo strumento), strumenti, risposta.
        // Come in Iris: il modello si prepara all'avvio e mentre parli (tempo a parte, non si aspetta),
        // e i comandi istantanei entrano nella conversazione. "senza-preparazione" (terzo argomento): come prima.
        const bool brief = arg == "breve";
        const bool prepare = !(argc > 3 && std::string(argv[3]) == "senza-preparazione");
        GameController game;
        game.load(); // cartella delle prove: profilo predefinito
        const GameProfile profile = game.profile();
        GameFakeHost gameHost(game.toolDefinition());
        Tools gameTools(gameHost);
        LlmEngine llm;
        if (!loadLlm(llm)) return 1;
        Agent agent(llm, gameTools);
        Agent::Persona persona;
        persona.userName = "REVO";
        persona.game = profile.name;
        persona.brief = brief;
        agent.setPersona(persona);
        auto elapsed = [](std::chrono::steady_clock::time_point t) {
            return (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t).count();
        };
        if (prepare) {
            const auto tw = std::chrono::steady_clock::now();
            agent.warmUp(nullptr);
            std::printf("  Preparazione all'avvio (istruzioni e strumenti): %lld ms\n", elapsed(tw));
        }
        const char* const script[] = {
            "prepara la nave",
            "rotta per Crusader",
            "È attiva.",
            "alza gli scudi e accendi i motori",
            "Spegni le cose, come si chiamano? Spegni i motori.",
            "Apri l'hangar.",
            "Mi posso una rotta per Seraphim?",
            "Riprova!",
            "Alzati dal posto di guida.",
            "Disattiva la modalità Capoled.",
            "Chiamala TC.",
            "Ricordati che il mio wingman si chiama Luca",
            "Come si chiama il mio wingman?",
            "Che cos'è il quantanium?",
            "Qual è il tempo domani a Bagnarola?",
            "Apri Facebook",
            "Metti un timer di 5 minuti",
            "Ehm.",
            "Poche volte.",
            "Quanto fa 12 per 7?",
            "Come si atterra in una stazione?",
            "Grazie",
        };
        long long totalFirst = 0;
        int counted = 0;
        for (const char* q : script) {
            std::printf("\n> %s\n", q);
            const std::string fact = prepare ? Assistant::memoryCommandText(q) : std::string();
            if (!fact.empty()) {
                std::printf("  ISTANTANEO: memoria \"%s\"\n", fact.c_str());
                agent.noteAction(q, "salvato in memoria");
                continue;
            }
            const Cmd::Match m = Cmd::match(profile, q);
            if (m.found) {
                std::printf("  ISTANTANEO: %s (\"%s\", %d%%)\n", m.command.id().c_str(), m.phrase.c_str(), (int)(m.score * 100 + 0.5));
                if (prepare) {
                    // Come in Iris: per la rotta il gioco qui dice di controllare (il caso del registro vero).
                    const bool route = m.command.intent == "set_route";
                    agent.noteAction(q, route ? "non riuscito (Ho premuto Imposta rotta ma non vedo comparire Annulla: controlla in gioco che la "
                                                "rotta sia attiva.)"
                                              : "eseguito");
                }
                continue;
            }
            if (prepare) {
                const auto tw = std::chrono::steady_clock::now();
                agent.warmUp(nullptr);
                std::printf("  (preparazione mentre parli: %lld ms)\n", elapsed(tw));
            }
            Agent::Callbacks cb;
            const auto t0 = std::chrono::steady_clock::now();
            long long first = -1;
            std::vector<std::string> spoken;
            auto mark = [&]() {
                if (first < 0) first = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
            };
            cb.onSentence = [&](const std::string& s) {
                mark();
                spoken.push_back(SpeechText::speakable(s));
            };
            cb.onTool = [&](const std::string& what) {
                mark();
                std::printf("  STRUMENTO: %s\n", what.c_str());
            };
            std::string answer;
            std::atomic<bool> cancel{ false };
            const Agent::Result r = agent.ask(q, cb, &cancel, answer);
            const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
            for (const auto& s : spoken) std::printf("  VOCE: %s\n", s.c_str());
            std::printf("  RISPOSTA (%s, primo segno %lld ms, tutto %lld ms): %s\n", r == Agent::Result::Ok ? "ok" : "ERRORE", first, ms,
                        answer.c_str());
            if (first >= 0) {
                totalFirst += first;
                counted++;
            }
        }
        std::printf("\nTempo medio al primo segno (voce o strumento): %lld ms su %d risposte del modello\n", counted ? totalFirst / counted : 0LL,
                    counted);
        return 0;
    }
    if (mode == "--voce-file" && argc >= 5) {
        // iris_tests --voce-file "Cosimo" testo.txt uscita.wav : la voce di Windows (come in Iris) in un file.
        FILE* in = nullptr;
        if (_wfopen_s(&in, TextUtils::utf8ToWide(argv[3]).c_str(), L"rb") != 0 || !in) return 1;
        std::string text;
        char tb[4096];
        size_t tn;
        while ((tn = fread(tb, 1, sizeof(tb), in)) > 0) text.append(tb, tn);
        fclose(in);
        std::vector<float> s24;
        std::thread([&]() { s24 = TtsEngine::synthesize(text, arg); }).join();
        if (s24.empty()) return 1;
        FILE* out = nullptr;
        if (_wfopen_s(&out, TextUtils::utf8ToWide(argv[4]).c_str(), L"wb") != 0 || !out) return 1;
        const uint32_t rate = TtsEngine::kSampleRate, dataBytes = (uint32_t)s24.size() * 2, riff = 36 + dataBytes, fmtSize = 16, byteRate = rate * 2;
        const uint16_t pcm = 1, channels = 1, align = 2, bits = 16;
        fwrite("RIFF", 1, 4, out); fwrite(&riff, 4, 1, out); fwrite("WAVEfmt ", 1, 8, out); fwrite(&fmtSize, 4, 1, out);
        fwrite(&pcm, 2, 1, out); fwrite(&channels, 2, 1, out); fwrite(&rate, 4, 1, out); fwrite(&byteRate, 4, 1, out);
        fwrite(&align, 2, 1, out); fwrite(&bits, 2, 1, out); fwrite("data", 1, 4, out); fwrite(&dataBytes, 4, 1, out);
        for (float x : s24) {
            int16_t v = (int16_t)std::clamp(x * 32767.0f, -32768.0f, 32767.0f);
            fwrite(&v, 2, 1, out);
        }
        fclose(out);
        std::printf("Scritto: %.1f s\n", s24.size() / (double)TtsEngine::kSampleRate);
        return 0;
    }
    if (mode == "--effetto" && argc >= 5) {
        // iris_tests --effetto robot ingresso.wav uscita.wav : l'effetto sulla voce applicato a un WAV (16 bit).
        FILE* in = nullptr;
        if (_wfopen_s(&in, TextUtils::utf8ToWide(argv[3]).c_str(), L"rb") != 0 || !in) return 1;
        std::vector<uint8_t> wav;
        uint8_t wb[65536];
        size_t wn;
        while ((wn = fread(wb, 1, sizeof(wb), in)) > 0) wav.insert(wav.end(), wb, wb + wn);
        fclose(in);
        uint32_t rate = 0;
        uint16_t channels = 0, bits = 0;
        std::vector<float> s;
        for (size_t p = 12; p + 8 <= wav.size();) {
            uint32_t size;
            memcpy(&size, &wav[p + 4], 4);
            if (!memcmp(&wav[p], "fmt ", 4) && p + 24 <= wav.size()) {
                memcpy(&channels, &wav[p + 10], 2);
                memcpy(&rate, &wav[p + 12], 4);
                memcpy(&bits, &wav[p + 22], 2);
            } else if (!memcmp(&wav[p], "data", 4) && bits == 16 && channels > 0) {
                const size_t frames = std::min<size_t>(size, wav.size() - p - 8) / (2 * channels);
                for (size_t f = 0; f < frames; ++f) {
                    int sum = 0;
                    for (int c = 0; c < channels; ++c) {
                        int16_t v;
                        memcpy(&v, &wav[p + 8 + (f * channels + c) * 2], 2);
                        sum += v;
                    }
                    s.push_back(sum / (32768.0f * channels));
                }
            }
            p += 8 + size + (size & 1);
        }
        if (s.empty() || rate == 0) {
            std::printf("WAV non letto (serve PCM 16 bit)\n");
            return 1;
        }
        if ((int)rate != TtsEngine::kSampleRate) { // come in Iris: tutto a 24 kHz
            const double ratio = (double)rate / TtsEngine::kSampleRate;
            std::vector<float> res((size_t)(s.size() / ratio));
            for (size_t i = 0; i < res.size(); ++i) {
                const double src = i * ratio;
                const size_t a = (size_t)src;
                res[i] = (float)(s[std::min(a, s.size() - 1)] * (1 - (src - a)) + s[std::min(a + 1, s.size() - 1)] * (src - a));
            }
            s.swap(res);
        }
        VoiceEffect fx(TtsEngine::kSampleRate);
        fx.setPreset(arg);
        for (size_t at = 0; at < s.size(); at += 2048) fx.process(s.data() + at, std::min<size_t>(2048, s.size() - at));
        const std::vector<float> t = fx.tail();
        s.insert(s.end(), t.begin(), t.end());
        FILE* out = nullptr;
        if (_wfopen_s(&out, TextUtils::utf8ToWide(argv[4]).c_str(), L"wb") != 0 || !out) return 1;
        const uint32_t orate = TtsEngine::kSampleRate, dataBytes = (uint32_t)s.size() * 2, riff = 36 + dataBytes, fmtSize = 16, byteRate = orate * 2;
        const uint16_t pcm = 1, ch = 1, align = 2, obits = 16;
        fwrite("RIFF", 1, 4, out); fwrite(&riff, 4, 1, out); fwrite("WAVEfmt ", 1, 8, out); fwrite(&fmtSize, 4, 1, out);
        fwrite(&pcm, 2, 1, out); fwrite(&ch, 2, 1, out); fwrite(&orate, 4, 1, out); fwrite(&byteRate, 4, 1, out);
        fwrite(&align, 2, 1, out); fwrite(&obits, 2, 1, out); fwrite("data", 1, 4, out); fwrite(&dataBytes, 4, 1, out);
        for (float x : s) {
            int16_t v = (int16_t)std::clamp(x * 32767.0f, -32768.0f, 32767.0f);
            fwrite(&v, 2, 1, out);
        }
        fclose(out);
        std::printf("Scritto: %s (%s, %.1f s)\n", argv[4], fx.preset().c_str(), s.size() / (double)TtsEngine::kSampleRate);
        return 0;
    }
    if (mode == "--voce-neurale" && argc >= 4) {
        // iris_tests --voce-neurale testo.txt uscita.wav : la voce neurale di Iris (Qwen3-TTS) in un file.
        FILE* in = nullptr;
        if (_wfopen_s(&in, TextUtils::utf8ToWide(argv[2]).c_str(), L"rb") != 0 || !in) return 1;
        std::string text;
        char tb[4096];
        size_t tn;
        while ((tn = fread(tb, 1, sizeof(tb), in)) > 0) text.append(tb, tn);
        fclose(in);
        NeuralVoice voice;
        auto t0 = std::chrono::steady_clock::now();
        if (!voice.load(AppPaths::toLibraryPath(AppPaths::neuralVoiceModelPath()), AppPaths::toLibraryPath(AppPaths::neuralVoiceMmprojPath()), 8)) {
            std::printf("Voce neurale non caricata\n");
            return 1;
        }
        auto t1 = std::chrono::steady_clock::now();
        voice.speak("Pronta.", [](const float*, size_t) { return true; }); // riscaldamento, come in Iris
        auto t2 = std::chrono::steady_clock::now();
        std::vector<float> pcm;
        int chunks = 0;
        long long firstMs = -1;
        // Frase per frase, come in Iris.
        SpeechText::SentenceSplitter splitter;
        std::vector<std::string> sentences = splitter.push(text + " ");
        std::string rest = splitter.flush();
        if (!rest.empty()) sentences.push_back(rest);
        for (const auto& s : sentences) {
            voice.speak(SpeechText::forVoice(SpeechText::speakable(s)), [&](const float* p, size_t n) { // come in Iris
                if (firstMs < 0) firstMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t2).count();
                pcm.insert(pcm.end(), p, p + n);
                chunks++;
                return true;
            });
        }
        auto t3 = std::chrono::steady_clock::now();
        auto ms = [](auto a, auto b) { return (long long)std::chrono::duration_cast<std::chrono::milliseconds>(b - a).count(); };
        std::printf("Caricamento %lld ms, riscaldamento %lld ms, %zu frasi in %lld ms (primo audio dopo %lld ms), %.1f s di voce in %d pezzi\n",
                    ms(t0, t1), ms(t1, t2), sentences.size(), ms(t2, t3), firstMs, pcm.size() / (double)voice.sampleRate(), chunks);
        FILE* out = nullptr;
        if (_wfopen_s(&out, TextUtils::utf8ToWide(argv[3]).c_str(), L"wb") != 0 || !out) return 1;
        const uint32_t rate = (uint32_t)voice.sampleRate(), dataBytes = (uint32_t)pcm.size() * 2, riff = 36 + dataBytes, fmtSize = 16, byteRate = rate * 2;
        const uint16_t fmtPcm = 1, channels = 1, align = 2, bits = 16;
        fwrite("RIFF", 1, 4, out); fwrite(&riff, 4, 1, out); fwrite("WAVEfmt ", 1, 8, out); fwrite(&fmtSize, 4, 1, out);
        fwrite(&fmtPcm, 2, 1, out); fwrite(&channels, 2, 1, out); fwrite(&rate, 4, 1, out); fwrite(&byteRate, 4, 1, out);
        fwrite(&align, 2, 1, out); fwrite(&bits, 2, 1, out); fwrite("data", 1, 4, out); fwrite(&dataBytes, 4, 1, out);
        for (float x : pcm) {
            int16_t v = (int16_t)std::clamp(x * 32767.0f, -32768.0f, 32767.0f);
            fwrite(&v, 2, 1, out);
        }
        fclose(out);
        return 0;
    }
    if (mode == "--voce-coerenza") {
        // iris_tests --voce-coerenza [cartella] [voce] : la voce neurale resta la stessa? 10 frasi, misure e WAV.
        return voice_coherence::run(arg.empty() ? std::wstring(L"campioni-voce\\coerenza") : TextUtils::utf8ToWide(arg), argc > 3 ? argv[3] : "",
                                    voice_coherence::parseSampling(argc, argv, 4));
    }
    if (mode == "--voce-semi" && argc >= 4) {
        // iris_tests --voce-semi primo ultimo [cartella] : le voci che il modello si inventa con ogni seme
        // (per scegliere le voci predefinite): F0, timbro stabile, cosa capisce whisper.
        return voice_coherence::seeds((uint32_t)std::strtoul(argv[2], nullptr, 10), (uint32_t)std::strtoul(argv[3], nullptr, 10),
                                      argc > 4 ? TextUtils::utf8ToWide(argv[4]) : std::wstring(L"campioni-voce\\semi"));
    }
    if (mode == "--voce-leggi") {
        // iris_tests --voce-leggi ["frase" ...] : cosa capisce whisper dalla voce neurale, col testo così com'è
        // e dopo SpeechText::forVoice (numeri e sigle in parole). Per trovare cosa la voce legge male.
        std::vector<std::string> phrases;
        for (int i = 2; i < argc; ++i) phrases.push_back(argv[i]);
        if (phrases.empty()) {
            phrases = { "Hai 16.960 UEC.", "Nella stiva ci sono 32 SCU di Quantanium.", "Il salto dura 2,5 minuti.", "Carburante al 15%.",
                        "Sei a 1.200 km da Lorville.", "Borase x4 e Agricium x2.", "Vai al punto CRU-L1.", "Atterra ad Area18.",
                        "Velocità 1.250 m/s.", "Domani alle 14:30.", "Temperatura -12 gradi.", "Hai un F7C e un 300i.",
                        "Signature 16,960: Borase.", "Ordine numero 3 su 7.", "Prezzo 1,05 UEC per unità." };
        }
        NeuralVoice voice;
        if (!voice.load(AppPaths::toLibraryPath(AppPaths::neuralVoiceModelPath()), AppPaths::toLibraryPath(AppPaths::neuralVoiceMmprojPath()), 8)) return 1;
        voice.setVoice(NeuralVoice::presets().front().id);
        voice.prepare();
        std::vector<std::vector<float>> raw, fixed;
        for (const auto& p : phrases) {
            for (auto* list : { &raw, &fixed }) {
                std::vector<float> pcm;
                voice.speak(list == &raw ? p : SpeechText::forVoice(p), [&](const float* x, size_t n) {
                    pcm.insert(pcm.end(), x, x + n);
                    return true;
                });
                list->push_back(VoiceMeasure::resample(pcm.data(), pcm.size(), voice.sampleRate(), 16000));
            }
        }
        voice.unload();
        WhisperEngine whisper;
        if (!whisper.init(AppPaths::toLibraryPath(AppPaths::whisperModelPath()), AppPaths::toLibraryPath(AppPaths::vadModelPath()), 8, true)) return 1;
        for (size_t i = 0; i < phrases.size(); ++i) {
            std::printf("\n%s\n  per la voce:     %s\n  capito (prima):  %s\n  capito (dopo):   %s\n", phrases[i].c_str(), SpeechText::forVoice(phrases[i]).c_str(),
                        whisper.transcribe(raw[i], "it", "").c_str(), whisper.transcribe(fixed[i], "it", "").c_str());
        }
        return 0;
    }
    if (mode == "--voce-confronta" && argc >= 4) {
        // iris_tests --voce-confronta a.wav b.wav ... : quanto si somigliano i timbri (x-vector), e F0 mediana.
        NeuralVoice voice;
        if (!voice.load(AppPaths::toLibraryPath(AppPaths::neuralVoiceModelPath()), AppPaths::toLibraryPath(AppPaths::neuralVoiceMmprojPath()), 8)) return 1;
        std::vector<std::vector<float>> xs;
        for (int i = 2; i < argc; ++i) {
            std::vector<float> pcm, x;
            int rate = 0;
            NeuralVoice::readWav(TextUtils::utf8ToWide(argv[i]), pcm, rate);
            voice.speakerEmbedding(pcm.data(), pcm.size(), rate, x);
            const std::vector<float> f0 = VoiceMeasure::pitch(pcm.data(), pcm.size(), rate);
            std::printf("%2d  F0 %5.0f Hz  %5.1f s  %s\n", i - 1, VoiceMeasure::median(f0), pcm.size() / (double)std::max(1, rate), argv[i]);
            xs.push_back(x);
        }
        std::printf("    ");
        for (size_t j = 0; j < xs.size(); ++j) std::printf("  %5zu", j + 1);
        std::printf("\n");
        for (size_t i = 0; i < xs.size(); ++i) {
            std::printf("%2zu  ", i + 1);
            for (size_t j = 0; j < xs.size(); ++j) std::printf("  %.3f", VoiceMeasure::cosine(xs[i], xs[j]));
            std::printf("\n");
        }
        return 0;
    }
    if (mode == "--voce-cuciture") {
        // iris_tests --voce-cuciture : il primo pezzo chiesto prima (12 fotogrammi) cambia l'audio? Salti tra i pezzi?
        return voice_coherence::seams();
    }
    if (mode == "--trascrivi" && !arg.empty()) {
        // Audio grezzo float32 mono 16 kHz (ffmpeg -i x.wav -ar 16000 -ac 1 -f f32le x.raw): cosa capisce whisper.
        FILE* f = nullptr;
        if (_wfopen_s(&f, TextUtils::utf8ToWide(arg).c_str(), L"rb") != 0 || !f) return 1;
        std::vector<float> samples;
        float buf[4096];
        size_t n;
        while ((n = fread(buf, sizeof(float), 4096, f)) > 0) samples.insert(samples.end(), buf, buf + n);
        fclose(f);
        WhisperEngine whisper;
        if (!whisper.init(AppPaths::toLibraryPath(AppPaths::whisperModelPath()), AppPaths::toLibraryPath(AppPaths::vadModelPath()), 8, true)) return 1;
        std::printf("%.1f s: %s\n", samples.size() / 16000.0, whisper.transcribe(samples, "it", "").c_str());
        return 0;
    }
    if (mode == "--microfono") {
        // Il microfono vero per qualche secondo: volume e probabilità di voce, secondo per secondo.
        // iris_tests --microfono "Microfono (Scarlett Solo USB)" [secondi]
        const int seconds = argc > 3 ? std::atoi(argv[3]) : 10;
        AudioCapture capture;
        if (!capture.open(arg.empty() ? std::vector<std::string>{} : std::vector<std::string>{ arg })) {
            std::printf("Microfono non aperto\n");
            return 1;
        }
        capture.setStreaming(true);
        VoiceDetector vad;
        vad.init(AppPaths::toLibraryPath(AppPaths::vadModelPath()), 1);
        std::vector<float> buffer, probs;
        for (int sec = 0; sec < seconds; ++sec) {
            Sleep(1000);
            std::vector<float> chunk = capture.takeStream();
            buffer.insert(buffer.end(), chunk.begin(), chunk.end());
            float peak = 0.0f, maxProb = 0.0f;
            double sum = 0.0;
            for (float x : chunk) {
                peak = std::max(peak, std::abs(x));
                sum += (double)x * x;
            }
            const int frames = (int)(buffer.size() / 512);
            vad.process(buffer.data(), frames * 512, probs);
            int over35 = 0, over20 = 0;
            double mean = 0.0;
            for (float pr : probs) {
                maxProb = std::max(maxProb, pr);
                over35 += pr >= 0.35f ? 1 : 0;
                over20 += pr >= 0.20f ? 1 : 0;
                mean += pr;
            }
            if (!probs.empty()) mean /= probs.size();
            buffer.erase(buffer.begin(), buffer.begin() + (size_t)frames * 512);
            std::printf("%2d s: picco %.4f, RMS %.4f, voce: media %.2f max %.2f, blocchi >=0,35: %d, >=0,20: %d su %zu\n", sec + 1, peak,
                        chunk.empty() ? 0.0 : std::sqrt(sum / chunk.size()), mean, maxProb, over35, over20, probs.size());
        }
        return 0;
    }
    if (mode == "--ascolto") {
        // Frasi dette da Elsa, con silenzio in mezzo: come un microfono acceso.
        const std::vector<std::pair<std::string, bool>> phrases = {
            { "Iris, che tempo fa domani a Roma?", true },
            { "Ieri sera ho parlato con Iris del lavoro e poi siamo usciti.", false },
            { "Che ore sono, Iris?", true },
            { "Passami il sale per favore.", false },
            { "Iris.", true },
            { "Ehi Iris, apri Spotify.", true },
        };
        // Volume della voce (argomento 3): 0.6 normale, 0.05 = microfono molto basso.
        const float volume = argc > 3 ? (float)std::atof(argv[3]) : 0.6f;
        std::vector<float> audio(16000, 0.0f);
        for (const auto& [text, _] : phrases) {
            std::vector<float> s24;
            std::thread([&]() { s24 = TtsEngine::synthesize(text, "Elsa"); }).join();
            const size_t n = s24.size() * 2 / 3; // 24 kHz -> 16 kHz
            for (size_t i = 0; i < n; ++i) {
                double src = i * 1.5;
                size_t a = (size_t)src;
                double t = src - a;
                float x0 = s24[std::min(a, s24.size() - 1)], x1 = s24[std::min(a + 1, s24.size() - 1)];
                audio.push_back((float)(x0 + (x1 - x0) * t) * volume);
            }
            audio.resize(audio.size() + 24000, 0.0f); // 1,5 s di silenzio
        }
        // Un filo di rumore di fondo, come una stanza vera.
        unsigned seed = 12345;
        for (float& x : audio) {
            seed = seed * 1103515245u + 12345u;
            x += ((int)((seed >> 16) % 2000) - 1000) / 1000.0f * 0.003f;
        }
        std::printf("Audio di prova: %.1f s\n", audio.size() / 16000.0);

        WhisperEngine whisper;
        VoiceDetector vad;
        if (!whisper.init(AppPaths::toLibraryPath(AppPaths::whisperModelPath()), AppPaths::toLibraryPath(AppPaths::vadModelPath()), 8, true) ||
            !vad.init(AppPaths::toLibraryPath(AppPaths::vadModelPath()), 1)) {
            std::printf("Modelli non caricati\n");
            return 1;
        }
        bool blocksOk = true;
        {
            // Diagnosi: le probabilità a blocchi da 512 (come nel programma, subito dopo il
            // caricamento, senza reset) e tutte insieme. Se a blocchi non sente niente
            // mentre tutto insieme sì, Iris non si sveglia col nome: la prova fallisce.
            std::vector<float> one, all;
            float maxOne = 0.0f, maxAll = 0.0f;
            int overOne = 0, overAll = 0;
            for (size_t pos = 0; pos + 512 <= audio.size(); pos += 512) {
                vad.process(audio.data() + pos, 512, one);
                if (!one.empty()) {
                    maxOne = std::max(maxOne, one[0]);
                    overOne += one[0] >= 0.5f ? 1 : 0;
                }
            }
            vad.reset();
            vad.process(audio.data(), (int)(audio.size() / 512 * 512), all);
            for (float p : all) {
                maxAll = std::max(maxAll, p);
                overAll += p >= 0.5f ? 1 : 0;
            }
            std::printf("Silero a blocchi: max %.2f, %d blocchi sopra 0,5. Tutto insieme: max %.2f, %d blocchi sopra 0,5 (su %zu)\n", maxOne,
                        overOne, maxAll, overAll, all.size());
            blocksOk = overOne * 2 >= overAll; // a blocchi deve sentire più o meno la stessa voce
            if (!blocksOk) std::printf("SBAGLIA a blocchi Silero non sente la voce (stato dell'LSTM non azzerato?)\n");
            vad.reset();
            float peak = 0.0f;
            double sum = 0.0;
            for (float x : audio) {
                peak = std::max(peak, std::abs(x));
                sum += (double)x * x;
            }
            std::printf("Audio: picco %.3f, RMS %.4f\n", peak, std::sqrt(sum / audio.size()));
            std::vector<float> first(audio.begin() + 16000, audio.begin() + std::min(audio.size(), (size_t)16000 * 4));
            std::printf("Whisper sui secondi 1-4 senza ritaglio: \"%s\"\n", whisper.transcribe(first, "it", "").c_str());
        }
        VadSegmenter seg;
        std::vector<float> probs;
        size_t index = 0;
        int ok = 0;
        for (size_t pos = 0; pos + 512 <= audio.size(); pos += 512) {
            vad.process(audio.data() + pos, 512, probs);
            if (!seg.push(audio.data() + pos, probs.empty() ? 0.0f : probs[0])) continue;
            std::vector<float> u = seg.takeUtterance();
            vad.reset();
            auto t0 = std::chrono::steady_clock::now();
            std::string text = whisper.transcribe(u, "it", argc > 2 ? (std::string(argv[2]) == "-" ? std::string() : std::string(argv[2])) : std::string("Parlo con Iris, la mia assistente vocale."));
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
            WakeWord::Match m = WakeWord::detect(text);
            const bool expected = index < phrases.size() && phrases[index].second;
            const bool right = m.found == expected;
            ok += right ? 1 : 0;
            std::printf("%s %.1f s di audio, whisper %lld ms: \"%s\" -> %s%s\n", right ? "OK     " : "SBAGLIA", u.size() / 16000.0, (long long)ms,
                        text.c_str(), m.found ? "PER IRIS" : "non per Iris", m.found ? (": \"" + m.command + "\"").c_str() : "");
            index++;
        }
        std::printf("\n%zu frasi trovate su %zu, %d giuste\n", index, phrases.size(), ok);
        return blocksOk && index == phrases.size() && ok == (int)phrases.size() ? 0 : 1;
    }
    if (mode == "--voce") {
        for (const auto& v : TtsEngine::voices()) std::printf("Voce installata: %s\n", v.c_str());
        TtsEngine tts;
        if (!tts.start()) return 1;
        tts.chime(TtsEngine::Chime::Wake);
        tts.speak(arg.empty() ? "Ciao, sono Iris. Come posso aiutarti?" : arg);
        Sleep(200);
        while (tts.isSpeaking()) Sleep(50);
        Sleep(300);
        tts.shutdown();
        return 0;
    }

    testWakeWord();
    testSpeechText();
    testVad();
    testChatFormat();
    testGame();
    testGameMatching();
    testProfileByHand();
    testMapOcr();
    testChatParser();
    testChatChannels();
    testScreenMatch();
    testTranslationPrompt();
    testInputPlan();
    testGlossary();
    testLanguageGuess();
    testGamerSlang();
    testScreenCapture();
    testGameChatInIris();
    testMining();
    testVoiceEffect();
    testSettingsAndTools();
    testUpdater();
    testAxis();
    testGameLog();
    testCommandEditor();
    testShipToggle();
    testWheel();
    testMiningLog();
    testInterfaceAndWindows();
    testTelemetry();
    if (mode == "--completi") testFull();

    std::printf("\n%d controlli, %d falliti\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
