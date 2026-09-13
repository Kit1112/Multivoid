// ue_wrap/core/call.h -- build a UFunction parameter frame and invoke it.
//
// Engine-wrapper layer (principle 7). A UFunction call via ProcessEvent needs a
// parameter frame with each argument at the exact byte offset the engine
// expects. ParamFrame allocates a correctly-sized, zeroed frame and lets you set
// arguments BY NAME -- the offsets come from reflection (the live UFunction's
// FProperty chain), so this is correct-by-construction and version-portable
// (no hardcoded offsets). Must be invoked on the game thread.

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ue_wrap {

class ParamFrame {
public:
    // `function` is a UFunction*. Allocates UFunction::PropertiesSize bytes,
    // zeroed (so unset params/out-params start clean).
    explicit ParamFrame(void* function);

    // A resolved UFunction. buf_ may be empty for a no-param function (valid: the
    // call passes a null params buffer). Param read/write paths guard buf_ size.
    bool valid() const { return fn_ != nullptr; }
    void* function() const { return fn_; }
    void* data() { return buf_.empty() ? nullptr : buf_.data(); }

    // Write `size` bytes from `src` at parameter `name`'s frame offset. Returns
    // false (and logs) if the param is unknown or would overflow the frame.
    bool SetRaw(const wchar_t* name, const void* src, int32_t size);

    // Read `size` bytes of parameter `name` (e.g. a ReturnValue / OUT param)
    // into `dst` after the call. Returns false if unknown/overflow.
    bool GetRaw(const wchar_t* name, void* dst, int32_t size) const;

    template <class T>
    bool Set(const wchar_t* name, const T& value) {
        return SetRaw(name, &value, static_cast<int32_t>(sizeof(T)));
    }
    template <class T>
    T Get(const wchar_t* name) const {
        T value{};
        GetRaw(name, &value, static_cast<int32_t>(sizeof(T)));
        return value;
    }

    // The frame's total size, and one parameter's offset within it. Both are read from
    // the live UFunction, so they carry the engine's own alignment and padding.
    //
    // These exist so a struct parameter whose LAYOUT we do not know can still be moved
    // between two frames: the byte span of an FGeometry sitting at offset 0 of one frame
    // is exactly the offset of whatever parameter follows it in another. Reading a struct
    // through a hand-written mirror of its fields is the thing this header was written to
    // avoid ("no hardcoded offsets"), and a struct we never dereference needs no mirror at
    // all. Both return -1 when the frame is invalid or the name is unknown -- never 0,
    // which is a legitimate offset for the first parameter.
    int32_t FrameSize() const { return meta_ ? meta_->frameSize : -1; }
    int32_t ParamOffset(const wchar_t* name) const { return OffsetOf(name); }

    // Internal metadata cache entry shared across every ParamFrame for the
    // same UFunction*. Built lazily on first construction for a given fn,
    // then reused as a read-only pointer by subsequent ParamFrames -- so
    // steady-state ParamFrame construction allocates ONLY buf_ (the
    // per-call zeroed frame), not the offsets vector or per-param wstrings.
    // Without the cache the constructor allocates two vectors and a wstring
    // per parameter on every call, churning malloc and free on the
    // per-snapshot Drive() and the per-tick observer dispatch paths.
    struct Metadata {
        int32_t frameSize = -1;  // < 0 => malformed UFunction; ParamFrame stays invalid
        std::vector<std::pair<std::wstring, int32_t>> offsets;
    };

private:
    // Param name -> frame offset, walked from the UFunction's FProperty chain ONCE
    // (cached process-wide; see ParamFrame::Metadata above).
    int32_t OffsetOf(const wchar_t* name) const;

    void* fn_ = nullptr;
    const Metadata* meta_ = nullptr;
    std::vector<uint8_t> buf_;
};

// Invoke `frame` on `object` (reflection::CallFunction under the hood). OUT
// params / ReturnValue are written back into the frame, readable via Get().
// Game thread only.
bool Call(void* object, ParamFrame& frame);

// What the reflected-call path costs, as counts rather than as an assertion. `frames` is every
// ParamFrame that resolved a frame size (a malformed UFunction is refused and logged, and is not
// one), `allocs` the subset that heap-allocated -- so `frames - allocs` is exactly the zero-size
// frames, which allocate nothing -- and `bytes` is what those allocations asked for. `le` buckets
// an allocating frame by size and `maxSize` is the largest seen, which is what an inline buffer
// would have to cover.
//
// Always counted, unlike MTA's own per-call instrument, which returns early unless somebody is
// watching and disarms itself once nobody has looked for fifteen seconds
// (`CPerfStat.FunctionTiming.cpp`): theirs samples a clock and walks a string-keyed map per call,
// while this is a handful of relaxed adds beside the malloc on the same line -- a call rate high
// enough to make the counter cost anything is one where the allocation it measures costs far
// more, so the instrument cannot outweigh its subject.
struct FrameStats {
    unsigned long long frames;   // ParamFrame constructions that resolved a frame size
    unsigned long long allocs;   // constructions that allocated (frameSize > 0)
    unsigned long long bytes;    // total bytes those allocations asked for
    unsigned long long le[5];    // allocating frames with frameSize <= 16 / 32 / 64 / 128 / 256
    int32_t maxSize;             // the largest frame size seen
};
FrameStats GetFrameStats();

}  // namespace ue_wrap
