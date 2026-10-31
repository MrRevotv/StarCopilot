#pragma once

#include <string>
#include <vector>

// Nomi propri di Star Citizen (pianeti, lune, città, stazioni, fazioni):
//  1) suggeriti a whisper, così li riconosce invece di storpiarli
//     ("sono atterrato a Kareah" non diventa "sono a terra");
//  2) all'LLM si dice di lasciare invariati quelli che compaiono nella frase.
// Aggiungi liberamente i nomi che usi di più.
inline const std::vector<std::string>& scKnownLocations() {
    static const std::vector<std::string> locations = {
        // Stanton: pianeti e lune
        "Stanton", "Crusader", "Hurston", "ArcCorp", "microTech",
        "Cellin", "Daymar", "Yela", "Lyria", "Wala", "Aberdeen", "Arial", "Magda", "Ita",
        "Calliope", "Clio", "Euterpe",
        // Città
        "Orison", "Lorville", "Area18", "New Babbage",
        // Stazioni e luoghi noti
        "Kareah", "Port Tressler", "Everus Harbor", "Baijini Point", "Seraphim Station",
        "Grim HEX", "Port Olisar", "Jumptown", "Klescher",
        "CRU-L1", "CRU-L4", "CRU-L5", "ARC-L1", "ARC-L2", "HUR-L1", "HUR-L2", "HUR-L3",
        "MIC-L1", "MIC-L2", "MIC-L5",
        // Pyro e Nyx
        "Pyro", "Ruin Station", "Checkmate Station", "Nyx", "Levski",
        // Fazioni citate spesso in chat
        "UEE", "Advocacy", "Crusader Security",
    };
    return locations;
}

// Navi, costruttori e oggetti di Star Citizen: all'LLM si dice di lasciarli
// com'erano ("the galaxy with a cargo module" non diventa "la galassia").
// Non a whisper: il suggerimento ha posto per poche centinaia di parole.
// Fuori le navi con nomi di tutti i giorni (Pulse, Blade, Spirit, Arrow...),
// che in una frase normale andrebbero tradotti.
inline const std::vector<std::string>& scKnownShips() {
    static const std::vector<std::string> ships = {
        // Costruttori
        "Aegis", "Anvil", "Argo", "Drake", "Esperia", "Gatac", "Greycat", "Kruger", "MISC", "Mirai",
        "Origin", "RSI", "Tumbril", "Aopoa", "Banu",
        // Navi
        "Aurora", "Avenger", "Buccaneer", "Caterpillar", "Carrack", "Clipper", "Constellation", "Corsair", "Cutlass",
        "Cutter", "Cyclone", "Eclipse", "Freelancer", "Galaxy", "Gladius", "Hammerhead", "Hercules", "Hornet",
        "Hull C", "Idris", "Ironclad", "Javelin", "Kraken", "Liberator", "Mercury Star Runner", "Mole", "Mustang",
        "Nursa", "Paladin", "Perseus", "Pisces", "Polaris", "Prospector", "Prowler", "Reclaimer", "Redeemer",
        "Retaliator", "Sabre", "Scorpius", "Starfarer", "Starlancer", "Syulen", "Terrapin", "Ursa", "Valkyrie",
        "Vanguard", "Vulture", "Zeus", "C2", "M2", "A2", "C8", "C8X",
        // Oggetti e sigle del gioco
        "medbed", "undersuit", "CCU", "aUEC", "SCU",
    };
    return ships;
}
