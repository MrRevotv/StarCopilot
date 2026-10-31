#include "html_utils.h"
#include "text_utils.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace Html {

namespace {
    std::string asciiLower(const std::string& s) {
        std::string out = s;
        for (char& c : out) c = (char)std::tolower((unsigned char)c);
        return out;
    }

    void appendUtf8(std::string& out, unsigned long cp) {
        if (cp == 0 || cp > 0x10FFFF) return;
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
        else { out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
    }

    struct Named { const char* name; unsigned long cp; };
    const Named kNamed[] = {
        { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' }, { "apos", '\'' }, { "nbsp", ' ' },
        { "agrave", 0xE0 }, { "aacute", 0xE1 }, { "egrave", 0xE8 }, { "eacute", 0xE9 }, { "igrave", 0xEC }, { "iacute", 0xED },
        { "ograve", 0xF2 }, { "oacute", 0xF3 }, { "ugrave", 0xF9 }, { "uacute", 0xFA }, { "Agrave", 0xC0 }, { "Egrave", 0xC8 },
        { "Eacute", 0xC9 }, { "Igrave", 0xCC }, { "Ograve", 0xD2 }, { "Ugrave", 0xD9 }, { "ccedil", 0xE7 }, { "ntilde", 0xF1 },
        { "auml", 0xE4 }, { "ouml", 0xF6 }, { "uuml", 0xFC }, { "Auml", 0xC4 }, { "Ouml", 0xD6 }, { "Uuml", 0xDC }, { "szlig", 0xDF },
        { "laquo", 0xAB }, { "raquo", 0xBB }, { "hellip", 0x2026 }, { "ndash", 0x2013 }, { "mdash", 0x2014 }, { "lsquo", 0x2018 },
        { "rsquo", 0x2019 }, { "ldquo", 0x201C }, { "rdquo", 0x201D }, { "euro", 0x20AC }, { "deg", 0xB0 }, { "copy", 0xA9 },
        { "reg", 0xAE }, { "trade", 0x2122 }, { "middot", 0xB7 }, { "bull", 0x2022 }, { "times", 0xD7 }, { "pound", 0xA3 },
        { "shy", 0 }, { "zwj", 0 }, { "zwnj", 0 },
    };

    // Il valore di un attributo dentro un tag: href="..." o href='...' o href=...
    std::string attribute(const std::string& tag, const char* name) {
        const std::string lower = asciiLower(tag);
        const std::string key = std::string(name) + "=";
        size_t pos = 0;
        while ((pos = lower.find(key, pos)) != std::string::npos) {
            // Deve essere un attributo intero ("data-href" no).
            if (pos > 0 && !std::isspace((unsigned char)lower[pos - 1])) { pos += key.size(); continue; }
            size_t v = pos + key.size();
            if (v >= tag.size()) return {};
            char quote = tag[v];
            if (quote == '"' || quote == '\'') {
                size_t end = tag.find(quote, v + 1);
                if (end == std::string::npos) return {};
                return tag.substr(v + 1, end - v - 1);
            }
            size_t end = v;
            while (end < tag.size() && !std::isspace((unsigned char)tag[end]) && tag[end] != '>') ++end;
            return tag.substr(v, end - v);
        }
        return {};
    }

    // L'indirizzo vero dietro il rinvio di DuckDuckGo.
    std::string realUrl(std::string href) {
        href = decodeEntities(href);
        size_t u = href.find("uddg=");
        if (u != std::string::npos) {
            size_t end = href.find('&', u);
            href = urlDecode(href.substr(u + 5, end == std::string::npos ? std::string::npos : end - u - 5));
        }
        if (TextUtils::startsWith(href, "//")) href = "https:" + href;
        return href;
    }

    bool isAd(const std::string& url) {
        return url.find("duckduckgo.com/y.js") != std::string::npos || url.find("ad_domain=") != std::string::npos ||
               url.find("ad_provider=") != std::string::npos;
    }

    // Toglie i blocchi <tag ...> ... </tag> (script, stili, menu...).
    void removeBlocks(std::string& html, std::string& lower, const char* tag) {
        const std::string open = std::string("<") + tag;
        const std::string close = std::string("</") + tag;
        size_t pos = 0;
        while ((pos = lower.find(open, pos)) != std::string::npos) {
            size_t after = pos + open.size();
            if (after < lower.size() && !(lower[after] == ' ' || lower[after] == '>' || lower[after] == '/' || lower[after] == '\n' ||
                                          lower[after] == '\t' || lower[after] == '\r')) {
                pos = after;
                continue;
            }
            size_t end = lower.find(close, after);
            if (end == std::string::npos) end = lower.size();
            else {
                size_t gt = lower.find('>', end);
                end = gt == std::string::npos ? lower.size() : gt + 1;
            }
            html.erase(pos, end - pos);
            lower.erase(pos, end - pos);
        }
    }

    // Il contenuto del primo <tag> ... </tag>, se c'è.
    bool innerOf(const std::string& html, const std::string& lower, const char* tag, std::string& out) {
        const std::string open = std::string("<") + tag;
        size_t pos = lower.find(open);
        while (pos != std::string::npos) {
            size_t after = pos + open.size();
            if (after < lower.size() && (lower[after] == ' ' || lower[after] == '>')) break;
            pos = lower.find(open, after);
        }
        if (pos == std::string::npos) return false;
        size_t start = lower.find('>', pos);
        if (start == std::string::npos) return false;
        size_t end = lower.find(std::string("</") + tag, start);
        if (end == std::string::npos) end = lower.size();
        out = html.substr(start + 1, end - start - 1);
        return true;
    }

    std::string cutAt(const std::string& text, size_t maxChars) {
        if (text.size() <= maxChars) return text;
        size_t cut = maxChars;
        // Non a metà di un carattere UTF-8.
        while (cut > 0 && ((unsigned char)text[cut] & 0xC0) == 0x80) --cut;
        size_t space = text.rfind(' ', cut);
        if (space != std::string::npos && space > maxChars * 3 / 4) cut = space;
        return text.substr(0, cut) + " [...]";
    }
}

std::string decodeEntities(const std::string& s) {
    if (s.find('&') == std::string::npos) return s;
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') { out += s[i]; continue; }
        size_t semi = s.find(';', i + 1);
        if (semi == std::string::npos || semi - i > 10) { out += '&'; continue; }
        std::string name = s.substr(i + 1, semi - i - 1);
        bool done = false;
        if (!name.empty() && name[0] == '#') {
            unsigned long cp = name.size() > 1 && (name[1] == 'x' || name[1] == 'X') ? std::strtoul(name.c_str() + 2, nullptr, 16)
                                                                                      : std::strtoul(name.c_str() + 1, nullptr, 10);
            if (cp == 0xA0) cp = ' ';
            appendUtf8(out, cp);
            done = true;
        } else {
            for (const Named& n : kNamed) {
                if (name == n.name) {
                    appendUtf8(out, n.cp);
                    done = true;
                    break;
                }
            }
        }
        if (done) i = semi;
        else out += '&';
    }
    return out;
}

