// coop/net/signaling_proof.h -- the proof this peer gives the signaling relay that it owns the
// name it is registering. Its own file because it is its own contract: the relay asks
// `nonce <64 hex>` and reads the NEXT line as `auth <128 hex>`, an Ed25519 signature over a
// domain-separated blob, and three parties have to agree on those bytes exactly -- this client,
// `server/src/bin/signaling.rs` (REGISTER_TAG), and the release gate, which carries its own copy
// of the tag and refuses a relay that does not challenge.
// A client that registered unproved would let anyone squat any host's name (security tracker
// A59), so every failure here is a refusal to register, never a warning.
//
// Nothing in here touches a socket, a queue or any registration state: the caller hands it the
// challenge line and its own identity and gets back the line to send, or false.

#pragma once

#include <cstddef>
#include <string>

namespace coop::net::signaling_proof {

// Validate the relay's challenge line and answer it. `line`/`len` is one inbound line without its
// newline (a trailing CR or space is tolerated, so a relay behind a line-ending-normalising proxy
// is not a protocol violation); `selfIdentity` is the rendered `gen:<64 hex>` this peer registers
// under. On success `out` is the complete `auth <128 hex>\n` line to send and the function returns
// true. False means the line was not a well-formed challenge, or this machine could not sign it --
// either way the caller drops the connection rather than registering unproved. Logs the reason.
bool AnswerChallenge(const char* line, size_t len, const std::string& selfIdentity,
                     std::string& out);

}  // namespace coop::net::signaling_proof
