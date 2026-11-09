// ENet link (M2). The only file that includes enet.h. One net thread makes every enet_* call; the sim thread talks to
// it through two SRWLOCK queues (NetSend / NetPoll) and never waits on the network. The net thread touches no game
// memory (Log only). It starts on the first F8 / autoconnect, never in DllMain (WSAStartup under the loader lock). It
// makes the password key (PBKDF2, too slow for the sim thread; NetKey): the host first, the guest per CHALLENGE.
// Lag shim (ini lag_ms / jitter_ms / loss_pct): incoming packets wait here before the sim thread sees them; loss drops
// only unreliable channel 1. A reliable packet never leaves before anything that arrived before it, on any channel (no
// pre-LOADING PSTATE after READY); PSTATEs keep their own jitter. RESEARCH.md §4.2, notes/session_impl.md.
#include <enet/enet.h> // first: winsock2.h before windows.h
#include "game.h"
#include "proto.h"
#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <iterator>

namespace {

// kind: NetEvent kind (in) / 0 data, 1 kick (out). serial: the connection it belongs to (below)
struct Item { uint8_t kind, channel; double at; std::vector<uint8_t> data; uint32_t serial; };
SRWLOCK g_lock = SRWLOCK_INIT;
std::deque<Item> g_in, g_out; // net -> sim, sim -> net
HANDLE g_thread;
std::atomic<bool> g_stop;
std::atomic<uint32_t> g_rtt;
NetConfig g_cfg;              // copied by NetStart before the thread starts
// Connection serial: +1 per accepted partner CONNECT (net thread), stamped on what Post queues. The sim thread takes it
// from each kNetConnected it pops (g_conn) and stamps its sends with it; the flush drops a send whose serial is not the
// current connection's, so a REJECT / kick / CHALLENGE / PSTATE written for a peer that has gone never reaches the next.
uint32_t g_serial; // net thread only
uint32_t g_conn;   // sim thread only (written in NetPoll, read in NetSend / NetKick)
uint8_t g_key[32]; // NetKey(): written by the net thread before the session can read it (proto.h), wiped when it ends
size_t g_inBlob;   // channel-2 items in g_in (under g_lock)
std::atomic<uint32_t> g_drops; // received reliable items (channels 0 and 2) dropped by a cap: Post or the lag shim
// channel-2 bytes in packets ENet still holds (queued or unacked; its send queue has no cap): +create, -free (freeCallback,
// net thread: the last fragment's ack, a failed send, a peer reset), so it is back at 0 after every relink
std::atomic<size_t> g_queued;

void Post(uint8_t kind, uint8_t ch, const void* d, size_t n) {
    AcquireSRWLockExclusive(&g_lock);
    // ponytail: a sim thread stalled for minutes (a minimized window) would grow the inbox. Past 4096 items outside
    // channel 2 a new PSTATE (channel 1, 20 Hz) evicts the oldest queued one, so the newest states survive and they never
    // crowd out the reliable control messages (LOADING / READY / BYE, already acked by ENet, never resent). The other
    // channels have separate caps (M6): channel 2 (blob chunks, the destruction stream, up to 4 KB each; host -> guest, so
    // only a guest ever queues it) stops at 8192 items (32 MB), channel 0 at 8192 items of its own (a peer flooding it before HELLO), so a destruction burst never
    // drops a control message. A drop is counted (NetDrops): a broken blob or stream makes the guest ask again
    // (session.cpp, destruction.cpp). Connect / disconnect / failure / note events always go in.
    if (kind == kNetData && ch == kChState && g_in.size() - g_inBlob >= 4096) {
        auto i = std::find_if(g_in.begin(), g_in.end(), [](const Item& it) { return it.kind == kNetData && it.channel == kChState; });
        if (i != g_in.end()) g_in.erase(i); // the front one in practice: O(1)
    }
    bool blob = kind == kNetData && ch == kChBlob;
    if (kind != kNetData || ch == kChState || (blob ? g_inBlob < 8192 : g_in.size() - g_inBlob < 8192)) {
        g_in.push_back({kind, ch, 0, std::vector<uint8_t>((const uint8_t*)d, (const uint8_t*)d + n), g_serial});
        g_inBlob += blob;
    } else
        g_drops++;
    ReleaseSRWLockExclusive(&g_lock);
}
__attribute__((format(printf, 2, 3))) void PostText(uint8_t kind, const char* fmt, ...) {
    char t[160];
    va_list a;
    va_start(a, fmt);
    int n = vsnprintf(t, sizeof t, fmt, a);
    va_end(a);
    Post(kind, 0, t, n < 0 ? 0 : n < (int)sizeof t ? n : sizeof t - 1);
}

uint32_t g_rng = 0x2545F491;
uint32_t Rand() { return g_rng ^= g_rng << 13, g_rng ^= g_rng >> 17, g_rng ^= g_rng << 5; } // xorshift32, net thread only

void Send(ENetPeer* peer, Item& it) {
    ENetPacket* p = enet_packet_create(it.data.data(), it.data.size(), it.channel == kChState ? 0 : ENET_PACKET_FLAG_RELIABLE);
    if (!p) return;
    if (it.channel == kChBlob) g_queued += p->dataLength, p->freeCallback = [](ENetPacket* q) { g_queued -= q->dataLength; };
    if (enet_peer_send(peer, it.channel, p) < 0) enet_packet_destroy(p); // not queued: still ours
}

void Run() {
    static bool initialised; // once per process: WSAStartup(1.1) + timeBeginPeriod(1); never deinitialised (process exit)
    if (!initialised && enet_initialize() != 0) return PostText(kNetFailed, "enet_initialize failed");
    initialised = true;
    ENetAddress addr{};
    addr.host = ENET_HOST_ANY, addr.port = g_cfg.port;
    // Host 4 slots, guest 1 (the dial below relies on peers[0]). One partner only: the CONNECT check below. ENet gives a
    // free slot to any bare CONNECT, even from a forged address (protocol.c:310-341), and one that never acks VERIFY_CONNECT
    // holds it 5-30 s with no event (protocol.c:142, 1375): the host reaps such half-open slots after 2 s while unlinked.
    ENetHost* host = enet_host_create(g_cfg.host ? &addr : nullptr, g_cfg.host ? 4 : 1, kChannels, 0, 0);
    if (!host) {
        if (g_cfg.host) PostText(kNetFailed, "UDP port %u is busy", (unsigned)g_cfg.port);
        else PostText(kNetFailed, "no UDP socket");
        return;
    }
    // The largest packet is a world blob chunk (4005 bytes; the blob goes in chunks, proto.h). ENet's 32 MB defaults
    // (host.c:104) let any peer that connects, before HELLO or the password, make this process allocate 32 MB from one
    // fragment header (protocol.c:580; enet_malloc aborts on failure). Also caps sends (peer.c:115). Waiting data: a
    // received packet past it is dropped unacked (peer.c:956, protocol.c commandError) and resent by the peer, which
    // also throttles it down; 256 KB stays above the sender's 64 KB reliable window, so a blob streams without that.
    host->maximumPacketSize = 4096, host->maximumWaitingData = 256 * 1024;
    Log("net: thread up, %s, lag %d ms, jitter %d ms, loss %d%%", g_cfg.host ? "hosting" : "guest", g_cfg.lagMs,
        g_cfg.jitterMs, g_cfg.lossPct);
    ENetPeer* peer = nullptr;
    bool connected = false, salted = false; // salted: guest, this connection's key is made (from its first CHALLENGE)
    int dials = 0;                          // guest: unanswered dials since the last connect (the HUD note at 3)
    double retryAt = 0, lastAny = 0, since[4] = {}; // since: host, when each slot entered ACKNOWLEDGING_CONNECT (0 = not)
    double kickAt = 0; // when the sim thread's kick was sent (0 = none): the peer is dropped if it has not left 2 s later
    std::vector<Item> delayed, out; // lag shim (net thread only), outbox batch
    size_t shimBlob = 0;            // channel-2 items in delayed (counted apart from the 4096 cap, as Post's g_inBlob)
    for (;;) {
        bool stopping = g_stop.load();
        double now = NowMs();
        if (!g_cfg.host && !peer && !stopping && now >= retryAt) { // guest: (re)connect every 3 s until linked or stopped
            if (enet_address_set_host(&addr, g_cfg.addr) != 0) {
                PostText(kNetFailed, "cannot resolve %s:%u", g_cfg.addr, (unsigned)g_cfg.port);
                break;
            }
            addr.port = g_cfg.port;
            // With peer NULL the one slot can only hold a foreign half-open peer: ENet takes a free slot for any bare
            // CONNECT (protocol.c:316-340), and one that never acks VERIFY_CONNECT keeps it 5-30 s with no event. Free it.
            if (host->peers[0].state != ENET_PEER_STATE_DISCONNECTED) enet_peer_reset(&host->peers[0]);
            // NULL is then only a failed enet_malloc, which aborts first (callbacks.c); a backstop, not a session end
            if (!(peer = enet_host_connect(host, &addr, kChannels, 0))) { Log("net: no free peer slot, retrying in 3 s"); retryAt = now + 3000; continue; }
            Log("net: connecting to %s:%u", g_cfg.addr, (unsigned)g_cfg.port);
        }
        // host, unlinked: reset slots half-open for over 2 s (see enet_host_create). A real guest acks VERIFY_CONNECT within
        // one RTT (a lost one is resent at ~0.5 s, then 1.5 s). ponytail: a forged
        // CONNECT flood faster than 4 per 2 s still holds every slot; only a firewall (or a cookie, not in ENet) stops that.
        for (size_t i = 0; g_cfg.host && i < host->peerCount; i++) {
            if (host->peers[i].state != ENET_PEER_STATE_ACKNOWLEDGING_CONNECT) since[i] = 0;
            else if (!since[i]) since[i] = now;
            else if (!peer && now - since[i] > 2000) enet_peer_reset(&host->peers[i]), since[i] = 0;
        }
        AcquireSRWLockExclusive(&g_lock);
        out.assign(std::make_move_iterator(g_out.begin()), std::make_move_iterator(g_out.end()));
        g_out.clear();
        ReleaseSRWLockExclusive(&g_lock);
        for (Item& it : out) {
            if (!connected || it.serial != g_serial) continue; // nothing to send to, or written for an earlier peer: dropped
            if (it.kind == 1) {
                enet_peer_disconnect_later(peer, 0); // after the queued REJECT
                if (!kickAt) kickAt = now;
            } else
                Send(peer, it);
        }
        if (stopping) break;
        ENetEvent ev;
        int r = enet_host_service(host, &ev, 5); // the 5 ms wait paces this loop
        if (r < 0) Sleep(5);
        // at most 256 events per pass, so a flooding peer cannot keep the flush, the outbox and the stop check from running;
        // the rest waits in ENet (maximumWaitingData) / the socket. Never fetch an event this pass will not handle.
        for (int n = 0; r > 0; r = ++n < 256 ? enet_host_service(host, &ev, 0) : 0) {
            if (ev.type == ENET_EVENT_TYPE_CONNECT) {
                // Not ours. Host: a second guest while linked (told at once: it gets a DISCONNECT and redials in 3 s).
                // Guest: only the peer it dialled; ENet also answers incoming CONNECTs on the guest's ephemeral port, and
                // between dials (peer NULL, the 3 s gap) the slot is free.
                if (g_cfg.host ? peer && peer != ev.peer : ev.peer != peer) { enet_peer_disconnect_now(ev.peer, 0); continue; }
                peer = ev.peer, connected = true, kickAt = 0, salted = false, dials = 0, ++g_serial;
                enet_peer_timeout(peer, 32, 15000, 60000); // set after the connect: a dial keeps ENet's default timeouts
                char ip[64] = "?";
                enet_address_get_host_ip(&peer->address, ip, sizeof ip);
                PostText(kNetConnected, "%s:%u", ip, (unsigned)peer->address.port); // carries the new serial
            } else if (ev.type == ENET_EVENT_TYPE_DISCONNECT) {
                if (ev.peer != peer) continue;
                for (Item& it : delayed) Post(kNetData, it.channel, it.data.data(), it.data.size()); // REJECT / BYE first
                delayed.clear(), shimBlob = 0;
                if (connected) Post(kNetDisconnected, 0, nullptr, 0);
                else {
                    Log("net: no answer from %s:%u, retrying in 3 s", g_cfg.addr, (unsigned)g_cfg.port);
                    if (++dials == 3) // the HUD once: a wrong address and a stale build look the same from here
                        PostText(kNetNote, "no answer from %s:%u yet: check the address, and that both games run the same "
                                           "RfgCoop version", g_cfg.addr, (unsigned)g_cfg.port);
                }
                peer = nullptr, connected = false, kickAt = 0, retryAt = NowMs() + 3000;
            } else if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
                ENetPacket* p = ev.packet;
                // channels 0 and 1 carry nothing bigger than PSTATE; channel 2 up to maximumPacketSize (ENet checks it).
                // Channel 2 is host -> guest only: the host drops it here, so its 8192-item Post cap and the lag-shim
                // exemption only ever hold data on the guest (an unauthenticated peer cannot park 32 MB on the host)
                bool drop = ev.peer != peer || !connected || (g_cfg.host && ev.channelID == kChBlob) ||
                            (ev.channelID != kChBlob && p->dataLength > sizeof(MsgPState)) ||
                            (ev.channelID == kChState && g_cfg.lossPct > 0 && (int)(Rand() % 100) < g_cfg.lossPct);
                // Guest: the key from the host's salt, made here (~35 ms, never on the sim thread) before this CHALLENGE is
                // queued, so the session has it when it answers. Once per connection: a host sending more costs no CPU
                // (the session stops on a second one), and a stock host's next CHALLENGE follows our next HELLO, which the
                // session sends only after it is done with this connection, so the key never changes under it.
                if (!drop && !g_cfg.host && !salted && *g_cfg.password && ev.channelID == kChControl &&
                    p->dataLength == sizeof(MsgChallenge) && p->data[0] == kChallenge) {
                    double t0 = NowMs();
                    salted = true;
                    if (!DeriveKey(g_cfg.password, p->data + offsetof(MsgChallenge, salt), g_key))
                        PostText(kNetFailed, "the password key could not be made (BCrypt)");
                    else Log("net: password key made from the host's salt in %.0f ms", NowMs() - t0);
                }
                // ponytail: a flood or a long lag_ms cannot grow the lag shim past 4096 items; reliable data past it is
                // lost (counted, NetDrops), as past Post's caps. Channel 2 is exempt and counted apart (M6: a destruction
                // burst or a world blob is thousands of packets inside one lag slot, and must never crowd out channels 0 / 1);
                // only a guest gets here with it, from the host it dialled.
                if (!drop && (ev.channelID == kChBlob || delayed.size() - shimBlob < 4096)) {
                    double at = now + g_cfg.lagMs + (g_cfg.jitterMs > 0 ? Rand() % (unsigned)(g_cfg.jitterMs + 1) : 0);
                    if (ev.channelID != kChState && at < lastAny) at = lastAny; // waits for everything that arrived before it
                    if (at > lastAny) lastAny = at;
                    delayed.push_back({kNetData, ev.channelID, at, std::vector<uint8_t>(p->data, p->data + p->dataLength)});
                    shimBlob += ev.channelID == kChBlob;
                } else if (!drop && ev.channelID != kChState)
                    g_drops++;
                enet_packet_destroy(p);
            }
        }
        now = NowMs();
        // A kicked peer that has not acked our DISCONNECT 2 s later (a client that never acks; a lost REJECT gets one
        // resend first) is dropped here: otherwise it keeps the one partner slot until ENet's 15-30 s timeout. A reset
        // sends nothing and raises no event, so the session is told here.
        if (peer && kickAt && now - kickAt > 2000) {
            Log("net: the kicked peer did not leave, dropped");
            enet_peer_reset(peer);
            delayed.clear(), shimBlob = 0;
            if (connected) Post(kNetDisconnected, 0, nullptr, 0);
            peer = nullptr, connected = false, kickAt = 0, retryAt = now + 3000;
        }
        // in arrival order. A reliable packet's `at` is >= every earlier packet's, so channel 0 stays in order and nothing
        // that arrived before LOADING / READY leaves after it (as on a real link); PSTATEs may still swap among themselves.
        for (size_t i = 0; i < delayed.size();)
            if (delayed[i].at <= now) {
                Post(kNetData, delayed[i].channel, delayed[i].data.data(), delayed[i].data.size());
                shimBlob -= delayed[i].channel == kChBlob;
                delayed.erase(delayed.begin() + i);
            } else
                i++;
        if (peer && connected) g_rtt = peer->roundTripTime;
    }
    if (peer && connected) { // graceful: the queued BYE goes out first, then up to 1 s for the disconnect ack
        enet_peer_disconnect_later(peer, 0);
        ENetEvent ev;
        for (double end = NowMs() + 1000; NowMs() < end;)
            if (enet_host_service(host, &ev, 50) > 0) {
                if (ev.type == ENET_EVENT_TYPE_RECEIVE) enet_packet_destroy(ev.packet);
                else if (ev.type == ENET_EVENT_TYPE_DISCONNECT) { peer = nullptr; break; }
            }
    }
    if (peer) enet_peer_reset(peer);
    enet_host_destroy(host);
}

