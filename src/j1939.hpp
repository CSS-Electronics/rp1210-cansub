#ifndef CANSUB_RP1210_J1939_HPP
#define CANSUB_RP1210_J1939_HPP

/* The J1939 engine: transport protocol, address claim, and the RP1210 message filter.
 *
 * Deliberately knows nothing about cansub::bus or Windows. Frames arrive through feed() and leave
 * through a sink callback, and time comes from an injected clock. That is what lets the whole
 * state machine, including the J1939/21 timers, be tested in milliseconds with no hardware; the
 * glue that connects it to a real channel lives in rp1210_api.cpp.
 *
 * Single threaded by construction: the driver's worker thread owns an engine and is the only caller
 * of feed(), tick() and submit(). The queues the RP1210 entry points touch are guarded separately,
 * by the client. */

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <vector>

#include "j1939_id.hpp"

namespace rp1210
{
namespace j1939
{

/* A classic CAN frame, which is all J1939 uses. Independent of cansub::frame_t on purpose. */
struct raw_frame_t
{
    uint32_t id = 0;
    uint8_t data[8] = {};
    uint8_t len = 0;
    uint64_t us = 0; /* Device timestamp, microseconds since the epoch. */
};

/* An assembled message, in the terms RP1210 reports them. */
struct message_t
{
    uint64_t us = 0; /* For a multi-packet message, the last packet's timestamp. */
    uint32_t pgn = 0;
    uint8_t priority = 0;
    uint8_t sa = 0;
    uint8_t da = address_global;
    bool bam = false;  /* Sets bit 7 of the How/Priority byte: BAM carried it, not RTS/CTS. */
    bool echo = false; /* Our own transmission, completed. */
    std::vector<uint8_t> data;
};

/* Transport protocol connection management control bytes (J1939/21 section 5.10). */
namespace tp
{
constexpr uint8_t rts = 16;
constexpr uint8_t cts = 17;
constexpr uint8_t eom_ack = 19;
constexpr uint8_t bam = 32;
constexpr uint8_t abort = 255;

/* Connection abort reasons. 4 is an AUTOSAR extension rather than an SAE one, but it is what
 * everybody sends when a CTS turns up mid-burst. */
constexpr uint8_t abort_busy = 1;
constexpr uint8_t abort_resources = 2;
constexpr uint8_t abort_timeout = 3;
constexpr uint8_t abort_cts_while_sending = 4;
constexpr uint8_t abort_other = 250;

constexpr size_t bytes_per_packet = 7;
constexpr size_t max_message = 1785; /* 255 packets of 7 bytes, and the limit of the RP1210 field. */
constexpr size_t min_message = 9;    /* Eight bytes or fewer travel as a single frame. */
} // namespace tp

/* Timeouts from J1939/21 section 5.10.3, in milliseconds. */
namespace timeout
{
constexpr uint32_t tr = 200;  /* Responder: answer an RTS within this. */
constexpr uint32_t th = 500;  /* Originator: hold after a CTS asking for zero packets. */
constexpr uint32_t t1 = 750;  /* Responder: gap between inbound data packets. */
constexpr uint32_t t2 = 1250; /* Responder: first data packet after we sent a CTS. */
constexpr uint32_t t3 = 1250; /* Originator: CTS or acknowledgement after we sent RTS or a burst. */
constexpr uint32_t t4 = 1050; /* Originator: ceiling on consecutive holds. */
constexpr uint32_t bam_default = 50; /* Broadcast interpacket time; the legal range is 50..200. */
} // namespace timeout

/* Filter flags for RP1210_Set_Message_Filtering_For_J1939. Flag 2 was the priority filter and was
 * withdrawn from the standard, which is why the command still carries a zero placeholder byte. */
constexpr uint8_t filter_pgn = 0x01;
constexpr uint8_t filter_source = 0x04;
constexpr uint8_t filter_destination = 0x08;
constexpr uint8_t filter_flags_mask = filter_pgn | filter_source | filter_destination;

struct filter_t
{
    uint8_t flags = 0;
    uint32_t pgn = 0;
    uint8_t sa = 0;
    uint8_t da = 0;
};

/* Whether the client is allowed to receive anything yet. RP1210 requires that a freshly connected
 * client sees nothing at all until it sets a filter or asks for everything. */
enum class gate_t
{
    none,
    all,
    list
};

enum class claim_state_t
{
    none,
    claiming,
    claimed,
    lost
};

/* What submit() and the claim calls report back, mapped to RP1210 error codes by the caller. */
enum class result_t
{
    ok,
    too_long,
    address_never_claimed,
    address_lost,
    queue_full,
    claim_failed,
    claim_in_progress
};

using frame_sink_t = std::function<void(const raw_frame_t &)>;
using clock_fn_t = std::function<uint64_t()>; /* Milliseconds, monotonic. */

class engine
{
public:
    struct config_t
    {
        /* How many packets we grant per CTS as a receiver. python-can-j1939 defaults to one, which
         * is legal but slow; sixteen is a common real-world choice. Whatever we pick, a peer that
         * grants us one at a time has to keep working. */
        uint8_t rx_packets_per_cts = 16;