std::string stripTags(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    bool inTag = false;
    for (char c : s) {
        if (c == '<') inTag = true;
        else if (c == '>' && inTag) inTag = false;
        else if (!inTag) out += c;
    }
    return TextUtils::collapseSpaces(decodeEntities(out));
}

std::string title(const std::string& html) {
    std::string inner;
    if (!innerOf(html, asciiLower(html), "title", inner)) return {};
    return stripTags(inner);
}

std::string toText(const std::string& input, size_t maxChars) {
    std::string html = input;
    std::string lower = asciiLower(html);

    // Commenti <!-- ... -->
    for (size_t pos; (pos = lower.find("<!--")) != std::string::npos;) {
        size_t end = lower.find("-->", pos + 4);
        end = end == std::string::npos ? lower.size() : end + 3;
        html.erase(pos, end - pos);
        lower.erase(pos, end - pos);
    }
    for (const char* tag : { "script", "style", "noscript", "svg", "head", "nav", "footer", "header", "aside", "form", "iframe",
                             "button", "select", "template", "figure" }) {
        removeBlocks(html, lower, tag);
    }

    // Se c'è un articolo, solo quello (purché abbia del testo vero).
    for (const char* tag : { "article", "main" }) {
        std::string inner;
        if (innerOf(html, lower, tag, inner) && stripTags(inner).size() > 400) {
            html = inner;
            lower = asciiLower(html);
            break;
        }
    }

    // I tag di blocco diventano "a capo", gli altri spariscono.
    std::string text;
    text.reserve(html.size() / 2);
    for (size_t i = 0; i < html.size(); ++i) {
        if (html[i] != '<') { text += html[i]; continue; }
        size_t end = html.find('>', i);
        if (end == std::string::npos) break;
        std::string tag = lower.substr(i + 1, std::min<size_t>(end - i - 1, 12));
        if (!tag.empty() && tag[0] == '/') tag = tag.substr(1);
        size_t stop = tag.find_first_of(" />\t\r\n");
        if (stop != std::string::npos) tag = tag.substr(0, stop);
        static const char* const blocks[] = { "p", "div", "br", "li", "h1", "h2", "h3", "h4", "h5", "h6", "tr", "section", "ul", "ol",
                                              "table", "blockquote", "dd", "dt", "td", "th", "article", "hr", "pre" };
        bool block = std::any_of(std::begin(blocks), std::end(blocks), [&](const char* b) { return tag == b; });
        text += block ? '\n' : ' ';
        i = end;
    }
    text = decodeEntities(text);

    // Righe pulite. Via quelle che sembrano menu: senza punteggiatura e corte,
    // o piene di Parole Con La Maiuscola ("Classifica Categorie Notizie Video").
    auto looksLikeMenu = [](const std::string& line) {
        if (line.find_first_of(".!?:;") != std::string::npos && line.size() >= 30) return false;
        if (line.size() < 60) return true;
        int words = 0, capitals = 0;
        bool start = true;
        for (unsigned char c : line) {
            if (c == ' ') { start = true; continue; }
            if (start) {
                words++;
                if (std::isupper(c)) capitals++;
                start = false;
            }
        }
        return words > 0 && capitals * 100 / words > 35;
    };
    std::string out;
    for (const std::string& raw : TextUtils::splitLines(text)) {
        std::string line = TextUtils::collapseSpaces(raw);
        if (looksLikeMenu(line)) continue;
        if (!out.empty()) out += '\n';
        out += line;
        if (out.size() > maxChars + 200) break;
    }
    return cutAt(out, maxChars);
}

