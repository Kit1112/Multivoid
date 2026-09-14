// coop/net/signaling_proof.cpp -- see signaling_proof.h.

#include "signaling_proof.h"

#include "coop/net/peer_identity.h"
#include "ue_wrap/core/log.h"

#include <cstring>

namespace coop::net::signaling_proof {
namespace {

// Must equal REGISTER_TAG in server/src/bin/signaling.rs. The instrument that covers the pair is
// the p2p_smoke scenario, whose two peers sign with this code and register against the real relay:
// if the bytes drift, both fail to register and the verdict goes red. A release gate carries its
// own copy of the tag and never runs this client.
constexpr char kRegisterTag[] = "multivoid-signaling-register-v1";
constexpr char kChallengePrefix[] = "nonce ";
constexpr size_t kNonceHexLen = 64;
// gen: plus 64 hex; the relay accepts exactly this width, which is what makes the un-delimited
// blob unambiguous.
constexpr size_t kIdentityLen = 4 + 64;

constexpr char kHexDigit[] = "0123456789abcdef";

}  // namespace

bool AnswerChallenge(const char* line, size_t len, const std::string& selfIdentity,
                     std::string& out) {
    // A trailing CR is tolerated, so a relay behind a line-ending-normalising proxy is not a
    // protocol violation.
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == ' ')) --len;

    constexpr size_t kPrefixLen = sizeof(kChallengePrefix) - 1;
    if (len != kPrefixLen + kNonceHexLen ||
        std::memcmp(line, kChallengePrefix, kPrefixLen) != 0) {
        UE_LOGE("signaling: expected a registration challenge and got a %zu-byte "
                "line that is not one -- this relay does not speak the "
                "nonce/auth exchange. REFUSING to register unproved.", len);
        return false;
    }
    const char* nonce = line + kPrefixLen;
    for (size_t i = 0; i < kNonceHexLen; ++i) {
        const char c = nonce[i];
        // Lowercase only, matching the server's alphabet: a proof must not be laxer about its
        // inputs than the name it proves.
        if (!(('0' <= c && c <= '9') || ('a' <= c && c <= 'f'))) {
            UE_LOGE("signaling: the registration challenge is not 64 lowercase "
                    "hex digits -- refusing to sign it");
            return false;
        }
    }

    // The blob is tag, identity, nonce with no separators: every field is fixed width, so the
    // concatenation is unambiguous. The identity's width is asserted, not assumed: nothing
    // upstream checks it (Create only rejects a spaced one), and signing an off-width identity
    // would produce a blob the relay cannot rebuild, so the honest failure is here.
    if (selfIdentity.size() != kIdentityLen) {
        UE_LOGE("signaling: our identity is %zu chars, not %zu -- refusing to sign "
                "a blob the relay cannot rebuild", selfIdentity.size(), kIdentityLen);
        return false;
    }
    std::string blob;
    blob.reserve(sizeof(kRegisterTag) - 1 + selfIdentity.size() + kNonceHexLen);
    blob.append(kRegisterTag, sizeof(kRegisterTag) - 1);
    blob.append(selfIdentity);
    blob.append(nonce, kNonceHexLen);

    const peer_identity::Sig sig = peer_identity::SignBlob(
        reinterpret_cast<const uint8_t*>(blob.data()), blob.size());

    out.clear();
    out.reserve(5 + sig.size() * 2 + 1);
    out.append("auth ");
    for (uint8_t b : sig) {
        out.push_back(kHexDigit[b >> 4U]);
        out.push_back(kHexDigit[b & 0xf]);
    }
    out.push_back('\n');
    return true;
}

}  // namespace coop::net::signaling_proof
