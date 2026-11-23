#include "sc_telemetry.h"
#include "log.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <utility>

// Numeri dei campi (package sc.game.telemetry.v1, schema estratto dall'exe di Star Citizen 5.0 PTU).
// Solo quelli che Iris legge; gli altri si saltano.
//   TelemetryStreamResponse: vehicle 3, weapons 4, events 5, actor 6, force_feedback 7, turret 8
//   VehicleFrame: state 2
//   VehicleState: name 1, class_name 2, lights_enabled 5, flight 8, power 9, shields 10, health 11 (float),
//                 master_mode 12
//   VehicleFlight: state 1, settings 2, instruments 3
//   VehicleFlightState: is_landed 1, gear 2, vtol 3 (VehicleComponentDeploymentState)
//   VehicleFlightSettings: ifcs_coupled 1, ifcs_esp 2, ifcs_g_safe 4, ifcs_cruise_mode 5, ifcs_speed_limiter 7,
//                          ifcs_proximity_assist 8 (Availability: enabled 1, active 2),
//                          flight_light_amplification 19 (bool)
//   VehicleFlightInstruments: speed 1, hydrogen_fuel 2, quantum_fuel 3 (float), boost 5 (engaged 1)
//   VehiclePowerState: powerplants 1, engines 2, weapons 3, coolers 4, life_support 5, gravity_generators 6,
//                      sensors 7, shield_generators 8, shield_emitters 9
//   VehicleShields: level 1 (float)
//   VehicleMasterModeState: mode 1 (NAVIGATION 1, SCM 2, STEALTH 3), spooled 2
//   WeaponsFrame: settings 1 (gimbals_locked 8), weapons 4 (WeaponState: is_powered 2),
//                 countermeasures 5 (type 1: DECOY 1, NOISE 2; amount 2)
//   ActorFrame: vitals 1, state 2
//   ActorVitals: health 1, oxygen 2 (Gauge: current 1, max 2)
//   ActorState: locomotion 1, crew_role 2, seat_name 3, uec_balance 4 (int64)

namespace ScTelemetry {

namespace {
    // --- Protobuf letto a mano -------------------------------------------------------------------

    enum Wire { kVarint = 0, kFixed64 = 1, kBytes = 2, kGroupStart = 3, kGroupEnd = 4, kFixed32 = 5 };

    // Un pezzo di buffer che si legge in avanti.
    struct Pb {
        const uint8_t* p = nullptr;
        const uint8_t* end = nullptr;

        bool atEnd() const { return p >= end; }

        bool varint(uint64_t& v) {
            v = 0;
            for (int shift = 0; shift < 70; shift += 7) {
                if (p >= end) return false;
                const uint8_t b = *p++;
                if (shift < 64) v |= (uint64_t)(b & 0x7F) << shift;
                if (!(b & 0x80)) return true;
            }
            return false; // più di 10 byte: rotto
        }

        // n byte little-endian (4 = fixed32, 8 = fixed64).
        bool fixed(size_t n, uint64_t& v) {
            if ((size_t)(end - p) < n) return false;
            v = 0;
            for (size_t i = 0; i < n; ++i) v |= (uint64_t)p[i] << (8 * i);
            p += n;
            return true;
        }
    };

    // Un campo letto: il numero, il tipo e il valore (varint e fixed) o i byte (length-delimited).
    struct Field {
        uint32_t num = 0;
        int wire = 0;
        uint64_t value = 0;
        Pb bytes;

        bool isVarint() const { return wire == kVarint; }
        bool isFloat() const { return wire == kFixed32; }
        bool isBytes() const { return wire == kBytes; }
        bool flag() const { return value != 0; }
        float asFloat() const {
            const uint32_t bits = (uint32_t)value;
            float f;
            std::memcpy(&f, &bits, sizeof(f));
            return f;
        }
        std::string text() const { return std::string((const char*)bytes.p, (size_t)(bytes.end - bytes.p)); }
    };

    bool readField(Pb& in, Field& f, int depth = 0);

    // Un gruppo (protobuf vecchio, proto3 non li usa): si salta fino alla sua fine.
    bool skipGroup(Pb& in, uint32_t num, int depth) {
        if (depth > 32) return false;
        Field inner;
        while (!in.atEnd()) {
            if (!readField(in, inner, depth + 1)) return false;
            if (inner.wire == kGroupEnd) return inner.num == num;
        }
        return false; // gruppo senza fine
    }