std::string urlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += (char)c;
        else if (c == ' ') out += '+';
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    return out;
}

std::string urlDecode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') out += ' ';
        else if (s[i] == '%' && i + 2 < s.size() && std::isxdigit((unsigned char)s[i + 1]) && std::isxdigit((unsigned char)s[i + 2])) {
            out += (char)std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

std::vector<SearchResult> parseDuckDuckGo(const std::string& html) {
    std::vector<SearchResult> results;
    const std::string marker = "class=\"result__a\"";
    size_t pos = 0;
    while ((pos = html.find(marker, pos)) != std::string::npos) {
        size_t tagStart = html.rfind("<a", pos);
        size_t tagEnd = html.find('>', pos);
        if (tagStart == std::string::npos || tagEnd == std::string::npos) break;
        size_t close = html.find("</a>", tagEnd);
        if (close == std::string::npos) break;
        SearchResult r;
        r.url = realUrl(attribute(html.substr(tagStart, tagEnd - tagStart), "href"));
        r.title = stripTags(html.substr(tagEnd + 1, close - tagEnd - 1));

        // Il frammento di testo: prima del risultato dopo.
        size_t next = html.find(marker, close);
        size_t snip = html.find("class=\"result__snippet\"", close);
        if (snip != std::string::npos && (next == std::string::npos || snip < next)) {
            size_t sStart = html.find('>', snip);
            size_t sEnd = sStart == std::string::npos ? std::string::npos : html.find("</a>", sStart);
            size_t sEndDiv = sStart == std::string::npos ? std::string::npos : html.find("</div>", sStart);
            if (sEnd == std::string::npos || (sEndDiv != std::string::npos && sEndDiv < sEnd)) sEnd = sEndDiv;
            if (sStart != std::string::npos && sEnd != std::string::npos) r.snippet = stripTags(html.substr(sStart + 1, sEnd - sStart - 1));
        }
        if (!r.url.empty() && !isAd(r.url) && !r.title.empty()) results.push_back(r);
        pos = close;
    }
    return results;
}

std::vector<SearchResult> parseDuckDuckGoLite(const std::string& html) {
    std::vector<SearchResult> results;
    size_t pos = 0;
    while (true) {
        size_t a = html.find("result-link", pos);
        if (a == std::string::npos) break;
        size_t tagStart = html.rfind("<a", a);
        size_t tagEnd = html.find('>', a);
        if (tagStart == std::string::npos || tagEnd == std::string::npos) break;
        size_t close = html.find("</a>", tagEnd);
        if (close == std::string::npos) break;
        SearchResult r;
        r.url = realUrl(attribute(html.substr(tagStart, tagEnd - tagStart), "href"));
        r.title = stripTags(html.substr(tagEnd + 1, close - tagEnd - 1));
        size_t next = html.find("result-link", close);
        size_t snip = html.find("result-snippet", close);
        if (snip != std::string::npos && (next == std::string::npos || snip < next)) {
            size_t sStart = html.find('>', snip);
            size_t sEnd = sStart == std::string::npos ? std::string::npos : html.find("</td>", sStart);
            if (sStart != std::string::npos && sEnd != std::string::npos) r.snippet = stripTags(html.substr(sStart + 1, sEnd - sStart - 1));
        }
        if (!r.url.empty() && !isAd(r.url) && !r.title.empty()) results.push_back(r);
        pos = close;
    }
    return results;
}

namespace {
    std::string base64UrlDecode(std::string s) {
        for (char& c : s) {
            if (c == '-') c = '+';
            else if (c == '_') c = '/';
        }
        auto value = [](char c) -> int {
            if (c >= 'A' && c <= 'Z') return c - 'A';
            if (c >= 'a' && c <= 'z') return c - 'a' + 26;
            if (c >= '0' && c <= '9') return c - '0' + 52;
            if (c == '+') return 62;
            if (c == '/') return 63;
            return -1;
        };
        std::string out;
        int buffer = 0, bits = 0;
        for (char c : s) {
            int v = value(c);
            if (v < 0) break; // "=" o fine
            buffer = (buffer << 6) | v;
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                out += (char)((buffer >> bits) & 0xFF);
            }
        }
        return out;
    }

    std::string bingUrl(std::string href) {
        href = decodeEntities(href);
        if (href.find("bing.com/ck/a") != std::string::npos) {
            size_t u = href.find("&u=a1");
            if (u == std::string::npos) u = href.find("?u=a1");
            if (u != std::string::npos) {
                size_t start = u + 5;
                size_t end = href.find('&', start);
                std::string decoded = base64UrlDecode(href.substr(start, end == std::string::npos ? std::string::npos : end - start));
                if (TextUtils::startsWith(decoded, "http")) return decoded;
            }
            return {};
        }
        return href;
    }
}

