#include "tools.h"
#include "html_utils.h"
#include "http_client.h"
#include "log.h"
#include "system_actions.h"
#include "text_utils.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <future>

namespace {
    using json = nlohmann::json;

    // Valori degli argomenti, tolleranti: il modello a volte scrive 300 come "300".
    std::string argString(const json& a, const char* key) {
        if (!a.contains(key)) return {};
        const json& v = a[key];
        if (v.is_string()) return TextUtils::trim(v.get<std::string>());
        if (v.is_number()) return v.dump();
        return {};
    }
    int argInt(const json& a, const char* key, int fallback) {
        if (!a.contains(key)) return fallback;
        const json& v = a[key];
        if (v.is_number()) return (int)std::lround(v.get<double>());
        if (v.is_string()) {
            try { return (int)std::lround(std::stod(v.get<std::string>())); } catch (...) {}
        }
        return fallback;
    }

    std::string componentEncode(const std::string& s) {
        std::string e = Html::urlEncode(s);
        std::string out;
        for (char c : e) out += c == '+' ? std::string("%20") : std::string(1, c);
        return out;
    }

    std::string domainOf(const std::string& url) {
        size_t start = url.find("://");
        start = start == std::string::npos ? 0 : start + 3;
        size_t end = url.find('/', start);
        std::string host = url.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (TextUtils::startsWith(host, "www.")) host = host.substr(4);
        return host;
    }

    // Pagine da cui non si ricava testo utile.
    bool skipPage(const std::string& url) {
        static const char* const skip[] = { ".pdf", "youtube.com", "youtu.be", "facebook.com", "instagram.com", "x.com/", "twitter.com",
                                            "tiktok.com", "linkedin.com", "reddit.com/r/" };
        const std::string lower = TextUtils::toLower(url);
        return std::any_of(std::begin(skip), std::end(skip), [&](const char* s) { return lower.find(s) != std::string::npos; });
    }

    std::string fetchText(const std::string& url, size_t maxChars, const std::atomic<bool>* cancel, std::string* title) {
        Http::Options o;
        o.timeoutMs = 6000;
        o.maxBytes = 1500 * 1024;
        o.cancel = cancel;
        Http::Response r = Http::get(url, o);
        if (!r.ok()) return {};
        const std::string ct = TextUtils::toLower(r.contentType);
        if (!ct.empty() && ct.find("html") == std::string::npos && ct.find("text") == std::string::npos) return {};
        const std::string html = Http::bodyUtf8(r);
        if (title) *title = Html::title(html);
        return Html::toText(html, maxChars);
    }

    // "2026-11-01" -> "sabato 1 novembre" (o "oggi"/"domani").
    std::string italianDay(const std::string& iso, int index) {
        if (index == 0) return "oggi";
        if (index == 1) return "domani";
        static const char* const days[] = { "domenica", "lunedì", "martedì", "mercoledì", "giovedì", "venerdì", "sabato" };
        static const char* const months[] = { "gennaio", "febbraio", "marzo", "aprile", "maggio", "giugno",
                                              "luglio", "agosto", "settembre", "ottobre", "novembre", "dicembre" };
        std::tm t{};
        if (std::sscanf(iso.c_str(), "%d-%d-%d", &t.tm_year, &t.tm_mon, &t.tm_mday) != 3) return iso;
        t.tm_year -= 1900;
        t.tm_mon -= 1;
        t.tm_hour = 12;
        std::mktime(&t);
        return std::string(days[t.tm_wday]) + " " + std::to_string(t.tm_mday) + " " + months[t.tm_mon];
    }

    std::string round0(double v) { return std::to_string((int)std::lround(v)); }

    std::string def(const char* name, const char* description, json properties, std::vector<std::string> required) {
        json f = { { "type", "function" },
                   { "function", { { "name", name }, { "description", description },
                                   { "parameters", { { "type", "object" }, { "properties", properties }, { "required", required } } } } } };
        return f.dump();
    }
}

