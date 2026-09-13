// coop/props/container_slice_wire.cpp -- see coop/props/container_slice_wire.h.

#include "coop/props/container_slice_wire.h"

#include "coop/items/save_record_wire.h"
#include "coop/net/blob_chunks.h"

namespace coop::props::container_slice_wire {
namespace {

namespace SR = ue_wrap::save_record;
namespace W  = coop::save_record_wire;

constexpr uint8_t kOpContents = 0;

void AppU16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back(static_cast<uint8_t>(v & 0xFF));
    b.push_back(static_cast<uint8_t>(v >> 8));
}

void AppU64(std::vector<uint8_t>& b, uint64_t v) {
    for (int i = 0; i < 8; ++i) b.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
}

bool RdU64(const std::vector<uint8_t>& b, size_t& o, uint64_t& v) {
    if (o + 8 > b.size()) return false;
    v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(b[o + i]) << (i * 8);
    o += 8;
    return true;
}

}  // namespace

std::vector<uint8_t> Pack(uint32_t eid, uint64_t baseHash,
                          const std::vector<SR::SaveRecord>& recs) {
    std::vector<uint8_t> b;
    b.push_back(kOpContents);
    W::AppU32(b, eid);
    AppU64(b, baseHash);
    AppU16(b, static_cast<uint16_t>(recs.size()));
    for (const auto& r : recs) W::SerSave(b, r);
    return b;
}

uint64_t ContentHash(uint32_t eid, const std::vector<SR::SaveRecord>& recs) {
    return coop::blob_chunks::Fnv64(Pack(eid, 0, recs));
}

bool ParseHeader(const std::vector<uint8_t>& b, size_t& o, uint32_t& outEid,
                 uint64_t& outBaseHash) {
    uint8_t op = 0;
    if (!W::RdU8(b, o, op) || op != kOpContents) return false;
    if (!W::RdU32(b, o, outEid)) return false;
    return RdU64(b, o, outBaseHash);
}

bool ParseRecords(const std::vector<uint8_t>& b, size_t& o, std::vector<SR::SaveRecord>& out,
                  const char** outWhy) {
    if (outWhy) *outWhy = "";
    if (o + 2 > b.size()) {
        if (outWhy) *outWhy = "no record count";
        return false;
    }
    const uint16_t n = static_cast<uint16_t>(b[o] | (b[o + 1] << 8));
    o += 2;
    if (n > kMaxRecords || !W::Feasible(n, b, o)) {
        if (outWhy) *outWhy = "the declared record count is past the cap or past the bytes";
        return false;
    }
    out.assign(n, SR::SaveRecord{});
    for (auto& r : out) {
        if (!W::DeSave(b, o, r)) {
            if (outWhy) *outWhy = "malformed record stream";
            out.clear();
            return false;
        }
    }
    return true;
}

}  // namespace coop::props::container_slice_wire