    bool readField(Pb& in, Field& f, int depth) {
        uint64_t key;
        if (!in.varint(key)) return false;
        if ((key >> 3) == 0 || (key >> 3) > 0x1FFFFFFF) return false;
        f.num = (uint32_t)(key >> 3);
        f.wire = (int)(key & 7);
        f.value = 0;
        f.bytes = {};
        switch (f.wire) {
        case kVarint: return in.varint(f.value);
        case kFixed64: return in.fixed(8, f.value);
        case kFixed32: return in.fixed(4, f.value);
        case kBytes: {
            uint64_t n;
            if (!in.varint(n) || n > (uint64_t)(in.end - in.p)) return false;
            f.bytes = { in.p, in.p + n };
            in.p += n;
            return true;
        }
        case kGroupStart: return skipGroup(in, f.num, depth);
        case kGroupEnd: return depth > 0; // solo dentro un gruppo
        default: return false;            // i tipi 6 e 7 non esistono
        }
    }

    // Chiama fn per ogni campo del messaggio. fn restituisce false se un sotto-messaggio è rotto.
    // Un campo con un tipo diverso da quello atteso si salta, come un campo sconosciuto.
    template <typename F>
    bool eachField(Pb in, F&& fn) {
        Field f;
        while (!in.atEnd()) {
            if (!readField(in, f)) return false;
            if (!fn(f)) return false;
        }
        return true;
    }

    Deploy toDeploy(uint64_t v) {
        switch (v) {
        case 1: return Deploy::Retracted;
        case 2: return Deploy::Deploying;
        case 3: return Deploy::Deployed;
        case 4: return Deploy::Retracting;
        default: return Deploy::Unknown;
        }
    }

    MasterMode toMasterMode(uint64_t v) {
        switch (v) {
        case 1: return MasterMode::Navigation;
        case 2: return MasterMode::Scm;
        case 3: return MasterMode::Stealth;
        default: return MasterMode::Unknown;
        }
    }

    Locomotion toLocomotion(uint64_t v) {
        switch (v) {
        case 1: return Locomotion::OnFoot;
        case 2: return Locomotion::Eva;
        case 3: return Locomotion::Seated;
        default: return Locomotion::Unknown;
        }
    }

    CrewRole toCrewRole(uint64_t v) {
        switch (v) {
        case 1: return CrewRole::Pilot;
        case 2: return CrewRole::Gunner;
        case 3: return CrewRole::Crew;
        default: return CrewRole::Unknown;
        }
    }

    int toCount(uint64_t v) { return (int)std::min<uint64_t>(v & 0xFFFFFFFFu, INT_MAX); } // uint32

    bool readAvailability(Pb in, Availability& a) {
        return eachField(in, [&](const Field& f) {
            if (f.isVarint() && f.num == 1) a.enabled = f.flag();
            if (f.isVarint() && f.num == 2) a.active = f.flag();
            return true;
        });
    }

    bool readFlightState(Pb in, ShipState& s) {
        s.hasFlightState = true;
        return eachField(in, [&](const Field& f) {
            if (!f.isVarint()) return true;
            if (f.num == 1) s.isLanded = f.flag();
            if (f.num == 2) s.gear = toDeploy(f.value);
            if (f.num == 3) s.vtol = toDeploy(f.value);
            return true;
        });
    }

    bool readFlightSettings(Pb in, ShipState& s) {
        s.hasFlightSettings = true;
        return eachField(in, [&](const Field& f) {
            if (f.num == 19 && f.isVarint()) s.lightAmplification = f.flag();
            if (!f.isBytes()) return true;
            switch (f.num) {
            case 1: return readAvailability(f.bytes, s.coupled);
            case 2: return readAvailability(f.bytes, s.esp);
            case 4: return readAvailability(f.bytes, s.gSafe);
            case 5: return readAvailability(f.bytes, s.cruise);
            case 7: return readAvailability(f.bytes, s.speedLimiter);
            case 8: return readAvailability(f.bytes, s.proximityAssist);
            default: return true;
            }
        });
    }

    bool readInstruments(Pb in, ShipState& s) {
        s.hasInstruments = true;
        return eachField(in, [&](const Field& f) {
            if (f.isFloat()) {
                if (f.num == 1) s.speed = f.asFloat();
                if (f.num == 2) s.hydrogenFuel = f.asFloat();
                if (f.num == 3) s.quantumFuel = f.asFloat();
            } else if (f.isBytes() && f.num == 5) {
                return eachField(f.bytes, [&](const Field& b) {
                    if (b.isVarint() && b.num == 1) s.boostEngaged = b.flag();
                    return true;
                });
            }
            return true;
        });
    }

