/* The J1939 engine. See j1939.hpp for the shape of it and why it is decoupled from the bus.
 *
 * The transport protocol follows SAE J1939/21 section 5.10, cross-checked against python-can-j1939
 * (MIT, see NOTICE.md). It deviates from that reference in six places, each because the reference
 * has a gap its own comments acknowledge:
 *
 *   1. A CTS rewinds the send position. The reference reads the "next packet" field and uses it only
 *      as a clamp, so a peer asking to resume after a lost packet gets the wrong data.
 *   2. Received packets are placed by sequence number, not appended in arrival order, and the
 *      message completes only when every packet is present. The reference appends and completes on
 *      byte count, so one duplicate truncates the message and acknowledges it as good.
 *   3. A connection abort is honoured whatever state the session is in, not only while waiting for
 *      a CTS.
 *   4. Inbound RTS and BAM headers are range-checked before anything is allocated.
 *   5. Short frames are rejected and unknown control bytes are dropped rather than throwing. This
 *      code runs inside a third-party process; it may not raise and may not read past a frame.
 *   6. A PDU1 parameter group number announced in a BAM carries a zero PS byte, as the standard
 *      requires. The reference zeroes it only on the RTS path. */

#include "j1939.hpp"

#include <algorithm>
#include <cstring>

namespace rp1210
{
namespace j1939
{

namespace
{

/* Packets needed to carry a message, seven bytes at a time. */
uint8_t packet_count(size_t size)
{
    return (uint8_t)((size + tp::bytes_per_packet - 1) / tp::bytes_per_packet);
}

void put_pgn(uint8_t *out, uint32_t pgn)
{
    out[0] = (uint8_t)(pgn & 0xFF);
    out[1] = (uint8_t)((pgn >> 8) & 0xFF);
    out[2] = (uint8_t)((pgn >> 16) & 0xFF);
}

uint32_t get_pgn(const uint8_t *in)
{
    return (uint32_t)in[0] | ((uint32_t)in[1] << 8) | ((uint32_t)in[2] << 16);
}

} // namespace

engine::engine(frame_sink_t sink, clock_fn_t now_ms, const config_t &config)
    : sink_(std::move(sink)), now_ms_(std::move(now_ms)), config_(config)
{
    if (config_.rx_packets_per_cts == 0)
    {
        config_.rx_packets_per_cts = 1;
    }
}

/* ---------------------------------------------------------------- frame output */

void engine::send_frame(uint32_t pgn, uint8_t priority, uint8_t sa, uint8_t da, const uint8_t *data,
                        uint8_t len)
{
    raw_frame_t f;

    f.id = encode(pgn, priority, sa, da);
    f.len = len;

    if (len > 0)
    {
        memcpy(f.data, data, len);
    }

    if (sink_)
    {
        sink_(f);
    }
}

/* Transport protocol control frames all carry eight bytes and travel on PGN 0xEC00. Only RTS and
 * BAM inherit the application's priority; the rest go out at 7, matching python-can-j1939 and the
 * common practice on real buses. */
void engine::send_cm(uint8_t sa, uint8_t da, uint8_t priority, const uint8_t body[8])
{
    send_frame(pgn_tp_cm, priority, sa, da, body, 8);
}

void engine::send_abort(uint8_t sa, uint8_t da, uint8_t reason, uint32_t pgn)
{
    uint8_t body[8] = {tp::abort, reason, 0xFF, 0xFF, 0xFF, 0, 0, 0};

    put_pgn(&body[5], pgn);
    send_cm(sa, da, 7, body);
}

void engine::send_address_claimed(uint8_t sa)
{
    send_frame(pgn_address_claim, 6, sa, address_global, claim_.name, 8);
}

/* ---------------------------------------------------------------- inbound */

void engine::feed(const raw_frame_t &frame, bool tx)
{
    const addr_t a = decode(frame.id);

    if (tx)
    {
        /* Our own transmission, echoed by the device. It never drives a receive state machine: the
         * driver would otherwise reassemble its own multi-packet message and hand it back as if a
         * peer had sent it. The echo an application sees is the assembled message, produced when
         * the session finishes, not these frames. */
        last_tx_us_ = frame.us;

        if (!echo_)
        {
            return;
        }

        /* Transport and network-management frames are the driver's own business, not the
         * application's, so their echoes are swallowed. */
        if (a.pgn == pgn_tp_cm || a.pgn == pgn_tp_dt || a.pgn == pgn_address_claim)
        {
            return;
        }

        message_t m;

        m.us = frame.us;
        m.pgn = a.pgn;
        m.priority = a.priority;
        m.sa = a.sa;
        m.da = a.da;
        m.echo = true;
        m.data.assign(frame.data, frame.data + frame.len);

        deliver(m);

        return;
    }

    /* When the application packetizes for itself, transport frames are ordinary traffic and the
     * engine keeps its hands off them entirely (RP1210E section 14.2). */
    if (!config_.app_packetizing && (a.pgn == pgn_tp_cm || a.pgn == pgn_tp_dt))
    {
        if (a.pgn == pgn_tp_cm)
        {
            handle_tp_cm(frame, a);
        }
        else
        {
            handle_tp_dt(frame, a);
        }

        return;
    }

    if (a.pgn == pgn_address_claim)
    {
        handle_address_claim(frame, a);
    }
    else if (a.pgn == pgn_request)
    {
        handle_request(frame, a);
    }

    message_t m;

    m.us = frame.us;
    m.pgn = a.pgn;
    m.priority = a.priority;
    m.sa = a.sa;
    m.da = a.da;
    m.data.assign(frame.data, frame.data + frame.len);

    deliver(m);
}

void engine::handle_tp_cm(const raw_frame_t &frame, const addr_t &a)
{
    /* A short connection-management frame is malformed. Dropping it is the only safe answer: the
     * fields below are at fixed offsets and there is nothing to interpret. */
    if (frame.len < 8)
    {
        note("transport frame shorter than eight bytes");

        return;
    }

    const uint8_t control = frame.data[0];
    const uint32_t inner = normalise_pgn(get_pgn(&frame.data[5]));

    switch (control)
    {
        case tp::rts:
        {
            /* Only answer a connection aimed at the address we hold. */
            if (claim_.state != claim_state_t::claimed || a.da != claim_.address)
            {
                return;
            }

            const uint16_t size = (uint16_t)(frame.data[1] | (frame.data[2] << 8));
            const uint8_t packets = frame.data[3];

            /* Range-check before allocating anything. An RTS announcing 65535 bytes is either a
             * broken node or a hostile one, and the reference implementation would hold a session
             * open for it. */
            if (size < tp::min_message || size > tp::max_message || packets != packet_count(size))
            {
                send_abort(a.da, a.sa, tp::abort_resources, inner);
                note("rejected a transport request with an inconsistent size");

                return;
            }

            if (find_rx(a.sa, a.da) != nullptr)
            {
                /* J1939/21: a second connection with the same peer has to be refused. */
                send_abort(a.da, a.sa, tp::abort_busy, inner);

                return;
            }

            rx_session_t *s = open_rx(a.sa, a.da);

            if (s == nullptr)
            {
                send_abort(a.da, a.sa, tp::abort_resources, inner);
                note("out of receive sessions");

                return;
            }

            s->bam = false;
            s->pgn = inner;
            s->priority = a.priority;
            s->size = size;
            s->packets = packets;
            s->data.assign(size, 0);
            s->got.assign(packets, false);
            s->last_us = frame.us;

            const uint8_t granted = (uint8_t)std::min<uint32_t>(config_.rx_packets_per_cts, packets);

            s->granted = granted;
            s->next_cts_at = granted;

            uint8_t body[8] = {tp::cts, granted, 1, 0xFF, 0xFF, 0, 0, 0};

            put_pgn(&body[5], inner);
            send_cm(a.da, a.sa, 7, body);

            s->deadline = now() + timeout::t2;

            break;
        }

        case tp::cts:
        {
            tx_session_t *s = find_tx(a.da, a.sa);

            if (s == nullptr)
            {
                send_abort(a.da, a.sa, tp::abort_resources, inner);

                return;
            }

            const uint8_t num = frame.data[1];
            const uint8_t next = frame.data[2];

            if (num == 0)
            {
                /* The peer is asking us to hold. T4 bounds how long we humour it. */
                s->holds++;

                if ((uint64_t)s->holds * timeout::th > timeout::t4)
                {
                    send_abort(s->sa, s->da, tp::abort_timeout, s->pgn);
                    finish_tx(*s, false);
                    note("peer held the connection open past T4");

                    return;
                }

                s->state = tx_state_t::waiting_cts;
                s->deadline = now() + timeout::th;

                return;
            }

            if (s->state == tx_state_t::sending_cts)
            {
                /* A CTS arriving while a burst is still going out means the peer has lost track. */
                send_abort(s->sa, s->da, tp::abort_cts_while_sending, s->pgn);
                finish_tx(*s, false);

                return;
            }

            s->holds = 0;

            /* Honour the "next packet" field as a rewind. This is the peer's only way to ask for a
             * lost packet again, and ignoring it -- as the reference does -- silently sends the
             * wrong data after any loss. */
            if (next == 0 || next > s->packets)
            {
                send_abort(s->sa, s->da, tp::abort_other, s->pgn);
                finish_tx(*s, false);

                return;
            }

            s->next_packet = (uint8_t)(next - 1);

            const uint8_t remaining = (uint8_t)(s->packets - s->next_packet);
            const uint8_t burst = (uint8_t)std::min<uint32_t>(num, remaining);

            s->burst_end = (uint8_t)(s->next_packet + burst - 1);
            s->state = tx_state_t::sending_cts;
            s->deadline = now();

            break;
        }

        case tp::eom_ack:
        {
            tx_session_t *s = find_tx(a.da, a.sa);

            if (s == nullptr)
            {
                send_abort(a.da, a.sa, tp::abort_resources, inner);

                return;
            }

            finish_tx(*s, true);

            break;
        }

        case tp::bam:
        {
            const uint16_t size = (uint16_t)(frame.data[1] | (frame.data[2] << 8));
            const uint8_t packets = frame.data[3];

            if (size < tp::min_message || size > tp::max_message || packets != packet_count(size))
            {
                /* A broadcast is never aborted; there is nobody to abort to. Just ignore it. */
                note("ignored a broadcast announcement with an inconsistent size");

                return;
            }

            /* A fresh announcement supersedes whatever was in flight from that node. */
            rx_session_t *s = find_rx(a.sa, address_global);

            if (s == nullptr)
            {
                s = open_rx(a.sa, address_global);
            }

            if (s == nullptr)
            {
                note("out of receive sessions");

                return;
            }

            s->bam = true;
            s->pgn = inner;
            s->priority = a.priority;
            s->size = size;
            s->packets = packets;
            s->data.assign(size, 0);
            s->got.assign(packets, false);
            s->granted = 0;
            s->next_cts_at = 0;
            s->last_us = frame.us;
            s->deadline = now() + timeout::t1;

            break;
        }

        case tp::abort:
        {
            /* Honour an abort in whatever state the session is in, and in either direction. The
             * reference only cancels while waiting for a CTS, so an abort mid-burst leaves it
             * transmitting at a peer that has already torn the connection down. */
            if (tx_session_t *s = find_tx(a.da, a.sa))
            {
                finish_tx(*s, false);
                note("peer aborted the transfer");
            }

            if (rx_session_t *r = find_rx(a.sa, a.da))
            {
                r->active = false;
                note("peer aborted the transfer");
            }

            break;
        }

        default:
        {
            /* Extended transport protocol control bytes and anything else we do not implement. Log
             * and drop; never throw out of a DLL loaded into someone else's process. */
            note("ignored an unknown transport control byte");

            break;
        }
    }
}

void engine::handle_tp_dt(const raw_frame_t &frame, const addr_t &a)
{
    if (frame.len < 8)
    {
        note("transport data frame shorter than eight bytes");

        return;
    }

    rx_session_t *s = find_rx(a.sa, a.da);

    if (s == nullptr)
    {
        /* No session: either we never saw the announcement or it has already timed out. */
        return;
    }

    const uint8_t seq = frame.data[0];

    if (seq == 0 || seq > s->packets)
    {
        if (!s->bam)
        {
            send_abort(s->da, s->sa, tp::abort_other, s->pgn);
        }

        s->active = false;
        note("transport data packet out of range");

        return;
    }

    const size_t index = (size_t)(seq - 1);
    const size_t offset = index * tp::bytes_per_packet;

    s->last_us = frame.us;

    /* Place by sequence number rather than appending. A duplicate then costs nothing and a reorder
     * is harmless, where the reference implementation would corrupt the message or finish it early. */
    if (!s->got[index])
    {
        const size_t count = std::min(tp::bytes_per_packet, s->data.size() - offset);

        memcpy(&s->data[offset], &frame.data[1], count);
        s->got[index] = true;
    }

    size_t missing = 0;

    for (size_t i = 0; i < s->got.size(); i++)
    {
        if (!s->got[i])
        {
            missing = i + 1;

            break;
        }
    }

    if (missing == 0)
    {
        complete_rx(*s);

        return;
    }

    if (!s->bam && seq >= s->next_cts_at)
    {
        /* End of the granted burst with packets still outstanding. Ask again from the first gap,
         * which is how a receiver recovers a lost packet. */
        const uint8_t start = (uint8_t)(missing - 1);
        const uint8_t remaining = (uint8_t)(s->packets - start);
        const uint8_t granted = (uint8_t)std::min<uint32_t>(config_.rx_packets_per_cts, remaining);

        uint8_t body[8] = {tp::cts, granted, (uint8_t)(start + 1), 0xFF, 0xFF, 0, 0, 0};

        put_pgn(&body[5], s->pgn);
        send_cm(s->da, s->sa, 7, body);

        s->granted = granted;
        s->next_cts_at = (uint8_t)(start + granted);
        s->deadline = now() + timeout::t2;

        return;
    }

    s->deadline = now() + timeout::t1;
}

void engine::complete_rx(rx_session_t &s)
{
    if (!s.bam)
    {
        uint8_t body[8] = {tp::eom_ack, (uint8_t)(s.size & 0xFF), (uint8_t)((s.size >> 8) & 0xFF),
                           s.packets, 0xFF, 0, 0, 0};

        put_pgn(&body[5], s.pgn);
        send_cm(s.da, s.sa, 7, body);
    }

    message_t m;

    m.us = s.last_us;
    m.pgn = s.pgn;
    m.priority = s.priority;
    m.sa = s.sa;
    /* A broadcast is always reported as addressed to everyone, even when it carried a PDU1 group.
     * A connection-mode transfer is reported as addressed to the address we hold. */
    m.da = s.bam ? address_global : s.da;
    m.bam = s.bam;
    m.data = s.data;

    s.active = false;

    deliver(m);
}

void engine::handle_address_claim(const raw_frame_t &frame, const addr_t &a)
{
    if (frame.len < 8)
    {
        return;
    }

    const bool ours = (claim_.state == claim_state_t::claiming
                       || claim_.state == claim_state_t::claimed)
                      && a.sa == claim_.address;

    if (!ours)
    {
        return;
    }

    const uint64_t theirs = name_value(frame.data);
    const uint64_t mine = name_value(claim_.name);

    if (theirs == mine)
    {
        /* The same NAME is either our own claim coming back to us or a genuine duplicate node.
         * Either way, conceding would be wrong. */
        return;
    }

    if (theirs < mine)
    {
        claim_.state = claim_state_t::lost;
        send_address_claimed(address_null);
        note("lost the address to a contender with a lower NAME");
    }
    else
    {
        /* We outrank the contender, so we restate our claim and keep the address. */
        send_address_claimed(claim_.address);
    }
}

void engine::handle_request(const raw_frame_t &frame, const addr_t &a)
{
    if (frame.len < 3)
    {
        return;
    }

    if (get_pgn(frame.data) != pgn_address_claim)
    {
        return;
    }

    if (a.da != address_global && a.da != claim_.address)
    {
        return;
    }

    if (claim_.state == claim_state_t::claimed)
    {
        send_address_claimed(claim_.address);
    }
    else if (claim_.state == claim_state_t::lost)
    {
        /* J1939/81 wants a node that could not claim to say so rather than stay silent, which is
         * what the reference implementation does. A tool scanning the bus should still see us. */
        send_address_claimed(address_null);
    }
}

/* ---------------------------------------------------------------- sessions */

engine::rx_session_t *engine::find_rx(uint8_t sa, uint8_t da)
{
    for (rx_session_t &s : rx_)
    {
        if (s.active && s.sa == sa && s.da == da)
        {
            return &s;
        }
    }

    return nullptr;
}

engine::rx_session_t *engine::open_rx(uint8_t sa, uint8_t da)
{
    for (rx_session_t &s : rx_)
    {
        if (!s.active)
        {
            s = rx_session_t();
            s.active = true;
            s.sa = sa;
            s.da = da;

            return &s;
        }
    }

    return nullptr;
}

engine::tx_session_t *engine::find_tx(uint8_t sa, uint8_t da)
{
    for (tx_session_t &s : tx_)
    {
        if (s.active && s.sa == sa && s.da == da)
        {
            return &s;
        }
    }

    return nullptr;
}

/* ---------------------------------------------------------------- outbound */

result_t engine::submit(uint32_t pgn, uint8_t priority, uint8_t sa, uint8_t da, bool prefer_bam,
                        const uint8_t *data, size_t len)
{
    if (len > tp::max_message)
    {
        return result_t::too_long;
    }

    if (len > 8)
    {
        if (config_.app_packetizing)
        {
            /* The application said it would do its own packetizing, so a long message is its bug. */
            return result_t::too_long;
        }

        /* A connection-mode transfer needs a standing address: the peer's clear-to-send is
         * addressed to our source address and we would have no right to answer it. A broadcast has
         * no such handshake, and a single frame is exempt outright, which RP1210E section 16.7
         * states explicitly. */
        const bool broadcast = prefer_bam || da == address_global;

        if (!broadcast)
        {
            if (claim_.state == claim_state_t::lost)
            {
                return result_t::address_lost;
            }

            if (claim_.state != claim_state_t::claimed || sa != claim_.address)
            {
                return result_t::address_never_claimed;
            }
        }
    }

    if (tx_queue_.size() >= config_.tx_queue_limit)
    {
        return result_t::queue_full;
    }

    pending_tx_t p;

    p.pgn = normalise_pgn(pgn);
    p.priority = (uint8_t)(priority & 0x07);
    p.sa = sa;
    p.da = da;
    /* Bit 7 of the How/Priority byte asks for a broadcast announcement. A message to the global
     * address is a broadcast regardless, which RP1210E calls out as a clarification over RP1210A. */
    p.bam = prefer_bam || da == address_global;
    p.data.assign(data, data + len);

    tx_queue_.push_back(std::move(p));

    return result_t::ok;
}

void engine::start_tx(const pending_tx_t &p)
{
    tx_session_t *s = nullptr;

    for (tx_session_t &candidate : tx_)
    {
        if (!candidate.active)
        {
            s = &candidate;

            break;
        }
    }

    if (s == nullptr)
    {
        return;
    }

    *s = tx_session_t();

    s->active = true;
    s->bam = p.bam;
    s->sa = p.sa;
    s->da = p.bam ? address_global : p.da;
    s->pgn = p.pgn;
    s->priority = p.priority;
    s->packets = packet_count(p.data.size());
    s->data = p.data;
    s->started = now();

    const uint16_t size = (uint16_t)p.data.size();

    if (p.bam)
    {
        uint8_t body[8] = {tp::bam, (uint8_t)(size & 0xFF), (uint8_t)((size >> 8) & 0xFF),
                           s->packets, 0xFF, 0, 0, 0};

        put_pgn(&body[5], s->pgn);
        send_cm(s->sa, address_global, s->priority, body);

        s->state = tx_state_t::sending_bam;
        s->deadline = now() + config_.bam_interpacket_ms;
    }
    else
    {
        const uint8_t max_per_cts =
            (uint8_t)std::min<uint32_t>(config_.rx_packets_per_cts, s->packets);

        uint8_t body[8] = {tp::rts, (uint8_t)(size & 0xFF), (uint8_t)((size >> 8) & 0xFF),
                           s->packets, max_per_cts, 0, 0, 0};

        put_pgn(&body[5], s->pgn);
        send_cm(s->sa, s->da, s->priority, body);

        s->state = tx_state_t::waiting_cts;
        s->deadline = now() + timeout::t3;
    }
}

void engine::finish_tx(tx_session_t &s, bool delivered)
{
    if (delivered && echo_)
    {
        message_t m;

        m.us = last_tx_us_;
        m.pgn = s.pgn;
        m.priority = s.priority;
        m.sa = s.sa;
        m.da = s.bam ? address_global : s.da;
        m.bam = s.bam;
        m.echo = true;
        m.data = s.data;

        deliver(m);
    }

    s = tx_session_t();
}

/* Sends one data packet of a session. The last one is padded to a full frame with 0xFF. */
void engine::pump_tx()
{
    const uint64_t t = now();

    for (tx_session_t &s : tx_)
    {
        if (!s.active)
        {
            continue;
        }

        switch (s.state)
        {
            case tx_state_t::waiting_cts:
            {
                if (t >= s.deadline)
                {
                    send_abort(s.sa, s.da, tp::abort_timeout, s.pgn);
                    finish_tx(s, false);
                    note("no clear-to-send arrived in time");
                }

                break;
            }

            case tx_state_t::sending_cts:
            {
                while (s.active && s.state == tx_state_t::sending_cts && t >= s.deadline)
                {
                    const size_t offset = (size_t)s.next_packet * tp::bytes_per_packet;
                    const size_t count = std::min(tp::bytes_per_packet, s.data.size() - offset);

                    uint8_t body[8];

                    body[0] = (uint8_t)(s.next_packet + 1);
                    memset(&body[1], 0xFF, tp::bytes_per_packet);
                    memcpy(&body[1], &s.data[offset], count);

                    /* State first, then the frame. On a fast link the peer's answer can arrive
                     * before the send call returns. */
                    const bool last_of_burst = (s.next_packet == s.burst_end);
                    const bool last_of_message = (s.next_packet + 1 >= s.packets);

                    s.next_packet++;

                    if (last_of_burst || last_of_message)
                    {
                        s.state = tx_state_t::waiting_cts;
                        s.deadline = t + timeout::t3;
                    }
                    else if (config_.cmdt_interpacket_ms > 0)
                    {
                        s.deadline = t + config_.cmdt_interpacket_ms;
                    }

                    send_frame(pgn_tp_dt, 7, s.sa, s.da, body, 8);

                    if (config_.cmdt_interpacket_ms > 0)
                    {
                        break;
                    }
                }

                break;
            }

            case tx_state_t::sending_bam:
            {
                if (t < s.deadline)
                {
                    break;
                }

                const size_t offset = (size_t)s.next_packet * tp::bytes_per_packet;
                const size_t count = std::min(tp::bytes_per_packet, s.data.size() - offset);

                uint8_t body[8];

                body[0] = (uint8_t)(s.next_packet + 1);
                memset(&body[1], 0xFF, tp::bytes_per_packet);
                memcpy(&body[1], &s.data[offset], count);

                s.next_packet++;

                const bool done = (s.next_packet >= s.packets);

                if (!done)
                {
                    s.deadline = t + config_.bam_interpacket_ms;
                }

                send_frame(pgn_tp_dt, 7, s.sa, address_global, body, 8);

                if (done)
                {
                    finish_tx(s, true);
                }

                break;
            }

            case tx_state_t::finished:
            case tx_state_t::idle:
            default:
            {
                s = tx_session_t();

                break;
            }
        }
    }

    /* Dispatch what is waiting. Single frames go out straight away; a multi-packet message needs a
     * free session and no other transfer already running to that peer, so it waits its turn without
     * holding up the short traffic behind it. */
    for (auto it = tx_queue_.begin(); it != tx_queue_.end();)
    {
        if (it->data.size() <= 8)
        {
            send_frame(it->pgn, it->priority, it->sa, it->da, it->data.data(),
                       (uint8_t)it->data.size());
            it = tx_queue_.erase(it);

            continue;
        }

        const uint8_t peer = it->bam ? address_global : it->da;

        bool busy = (find_tx(it->sa, peer) != nullptr);
        bool room = false;

        for (const tx_session_t &s : tx_)
        {
            if (!s.active)
            {
                room = true;

                break;
            }
        }

        if (busy || !room)
        {
            ++it;

            continue;
        }

        start_tx(*it);
        it = tx_queue_.erase(it);
    }
}

/* ---------------------------------------------------------------- timers */

uint32_t engine::tick()
{
    const uint64_t t = now();

    if (claim_.state == claim_state_t::claiming && t >= claim_.settle_at)
    {
        claim_.state = claim_state_t::claimed;
    }

    for (rx_session_t &s : rx_)
    {
        if (s.active && t >= s.deadline)
        {
            /* A broadcast is discarded without a word: J1939/21 has no way to abort one, and the
             * originator is not listening for a reply. */
            if (!s.bam)
            {
                send_abort(s.da, s.sa, tp::abort_timeout, s.pgn);
            }

            s.active = false;
            note("a transfer timed out and was discarded");
        }
    }

    pump_tx();

    /* Report how long the caller may sleep. Capped so a stop request is noticed promptly. */
    uint64_t next = t + 50;

    for (const rx_session_t &s : rx_)
    {
        if (s.active && s.deadline < next)
        {
            next = s.deadline;
        }
    }

    for (const tx_session_t &s : tx_)
    {
        if (s.active && s.deadline < next)
        {
            next = s.deadline;
        }
    }

    if (claim_.state == claim_state_t::claiming && claim_.settle_at < next)
    {
        next = claim_.settle_at;
    }

    return (next <= t) ? 0 : (uint32_t)(next - t);
}

/* ---------------------------------------------------------------- address claim */

result_t engine::begin_claim(uint8_t address, const uint8_t name[8])
{
    if (claim_.state == claim_state_t::claimed && claim_.address != address)
    {
        return result_t::claim_failed;
    }

    memcpy(claim_.name, name, 8);
    claim_.address = address;

    send_address_claimed(address);

    /* J1939/81: an address in the preferred range 0..127 or the service tool range 248..253 may be
     * used as soon as the claim is out. Only the arbitrary range has to sit out the contention
     * window, so a diagnostic tool claiming 249 is ready immediately. */
    if (address_needs_veto_wait(address))
    {
        claim_.state = claim_state_t::claiming;
        claim_.settle_at = now() + 250;
    }
    else
    {
        claim_.state = claim_state_t::claimed;
        claim_.settle_at = now();
    }

    return result_t::ok;
}

bool engine::claim_settled() const
{
    return claim_.state != claim_state_t::claiming;
}

uint8_t engine::claimed_address() const
{
    return (claim_.state == claim_state_t::claimed) ? claim_.address : address_null;
}

result_t engine::release_claim(uint8_t address)
{
    if (claim_.state == claim_state_t::none || claim_.address != address)
    {
        return result_t::address_never_claimed;
    }

    claim_ = claim_t();

    return result_t::ok;
}

/* ---------------------------------------------------------------- filtering */

void engine::set_gate(gate_t gate)
{
    gate_ = gate;

    if (gate != gate_t::list)
    {
        filters_.clear();
    }
}

void engine::set_filter_exclusive(bool exclusive)
{
    filter_exclusive_ = exclusive;
    filters_.clear();
    /* An exclusive set with nothing in it blocks nothing, so everything passes until the first
     * entry arrives; an inclusive set with nothing in it passes nothing. */
    gate_ = exclusive ? gate_t::list : gate_t::none;
}

bool engine::add_filter(const filter_t &filter)
{
    if (filters_.size() >= max_filters)
    {
        return false;
    }

    filter_t f = filter;

    f.pgn = normalise_pgn(f.pgn);
    filters_.push_back(f);
    gate_ = gate_t::list;

    return true;
}

void engine::clear_filters()
{
    filters_.clear();
}

bool engine::passes_filter(const message_t &m) const
{
    if (gate_ == gate_t::none)
    {
        return false;
    }

    if (gate_ == gate_t::all)
    {
        return true;
    }

    bool matched = false;

    for (const filter_t &f : filters_)
    {
        if ((f.flags & filter_pgn) != 0 && f.pgn != m.pgn)
        {
            continue;
        }

        if ((f.flags & filter_source) != 0 && f.sa != m.sa)
        {
            continue;
        }

        if ((f.flags & filter_destination) != 0 && f.da != m.da)
        {
            continue;
        }

        matched = true;

        break;
    }

    return filter_exclusive_ ? !matched : matched;
}

void engine::deliver(const message_t &m)
{
    if (!passes_filter(m))
    {
        return;
    }

    if (rx_queue_.size() >= config_.rx_queue_limit)
    {
        rx_queue_.pop_front();
        note("dropped a message from a full receive queue");
    }

    rx_queue_.push_back(m);
}

bool engine::pop(message_t &out)
{
    if (rx_queue_.empty())
    {
        return false;
    }

    out = std::move(rx_queue_.front());
    rx_queue_.pop_front();

    return true;
}

const message_t *engine::front() const
{
    return rx_queue_.empty() ? nullptr : &rx_queue_.front();
}

void engine::drop_front()
{
    if (!rx_queue_.empty())
    {
        rx_queue_.pop_front();
    }
}

void engine::reannounce()
{
    if (claim_.state == claim_state_t::claimed)
    {
        send_address_claimed(claim_.address);
    }
}

void engine::flush()
{
    rx_queue_.clear();
    tx_queue_.clear();

    for (rx_session_t &s : rx_)
    {
        s = rx_session_t();
    }

    for (tx_session_t &s : tx_)
    {
        s = tx_session_t();
    }
}

void engine::set_interpacket_ms(uint32_t ms)
{
    /* RP1210 lets an application ask for anything; J1939/21 allows 50..200 ms between broadcast
     * packets. Clamp rather than refuse, so a request for "as fast as possible" gets the fastest
     * legal answer instead of an error. */
    config_.bam_interpacket_ms = std::min<uint32_t>(std::max<uint32_t>(ms, 10), 200);
}

} // namespace j1939
} // namespace rp1210