std::vector<std::string> Tools::definitions(bool internet) const {
    std::vector<std::string> d;
    if (internet) {
        d.push_back(def("web_search",
                        "Search the internet for up-to-date or factual information: news, facts, prices, sports results, opening hours, "
                        "people, products, events after 2023, or anything you are not sure about. Returns the top results with the text "
                        "of the best pages.",
                        { { "query", { { "type", "string" }, { "description", "search query, usually in Italian" } } } }, { "query" }));
        d.push_back(def("read_webpage", "Read the text of a web page, for details not in the search results.",
                        { { "url", { { "type", "string" }, { "description", "full address of the page" } } } }, { "url" }));
        d.push_back(def("get_weather",
                        "Current weather and forecast for the next days. Use it for any question about weather, temperature, rain, wind.",
                        { { "location", { { "type", "string" }, { "description", "city name; empty = the user's home city" } } } }, {}));
        d.push_back(def("get_news", "Latest Italian news headlines (Google News), in general or about a topic.",
                        { { "topic", { { "type", "string" }, { "description", "topic to search; empty = top headlines" } } } }, {}));
    }
    d.push_back(def("open_app", "Open a program installed on the PC (e.g. Spotify, Steam, Discord, Chrome, Blocco note, Calcolatrice).",
                    { { "name", { { "type", "string" }, { "description", "program name as the user said it" } } } }, { "name" }));
    d.push_back(def("open_website", "Open a website in the browser.",
                    { { "url", { { "type", "string" }, { "description", "address, e.g. youtube.com" } } } }, { "url" }));
    d.push_back(def("set_timer", "Start a countdown timer; when it ends you will announce it.",
                    { { "seconds", { { "type", "integer" }, { "description", "duration in seconds (5 minutes = 300)" } } },
                      { "label", { { "type", "string" }, { "description", "what the timer is for, e.g. pasta" } } } },
                    { "seconds" }));
    d.push_back(def("cancel_timers", "Cancel all running timers.", json::object(), {}));
    d.push_back(def("set_volume", "Change the PC audio volume.",
                    { { "action", { { "type", "string" }, { "enum", { "set", "up", "down", "mute", "unmute" } } } },
                      { "level", { { "type", "integer" }, { "description", "0-100 for set; step for up/down (default 10)" } } } },
                    { "action" }));
    d.push_back(def("media_control", "Control music or video playing on the PC (Spotify, YouTube...).",
                    { { "action", { { "type", "string" }, { "enum", { "play_pause", "next", "previous", "stop" } } } } }, { "action" }));
    d.push_back(def("remember", "Save a fact about the user to long-term memory, when the user asks you to remember something.",
                    { { "fact", { { "type", "string" }, { "description", "the fact, as a short sentence" } } } }, { "fact" }));
    for (const auto& extra : m_host.extraToolDefinitions()) d.push_back(extra);
    return d;
}

std::string Tools::describe(const std::string& name, const std::string& argumentsJson) {
    json a = json::parse(argumentsJson, nullptr, false);
    if (!a.is_object()) a = json::object();
    if (name == "web_search") return "Cerco su internet: " + argString(a, "query");
    if (name == "read_webpage") return "Leggo la pagina " + domainOf(argString(a, "url"));
    if (name == "get_weather") {
        std::string where = argString(a, "location");
        return where.empty() ? "Controllo il meteo" : "Controllo il meteo: " + where;
    }
    if (name == "get_news") {
        std::string topic = argString(a, "topic");
        return topic.empty() ? "Leggo le ultime notizie" : "Cerco notizie: " + topic;
    }
    if (name == "open_app") return "Apro " + argString(a, "name");
    if (name == "open_website") return "Apro " + argString(a, "url");
    if (name == "set_timer") return "Imposto un timer";
    if (name == "cancel_timers") return "Annullo i timer";
    if (name == "set_volume") return "Regolo il volume";
    if (name == "media_control") return "Controllo la musica";
    if (name == "remember") return "Me lo segno";
    if (name == "game_command") {
        std::string what = argString(a, "command"), where = argString(a, "destination");
        return "Star Citizen: " + what + (where.empty() ? "" : " " + where);
    }
    return "Uso lo strumento " + name;
}