    bool readFlight(Pb in, ShipState& s) {
        return eachField(in, [&](const Field& f) {
            if (!f.isBytes()) return true;
            if (f.num == 1) return readFlightState(f.bytes, s);
            if (f.num == 2) return readFlightSettings(f.bytes, s);
            if (f.num == 3) return readInstruments(f.bytes, s);
            return true;
        });
    }

    bool readPower(Pb in, ShipState& s) {
        s.hasPower = true;
        return eachField(in, [&](const Field& f) {
            if (!f.isVarint()) return true;
            switch (f.num) {
            case 1: s.powerPlants = f.flag(); break;
            case 2: s.engines = f.flag(); break;
            case 3: s.weapons = f.flag(); break;
            case 4: s.coolers = f.flag(); break;
            case 5: s.lifeSupport = f.flag(); break;
            case 6: s.gravityGenerators = f.flag(); break;
            case 7: s.sensors = f.flag(); break;
            case 8: s.shieldGenerators = f.flag(); break;
            case 9: s.shieldEmitters = f.flag(); break;
            default: break;
            }
            return true;
        });
    }

    bool readShields(Pb in, ShipState& s) {
        s.hasShields = true;
        return eachField(in, [&](const Field& f) {
            if (f.isFloat() && f.num == 1) s.shieldLevel = f.asFloat();
            return true;
        });
    }

    bool readMasterMode(Pb in, ShipState& s) {
        s.hasMasterMode = true;
        return eachField(in, [&](const Field& f) {
            if (f.isVarint() && f.num == 1) s.masterMode = toMasterMode(f.value);
            if (f.isVarint() && f.num == 2) s.spooled = f.flag();
            return true;
        });
    }

    bool readVehicleState(Pb in, ShipState& s) {
        return eachField(in, [&](const Field& f) {
            if (f.isVarint() && f.num == 5) s.lightsOn = f.flag();
            if (f.isFloat() && f.num == 11) s.health = f.asFloat();
            if (!f.isBytes()) return true;
            switch (f.num) {
            case 1: s.vehicleName = f.text(); return true;
            case 2: s.className = f.text(); return true;
            case 8: return readFlight(f.bytes, s);
            case 9: return readPower(f.bytes, s);
            case 10: return readShields(f.bytes, s);
            case 12: return readMasterMode(f.bytes, s);
            default: return true;
            }
        });
    }

    bool readVehicle(Pb in, ShipState& s) {
        s.hasVehicle = true;
        return eachField(in, [&](const Field& f) {
            if (f.isBytes() && f.num == 2) return readVehicleState(f.bytes, s);
            return true;
        });
    }

    bool readWeapons(Pb in, ShipState& s) {
        s.hasWeapons = true;
        return eachField(in, [&](const Field& f) {
            if (!f.isBytes()) return true;
            if (f.num == 1) {
                return eachField(f.bytes, [&](const Field& x) {
                    if (x.isVarint() && x.num == 8) s.gimbalsLocked = x.flag();
                    return true;
                });
            }
            if (f.num == 4) {
                bool powered = false;
                if (!eachField(f.bytes, [&](const Field& x) {
                        if (x.isVarint() && x.num == 2) powered = x.flag();
                        return true;
                    }))
                    return false;
                s.weaponCount++;
                if (powered) s.weaponsPowered++;
            }
            if (f.num == 5) {
                uint64_t type = 0;
                int amount = 0;
                if (!eachField(f.bytes, [&](const Field& x) {
                        if (x.isVarint() && x.num == 1) type = x.value;
                        if (x.isVarint() && x.num == 2) amount = toCount(x.value);
                        return true;
                    }))
                    return false;
                // Più lanciatori dello stesso tipo: si sommano.
                int* slot = type == 1 ? &s.decoys : type == 2 ? &s.noise : nullptr;
                if (slot) *slot = (*slot < 0 ? 0 : *slot) + amount;
            }
            return true;
        });
    }

    bool readGauge(Pb in, float& current, float* max) {
        return eachField(in, [&](const Field& f) {
            if (f.isFloat() && f.num == 1) current = f.asFloat();
            if (f.isFloat() && f.num == 2 && max) *max = f.asFloat();
            return true;
        });
    }

    bool readActor(Pb in, ShipState& s) {
        s.hasActor = true;
        return eachField(in, [&](const Field& f) {
            if (!f.isBytes()) return true;
            if (f.num == 1) {
                return eachField(f.bytes, [&](const Field& v) {
                    if (v.isBytes() && v.num == 1) return readGauge(v.bytes, s.actorHealth, &s.actorHealthMax);
                    if (v.isBytes() && v.num == 2) return readGauge(v.bytes, s.oxygen, nullptr);
                    return true;
                });
            }
            if (f.num == 2) {
                return eachField(f.bytes, [&](const Field& x) {
                    if (x.isVarint() && x.num == 1) s.locomotion = toLocomotion(x.value);
                    if (x.isVarint() && x.num == 2) s.crewRole = toCrewRole(x.value);
                    if (x.isBytes() && x.num == 3) s.seatName = x.text();
                    if (x.isVarint() && x.num == 4) s.uec = (long long)x.value; // int64: i negativi sono varint da 10 byte
                    return true;
                });
            }
            return true;
        });
    }

