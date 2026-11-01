#pragma once

#include <string>
#include <vector>

// Mining di Star Citizen: dalla "signature" letta sullo scanner (il numero
// accanto al segnaposto, es. 16,960) a che cosa c'è sotto (Copper × 4).
//
// Ogni materiale ha una firma base; una signature è sempre firma × quantità
// (quante rocce, o quanti pezzi di salvage). Il calcolo è quello di Mining
// Signature Calculator (divisibilità esatta), con la tabella di mining_ocr
// (Scrap del salvage compreso) e in più:
//  - le letture dell'OCR "sporche" ("2 16,960" col segnaposto letto come 2,
//    "16.96O", "l6 960") si ripuliscono;
//  - se il numero non torna con nessun materiale si prova a correggere UNA
//    cifra confusa dall'OCR (8/3/6/0/9, 1/7, 5/6): si accetta solo se la
//    correzione possibile è una sola, e il risultato è segnato "probabile".
// Tutto puro: niente finestre né file (le prove lo usano direttamente).
namespace Mining {

    struct Material {
        std::string name;   // "Copper (Rame)"
        int base = 0;       // 4240
        std::string kind;   // "minerale" o "salvage"
        bool operator==(const Material& o) const { return name == o.name && base == o.base && kind == o.kind; }
    };

    // La tabella che c'è dentro il programma (al primo avvio diventa il file modificabile).
    // Tipi: "minerale" (rocce, 1..quantità massima), "salvage" (rottami), "roc" e "fps"
    // (depositi a terra: la signature dice quanti, fino a 8, non quale minerale).
    std::vector<Material> defaultMaterials();
    // Solo i depositi a terra (per aggiungerli alle tabelle salvate prima che esistessero).
    std::vector<Material> groundDeposits();

    // Rarità (1 comune ... 5 leggendario) e minerali secondari di un minerale da nave,
    // dalla tabella di Citizen Assistant (SCMINER). nullptr se il nome non è noto.
    struct OreInfo {
        const char* name;      // inglese, come nel gioco
        int rarity;
        const char* secondary; // "" = nessuno
    };
    const OreInfo* oreInfo(const std::string& materialName);
    std::string rarityName(int rarity); // "EPICO"...

    // Tabella in testo: una riga per materiale, "NOME,FIRMA[,TIPO]" (anche ';' o tab).
    // Righe vuote, commenti (#) e intestazioni ("name,base_signature") si saltano.
    // errors: le righe scartate, con il motivo.
    std::vector<Material> parseTable(const std::string& text, std::vector<std::string>* errors = nullptr);
    std::string formatTable(const std::vector<Material>& materials);

    // Il file "materialsdb" di Mining Signature Calculator (MSCDB1 + XOR + zlib)
    // -> CSV in chiaro. false se non è quel formato o è rovinato.
    bool decodeMscDatabase(const std::string& bytes, std::string& csvOut);

    struct Match {
        std::string name;
        std::string kind;
        int base = 0;
        int quantity = 0;
    };

    // Tutti i materiali con firma × quantità == value (quantità 1..maxQuantity),
    // dalla quantità più piccola.
    std::vector<Match> findMatches(int value, const std::vector<Material>& materials, int maxQuantity = 40);

    struct Decoded {
        bool hasValue = false;     // si è trovato un numero
        int value = 0;             // la signature scelta
        int readValue = 0;         // il numero letto, prima di un'eventuale correzione
        bool corrected = false;    // valore corretto (segnaposto tolto, cifra illeggibile o confusa): "probabile"
        bool digitFix = false;     // corretta una cifra letta pulita ma simile (8/0/3...): la più incerta
        std::string note;          // perché è stato corretto, o i dubbi
        std::vector<Match> matches;
        std::vector<int> candidates; // i numeri trovati nel testo (per la diagnosi)
    };

    // I numeri plausibili nel testo dell'OCR (separatori delle migliaia
    // compresi, lettere scambiate per cifre ripulite), nell'ordine.
    std::vector<int> numbersInText(const std::string& text);

    Decoded decodeText(const std::string& ocrText, const std::vector<Material>& materials, int maxQuantity = 40);
    Decoded decodeValue(int value, const std::vector<Material>& materials, int maxQuantity = 40);

    // "16960" -> "16.960"
    std::string formatValue(int value);
    // "Copper (Rame) × 4" e simili, per il riquadro e la conversazione.
    std::string describeMatch(const Match& m);
    // Una riga sola: "16.960 = Copper (Rame) × 4" / "16.960: sconosciuta".
    std::string summary(const Decoded& d);

    // --- Voce --------------------------------------------------------------------------
    // "Iris, che minerale è?" / "scansiona" -> leggi lo schermo;
    // "Iris, signature 16960" / "firma sedicimila novecentosessanta" -> quel valore.
    enum class VoiceKind { None, Scan, Value };
    struct VoiceCommand {
        VoiceKind kind = VoiceKind::None;
        int value = 0;
    };
    VoiceCommand parseVoice(const std::string& command);

    // Numeri in lettere (italiano): "sedicimila novecentosessanta" -> 16960.
    // -1 se non è un numero.
    long long parseItalianNumber(const std::string& words);

} // namespace Mining
