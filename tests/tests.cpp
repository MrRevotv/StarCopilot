// Prove automatiche di Iris.
//   iris_tests.exe                 prove veloci (niente modelli, niente rete, nessun file toccato)
//   iris_tests.exe --completi      anche il modello di linguaggio vero e la rete
//
// Prove a mano dei singoli pezzi:
//   iris_tests.exe --chiedi "che tempo fa a Roma?"   una domanda a Iris (modello + strumenti), senza voce
//   iris_tests.exe --cerca "testo"                    solo la ricerca su internet
//   iris_tests.exe --meteo "Milano"                   solo il meteo
//   iris_tests.exe --notizie ["argomento"]            solo le notizie
//   iris_tests.exe --app "nome"                       quale programma aprirebbe (senza aprirlo)
//   iris_tests.exe --voce "testo"                     la voce di Windows
//   iris_tests.exe --pagina "https://..."             il testo che Iris legge da una pagina
//   iris_tests.exe --ascolto                          l'ascolto continuo con frasi dette dalla voce di Windows
//                                                     (Silero + whisper + "Iris", come dal microfono)

#include "agent.h"
#include "app_paths.h"
#include "app_settings.h"
#include "assistant.h"
#include "audio_capture.h"
#include "chat_format.h"
#include "chat_parser.h"
#include "command_matcher.h"
#include "game_chat.h"
#include "game_controller.h"
#include "game_input.h"
#include "game_profile.h"
#include "gamer_slang.h"
#include "glossary.h"
#include "gpu.h"
#include "html_utils.h"
#include "http_client.h"
#include "input_sender.h"
#include "language_guess.h"
#include "languages.h"
#include "llm_engine.h"
#include "neural_voice.h"
#include "sc_locations.h"
#include "screen_capture.h"
#include "screen_match.h"
#include "speech_utils.h"
#include "translation_prompt.h"
#include "speech_text.h"
#include "system_actions.h"
#include "text_utils.h"
#include "tools.h"
#include "tts_engine.h"
#include "vad_segmenter.h"
#include "voice_detector.h"
#include "voice_effect.h"
#include "wake_word.h"
#include "whisper_engine.h"
#include "ui/orb.h"
#include "ui/ui.h"