DWORD WINAPI Thread(void*) {
    // Host: the password's key first, from this session's salt (PBKDF2, ~35 ms: never on the sim thread); nothing
    // reaches the session before it is made, and the password is wiped. The guest makes its key per CHALLENGE (Run).
    double t0 = NowMs();
    bool pw = g_cfg.host && *g_cfg.password, keyed = !pw || DeriveKey(g_cfg.password, g_cfg.salt, g_key);
    if (g_cfg.host) SecureZeroMemory(g_cfg.password, sizeof g_cfg.password);
    if (!keyed) PostText(kNetFailed, "the password key could not be made (BCrypt)");
    else {
        if (pw) Log("net: password key made in %.0f ms", NowMs() - t0);
        Run();
    }
    SecureZeroMemory(g_cfg.password, sizeof g_cfg.password);
    SecureZeroMemory(g_key, sizeof g_key);
    Log("net: thread down");
    return 0;
}

} // namespace

bool NetStart(const NetConfig& c) {
    if (g_thread) { // a previous session's thread: it ends within ~1 s of NetStop
        if (WaitForSingleObject(g_thread, 3000) != WAIT_OBJECT_0) return Log("net: previous net thread still running"), false;
        CloseHandle(g_thread), g_thread = nullptr;
    }
    AcquireSRWLockExclusive(&g_lock);
    g_in.clear(), g_out.clear(), g_conn = 0, g_inBlob = 0;
    ReleaseSRWLockExclusive(&g_lock);
    g_cfg = c, g_stop = false, g_rtt = 0;
    g_rng ^= (uint32_t)NowMs();
    if (!(g_thread = CreateThread(nullptr, 0, Thread, nullptr, 0, nullptr))) {
        SecureZeroMemory(g_cfg.password, sizeof g_cfg.password); // the thread would have
        return Log("net: CreateThread failed"), false;
    }
    return true;
}