std::vector<SearchResult> parseBing(const std::string& html) {
    std::vector<SearchResult> results;
    const std::string marker = "class=\"b_algo\"";
    size_t pos = html.find(marker);
    while (pos != std::string::npos) {
        size_t next = html.find(marker, pos + marker.size());
        const std::string block = html.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
        pos = next;

        size_t h2 = block.find("<h2");
        if (h2 == std::string::npos) continue;
        size_t a = block.find("<a ", h2);
        if (a == std::string::npos) continue;
        size_t aEnd = block.find('>', a);
        size_t close = aEnd == std::string::npos ? std::string::npos : block.find("</a>", aEnd);
        if (close == std::string::npos) continue;
        SearchResult r;
        r.url = bingUrl(attribute(block.substr(a, aEnd - a), "href"));
        r.title = stripTags(block.substr(aEnd + 1, close - aEnd - 1));
        size_t caption = block.find("class=\"b_caption\"");
        if (caption == std::string::npos) caption = block.find("b_lineclamp");
        if (caption != std::string::npos) {
            size_t p = block.find("<p", caption);
            size_t pStart = p == std::string::npos ? std::string::npos : block.find('>', p);
            size_t pEnd = pStart == std::string::npos ? std::string::npos : block.find("</p>", pStart);
            if (pEnd != std::string::npos) r.snippet = stripTags(block.substr(pStart + 1, pEnd - pStart - 1));
        }
        if (!r.url.empty() && !r.title.empty()) results.push_back(r);
    }
    return results;
}