#include <windows.h>
#include <objbase.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <cstring>
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

    // Per gli strumenti: timer e memoria finti.
    class FakeHost : public ToolHost {
    public:
        void addTimer(int seconds, const std::string& label) override { std::printf("  [timer %d s: %s]\n", seconds, label.c_str()); }
        int cancelTimers() override { return 0; }
        std::string homeCity() override { return "Milano"; }
        void remember(const std::string& fact) override { std::printf("  [memoria: %s]\n", fact.c_str()); }
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
        CHECK(WakeWord::isStopCommand("basta"));
        CHECK(WakeWord::isStopCommand("Stop!"));
        CHECK(WakeWord::isStopCommand("Zitta."));
        CHECK(WakeWord::isStopCommand("Basta così, grazie"));
        CHECK(!WakeWord::isStopCommand("basta con la pasta per stasera, cosa cucino?"));
        CHECK(!WakeWord::isStopCommand("che tempo fa"));
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

    void testHtml() {
        std::printf("Pagine web e ricerche\n");
        CHECK_STR(Html::decodeEntities("perch&eacute; &amp; &#8217;ok&#x27; &lt;b&gt;"), "perch\xC3\xA9 & \xE2\x80\x99ok' <b>");
        CHECK_STR(Html::stripTags("<b>Ciao</b>  <i>mondo</i> &amp; co"), "Ciao mondo & co");
        CHECK_STR(Html::urlDecode("https%3A%2F%2Fwww.example.com%2Fa%3Fb%3D1"), "https://www.example.com/a?b=1");
        CHECK_STR(Html::urlEncode("che tempo fa?"), "che+tempo+fa%3F");

        const std::string ddg =
            "<div class=\"result results_links results_links_deep result--ad \"><h2 class=\"result__title\">"
            "<a rel=\"nofollow\" class=\"result__a\" href=\"https://duckduckgo.com/y.js?ad_domain=x.com&amp;ad_provider=b\">Pubblicita</a></h2>"
            "<a class=\"result__snippet\" href=\"#\">Compra ora</a></div>"
            "<div class=\"result results_links\"><h2 class=\"result__title\">"
            "<a rel=\"nofollow\" class=\"result__a\" href=\"//duckduckgo.com/l/?uddg=https%3A%2F%2Fit.wikipedia.org%2Fwiki%2FRoma&amp;rut=abc\">Roma - <b>Wikipedia</b></a>"
            "</h2><a class=\"result__snippet\" href=\"//duckduckgo.com/l/?uddg=x\">Roma &egrave; la <b>capitale</b> d'Italia.</a></div>"
            "<div class=\"result\"><a rel=\"nofollow\" class=\"result__a\" href=\"https://www.comune.roma.it/\">Comune di Roma</a></div>";
        auto r = Html::parseDuckDuckGo(ddg);
        CHECK(r.size() == 2);
        if (r.size() == 2) {
            CHECK_STR(r[0].title, "Roma - Wikipedia");
            CHECK_STR(r[0].url, "https://it.wikipedia.org/wiki/Roma");
            CHECK_STR(r[0].snippet, "Roma \xC3\xA8 la capitale d'Italia.");
            CHECK_STR(r[1].url, "https://www.comune.roma.it/");
            CHECK_STR(r[1].snippet, "");
        }

        const std::string lite =
            "<tr><td><a rel=\"nofollow\" href=\"https://www.ansa.it/notizia\" class='result-link'>Notizia ANSA</a></td></tr>"
            "<tr><td class='result-snippet'>Il testo della <b>notizia</b>.</td></tr>"
            "<tr><td><a rel=\"nofollow\" href=\"//duckduckgo.com/l/?uddg=https%3A%2F%2Fexample.org%2F\" class='result-link'>Esempio</a></td></tr>";
        auto l = Html::parseDuckDuckGoLite(lite);
        CHECK(l.size() == 2);
        if (l.size() == 2) {
            CHECK_STR(l[0].snippet, "Il testo della notizia.");
            CHECK_STR(l[1].url, "https://example.org/");
        }

        const std::string bing =
            "<ol id=\"b_results\"><li class=\"b_algo\" data-id iid=SERP.5355><div class=\"b_tpcn\"><a class=\"tilk\" href=\"https://www.bing.com/ck/a?!&amp;&amp;p=c8b0&amp;u=a1aHR0cHM6Ly93d3cuY2hpbWFnYXppbmUuaXQvbmV3cy8&amp;ntb=1\">"
            "<cite>https://www.chimagazine.it</cite></a></div><h2><a target=\"_blank\" href=\"https://www.bing.com/ck/a?!&amp;&amp;p=c8b0&amp;"
            "u=a1aHR0cHM6Ly93d3cuY2hpbWFnYXppbmUuaXQvbmV3cy8&amp;ntb=1\" h=\"ID=SERP,5145.2\">News - <strong>Chi</strong> Magazine</a></h2>"
            "<div class=\"b_caption\"><p class=\"b_lineclamp2\"><span class=\"news_dt\">2 mar 2026</span>&nbsp;&#0183;&#32;Lui &#232; uno dei calciatori</p></div></li>"
            "<li class=\"b_algo\"><h2><a href=\"https://example.com/diretto\">Diretto</a></h2></li></ol>";
        auto b = Html::parseBing(bing);
        CHECK(b.size() == 2);
        if (b.size() == 2) {
            CHECK_STR(b[0].url, "https://www.chimagazine.it/news/");
            CHECK_STR(b[0].title, "News - Chi Magazine");
            CHECK_STR(b[0].snippet, "2 mar 2026 \xC2\xB7 Lui \xC3\xA8 uno dei calciatori");
            CHECK_STR(b[1].url, "https://example.com/diretto");
        }

        const std::string page =
            "<html><head><title>Titolo &amp; pagina</title><style>.x{}</style></head><body>"
            "<nav>Home Contatti Chi siamo Menu principale del sito</nav>"
            "<script>var x = 'non leggere questo testo per niente';</script>"
            "<article><h1>Il titolo dell'articolo è qui</h1><p>Questo è il primo paragrafo dell'articolo, abbastanza lungo da essere tenuto.</p>"
            "<p>Questo è il secondo paragrafo, anche lui con parecchie parole dentro per sembrare vero.</p>"
            "<p>" + std::string(400, 'a') + " fine.</p></article>"
            "<footer>Copyright tutti i diritti riservati alla redazione</footer></body></html>";
        CHECK_STR(Html::title(page), "Titolo & pagina");
        std::string text = Html::toText(page, 3000);
        CHECK(text.find("primo paragrafo") != std::string::npos);
        CHECK(text.find("secondo paragrafo") != std::string::npos);
        CHECK(text.find("non leggere") == std::string::npos);
        CHECK(text.find("Copyright") == std::string::npos);
        CHECK(text.find("Contatti") == std::string::npos);
        CHECK(Html::toText(page, 60).size() < 80);

        CHECK_STR(Html::charset("text/html; charset=ISO-8859-1", ""), "iso-8859-1");
        CHECK_STR(Html::charset("text/html", "<meta charset=\"utf-8\">"), "utf-8");
        CHECK_STR(Html::charset("", "<html>"), "");

        const std::string rss =
            "<rss><channel><title>Google News</title>"
            "<item><title>Grande notizia di oggi - ANSA</title><link>https://news.google.com/a</link>"
            "<pubDate>Fri, 31 Oct 2026 08:00:00 GMT</pubDate><source url=\"https://www.ansa.it\">ANSA</source></item>"
            "<item><title><![CDATA[Altra notizia &amp; altro]]></title><pubDate>Fri, 31 Oct 2026 07:00:00 GMT</pubDate></item>"
            "</channel></rss>";
        auto items = Html::parseRss(rss);
        CHECK(items.size() == 2);
        if (items.size() == 2) {
            CHECK_STR(items[0].title, "Grande notizia di oggi");
            CHECK_STR(items[0].source, "ANSA");
            CHECK_STR(items[1].title, "Altra notizia & altro");
        }
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
        CHECK_STR(ChatFormat::cleanAnswer("Ciao!<|im_end|>"), "Ciao!");
        CHECK_STR(ChatFormat::cleanAnswer("<think>\nhmm\n</think>\n\nRisposta."), "Risposta.");
        CHECK_STR(ChatFormat::cleanAnswer("Ok. <tool_call>{\"name\""), "Ok.");
    }

    void testApps() {
        std::printf("Programmi\n");
        std::vector<std::string> names = { "Spotify", "Steam", "Discord", "Blocco note", "Calcolatrice", "Microsoft Word", "Google Chrome",
                                           "Uninstall Spotify", "Star Citizen", "RSI Launcher" };
        CHECK(SystemActions::bestAppMatch(names, "spotify") == 0);
        CHECK(SystemActions::bestAppMatch(names, "Spotifai") == 0);
        CHECK(SystemActions::bestAppMatch(names, "il blocco note") == 3);
        CHECK(SystemActions::bestAppMatch(names, "word") == 5);
        CHECK(SystemActions::bestAppMatch(names, "chrome") == 6);
        CHECK(SystemActions::bestAppMatch(names, "la calcolatrice") == 4);
        CHECK(SystemActions::bestAppMatch(names, "steam") == 1);
        CHECK(SystemActions::bestAppMatch(names, "star citizen") == 8);
        CHECK(SystemActions::bestAppMatch(names, "xyz qwerty") == -1);
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
            std::printf("  %-9s volume %.2f del originale, picco %.2f\n", id, wetRms / dryRms, peak);
        }
    }

    void testSettingsAndTools() {
        std::printf("Impostazioni e strumenti\n");
        AppSettings s;
        s.userName = "Roberto";
        s.homeCity = "Milano";
        s.wakeWordEnabled = false;
        s.followUpSeconds = 9;
        s.voiceName = "Cosimo";
        TalkTrigger t;
        t.vk = VK_XBUTTON1;
        s.triggers.push_back(t);
        AppSettings back = AppSettings::parse(s.serialize());
        CHECK_STR(back.userName, "Roberto");
        CHECK_STR(back.homeCity, "Milano");
        CHECK(!back.wakeWordEnabled);
        CHECK(back.followUpSeconds == 9);
        CHECK_STR(back.voiceName, "Cosimo");
        CHECK(back.triggers.size() == 1 && back.triggers[0].vk == VK_XBUTTON1);
        AppSettings bad = AppSettings::parse("speechRate=99\nvadThreshold=-3\nfollowUpSeconds=500\n");
        bad.sanitize();
        CHECK(bad.speechRate == 2.0 && bad.vadThreshold == 0.1 && bad.followUpSeconds == 30);

        CHECK_STR(Tools::weatherDescription(61), "pioggia debole");
        CHECK_STR(Tools::weatherDescription(0), "sereno");
        CHECK_STR(Tools::describe("web_search", "{\"query\":\"meteo Roma\"}"), "Cerco su internet: meteo Roma");
        FakeHost host;
        Tools tools(host);
        CHECK(tools.definitions(true).size() == 11);
        CHECK(tools.definitions(false).size() == 7);
        CHECK(TextUtils::startsWith(tools.run("web_search", "{\"query\":\"x\"}", false, nullptr), "Errore: internet"));
        CHECK(TextUtils::startsWith(tools.run("inesistente", "{}", true, nullptr), "Errore"));
        CHECK(TextUtils::startsWith(tools.run("set_timer", "{\"seconds\": \"300\", \"label\": \"pasta\"}", true, nullptr), "Timer avviato: 300"));
        CHECK(TextUtils::startsWith(tools.run("set_timer", "{\"minutes\": 2}", true, nullptr), "Timer avviato: 120"));

        LlmEngine llm;
        Agent agent(llm, tools);
        Agent::Persona persona;
        persona.userName = "Roberto";
        persona.memory = { "Gioca a Star Citizen" };
        agent.setPersona(persona);
        std::string sys = agent.systemPrompt();
        CHECK(sys.find("di Roberto") != std::string::npos);
        CHECK(sys.find("Gioca a Star Citizen") != std::string::npos);
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

        used.clear();
        a = askOnce(agent, "Che tempo fa domani a Roma?", &used);
        CHECK(!used.empty() && used[0].find("meteo") != std::string::npos);
        CHECK(!a.empty());

        used.clear();
        a = askOnce(agent, "E a Milano?", &used);
        CHECK(!used.empty());

        used.clear();
        a = askOnce(agent, "Metti un timer di 5 minuti per la pasta", &used);
        CHECK(!used.empty() && used[0].find("timer") != std::string::npos);

        used.clear();
        a = askOnce(agent, "Cerca su internet chi ha vinto l'ultimo Gran Premio di Formula 1", &used);
        CHECK(!used.empty());
    }
}

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
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
    if (mode == "--cerca" && !arg.empty()) {
        std::printf("%s\n", tools.run("web_search", "{\"query\":\"" + arg + "\"}", true, nullptr).c_str());
        return 0;
    }
    if (mode == "--meteo") {
        std::printf("%s\n", tools.run("get_weather", "{\"location\":\"" + arg + "\"}", true, nullptr).c_str());
        return 0;
    }
    if (mode == "--notizie") {
        std::printf("%s\n", tools.run("get_news", "{\"topic\":\"" + arg + "\"}", true, nullptr).c_str());
        return 0;
    }
    if (mode == "--pagina" && !arg.empty()) {
        std::printf("%s\n", tools.run("read_webpage", "{\"url\":\"" + arg + "\"}", true, nullptr).c_str());
        return 0;
    }
    if (mode == "--app" && !arg.empty()) {
        auto apps = SystemActions::listApps();
        std::vector<std::string> names;
        for (const auto& a : apps) names.push_back(a.name);
        double score = 0;
        int i = SystemActions::bestAppMatch(names, arg, &score);
        std::printf("%zu programmi nel menu Start. \"%s\" -> %s (%.0f%%)\n", apps.size(), arg.c_str(), i >= 0 ? apps[i].name.c_str() : "(nessuno)", score * 100);
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
            voice.speak(SpeechText::speakable(s), [&](const float* p, size_t n) {
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
        {
            // Diagnosi: le probabilità a blocchi da 512 (come nel programma) e tutte insieme.
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
        return index == phrases.size() && ok == (int)phrases.size() ? 0 : 1;
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
    testHtml();
    testChatFormat();
    testApps();
    testGame();
    testChatParser();
    testScreenMatch();
    testTranslationPrompt();
    testInputPlan();
    testGlossary();
    testLanguageGuess();
    testGamerSlang();
    testScreenCapture();
    testGameChatInIris();
    testVoiceEffect();
    testSettingsAndTools();
    if (mode == "--completi") testFull();

    std::printf("\n%d controlli, %d falliti\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