std::string Tools::run(const std::string& name, const std::string& argumentsJson, bool internet, const std::atomic<bool>* cancel) {
    json a = json::parse(argumentsJson, nullptr, false);
    if (!a.is_object()) a = json::object();
    std::string extra;
    if (m_host.runExtraTool(name, argumentsJson, cancel, extra)) return extra;
    const bool needsInternet = name == "web_search" || name == "read_webpage" || name == "get_weather" || name == "get_news";
    if (needsInternet && !internet) return "Errore: internet è disattivato nelle impostazioni di Iris.";

    if (name == "web_search") {
        std::string q = argString(a, "query");
        return q.empty() ? "Errore: manca la ricerca da fare." : webSearch(q, cancel);
    }
    if (name == "read_webpage") return readWebpage(argString(a, "url"), cancel);
    if (name == "get_weather") return weather(argString(a, "location"), cancel);
    if (name == "get_news") return news(argString(a, "topic"), cancel);

    std::string out;
    if (name == "open_app") {
        bool ok = SystemActions::launchApp(argString(a, "name"), out);
        return ok ? "Aperto: " + out : "Errore: " + out;
    }
    if (name == "open_website") {
        bool ok = SystemActions::openUrl(argString(a, "url"), out);
        return ok ? "Aperto nel browser: " + out : "Errore: " + out;
    }
    if (name == "set_timer") {
        int seconds = argInt(a, "seconds", 0);
        if (seconds <= 0) seconds = argInt(a, "minutes", 0) * 60;
        if (seconds <= 0 || seconds > 24 * 3600) return "Errore: durata del timer non valida.";
        std::string label = argString(a, "label");
        m_host.addTimer(seconds, label);
        return "Timer avviato: " + std::to_string(seconds) + " secondi" + (label.empty() ? "" : " (" + label + ")") + ".";
    }
    if (name == "cancel_timers") {
        int n = m_host.cancelTimers();
        return n == 0 ? "Non c'erano timer attivi." : "Annullati " + std::to_string(n) + " timer.";
    }
    if (name == "set_volume") {
        bool ok = SystemActions::volume(argString(a, "action"), argInt(a, "level", -1), out);
        return ok ? "Fatto: " + out + "." : "Errore: " + out;
    }
    if (name == "media_control") {
        bool ok = SystemActions::mediaKey(argString(a, "action"), out);
        return ok ? "Fatto." : "Errore: " + out;
    }
    if (name == "remember") {
        std::string fact = argString(a, "fact");
        if (fact.empty()) return "Errore: niente da ricordare.";
        m_host.remember(fact);
        return "Salvato in memoria: " + fact;
    }
    return "Errore: lo strumento \"" + name + "\" non esiste.";
}