std::string charset(const std::string& contentType, const std::string& html) {
    auto after = [](const std::string& lower, size_t pos) {
        size_t start = pos;
        while (start < lower.size() && (lower[start] == '"' || lower[start] == '\'' || lower[start] == ' ')) ++start;
        size_t end = start;
        while (end < lower.size() && (std::isalnum((unsigned char)lower[end]) || lower[end] == '-' || lower[end] == '_')) ++end;
        return lower.substr(start, end - start);
    };
    std::string ct = asciiLower(contentType);
    size_t p = ct.find("charset=");
    if (p != std::string::npos) return after(ct, p + 8);
    std::string head = asciiLower(html.substr(0, std::min<size_t>(html.size(), 4096)));
    p = head.find("charset=");
    if (p != std::string::npos) return after(head, p + 8);
    return {};
}

std::vector<FeedItem> parseRss(const std::string& xml) {
    auto element = [](const std::string& block, const char* tag) -> std::string {
        const std::string open = std::string("<") + tag;
        size_t start = block.find(open);
        if (start == std::string::npos) return {};
        size_t gt = block.find('>', start);
        if (gt == std::string::npos) return {};
        size_t end = block.find(std::string("</") + tag + ">", gt);
        if (end == std::string::npos) return {};
        std::string value = block.substr(gt + 1, end - gt - 1);
        // <![CDATA[ ... ]]>
        if (TextUtils::startsWith(value, "<![CDATA[")) {
            size_t close = value.rfind("]]>");
            value = value.substr(9, close == std::string::npos ? std::string::npos : close - 9);
        }
        return stripTags(value);
    };

    std::vector<FeedItem> items;
    size_t pos = 0;
    while ((pos = xml.find("<item>", pos)) != std::string::npos) {
        size_t end = xml.find("</item>", pos);
        if (end == std::string::npos) break;
        const std::string block = xml.substr(pos, end - pos);
        FeedItem item;
        item.title = element(block, "title");
        item.source = element(block, "source");
        item.date = element(block, "pubDate");
        item.link = element(block, "link");
        // Google News aggiunge " - Fonte" al titolo: la fonte c'è già a parte.
        if (!item.source.empty()) {
            const std::string suffix = " - " + item.source;
            if (item.title.size() > suffix.size() && item.title.compare(item.title.size() - suffix.size(), suffix.size(), suffix) == 0) {
                item.title.resize(item.title.size() - suffix.size());
            }
        }
        if (!item.title.empty()) items.push_back(item);
        pos = end;
    }
    return items;
}

} // namespace Html