    // --- HTTP/2 ----------------------------------------------------------------------------------

    enum FrameType : uint8_t {
        kData = 0, kHeaders = 1, kPriority = 2, kRstStream = 3, kSettings = 4,
        kPushPromise = 5, kPing = 6, kGoAway = 7, kWindowUpdate = 8, kContinuation = 9,
    };
    constexpr uint8_t kEndStream = 0x1, kAck = 0x1, kEndHeaders = 0x4, kPadded = 0x8, kPriorityFlag = 0x20;
    constexpr uint32_t kStream = 1;          // l'unico stream che apriamo
    constexpr uint32_t kWindow = 1u << 20;   // finestra di ricezione: 1 MB, il gioco non deve mai aspettare
    constexpr const char* kPath = "/sc.game.telemetry.v1.GameTelemetryService/TelemetryStream";

    void put32(std::string& s, uint32_t v) {
        s += (char)((v >> 24) & 0xFF);
        s += (char)((v >> 16) & 0xFF);
        s += (char)((v >> 8) & 0xFF);
        s += (char)(v & 0xFF);
    }

    uint32_t get32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

    std::string frameBytes(uint8_t type, uint8_t flags, uint32_t stream, const std::string& payload) {
        const uint32_t n = (uint32_t)payload.size();
        std::string f;
        f += (char)((n >> 16) & 0xFF);
        f += (char)((n >> 8) & 0xFF);
        f += (char)(n & 0xFF);
        f += (char)type;
        f += (char)flags;
        put32(f, stream & 0x7FFFFFFF);
        f += payload;
        return f;
    }

    std::string windowUpdate(uint32_t stream, uint32_t increment) {
        std::string p;
        put32(p, increment & 0x7FFFFFFF);
        return frameBytes(kWindowUpdate, 0, stream, p);
    }

    // Un numero HPACK con un prefisso di prefixBits bit (RFC 7541, 5.1).
    void hpackInt(std::string& out, uint8_t first, int prefixBits, size_t v) {
        const size_t max = ((size_t)1 << prefixBits) - 1;
        if (v < max) {
            out += (char)(first | v);
            return;
        }
        out += (char)(first | max);
        v -= max;
        while (v >= 128) {
            out += (char)((v & 0x7F) | 0x80);
            v >>= 7;
        }
        out += (char)v;
    }

    // Un'intestazione "letterale senza indicizzazione, nome nuovo" (primo byte 0000 0000), senza Huffman.
    void hpackLiteral(std::string& out, const std::string& name, const std::string& value) {
        out += '\0';
        hpackInt(out, 0, 7, name.size());
        out += name;
        hpackInt(out, 0, 7, value.size());
        out += value;
    }
} // namespace

bool decodeFrame(const uint8_t* data, size_t size, ShipState& out) {
    out = ShipState{};
    if (!data && size > 0) return false;
    const Pb in{ data, data + size };
    return eachField(in, [&](const Field& f) {
        if (!f.isBytes()) return true;
        if (f.num == 3) return readVehicle(f.bytes, out);
        if (f.num == 4) return readWeapons(f.bytes, out);
        if (f.num == 6) return readActor(f.bytes, out);
        return true; // events, force_feedback, turret: non servono
    });
}

namespace detail {

    std::string requestHeaderBlock(int port) {
        std::string block;
        hpackLiteral(block, ":method", "POST");
        hpackLiteral(block, ":scheme", "http");
        hpackLiteral(block, ":path", kPath);
        hpackLiteral(block, ":authority", "127.0.0.1:" + std::to_string(port));
        hpackLiteral(block, "content-type", "application/grpc");
        hpackLiteral(block, "te", "trailers");
        return block;
    }

    std::string clientOpening(int port) {
        std::string out = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
        std::string settings;
        auto setting = [&](uint16_t id, uint32_t value) {
            settings += (char)(id >> 8);
            settings += (char)(id & 0xFF);
            put32(settings, value);
        };
        setting(0x2, 0);       // ENABLE_PUSH = 0
        setting(0x4, kWindow); // INITIAL_WINDOW_SIZE
        out += frameBytes(kSettings, 0, 0, settings);
        out += windowUpdate(0, kWindow - 65535); // la finestra della connessione parte da 65535
        // Il blocco delle intestazioni è di poche centinaia di byte: sta sempre in un frame (minimo 16384).
        out += frameBytes(kHeaders, kEndHeaders, kStream, requestHeaderBlock(port));
        out += frameBytes(kData, kEndStream, kStream, std::string(5, '\0')); // TelemetryStreamRequest vuoto
        return out;
    }