        /* Broadcast interpacket time, from RP1210_Set_J1939_Interpacket_Time. */
        uint32_t bam_interpacket_ms = timeout::bam_default;

        /* Optional floor between data packets inside one CTS window. Zero bursts the whole window.
         * Standard-conforming peers do not need this; some real ones drop frames without it. */
        uint32_t cmdt_interpacket_ms = 0;

        /* The application does its own packetizing (ClientConnect's nIsAppPacketizingIncomingMsgs).
         * Transport frames then pass straight through and nothing is segmented or reassembled. */
        bool app_packetizing = false;

        size_t rx_queue_limit = 1024;
        size_t tx_queue_limit = 64;
    };

    engine(frame_sink_t sink, clock_fn_t now_ms, const config_t &config = {});

    /* One received frame. Frames the device echoes back to us carry tx = true; they time our own
     * transmissions and are never fed to the receive state machines. */
    void feed(const raw_frame_t &frame, bool tx);

    /* Runs timers and the transmit pump. Returns how long the caller may sleep, in milliseconds. */
    uint32_t tick();

    /* Queues an application message for transmission, segmenting it if it does not fit in a frame. */
    result_t submit(uint32_t pgn, uint8_t priority, uint8_t sa, uint8_t da, bool prefer_bam,
                    const uint8_t *data, size_t len);

    /* Assembled, filter-passed messages waiting for RP1210_ReadMessage. front() lets the caller
     * size its buffer before committing: RP1210 reports a buffer that is too small as an error and
     * the message stays queued for a second attempt, rather than being lost to it. */
    bool pop(message_t &out);
    const message_t *front() const;
    void drop_front();
    size_t pending() const { return rx_queue_.size(); }
    void flush();

    /* Restates a held address claim, after the channel has been down. */
    void reannounce();

    /* Address claim. begin_claim starts the procedure; claim_settled reports whether the contention
     * window has closed, for the blocking form of the command. */
    result_t begin_claim(uint8_t address, const uint8_t name[8]);
    bool claim_settled() const;
    claim_state_t claim_state() const { return claim_.state; }
    uint8_t claimed_address() const;
    result_t release_claim(uint8_t address);

    /* Filtering. */
    void set_gate(gate_t gate);
    void set_filter_exclusive(bool exclusive);
    bool add_filter(const filter_t &filter);
    void clear_filters();

    void set_interpacket_ms(uint32_t ms);
    void set_echo(bool on) { echo_ = on; }

