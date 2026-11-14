#ifndef CANSUB_RP1210_CLIENT_HPP
#define CANSUB_RP1210_CLIENT_HPP

/* Per-client state behind the RP1210 entry points. One client is one CAN channel on one device.
 *
 * All device knowledge lives in libcansub; this layer only translates. */

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "cansub/bus.hpp"
#include "j1939.hpp"
#include "rp1210_codes.h"

namespace rp1210
{

/* RP1210 client ids run 0..127. The device offers far fewer channels, but the table is cheap. */
constexpr int max_clients = 128;

/* Which protocol the application asked for in RP1210_ClientConnect. The two behave differently
 * enough -- message layout, filtering, and which commands are legal -- that every entry point
 * branches on it. */
enum class protocol_t
{
    can,
    j1939
};

struct client_t
{
    bool connected = false;
    short id = -1;

    protocol_t protocol = protocol_t::can;

    /* nIsAppPacketizingIncomingMsgs. When set, the application does its own J1939 transport
     * protocol in both directions and the driver keeps out of it (RP1210E section 14.2). */
    bool app_packetizing = false;

    std::unique_ptr<cansub::bus> can;

    std::string host;
    int channel = 0;
    cansub::config_t config;

    /* Echo mode changes the wire layout of every subsequent read: with it on, a message gains a
     * byte between the timestamp and the message type (RP1210E section 17.7). */
    bool echo = false;

    /* RPCMD_SET_BLOCKTIMEOUT, in milliseconds, for a blocking read. */
    int block_timeout_ms = 5000;

    /* RP1210 sets filters one identifier at a time, so both lists accumulate.
     *
     * Inclusive filters go to the device's hardware. Exclusive filters have no hardware equivalent,
     * since CANsub filters are accept-only, so an exclusive set passes everything in hardware and
     * drops the listed identifiers here. */
    bool filter_exclusive = false;
    std::vector<cansub::filter_t> inclusive;

    /* An exclusive entry is an identifier and a mask, the same pair RP1210's filter command
     * carries, evaluated together: a frame is blocked when frame.id & mask == id & mask. */
    std::vector<uint32_t> exclusive_ids;
    std::vector<uint32_t> exclusive_masks;

    short last_error = RP1210_NO_ERRORS;

    /* J1939 only. The engine holds the transport protocol and address claim state machines; the
     * worker thread owns the bus for the lifetime of the client and is the only thing that calls
     * send and recv on it.
     *
     * A thread is unavoidable here. RP1210 lets an application transmit without ever reading, and
     * block for seconds when it does read, but J1939 obliges the driver to answer a request to send
     * within 200 ms, pace broadcast packets, run the J1939/21 timers and defend a claimed address
     * the whole time. None of that can be driven from inside the API calls.
     *
     * engine_mutex guards the engine against the API entry points; engine_wake lets a submitted
     * message or a command interrupt the worker's sleep. */
    std::unique_ptr<j1939::engine> engine;
    std::thread worker;
    std::mutex engine_mutex;
    std::condition_variable engine_wake;
    std::condition_variable rx_wake; /* A blocking read waits here. */
    std::atomic<bool> worker_stop{false};

    void start_worker();
    void stop_worker();
};

/* The client table is shared across threads because a host application may read on one thread and
 * send on another. */
client_t *acquire(short id);
client_t *allocate(short &id);
void release(short id);
std::mutex &table_mutex();

} // namespace rp1210

#endif