    void GrpcReassembler::feed(const uint8_t* data, size_t size, std::vector<std::string>& messages) {
        while (size > 0) {
            if (headerHave_ < 5) {
                const size_t take = std::min(size, 5 - headerHave_);
                std::memcpy(header_ + headerHave_, data, take);
                headerHave_ += take;
                data += take;
                size -= take;
                if (headerHave_ < 5) break;
                remaining_ = get32(header_ + 1);
                // Compresso (1): Iris non chiede la compressione e non la sa leggere.
                keep_ = header_[0] == 0 && remaining_ <= maxMessage;
                if (!keep_) skipped_++;
                body_.clear();
                if (keep_) body_.reserve(remaining_);
                if (remaining_ == 0) {
                    if (keep_) messages.emplace_back();
                    headerHave_ = 0;
                }
                continue;
            }
            const size_t take = std::min(size, remaining_);
            if (keep_) body_.append((const char*)data, take);
            data += take;
            size -= take;
            remaining_ -= take;
            if (remaining_ == 0) {
                if (keep_) messages.push_back(std::move(body_));
                body_.clear();
                headerHave_ = 0;
            }
        }
    }

    bool Http2Receiver::close(std::string reason) {
        if (!closed_) {
            closed_ = true;
            closeReason = std::move(reason);
        }
        return false;
    }

    bool Http2Receiver::feed(const uint8_t* data, size_t size) {
        if (closed_) return false;
        buffer_.append((const char*)data, size);
        size_t pos = 0;
        bool open = true;
        while (open && buffer_.size() - pos >= 9) {
            const uint8_t* h = (const uint8_t*)buffer_.data() + pos;
            const uint32_t len = ((uint32_t)h[0] << 16) | ((uint32_t)h[1] << 8) | h[2];
            if (len > kMaxFrame) {
                open = close("frame troppo grande (" + std::to_string(len) + " byte)");
                break;
            }
            if (buffer_.size() - pos < 9 + (size_t)len) break; // il resto arriva col prossimo recv
            open = frame(h[3], h[4], get32(h + 5) & 0x7FFFFFFF, h + 9, len);
            pos += 9 + (size_t)len;
        }
        buffer_.erase(0, pos);
        // Rimandiamo indietro i byte consumati: così le finestre restano sempre aperte.
        if (open && consumedConn_ > 0) reply += windowUpdate(0, (uint32_t)consumedConn_);
        if (open && consumedStream_ > 0) reply += windowUpdate(kStream, (uint32_t)consumedStream_);
        consumedConn_ = consumedStream_ = 0;
        return open;
    }

