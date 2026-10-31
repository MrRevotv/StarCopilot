#pragma once

#include <string>
#include <vector>

// Dalle pagine web al testo che il modello può leggere. Funzioni pure (niente
// rete): provate in tests/tests.cpp con pezzi di pagine vere.
namespace Html {

    struct SearchResult {
        std::string title;
        std::string url;
        std::string snippet;
    };

    // "&amp;" -> "&", "&egrave;" -> "è", "&#8217;" -> "’"...
    std::string decodeEntities(const std::string& s);
    // Toglie i tag e decodifica le entità (per titoli e frammenti brevi).
    std::string stripTags(const std::string& s);
    // Il testo principale di una pagina: senza script, menu, piè di pagina;
    // se c'è <article> o <main>, solo quello. Al massimo maxChars caratteri.
    std::string toText(const std::string& html, size_t maxChars);
    std::string title(const std::string& html);

    std::string urlEncode(const std::string& s);
    std::string urlDecode(const std::string& s);

    // I risultati di DuckDuckGo (html.duckduckgo.com e lite.duckduckgo.com),
    // senza le pubblicità. Gli indirizzi "duckduckgo.com/l/?uddg=..." diventano quelli veri.
    std::vector<SearchResult> parseDuckDuckGo(const std::string& html);
    std::vector<SearchResult> parseDuckDuckGoLite(const std::string& html);
    // I risultati di Bing (riserva quando DuckDuckGo rallenta chi cerca troppo spesso).
    // Gli indirizzi "bing.com/ck/a?...&u=a1<base64>" diventano quelli veri.
    std::vector<SearchResult> parseBing(const std::string& html);

    // La codifica dichiarata ("utf-8", "iso-8859-1"...), in minuscolo; vuoto se non c'è.
    std::string charset(const std::string& contentType, const std::string& html);

    // Le notizie di un feed RSS (Google News): titolo, fonte, data.
    struct FeedItem {
        std::string title;
        std::string source;
        std::string date;
        std::string link;
    };
    std::vector<FeedItem> parseRss(const std::string& xml);

} // namespace Html
