// coop/net/signaling_client.h -- the out-of-band rendezvous channel for P2P (ICE), ported from
// GameNetworkingSockets' trivial_signaling_client example (BSD-3, Valve). A line-oriented TCP
// stream: the first line is "<token> <identity>", every later line "<dest-identity>
// <hex-payload>", routed by the server to the connection registered under that identity.
// Registration is proved: the server sends "nonce <64 hex>" and we answer "auth <128 hex>", an
// Ed25519 signature by the key our identity names; a relay that never challenges is refused
// (a release gate proves the deployed relay speaks the challenge before a release), then kept
// proved on a timer: a line addressed to our own identity returns only while the relay still
// routes that name here, and silence retires the registration (see kEchoProbeInterval). One
// client per P2P Session: it keeps the connection (auto-reconnect), hands GNS a per-connection
// signaling object whose SendSignal hex-encodes and enqueues, and Poll() drains inbound lines
// into ReceivedP2PCustomSignal. SendSignal may run on any thread and Poll() on the net thread;
// the socket and the queue are under a recursive mutex, and received signals are dispatched
// after it is released, since ReceivedP2PCustomSignal takes a GNS lock another thread may hold
// while calling SendSignal.

#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>

#pragma warning(push)
#pragma warning(disable: 4100 4127 4191 4244 4245 4267 4310 4324 4458)
#include <steam/steamnetworkingtypes.h>          // SteamNetworkingIdentity
#include <steam/steamnetworkingcustomsignaling.h>  // ISteamNetworkingConnectionSignaling
#pragma warning(pop)

class ISteamNetworkingSockets;

namespace coop::net {

// What the rendezvous knows about a dial that is ending, so a joiner can say WHICH half went
// quiet. The transport reports every dead dial as its own timeout -- true, and useless to a
// player: a relay this machine never reached, a host whose registration died of idleness and a
// host that is simply switched off all read the same. These are the facts a joiner owns without
// asking anyone, and asking the relay instead is what we will not do -- an `unroutable` answer
// hands every token holder a presence oracle for any identity it knows, the same reason this
// client's own SendRejectionSignal is mute.
struct DialReport {
    // Whether our OWN name still routes to this connection: the question a joiner's host failed,
    // and the one a keepalive cannot answer. Unknown is a real answer, and the honest one early --
    // a socket that is up and proved but whose first echo is still in flight knows nothing yet, so
    // a verdict is named on Live or Down and never on a guess.
    enum class Registration { Unknown, Live, Down };
    Registration registration = Registration::Unknown;
    bool     peerAnswered = false;  // a line came back from the dialled identity
    uint32_t linesToPeer = 0;       // lines addressed to it: a dial that sent nothing is its own fact
};

// enable_shared_from_this: each per-connection ConnectionSignaling co-owns the client, so a
// connection GNS is still tearing down (and may still call SendSignal on) keeps the transport
// alive after Session::Stop() drops its reference.
class SignalingClient : public std::enable_shared_from_this<SignalingClient> {
public:
    // Resolve and begin connecting to `serverAddr` ("host:port", port 10000 by default); `sockets`'
    // identity becomes the greeting and the ReceivedP2PCustomSignal target. nullptr on a bad
    // address or null sockets. Session holds the returned shared_ptr.
    static std::shared_ptr<SignalingClient> Create(const std::string& serverAddr,
                                                    const std::string& token,
                                                    ISteamNetworkingSockets* sockets);
    ~SignalingClient();

    SignalingClient(const SignalingClient&) = delete;
    SignalingClient& operator=(const SignalingClient&) = delete;

    // A per-connection signaling object for ConnectP2PCustomSignaling or an accepted inbound
    // request; GNS owns it and calls Release().
    ISteamNetworkingConnectionSignaling* CreateSignalingForConnection(
        const SteamNetworkingIdentity& peer);

    // Net thread: drain inbound signals, flush the outbound queue, reconnect if dropped. Cheap when
    // idle.
    void Poll();

    // A client is dialling `peer`: from here the report's counters describe THAT dial. Called once
    // per session by the P2P client start and never by a host, whose inbound peers are many and
    // none of whose failures this answers. Takes the identity rather than the configured string
    // and renders it here, because the name that has to MATCH is GNS's rendering of it -- the one
    // SendSignal addresses, and the one the relay stamps on the far peer's lines from what that
    // peer registered under. A configured `gen:` line differing only in case would count nothing.
    void NoteDialing(const SteamNetworkingIdentity& peer);

    // The rendezvous half of why a dial ended, for the site that turns a close into a sentence.
    // A snapshot under the lock; no engine calls, cheap, and safe from any thread.
    DialReport ReportDial();

private:
    struct ConnectionSignaling;  // per-connection ISteamNetworkingConnectionSignaling
    friend struct ConnectionSignaling;