    bool Http2Receiver::frame(uint8_t type, uint8_t flags, uint32_t stream, const uint8_t* p, uint32_t len) {
        if (inHeaders_ && (type != kContinuation || stream != headersStream_)) return close("intestazioni interrotte da un altro frame");
        switch (type) {
        case kData: {
            if (stream == 0) return close("DATA sullo stream 0");
            // La finestra conta tutto il frame, anche il riempimento.
            consumedConn_ += len;
            if (stream == kStream) consumedStream_ += len;
            const uint8_t* body = p;
            uint32_t n = len;
            if (flags & kPadded) {
                if (len < 1 || p[0] >= len) return close("riempimento sbagliato in DATA");
                body = p + 1;
                n = len - 1 - p[0];
            }
            if (stream != kStream) return true;
            grpc_.feed(body, n, messages);
            if (flags & kEndStream) return close("il gioco ha chiuso il flusso");
            return true;
        }
        case kHeaders: {
            // Le intestazioni della risposta e i trailer (grpc-status) non servono: non si decodificano,
            // ma riempimento e priorità devono stare nel frame.
            if (stream == 0) return close("HEADERS sullo stream 0");
            uint32_t skip = 0, pad = 0;
            if (flags & kPadded) {
                if (len < 1) return close("riempimento sbagliato in HEADERS");
                pad = p[0];
                skip = 1;
            }
            if (flags & kPriorityFlag) skip += 5;
            if (skip + pad > len) return close("riempimento sbagliato in HEADERS");
            const bool ends = stream == kStream && (flags & kEndStream);
            if (!(flags & kEndHeaders)) {
                inHeaders_ = true;
                headersStream_ = stream;
                endAfterHeaders_ = ends;
                return true;
            }
            if (ends) return close("il gioco ha chiuso il flusso");
            return true;
        }
        case kContinuation:
            if (!inHeaders_) return close("CONTINUATION fuori posto");
            if (flags & kEndHeaders) {
                inHeaders_ = false;
                if (endAfterHeaders_) return close("il gioco ha chiuso il flusso");
            }
            return true;
        case kRstStream:
            if (stream == 0 || len != 4) return close("RST_STREAM sbagliato");
            if (stream == kStream) return close("flusso annullato dal gioco (RST_STREAM, codice " + std::to_string(get32(p)) + ")");
            return true;
        case kSettings:
            if (stream != 0) return close("SETTINGS fuori dallo stream 0");
            if (flags & kAck) return len == 0 ? true : close("ACK di SETTINGS con dei dati");
            if (len % 6 != 0) return close("SETTINGS di lunghezza sbagliata");
            // I valori non servono: Iris manda solo frame piccoli e nessun DATA dopo la richiesta.
            reply += frameBytes(kSettings, kAck, 0, {});
            return true;
        case kPushPromise:
            return close("PUSH_PROMISE con il push spento");
        case kPing:
            if (stream != 0 || len != 8) return close("PING sbagliato");
            if (!(flags & kAck)) reply += frameBytes(kPing, kAck, 0, std::string((const char*)p, 8));
            return true;
        case kGoAway:
            return close(len >= 8 ? "il gioco chiude la connessione (GOAWAY, codice " + std::to_string(get32(p + 4)) + ")"
                                  : std::string("il gioco chiude la connessione (GOAWAY)"));
        case kWindowUpdate:
            if (len != 4) return close("WINDOW_UPDATE sbagliato");
            return true; // Iris non manda dati: la sua finestra di invio non serve
        default:
            return true; // PRIORITY e i tipi sconosciuti si ignorano
        }
    }

    void FreezeWatch::add(const std::string& bytes, int64_t ms) {
        if (!any_ || bytes != last_) {
            last_ = bytes;
            since_ = ms;
            any_ = true;
        }
        latest_ = ms;
    }

} // namespace detail

namespace {
    constexpr int kRetryMs = 3000;          // gioco chiuso o telemetria spenta: un tentativo ogni 3 s
    constexpr int kRetryAfterLossMs = 1000; // flusso chiuso dopo dei frame: si riprova prima
    constexpr int kConnectMs = 2000;
    constexpr int kFreshMs = 3000;          // "collegata" = l'ultimo frame è arrivato da al massimo 3 s
    constexpr int kFrozenMs = 2000;         // stessi byte da 2 s: gioco al menu

    int64_t nowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // WSAStartup una volta sola, per tutta la vita del programma.
    bool winsockReady() {
        static const bool ok = [] {
            WSADATA wsa;
            return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
        }();
        return ok;
    }

    bool sendAll(SOCKET s, const std::string& data) {
        size_t done = 0;
        while (done < data.size()) {
            const int sent = send(s, data.data() + done, (int)std::min<size_t>(data.size() - done, 1 << 16), 0);
            if (sent <= 0) return false;
            done += (size_t)sent;
        }
        return true;
    }

    // Una connessione TCP a 127.0.0.1:port che stop() può interrompere. INVALID_SOCKET = non raggiungibile.
    SOCKET connectLocal(int port, const std::atomic<bool>& stopping) {
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) return s;
#ifdef SIO_TCP_INITIAL_RTO
        // Porta chiusa: Windows riprova il SYN per circa 2 s prima di arrendersi. Su 127.0.0.1 non serve.
        TCP_INITIAL_RTO_PARAMETERS rto{ TCP_INITIAL_RTO_UNSPECIFIED_RTT, TCP_INITIAL_RTO_NO_SYN_RETRANSMISSIONS };
        DWORD bytes = 0;
        WSAIoctl(s, SIO_TCP_INITIAL_RTO, &rto, sizeof(rto), nullptr, 0, &bytes, nullptr, nullptr);
#endif
        u_long nonBlocking = 1;
        ioctlsocket(s, FIONBIO, &nonBlocking);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons((u_short)port);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bool ok = connect(s, (sockaddr*)&a, sizeof(a)) == 0;
        if (!ok && WSAGetLastError() == WSAEWOULDBLOCK) {
            for (int waited = 0; waited < kConnectMs && !stopping; waited += 100) {
                fd_set writable, failed;
                FD_ZERO(&writable);
                FD_ZERO(&failed);
                FD_SET(s, &writable);
                FD_SET(s, &failed);
                timeval timeout{ 0, 100 * 1000 };
                const int ready = select(0, nullptr, &writable, &failed, &timeout);
                if (ready < 0 || FD_ISSET(s, &failed)) break;
                if (ready > 0 && FD_ISSET(s, &writable)) {
                    int error = 0;
                    int size = sizeof(error);
                    ok = getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&error, &size) == 0 && error == 0;
                    break;
                }
            }
        }
        if (!ok || stopping) {
            closesocket(s);
            return INVALID_SOCKET;
        }
        nonBlocking = 0;
        ioctlsocket(s, FIONBIO, &nonBlocking);
        BOOL noDelay = TRUE;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&noDelay, sizeof(noDelay));
        DWORD sendTimeout = 2000; // le risposte sono di pochi byte: se non partono, il gioco è bloccato
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&sendTimeout, sizeof(sendTimeout));
        return s;
    }
} // namespace

