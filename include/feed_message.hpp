#pragma once
#include <cstdint>
#include <type_traits>
#include "order.hpp"

// Synthetic ITCH-style feed format: fixed-size, type-tagged, binary
// records. Deliberately NOT a certified/exchange-accurate protocol (see
// README "What this deliberately is NOT") — it's the inbound order-intent
// format this project's own generator and replay engine speak.
//
// Host-native layout only: no explicit endianness handling, no manual
// byte packing. That's fine because every producer and consumer of this
// format is this project running on the same machine; it stops being fine
// the day something else needs to send these bytes over a real wire, at
// which point this needs a defined little/big-endian encoding.
//
// Every record is the same fixed size regardless of type (a few bytes
// wasted on CancelOrder) so a parser can index a byte buffer as a plain
// array of FeedMessage with no length-prefix bookkeeping — this is also
// exactly the shape Phase 2's AF_XDP RX path will reinterpret_cast frames
// as, so keep this POD and allocation-free.

enum class MsgType : std::uint8_t { AddOrder = 1, CancelOrder = 2 };

struct FeedMessage {
    MsgType  type;
    Side     side;     // meaningful only for AddOrder
    OrderId  id;
    Price    price;    // meaningful only for AddOrder
    Quantity qty;       // meaningful only for AddOrder
    Nanos    ts;
};

static_assert(std::is_trivially_copyable_v<FeedMessage>,
              "FeedMessage must be safely memcpy/reinterpret_cast-able (raw file I/O, future AF_XDP frames)");

// File header for saved feed logs: a magic tag plus a record count so a
// reader can sanity-check before blindly trusting the byte count.
struct FeedFileHeader {
    char magic[8] = {'O', 'B', 'F', 'E', 'E', 'D', '1', '\0'};
    std::uint64_t count = 0;
};

static_assert(std::is_trivially_copyable_v<FeedFileHeader>, "FeedFileHeader must be raw-file-I/O safe");

inline bool feed_magic_valid(const FeedFileHeader& hdr) {
    static constexpr char kMagic[8] = {'O', 'B', 'F', 'E', 'E', 'D', '1', '\0'};
    for (int i = 0; i < 8; ++i) {
        if (hdr.magic[i] != kMagic[i]) return false;
    }
    return true;
}