    SignalingClient(std::string host, std::string service, std::string token,
                    ISteamNetworkingSockets* sockets);

    void ResolveServerAddr();   // ctor-time, on the constructing thread (may block on DNS)
    void CloseSocketLocked();   // caller holds sockMutex_
    void ConnectLocked();       // caller holds sockMutex_ (no DNS -- uses the cached addr)
    void Enqueue(const std::string& line);  // thread-safe; line is '\n'-terminated
    // The same, at the front; only the registration proof uses it, since the relay reads the line
    // after the challenge as the proof and a queued ICE signal must not overtake it.
    void EnqueueFront(const std::string& line);

    // Where this connection is in the registration handshake; reset by every ConnectLocked, since a
    // reconnect re-greets and the server issues a fresh nonce.
    enum class RegState { AwaitingChallenge, ProofSent };

    // Answer the server's "nonce <64 hex>"; false if the line is not a well-formed challenge or we
    // cannot sign, and the caller drops the connection. Net thread.
    bool AnswerChallenge(const char* line, size_t len);

    // An inbound line from our own identity with an empty payload: the relay resolved our name to
    // this connection, so the registration is live. Pushes the deadline out and logs the first one
    // on each socket. Net thread (the dispatch pass, which runs outside the lock) -- it takes
    // sockMutex_ itself.
    void NoteRegistrationEcho();

    const std::string host_;
    const std::string service_;     // port, as a string for getaddrinfo
    const std::string token_;       // shared bearer token sent in the greeting
    ISteamNetworkingSockets* const sockets_;
    std::string selfIdentity_;      // rendered local identity (no newline)
    std::string greeting_;          // "<token> <identity>\n"
    bool wsaStarted_ = false;       // we successfully called WSAStartup
    bool identityOk_ = true;        // false if our identity is invalid/spaced -> Create() returns nullptr

    // The server address, resolved once in the ctor so reconnects never call the blocking
    // getaddrinfo on the net thread; opaque bytes keep winsock types out of this header.
    bool resolved_ = false;
    int  resolvedFamily_ = 0;
    int  resolvedLen_ = 0;
    unsigned char resolvedAddr_[128] = {};  // >= sizeof(sockaddr_storage) (128 on Win)

    std::recursive_mutex sockMutex_;
    // SOCKET as uintptr_t, so this header needs no winsock include (and its ordering constraint on
    // every includer); ~0 is INVALID_SOCKET.
    std::uintptr_t sock_ = static_cast<std::uintptr_t>(~static_cast<std::uintptr_t>(0));
    std::string inBuf_;             // accumulates inbound bytes (net-thread only)
    std::deque<std::string> sendQueue_;  // outbound lines awaiting flush (greeting at front)
    // Reconnect backoff: the earliest time Poll() may retry after a drop, or a down server is
    // dialled at ~200 Hz. Net thread only.
    std::chrono::steady_clock::time_point nextConnectAttempt_{};
    // Registration handshake state (net thread only). The deadline is generous: it turns "an old
    // relay never challenged us" into one named error line, not a latency check.
    RegState regState_ = RegState::AwaitingChallenge;
    // Set when the greeting has left the socket; before it, silence means "not connected", not "old
    // relay", so it gates the fail-closed deadline and the send gate.
    bool greetingSent_ = false;
    std::chrono::steady_clock::time_point challengeDeadline_{};
    // When a connect that is still in progress gives up. A non-blocking connect signals failure
    // the same way it signals "not finished yet", so without this the ONE thing the backoff cannot
    // retry is a socket that never connected. Written by ConnectLocked (the ctor's call included,
    // so not the net thread alone) and read by Poll; both under sockMutex_.
    std::chrono::steady_clock::time_point connectDeadline_{};

    // The registration's own liveness, all three cleared by CloseSocketLocked so a fresh socket
    // starts owing a fresh proof of routing. Written on the net thread and by the ctor's
    // ConnectLocked, read on the net thread; all under sockMutex_.
    std::chrono::steady_clock::time_point nextEchoProbe_{};  // epoch = probe as soon as registered
    std::chrono::steady_clock::time_point lastEchoProbe_{};  // for the round trip in the log
    // When unanswered probes mean the name no longer routes here. Armed by the first probe and
    // pushed out by every echo, so it moves only on evidence.
    std::chrono::steady_clock::time_point echoDeadline_{};
    bool echoSeen_ = false;  // an echo has come back on THIS socket (the first one logs)

    // The dial a client is making, and what crossed for it. One identity, not a map: a joiner
    // dials exactly one host, and a host -- which does have many peers -- never sets this, so
    // nothing here grows with the number of strangers who signal us. All under sockMutex_,
    // written from the net thread (inbound) and from GNS threads (SendSignal).
    std::string dialledPeer_;
    uint32_t    dialLinesOut_ = 0;
    uint32_t    dialLinesIn_ = 0;
};

}  // namespace coop::net