struct Client::Impl {
    enum class Link { Searching, Unreachable, NoData, Open, Lost };

    std::mutex control; // start() e stop(), uno alla volta
    std::thread thread;
    std::atomic<bool> stopping{ false };
    int port = 2942;    // cambia solo a thread fermo (sotto control e mu)

    // Sotto mu.
    mutable std::mutex mu;
    std::condition_variable cv;
    bool running = false;
    Link link = Link::Searching;
    std::optional<ShipState> last;
    int64_t lastFrameMs = 0;
    detail::FreezeWatch freeze{ kFrozenMs };
    uint64_t frames = 0;

    // Solo del thread della connessione (e di stop(), a thread fermo).
    bool announced = false; // "collegata" già scritto nel registro

    std::string where() const { return "127.0.0.1:" + std::to_string(port); }

    void setLink(Link l) {
        std::lock_guard<std::mutex> lock(mu);
        link = l;
    }

    void forget(Link l) {
        std::lock_guard<std::mutex> lock(mu);
        link = l;
        last.reset();
        freeze.reset();
    }

    void waitOrStop(int ms) {
        std::unique_lock<std::mutex> lock(mu);
        cv.wait_for(lock, std::chrono::milliseconds(ms), [&] { return stopping.load(); });
    }

    void onMessage(const std::string& message, bool& gotFrames, bool& badLogged) {
        ShipState state;
        if (!decodeFrame((const uint8_t*)message.data(), message.size(), state)) {
            if (!badLogged) Log::warn("Telemetria di Star Citizen: frame non leggibile (" + std::to_string(message.size()) + " byte), lo salto");
            badLogged = true;
            return;
        }
        const int64_t now = nowMs();
        {
            std::lock_guard<std::mutex> lock(mu);
            last = std::move(state);
            lastFrameMs = now;
            freeze.add(message, now);
            frames++;
        }
        gotFrames = true;
        if (!announced) {
            announced = true;
            Log::info("Telemetria di Star Citizen: collegata (" + where() + ")");
        }
    }

    // Una connessione, dalla richiesta alla chiusura. Restituisce il motivo della fine.
    std::string session(SOCKET s, bool& gotFrames) {
        if (!sendAll(s, detail::clientOpening(port))) return "richiesta non inviata";
        detail::Http2Receiver rx;
        std::vector<uint8_t> buffer(64 * 1024);
        bool badLogged = false;
        while (!stopping) {
            // Mai un recv che aspetta per sempre: stop() deve rispondere subito.
            fd_set readable;
            FD_ZERO(&readable);
            FD_SET(s, &readable);
            timeval timeout{ 0, 200 * 1000 };
            const int ready = select(0, &readable, nullptr, nullptr, &timeout);
            if (ready < 0) return "errore di rete " + std::to_string(WSAGetLastError());
            if (ready == 0) continue;
            const int n = recv(s, (char*)buffer.data(), (int)buffer.size(), 0);
            if (n == 0) return "il gioco ha chiuso la connessione";
            if (n < 0) return "connessione interrotta (errore " + std::to_string(WSAGetLastError()) + ")";
            const bool open = rx.feed(buffer.data(), (size_t)n);
            for (const std::string& message : rx.messages) onMessage(message, gotFrames, badLogged);
            rx.messages.clear();
            if (!open) return rx.closeReason;
            if (!rx.reply.empty() && !sendAll(s, rx.reply)) return "risposta al gioco non inviata";
            rx.reply.clear();
        }
        return "Iris si ferma";
    }