std::string Tools::webSearch(const std::string& query, const std::atomic<bool>* cancel) {
    Http::Options o;
    o.timeoutMs = 8000;
    o.cancel = cancel;
    std::vector<Html::SearchResult> results;

    Http::Response r = Http::get("https://html.duckduckgo.com/html/?q=" + Html::urlEncode(query) + "&kl=it-it", o);
    if (r.ok()) results = Html::parseDuckDuckGo(Http::bodyUtf8(r));
    if (results.empty() && !(cancel && cancel->load())) {
        Log::warn("Ricerca: DuckDuckGo html senza risultati (" + std::to_string(r.status) + " " + r.error + "), provo la versione lite");
        r = Http::post("https://lite.duckduckgo.com/lite/", "q=" + Html::urlEncode(query) + "&kl=it-it", "application/x-www-form-urlencoded", o);
        if (r.ok()) results = Html::parseDuckDuckGoLite(Http::bodyUtf8(r));
    }
    if (results.empty() && !(cancel && cancel->load())) {
        // DuckDuckGo rallenta chi cerca troppo spesso (risponde 202): allora Bing.
        Log::warn("Ricerca: DuckDuckGo non risponde (" + std::to_string(r.status) + "), provo Bing");
        r = Http::get("https://www.bing.com/search?q=" + Html::urlEncode(query) + "&setlang=it&cc=IT", o);
        if (r.ok()) results = Html::parseBing(Http::bodyUtf8(r));
    }
    if (results.empty() && !(cancel && cancel->load())) {
        // Ultima spiaggia: Wikipedia.
        Log::warn("Ricerca: nemmeno Bing risponde (" + std::to_string(r.status) + "), provo Wikipedia");
        Http::Response w = Http::get("https://it.wikipedia.org/w/api.php?action=query&format=json&list=search&srlimit=4&utf8=1&srsearch=" +
                                         componentEncode(query), o);
        json j = json::parse(w.body, nullptr, false);
        if (w.ok() && j.is_object() && j.contains("query") && j["query"].contains("search")) {
            for (const auto& s : j["query"]["search"]) {
                Html::SearchResult sr;
                sr.title = s.value("title", "");
                sr.snippet = Html::stripTags(s.value("snippet", ""));
                std::string page = sr.title;
                std::replace(page.begin(), page.end(), ' ', '_');
                sr.url = "https://it.wikipedia.org/wiki/" + componentEncode(page);
                results.push_back(sr);
            }
        }
    }
    if (cancel && cancel->load()) return "Annullato.";
    if (results.empty()) return "Errore: la ricerca non ha dato risultati (" + (r.error.empty() ? "HTTP " + std::to_string(r.status) : r.error) + ").";
    if (results.size() > 6) results.resize(6);

    // Il testo delle due pagine migliori, scaricate insieme.
    std::vector<std::future<std::pair<std::string, std::string>>> pages;
    for (const auto& res : results) {
        if (pages.size() >= 2) break;
        if (skipPage(res.url)) continue;
        const std::string url = res.url;
        pages.push_back(std::async(std::launch::async, [url, cancel]() {
            std::string title;
            std::string text = fetchText(url, 1800, cancel, &title);
            return std::make_pair(url, text);
        }));
    }

    std::string out = "Risultati della ricerca \"" + query + "\":\n";
    for (size_t i = 0; i < results.size(); ++i) {
        out += std::to_string(i + 1) + ". " + results[i].title + " (" + domainOf(results[i].url) + ")\n";
        if (!results[i].snippet.empty()) out += "   " + results[i].snippet + "\n";
    }
    for (auto& p : pages) {
        auto [url, text] = p.get();
        if (text.size() < 200) continue;
        out += "\nTesto della pagina " + domainOf(url) + ":\n" + text + "\n";
    }
    return out;
}

std::string Tools::readWebpage(const std::string& url, const std::atomic<bool>* cancel) {
    if (!TextUtils::startsWith(url, "http://") && !TextUtils::startsWith(url, "https://")) return "Errore: indirizzo non valido.";
    std::string title;
    std::string text = fetchText(url, 5000, cancel, &title);
    if (text.empty()) return "Errore: la pagina non si apre o non contiene testo.";
    return "Pagina " + domainOf(url) + (title.empty() ? "" : " (" + title + ")") + ":\n" + text;
}

