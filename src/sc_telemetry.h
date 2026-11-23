#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

// La telemetria locale di Star Citizen. Dalla 4.10.2 / 5.0 il gioco ha un server gRPC sul PC
// (127.0.0.1:2942, senza TLS) che manda lo stato di nave e personaggio circa 20 volte al secondo
// (GameTelemetryService/TelemetryStream). Va acceso dal giocatore, in user.cfg:
//     grpc.client.server.enabled = 1
//     server.game.telemetry.enabled = 1
// Iris la legge per sapere com'è la nave prima di premere un tasto che fa avanti e indietro (luci,
// armi, scudi, carrello...): "accendi le armi" con le armi già accese non le spegne più.
// SOLO LETTURA: Iris non usa TriggerGameAction (con la telemetria accesa qualunque programma può
// far partire l'autodistruzione: Iris non chiama mai quelle azioni).
//
// Niente librerie gRPC: un client HTTP/2 minimo (h2c, Winsock) e il protobuf letto a mano
// (package sc.game.telemetry.v1, schema estratto dall'exe; i numeri dei campi sono in
// sc_telemetry.cpp). I campi sconosciuti si saltano: una patch che aggiunge campi non rompe niente.
namespace ScTelemetry {

    // Carrello, VTOL, trasformazione (VehicleComponentDeploymentState).
    enum class Deploy { Unknown, Retracted, Deploying, Deployed, Retracting };
    enum class MasterMode { Unknown, Navigation, Scm, Stealth };
    enum class Locomotion { Unknown, OnFoot, Eva, Seated };
    enum class CrewRole { Unknown, Pilot, Gunner, Crew };

    // Un'impostazione di volo: c'è su questa nave (enabled) ed è accesa (active).
    struct Availability {
        bool enabled = false;
        bool active = false;
    };

    // Un frame decodificato. Le sezioni che il gioco non manda restano has* = false (a piedi arriva solo
    // "actor"; la nave compare circa 6 s dopo essersi seduti al posto di pilotaggio).
    struct ShipState {
        // --- vehicle.state ---
        bool hasVehicle = false;
        std::string vehicleName;   // "AEGS_Gladius_200000010670" (con l'ID dell'entità)
        std::string className;     // "AEGS_Gladius": il modello
        bool lightsOn = false;
        float health = 0.0f;       // 0..1

        bool hasPower = false;     // vehicle.state.power
        bool powerPlants = false, engines = false, weapons = false, coolers = false, lifeSupport = false;
        bool gravityGenerators = false, sensors = false, shieldGenerators = false, shieldEmitters = false;

        bool hasFlightState = false; // vehicle.state.flight.state
        bool isLanded = false;
        Deploy gear = Deploy::Unknown;   // affidabile (DEPLOYED -> RETRACTING -> RETRACTED -> DEPLOYING)
        Deploy vtol = Deploy::Unknown;   // NON affidabile: DEPLOYED anche su navi senza VTOL

        bool hasFlightSettings = false;  // vehicle.state.flight.settings
        Availability coupled, esp, gSafe, cruise, speedLimiter, proximityAssist;
        bool lightAmplification = false; // visione notturna del pilota

        bool hasInstruments = false;     // vehicle.state.flight.instruments
        float speed = 0.0f;              // m/s
        float hydrogenFuel = 0.0f;       // 0..1
        float quantumFuel = 0.0f;        // spesso sempre 0 (non popolato)
        bool boostEngaged = false;

        bool hasShields = false;
        float shieldLevel = 0.0f;        // 0..1 (0 a scudi spenti)

        bool hasMasterMode = false;
        MasterMode masterMode = MasterMode::Unknown;
        bool spooled = false;            // false per qualche secondo dopo il cambio NAV/SCM

        // --- weapons ---
        bool hasWeapons = false;
        bool gimbalsLocked = false;
        int weaponCount = 0;
        int weaponsPowered = 0;
        int decoys = -1, noise = -1;     // contromisure; -1 = non arrivate (il primo frame dà 0)

        // --- actor ---
        bool hasActor = false;
        Locomotion locomotion = Locomotion::Unknown;
        CrewRole crewRole = CrewRole::Unknown;
        std::string seatName;
        float actorHealth = 0.0f, actorHealthMax = 0.0f;
        float oxygen = 0.0f;
        long long uec = 0;
    };

    // Un TelemetryStreamResponse: i byte del messaggio protobuf, SENZA i 5 byte dell'intestazione gRPC.
    // I campi sconosciuti si saltano. false = protobuf rotto (out non è affidabile).
    bool decodeFrame(const uint8_t* data, size_t size, ShipState& out);

    class Client {
    public:
        Client();
        ~Client();
        Client(const Client&) = delete;
        Client& operator=(const Client&) = delete;

        // Un thread che si collega al gioco e si ricollega da solo (ogni qualche secondo se il gioco è
        // chiuso o la telemetria è spenta: un tentativo su 127.0.0.1 costa niente). 2942 = la porta
        // predefinita del gioco. start() due volte = una sola connessione; stop() aspetta il thread.
        void start(int port = 2942);
        void stop();
        bool running() const;