    void loop() {
        bool warnedNoData = false;
        while (!stopping) {
            const SOCKET s = connectLocal(port, stopping);
            if (stopping) {
                if (s != INVALID_SOCKET) closesocket(s);
                break;
            }
            if (s == INVALID_SOCKET) {
                setLink(Link::Unreachable);
                waitOrStop(kRetryMs);
                continue;
            }
            setLink(Link::Open);
            bool gotFrames = false;
            const std::string reason = session(s, gotFrames);
            closesocket(s);
            if (stopping) break;
            forget(gotFrames ? Link::Lost : Link::NoData);
            if (announced) {
                Log::warn("Telemetria di Star Citizen: persa (" + reason + ")");
                announced = false;
                warnedNoData = false;
            } else if (!warnedNoData) {
                Log::warn("Telemetria di Star Citizen: il gioco risponde su " + where() + " ma non manda dati (" + reason + ")");
                warnedNoData = true;
            }
            waitOrStop(gotFrames ? kRetryAfterLossMs : kRetryMs);
        }
    }

    void stopThread() {
        if (!thread.joinable()) return;
        {
            std::lock_guard<std::mutex> lock(mu);
            stopping = true;
        }
        cv.notify_all();
        thread.join();
        forget(Link::Searching);
        {
            std::lock_guard<std::mutex> lock(mu);
            running = false;
        }
        if (announced) Log::info("Telemetria di Star Citizen: scollegata");
        announced = false;
    }
};

Client::Client() : m(std::make_unique<Impl>()) {}

Client::~Client() { stop(); }

void Client::start(int port) {
    if (port <= 0 || port > 65535) port = 2942;
    std::lock_guard<std::mutex> control(m->control);
    if (m->thread.joinable()) {
        if (port == m->port) return;
        m->stopThread();
    }
    if (!winsockReady()) {
        Log::warn("Telemetria di Star Citizen: la rete di Windows non è disponibile");
        return;
    }
    m->stopping = false;
    m->announced = false;
    m->forget(Impl::Link::Searching);
    {
        std::lock_guard<std::mutex> lock(m->mu); // statusText() legge la porta da un altro thread
        m->port = port;
        m->running = true;
    }
    // Una riga sola all'avvio; poi solo i cambi di stato (collegata, persa), mai i singoli tentativi.
    Log::info("Telemetria di Star Citizen: la leggo da 127.0.0.1:" + std::to_string(port) + " (solo lettura)");
    Impl* impl = m.get();
    m->thread = std::thread([impl] { impl->loop(); });
}

void Client::stop() {
    std::lock_guard<std::mutex> control(m->control);
    m->stopThread();
}

bool Client::running() const {
    std::lock_guard<std::mutex> lock(m->mu);
    return m->running;
}

std::optional<ShipState> Client::current(int maxAgeMs) const {
    std::lock_guard<std::mutex> lock(m->mu);
    if (!m->last || m->freeze.frozen()) return std::nullopt;
    if (nowMs() - m->lastFrameMs > maxAgeMs) return std::nullopt;
    return m->last;
}

uint64_t Client::frameCount() const {
    std::lock_guard<std::mutex> lock(m->mu);
    return m->frames;
}

bool Client::connected() const {
    std::lock_guard<std::mutex> lock(m->mu);
    return m->running && m->link == Impl::Link::Open && m->last && nowMs() - m->lastFrameMs <= kFreshMs;
}

std::string Client::statusText() const {
    std::lock_guard<std::mutex> lock(m->mu);
    if (!m->running) return "Non attiva";
    const std::string where = m->where();
    switch (m->link) {
    case Impl::Link::Searching: return "Cerco il gioco su " + where + "...";
    case Impl::Link::Unreachable: return "Non raggiungibile su " + where + ": telemetria spenta o gioco chiuso";
    case Impl::Link::NoData: return "Il gioco risponde su " + where + " ma non manda dati (telemetria spenta?)";
    case Impl::Link::Lost: return "Collegamento perso, mi ricollego...";
    case Impl::Link::Open: break;
    }
    if (!m->last || nowMs() - m->lastFrameMs > kFreshMs) return "Collegata, in attesa dei dati (gioco al menu o in caricamento)";
    if (m->freeze.frozen()) return "Collegata, gioco al menu";
    const ShipState& s = *m->last;
    if (s.hasVehicle) {
        const std::string& name = !s.className.empty() ? s.className : s.vehicleName;
        return name.empty() ? "Collegata (in nave)" : "Collegata: " + name;
    }
    if (s.hasActor) {
        switch (s.locomotion) {
        case Locomotion::OnFoot: return "Collegata (a piedi)";
        case Locomotion::Eva: return "Collegata (in EVA)";
        case Locomotion::Seated: return "Collegata (seduto)";
        default: break;
        }
    }
    return "Collegata";
}

} // namespace ScTelemetry