std::string Tools::weather(const std::string& requested, const std::atomic<bool>* cancel) {
    std::string location = requested.empty() ? m_host.homeCity() : requested;
    if (location.empty()) return "Errore: non so di quale città vuoi il meteo. Chiedilo all'utente (può anche impostare la sua città nelle impostazioni di Iris).";
    Http::Options o;
    o.timeoutMs = 8000;
    o.cancel = cancel;

    // 1) Dove si trova.
    Http::Response g = Http::get("https://geocoding-api.open-meteo.com/v1/search?count=1&language=it&format=json&name=" + componentEncode(location), o);
    json gj = json::parse(g.body, nullptr, false);
    if (!g.ok() || !gj.is_object() || !gj.contains("results") || gj["results"].empty()) {
        return g.ok() ? "Errore: non trovo la località \"" + location + "\"." : "Errore: servizio meteo non raggiungibile (" + g.error + ").";
    }
    const json& place = gj["results"][0];
    const double lat = place.value("latitude", 0.0), lon = place.value("longitude", 0.0);
    std::string placeName = place.value("name", location);
    const std::string region = place.value("admin1", ""), country = place.value("country", "");
    if (!region.empty()) placeName += ", " + region;
    if (!country.empty() && country != "Italia") placeName += ", " + country;

    // 2) Il tempo.
    char url[512];
    std::snprintf(url, sizeof(url),
                  "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
                  "&current=temperature_2m,apparent_temperature,relative_humidity_2m,weather_code,wind_speed_10m,precipitation"
                  "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max,precipitation_sum"
                  "&timezone=auto&forecast_days=4",
                  lat, lon);
    Http::Response f = Http::get(url, o);
    json fj = json::parse(f.body, nullptr, false);
    if (!f.ok() || !fj.is_object() || !fj.contains("current") || !fj.contains("daily")) return "Errore: previsioni non disponibili.";

    const json& c = fj["current"];
    std::string out = "Meteo per " + placeName + ".\nAdesso: " + weatherDescription(c.value("weather_code", -1)) + ", " +
                      round0(c.value("temperature_2m", 0.0)) + " gradi (percepiti " + round0(c.value("apparent_temperature", 0.0)) +
                      "), umidità " + round0(c.value("relative_humidity_2m", 0.0)) + "%, vento " + round0(c.value("wind_speed_10m", 0.0)) +
                      " km/h";
    if (c.value("precipitation", 0.0) > 0.0) out += ", precipitazioni in corso";
    out += ".\n";
    const json& d = fj["daily"];
    const size_t days = d.contains("time") ? d["time"].size() : 0;
    for (size_t i = 0; i < days; ++i) {
        auto at = [&](const char* key) { return d.contains(key) && i < d[key].size() && d[key][i].is_number() ? d[key][i].get<double>() : 0.0; };
        out += italianDay(d["time"][i].get<std::string>(), (int)i) + ": " + weatherDescription((int)at("weather_code")) + ", minima " +
               round0(at("temperature_2m_min")) + " e massima " + round0(at("temperature_2m_max")) + " gradi, probabilità di pioggia " +
               round0(at("precipitation_probability_max")) + "%";
        if (at("precipitation_sum") >= 1.0) out += " (" + round0(at("precipitation_sum")) + " mm)";
        out += ".\n";
    }
    return out;
}

std::string Tools::news(const std::string& topic, const std::atomic<bool>* cancel) {
    Http::Options o;
    o.timeoutMs = 8000;
    o.cancel = cancel;
    const std::string url = topic.empty() ? "https://news.google.com/rss?hl=it&gl=IT&ceid=IT:it"
                                          : "https://news.google.com/rss/search?hl=it&gl=IT&ceid=IT:it&q=" + componentEncode(topic);
    Http::Response r = Http::get(url, o);
    if (!r.ok()) return "Errore: notizie non disponibili (" + (r.error.empty() ? "HTTP " + std::to_string(r.status) : r.error) + ").";
    std::vector<Html::FeedItem> items = Html::parseRss(Http::bodyUtf8(r));
    if (items.empty()) return "Nessuna notizia trovata.";
    if (items.size() > 8) items.resize(8);
    std::string out = topic.empty() ? "Ultime notizie:\n" : "Notizie su \"" + topic + "\":\n";
    for (const auto& it : items) {
        out += "- " + it.title;
        if (!it.source.empty()) out += " (" + it.source + (it.date.size() > 16 ? ", " + it.date.substr(0, 16) : "") + ")";
        out += "\n";
    }
    return out;
}

std::string Tools::weatherDescription(int code) {
    switch (code) {
    case 0: return "sereno";
    case 1: return "prevalentemente sereno";
    case 2: return "parzialmente nuvoloso";
    case 3: return "coperto";
    case 45: case 48: return "nebbia";
    case 51: return "pioggerella leggera";
    case 53: return "pioggerella";
    case 55: return "pioggerella intensa";
    case 56: case 57: return "pioggerella gelata";
    case 61: return "pioggia debole";
    case 63: return "pioggia moderata";
    case 65: return "pioggia forte";
    case 66: case 67: return "pioggia gelata";
    case 71: return "neve debole";
    case 73: return "neve moderata";
    case 75: return "neve forte";
    case 77: return "nevischio";
    case 80: return "rovesci deboli";
    case 81: return "rovesci";
    case 82: return "rovesci violenti";
    case 85: case 86: return "rovesci di neve";
    case 95: return "temporale";
    case 96: case 99: return "temporale con grandine";
    }
    return "condizioni non disponibili";
}