        // Lo stato del gioco ADESSO: l'ultimo frame, se arrivato da al massimo maxAgeMs e se il flusso
        // non è "congelato" (al menu, e per qualche secondo al caricamento, il gioco ripete sempre lo
        // stesso frame identico byte per byte: dopo circa 2 s di frame identici non vale più).
        // nullopt = non si sa (gioco chiuso, telemetria spenta, menu): chi chiede fa come senza.
        std::optional<ShipState> current(int maxAgeMs = 1500) const;

        // Quanti frame sono arrivati finora (cresce a ogni frame): per aspettare un frame più nuovo.
        uint64_t frameCount() const;

        // Collegati al server e arrivano frame.
        bool connected() const;
        // Per la pagina e il registro, in italiano: "Collegata: AEGS_Gladius", "Collegata (a piedi)",
        // "Collegata, gioco al menu", "Non raggiungibile su 127.0.0.1:2942 (telemetria spenta?)"...
        std::string statusText() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> m;
    };

} // namespace ScTelemetry

// --- Solo per le prove (iris_tests) ----------------------------------------------------------------
// I pezzi del protocollo senza rete, per provarli da soli. Il resto di Iris non li usa: usa Client.
#include <vector>

namespace ScTelemetry::detail {

    // Il blocco HPACK della richiesta (:method POST, :scheme http, :path ..., :authority 127.0.0.1:porta,
    // content-type application/grpc, te trailers): letterali "senza indicizzazione", senza Huffman.
    std::string requestHeaderBlock(int port);

    // Tutto quello che Iris manda appena collegata: preambolo HTTP/2, SETTINGS, WINDOW_UPDATE della
    // connessione, HEADERS della richiesta e DATA col messaggio gRPC vuoto (END_STREAM), sullo stream 1.
    std::string clientOpening(int port);

    // I messaggi gRPC rimessi insieme dai pezzi dei DATA: 1 byte di compressione, 4 di lunghezza
    // (big-endian), poi il messaggio. I compressi e quelli troppo grandi si saltano senza tenerli in memoria.
    class GrpcReassembler {
    public:
        size_t maxMessage = 4u << 20;
        // Aggiunge i byte di un DATA; i messaggi interi finiscono in fondo a messages.
        void feed(const uint8_t* data, size_t size, std::vector<std::string>& messages);
        int skipped() const { return skipped_; }
        bool midMessage() const { return headerHave_ > 0 || remaining_ > 0; }

    private:
        uint8_t header_[5] = {};
        size_t headerHave_ = 0;
        size_t remaining_ = 0;
        bool keep_ = false;
        std::string body_;
        int skipped_ = 0;
    };

    // La parte di HTTP/2 che riceve. Si danno i byte arrivati dal server; escono i messaggi gRPC dello
    // stream 1 e i byte da rimandare (ACK di SETTINGS e PING, WINDOW_UPDATE per i byte consumati).
    class Http2Receiver {
    public:
        static constexpr uint32_t kMaxFrame = 16384; // non lo cambiamo nei SETTINGS: è il valore predefinito

        // false = connessione finita (GOAWAY, RST_STREAM, fine dello stream, errore): il motivo è in closeReason.
        // I messaggi arrivati prima della fine restano in messages.
        bool feed(const uint8_t* data, size_t size);

        std::vector<std::string> messages; // protobuf senza i 5 byte gRPC; li svuota chi li legge
        std::string reply;                 // da mandare al server; lo svuota chi lo manda
        std::string closeReason;
        int skippedMessages() const { return grpc_.skipped(); }

    private:
        bool frame(uint8_t type, uint8_t flags, uint32_t stream, const uint8_t* p, uint32_t len);
        bool close(std::string reason);

        std::string buffer_;
        GrpcReassembler grpc_;
        bool closed_ = false;
        bool inHeaders_ = false;     // HEADERS senza END_HEADERS: ora vengono solo CONTINUATION
        uint32_t headersStream_ = 0;
        bool endAfterHeaders_ = false;
        uint64_t consumedConn_ = 0, consumedStream_ = 0;
    };

    // I frame "congelati" del menu: gli stessi byte, uno dopo l'altro, da almeno frozenMs.
    class FreezeWatch {
    public:
        explicit FreezeWatch(int64_t ms = 2000) : frozenMs(ms) {}
        int64_t frozenMs;
        // Un frame arrivato all'istante ms (millisecondi di un orologio qualsiasi che va avanti).
        void add(const std::string& bytes, int64_t ms);
        bool frozen() const { return any_ && latest_ - since_ >= frozenMs; }
        void reset() { *this = FreezeWatch(frozenMs); }

    private:
        std::string last_;
        int64_t since_ = 0, latest_ = 0;
        bool any_ = false;
    };

} // namespace ScTelemetry::detail — solo per le prove