    /* Diagnostics surfaced through RP1210_GetLastErrorMsg. */
    const char *last_note() const { return note_; }

private:
    struct rx_session_t
    {
        bool active = false;
        bool bam = false;
        uint8_t sa = 0;
        uint8_t da = 0;
        uint32_t pgn = 0;
        uint8_t priority = 0;
        uint16_t size = 0;
        uint8_t packets = 0;
        uint8_t next_cts_at = 0; /* Sequence number that triggers the next CTS. */
        uint8_t granted = 0;
        uint64_t deadline = 0;
        uint64_t last_us = 0;
        std::vector<uint8_t> data;
        std::vector<bool> got; /* One entry per packet, so duplicates and reorders are harmless. */
    };

    enum class tx_state_t
    {
        idle,
        waiting_cts,
        sending_cts,
        sending_bam,
        finished
    };

    struct tx_session_t
    {
        bool active = false;
        tx_state_t state = tx_state_t::idle;
        bool bam = false;
        uint8_t sa = 0;
        uint8_t da = 0;
        uint32_t pgn = 0;
        uint8_t priority = 0;
        uint8_t packets = 0;
        uint8_t next_packet = 0;  /* Zero-based index of the next packet to send. */
        uint8_t burst_end = 0;    /* Last packet index of the current CTS window. */
        uint32_t holds = 0;       /* Consecutive zero-packet CTS messages, bounded by T4. */
        uint64_t deadline = 0;
        uint64_t started = 0;
        std::vector<uint8_t> data;
    };

    struct pending_tx_t
    {
        uint32_t pgn = 0;
        uint8_t priority = 0;
        uint8_t sa = 0;
        uint8_t da = 0;
        bool bam = false;
        std::vector<uint8_t> data;
    };

    struct claim_t
    {
        claim_state_t state = claim_state_t::none;
        uint8_t address = address_null;
        uint8_t name[8] = {};
        uint64_t settle_at = 0;
    };

    void handle_tp_cm(const raw_frame_t &frame, const addr_t &a);
    void handle_tp_dt(const raw_frame_t &frame, const addr_t &a);
    void handle_address_claim(const raw_frame_t &frame, const addr_t &a);
    void handle_request(const raw_frame_t &frame, const addr_t &a);

    rx_session_t *find_rx(uint8_t sa, uint8_t da);
    rx_session_t *open_rx(uint8_t sa, uint8_t da);
    tx_session_t *find_tx(uint8_t sa, uint8_t da);

    void send_frame(uint32_t pgn, uint8_t priority, uint8_t sa, uint8_t da, const uint8_t *data,
                    uint8_t len);
    void send_cm(uint8_t sa, uint8_t da, uint8_t priority, const uint8_t body[8]);
    void send_abort(uint8_t sa, uint8_t da, uint8_t reason, uint32_t pgn);
    void send_address_claimed(uint8_t sa);

    void complete_rx(rx_session_t &s);
    void pump_tx();
    void start_tx(const pending_tx_t &p);
    void finish_tx(tx_session_t &s, bool delivered);

    void deliver(const message_t &m);
    bool passes_filter(const message_t &m) const;

    uint64_t now() const { return now_ms_(); }
    void note(const char *text) { note_ = text; }

    frame_sink_t sink_;
    clock_fn_t now_ms_;
    config_t config_;

    static constexpr size_t max_rx_sessions = 8;
    static constexpr size_t max_tx_sessions = 4;

    rx_session_t rx_[max_rx_sessions];
    tx_session_t tx_[max_tx_sessions];
    std::deque<pending_tx_t> tx_queue_;
    std::deque<message_t> rx_queue_;

    claim_t claim_;

    gate_t gate_ = gate_t::none;
    bool filter_exclusive_ = false;
    std::vector<filter_t> filters_;
    static constexpr size_t max_filters = 64;

    bool echo_ = false;
    uint64_t last_tx_us_ = 0; /* Device timestamp of our most recent transmission. */
    const char *note_ = "";
};

} // namespace j1939
} // namespace rp1210

#endif