void NetStop() { g_stop = true; }

void NetSend(uint8_t channel, const void* data, size_t n) {
    AcquireSRWLockExclusive(&g_lock);
    g_out.push_back({0, channel, 0, std::vector<uint8_t>((const uint8_t*)data, (const uint8_t*)data + n), g_conn});
    ReleaseSRWLockExclusive(&g_lock);
}

void NetKick() {
    AcquireSRWLockExclusive(&g_lock);
    g_out.push_back({1, 0, 0, {}, g_conn});
    ReleaseSRWLockExclusive(&g_lock);
}

bool NetPoll(NetEvent& e) {
    AcquireSRWLockExclusive(&g_lock);
    bool any = !g_in.empty();
    if (any) {
        Item& it = g_in.front();
        e.kind = it.kind, e.channel = it.channel, e.data = std::move(it.data);
        if (it.kind == kNetConnected) g_conn = it.serial; // the sends that follow are for this peer
        g_inBlob -= it.kind == kNetData && it.channel == kChBlob;
        g_in.pop_front();
    }
    ReleaseSRWLockExclusive(&g_lock);
    return any;
}

uint32_t NetRtt() { return g_rtt; }

uint32_t NetDrops() { return g_drops; }

size_t NetQueued() { return g_queued; }

size_t NetRangeCoded(const void* p, size_t n) { // sim thread only: its own coder context (the net thread never has one)
    static void* ctx = enet_range_coder_create();
    static uint8_t out[4096];
    ENetBuffer b;
    b.data = (void*)p, b.dataLength = n;
    size_t r = ctx && n <= sizeof out ? enet_range_coder_compress(ctx, &b, 1, n, out, sizeof out) : 0;
    return r ? r : n; // 0: it would not shrink (ENet then sends it raw)
}

const uint8_t* NetKey() { return g_key; }
