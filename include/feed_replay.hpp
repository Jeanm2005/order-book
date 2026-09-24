#pragma once
#include <cstdio>
#include <string>
#include <vector>
#include "feed_message.hpp"
#include "order_book.hpp"

// Applies one feed message to the book. This is the exact function Phase 2's
// AF_XDP RX path will call per-frame once that's wired up — keep it
// allocation-free and branch-light, it's the only hot-path piece of Phase 1.
template <typename Book>
inline void apply_message(Book& ob, const FeedMessage& msg) {
    switch (msg.type) {
        case MsgType::AddOrder:
            ob.add_order(msg.id, msg.side, msg.price, msg.qty, msg.ts);
            break;
        case MsgType::CancelOrder:
            ob.cancel_order(msg.id);
            break;
    }
}

// --- Ordinary (non-hot-path) file I/O: log generation/loading is setup
// code, not something an incoming order passes through. -------------------

inline bool save_feed_file(const std::string& path, const std::vector<FeedMessage>& msgs) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;

    FeedFileHeader hdr;
    hdr.count = msgs.size();
    bool ok = std::fwrite(&hdr, sizeof(hdr), 1, f) == 1;
    if (ok && !msgs.empty()) {
        ok = std::fwrite(msgs.data(), sizeof(FeedMessage), msgs.size(), f) == msgs.size();
    }
    std::fclose(f);
    return ok;
}

// Returns false (and leaves `out` empty) on a missing file, bad magic, or
// short read — never partially-populates `out`.
inline bool load_feed_file(const std::string& path, std::vector<FeedMessage>& out) {
    out.clear();
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;

    FeedFileHeader hdr;
    if (std::fread(&hdr, sizeof(hdr), 1, f) != 1 || !feed_magic_valid(hdr)) {
        std::fclose(f);
        return false;
    }

    std::vector<FeedMessage> msgs(hdr.count);
    bool ok = true;
    if (hdr.count > 0) {
        ok = std::fread(msgs.data(), sizeof(FeedMessage), msgs.size(), f) == msgs.size();
    }
    std::fclose(f);
    if (!ok) return false;

    out = std::move(msgs);
    return true;
}

// Replays every message in `msgs` into `ob` in order via apply_message().
template <typename Book>
inline void replay(Book& ob, const std::vector<FeedMessage>& msgs) {
    for (const auto& msg : msgs) {
        apply_message(ob, msg);
    }
}
