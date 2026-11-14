/* J1939 layer tests that need no hardware.
 *
 * Covers the 29-bit identifier mapping exhaustively and the NAME rules used by address claim. The
 * identifier bit layout is the part of a J1939 implementation that is easiest to get subtly wrong
 * and hardest to debug on a bus, so it is pinned here rather than only on the wire.
 *
 * Several vectors are lifted from python-can-j1939's own pytest suite (MIT, see NOTICE.md), which
 * pins the exact frames a working stack puts on the wire. Agreeing with an independently tested
 * implementation is worth more than agreeing with our own reading of the standard. */

#include <cstdio>
#include <initializer_list>
#include <vector>

#include "j1939.hpp"
#include "j1939_id.hpp"

using namespace rp1210::j1939;

namespace
{

int failures = 0;

void check(bool condition, const char *what)
{
    if (!condition)
    {
        printf("  FAIL %s\n", what);
        failures++;
    }
}

struct id_vector_t
{
    uint32_t id;
    uint32_t pgn;
    uint8_t priority;
    uint8_t sa;
    uint8_t da;
    const char *what;
};

/* Both directions must agree for each of these. The DA and PGN columns are already in the
 * normalised form RP1210 reports, so encode(decode(id)) is the identity for every row. */
const id_vector_t id_vectors[] = {
    {0x0CF00400, 0x00F004, 3, 0x00, 0xFF, "PDU2 EEC1 from the engine"},
    {0x18FECA00, 0x00FECA, 6, 0x00, 0xFF, "PDU2 DM1"},
    {0x18FF2321, 0x00FF23, 6, 0x21, 0xFF, "PDU2 proprietary B"},
    {0x18EF1721, 0x00EF00, 6, 0x21, 0x17, "PDU1 proprietary A, addressed"},
    {0x18EA00F9, 0x00EA00, 6, 0xF9, 0x00, "PDU1 request to address 0"},
    {0x18EAFFF9, 0x00EA00, 6, 0xF9, 0xFF, "PDU1 request to the global address"},
    {0x18EEFF80, 0x00EE00, 6, 0x80, 0xFF, "address claimed"},
    {0x18EEFFFE, 0x00EE00, 6, 0xFE, 0xFF, "cannot claim address"},
    {0x1CEC2180, 0x00EC00, 7, 0x80, 0x21, "TP.CM to a peer"},
    {0x1CEB2180, 0x00EB00, 7, 0x80, 0x21, "TP.DT to a peer"},
    {0x1CECFF80, 0x00EC00, 7, 0x80, 0xFF, "TP.CM BAM"},
    {0x19F2000B, 0x01F200, 6, 0x0B, 0xFF, "PDU2 with the data page set"},
    {0x19EF170B, 0x01EF00, 6, 0x0B, 0x17, "PDU1 with the data page set"},
    {0x0EF00400, 0x02F004, 3, 0x00, 0xFF, "PDU2 with the extended data page set"},
    {0x1FFFFF00, 0x03FFFF, 7, 0x00, 0xFF, "top of the identifier range"},
    {0x00EF1721, 0x00EF00, 0, 0x21, 0x17, "priority zero is legal"},
    /* From python-can-j1939's test_ecu.py, which pins these on a working stack. */
    {0x18EC9B90, 0x00EC00, 6, 0x90, 0x9B, "TP.CM RTS, reference vector"},
    {0x1CEB9B90, 0x00EB00, 7, 0x90, 0x9B, "TP.DT, reference vector"},
    {0x18ECFF90, 0x00EC00, 6, 0x90, 0xFF, "TP.CM BAM, reference vector"},
    {0x1CEBFF90, 0x00EB00, 7, 0x90, 0xFF, "TP.DT broadcast, reference vector"},
};

void test_id_vectors()
{
    printf("identifier vectors\n");

    for (const id_vector_t &v : id_vectors)
    {
        const addr_t got = decode(v.id);

        check(got.pgn == v.pgn, v.what);
        check(got.priority == v.priority, v.what);
        check(got.sa == v.sa, v.what);
        check(got.da == v.da, v.what);

        check(encode(v.pgn, v.priority, v.sa, v.da) == v.id, v.what);
    }

    printf("  %zu vectors, both directions\n", sizeof(id_vectors) / sizeof(id_vectors[0]));
}

/* A PDU2 message is a broadcast whatever the caller asks for, so a destination given alongside a
 * PDU2 PGN is ignored rather than folded into the identifier. Getting this wrong turns a broadcast
 * into a peer-to-peer message aimed at a group extension. */
void test_pdu2_ignores_destination()
{
    printf("PDU2 ignores the destination\n");

    check(encode(0x00F004, 3, 0x00, 0x21) == 0x0CF00400, "PDU2 with DA 0x21 stays a broadcast");
    check(decode(0x0CF00400).da == 0xFF, "PDU2 reports the global destination");

    printf("  a destination alongside a PDU2 PGN is dropped\n");
}

/* A PDU1 PGN carries a zero PS byte; the destination lives in the DA field. An application that
 * bakes the destination into the PGN instead must still work, so normalise_pgn strips it. */
void test_pdu1_normalisation()
{
    printf("PDU1 normalisation\n");

    check(normalise_pgn(0x00EF17) == 0x00EF00, "PDU1 PS byte stripped");
    check(normalise_pgn(0x00FF23) == 0x00FF23, "PDU2 PS byte kept");
    check(normalise_pgn(0x01EF17) == 0x01EF00, "PDU1 with the data page set");
    check(decode(0x18EF1721).pgn == 0x00EF00, "decode normalises");

    check(pdu_format(0x00EF00) == 0xEF, "PDU format extracted");
    check(is_pdu1(0x00EFFF), "PF 0xEF is PDU1");
    check(is_pdu2(0x00F004), "PF 0xF0 is PDU2");
    check(is_pdu1(0x00EF00) && !is_pdu1(0x00F000), "the boundary sits at PF 240");

    printf("  PS stripped for PDU1, kept for PDU2, boundary at PF 240\n");
}

/* Every PGN, both page bits included, must survive a round trip. This is the test that would catch
 * a lost extended data page bit or a mis-shifted PDU format. */
void test_exhaustive_round_trip()
{
    printf("exhaustive round trip\n");

    int mismatches = 0;

    for (uint32_t pgn = 0; pgn <= 0x3FFFF; pgn++)
    {
        const uint8_t priority = 6;
        const uint8_t sa = 0x80;
        const uint8_t da = 0x21;

        const uint32_t id = encode(pgn, priority, sa, da);
        const addr_t got = decode(id);

        const uint32_t want_pgn = normalise_pgn(pgn);
        const uint8_t want_da = is_pdu1(pgn) ? da : (uint8_t)0xFF;

        if (got.pgn != want_pgn || got.priority != priority || got.sa != sa || got.da != want_da)
        {
            mismatches++;
        }

        if (encode(got.pgn, got.priority, got.sa, got.da) != id)
        {
            mismatches++;
        }
    }

    check(mismatches == 0, "all 2^18 PGNs round trip");

    printf("  262144 PGNs, encode then decode then encode\n");
}

/* The identifier is 29 bits and every bit must land somewhere. Walking a single bit through the
 * whole range catches a mask that is one bit too narrow. */
void test_identifier_bits()
{
    printf("identifier bit coverage\n");

    int mismatches = 0;

    for (int bit = 0; bit < 29; bit++)
    {
        const uint32_t id = 1u << bit;
        const addr_t got = decode(id);

        if (encode(got.pgn, got.priority, got.sa, got.da) != id)
        {
            mismatches++;
        }
    }

    check(mismatches == 0, "every identifier bit survives a round trip");

    printf("  29 single-bit identifiers\n");
}

/* NAME contention. The bytes are little-endian and the lower 64-bit value wins; an equal NAME means
 * the contender is us, seen through an echo, and must not be treated as a loss. */
void test_name_rules()
{
    printf("NAME rules\n");

    /* From python-can-j1939's test_ca.py: identity number 1234567, manufacturer code 666, ECU
     * instance 2, function instance 16, function 201, vehicle system 127, system instance 2,
     * industry group 5, not arbitrary address capable. */
    const uint8_t ours[8] = {135, 214, 82, 83, 130, 201, 254, 82};
    const uint8_t lower[8] = {135, 214, 82, 83, 130, 111, 254, 82};
    const uint8_t higher[8] = {135, 214, 82, 83, 130, 222, 254, 82};
    const uint8_t arbitrary[8] = {135, 214, 82, 83, 130, 201, 254, 210};

    check(name_value(ours) == 0x52FEC9825352D687ull, "NAME reads little-endian");

    check(name_value(lower) < name_value(ours), "a lower NAME wins the contention");
    check(name_value(higher) > name_value(ours), "a higher NAME yields");
    check(name_value(ours) == name_value(ours), "an equal NAME is our own echo");

    check(!name_is_arbitrary_address_capable(ours), "byte 7 bit 7 clear");
    check(name_is_arbitrary_address_capable(arbitrary), "byte 7 bit 7 set");
    check(name_value(arbitrary) > name_value(ours),
          "arbitrary address capable sorts above a fixed address");

    printf("  little-endian, lower wins, equal is our own echo\n");
}

/* J1939-81 lets the preferred and service tool ranges start using an address immediately; only the
 * arbitrary range has to sit out the contention window. The boundaries are off-by-one bait. */
void test_claim_ranges()
{
    printf("address claim ranges\n");

    check(!address_needs_veto_wait(0), "address 0 is immediate");
    check(!address_needs_veto_wait(127), "address 127 is immediate");
    check(address_needs_veto_wait(128), "address 128 waits");
    check(address_needs_veto_wait(247), "address 247 waits");
    check(!address_needs_veto_wait(248), "address 248 is immediate");
    check(!address_needs_veto_wait(249), "the offboard diagnostic tool is immediate");
    check(!address_needs_veto_wait(253), "address 253 is immediate");

    check(address_is_claimable(253), "253 can be claimed");
    check(!address_is_claimable(address_null), "the null address cannot be claimed");
    check(!address_is_claimable(address_global), "the global address cannot be claimed");

    printf("  0..127 and 248..253 immediate, 128..247 wait\n");
}

/* ---------------------------------------------------------------- transport protocol
 *
 * The engine takes its frames through a sink and its time from a callback, so the whole of
 * J1939/21 -- including the T1..T4 timers -- can be exercised here in microseconds of wall clock
 * and with no CAN hardware at all. */

const uint8_t test_name[8] = {135, 214, 82, 83, 130, 201, 254, 82};

struct harness
{
    uint64_t clock = 1000;
    std::vector<raw_frame_t> sent;
    engine eng;

    explicit harness(const engine::config_t &config = engine::config_t())
        : eng([this](const raw_frame_t &f) { sent.push_back(f); }, [this] { return clock; }, config)
    {
        eng.set_gate(gate_t::all);
    }

    void advance(uint32_t ms)
    {
        clock += ms;
        eng.tick();
    }

    void rx(uint32_t id, std::initializer_list<uint8_t> bytes)
    {
        raw_frame_t f;

        f.id = id;
        f.len = (uint8_t)bytes.size();
        f.us = clock * 1000;

        uint8_t i = 0;

        for (uint8_t b : bytes)
        {
            f.data[i++] = b;
        }

        eng.feed(f, false);
    }

    void claim(uint8_t address)
    {
        eng.begin_claim(address, test_name);
        advance(300);
        sent.clear();
    }
};

/* A missing frame is a test failure, not a crash: return an empty frame so a failed expectation
 * reports and the rest of the suite still runs. */
raw_frame_t at(const std::vector<raw_frame_t> &frames, size_t index)
{
    return index < frames.size() ? frames[index] : raw_frame_t();
}

bool body_is(const raw_frame_t &f, std::initializer_list<uint8_t> want)
{
    if (f.len != (uint8_t)want.size())
    {
        return false;
    }

    uint8_t i = 0;

    for (uint8_t b : want)
    {
        if (f.data[i++] != b)
        {
            return false;
        }
    }

    return true;
}

std::vector<uint8_t> ramp(size_t n)
{
    std::vector<uint8_t> v(n);

    for (size_t i = 0; i < n; i++)
    {
        v[i] = (uint8_t)(i + 1);
    }

    return v;
}

/* A broadcast of 20 bytes is one announcement and three data packets, paced by the interpacket
 * time, with the short last packet padded out to a full frame. */
void test_bam_transmit()
{
    printf("broadcast transmit\n");

    harness h;
    const std::vector<uint8_t> payload = ramp(20);

    check(h.eng.submit(0x00FEF1, 6, 0x80, 0xFF, true, payload.data(), payload.size())
              == result_t::ok,
          "broadcast accepted");

    h.advance(0);
    check(h.sent.size() == 1, "the announcement goes out first");
    check(at(h.sent, 0).id == 0x18ECFF80, "announcement identifier");
    check(body_is(at(h.sent, 0), {32, 20, 0, 3, 0xFF, 0xF1, 0xFE, 0x00}), "announcement body");

    h.advance(49);
    check(h.sent.size() == 1, "nothing before the interpacket time elapses");

    h.advance(1);
    check(h.sent.size() == 2, "first data packet after 50 ms");
    check(at(h.sent, 1).id == 0x1CEBFF80, "data packets go out at priority 7");
    check(body_is(at(h.sent, 1), {1, 1, 2, 3, 4, 5, 6, 7}), "first data packet");

    h.advance(50);
    check(body_is(at(h.sent, 2), {2, 8, 9, 10, 11, 12, 13, 14}), "second data packet");

    h.advance(50);
    check(h.sent.size() == 4, "four frames in total");
    check(body_is(at(h.sent, 3), {3, 15, 16, 17, 18, 19, 20, 0xFF}), "last packet padded with 0xFF");

    printf("  announcement, three packets, 50 ms apart, 0xFF padding\n");
}

/* The interpacket time is settable, and clamped to the range J1939/21 allows. */
void test_bam_interpacket_time()
{
    printf("broadcast interpacket time\n");

    harness h;
    const std::vector<uint8_t> payload = ramp(20);

    h.eng.set_interpacket_ms(200);
    h.eng.submit(0x00FEF1, 6, 0x80, 0xFF, true, payload.data(), payload.size());
    h.advance(0);
    h.advance(199);
    check(h.sent.size() == 1, "200 ms spacing is honoured");
    h.advance(1);
    check(h.sent.size() == 2, "and the packet follows");

    harness fast;

    fast.eng.set_interpacket_ms(0);
    fast.eng.submit(0x00FEF1, 6, 0x80, 0xFF, true, payload.data(), payload.size());
    fast.advance(0);
    fast.advance(10);
    check(fast.sent.size() == 2, "a request for zero is clamped up to 10 ms, not refused");

    printf("  settable, clamped to 10..200 ms\n");
}

/* A broadcast arriving in three packets becomes one message, stamped with the last packet's time
 * and flagged as having travelled by broadcast. */
void test_bam_receive()
{
    printf("broadcast receive\n");

    harness h;

    h.rx(0x18ECFF21, {32, 20, 0, 3, 0xFF, 0xF1, 0xFE, 0x00});
    h.rx(0x1CEBFF21, {1, 1, 2, 3, 4, 5, 6, 7});
    h.advance(300);
    h.rx(0x1CEBFF21, {2, 8, 9, 10, 11, 12, 13, 14});
    h.rx(0x1CEBFF21, {3, 15, 16, 17, 18, 19, 20, 0xFF});

    const uint64_t last = h.clock * 1000;

    message_t m;

    check(h.eng.pop(m), "one message assembled");
    check(m.pgn == 0x00FEF1, "parameter group");
    check(m.sa == 0x21, "source address");
    check(m.da == 0xFF, "a broadcast is addressed to everyone");
    check(m.bam, "reported as carried by broadcast");
    check(m.data.size() == 20, "padding stripped");
    check(m.data[19] == 20, "last byte");
    check(m.us == last, "stamped with the last packet, not the announcement");
    check(!h.eng.pop(m), "and nothing else");
    check(h.sent.empty(), "a broadcast is never acknowledged");

    printf("  reassembled, padding stripped, last-packet timestamp\n");
}

/* Transport frames are the driver's business. An application asking for everything must still not
 * see them. */
void test_transport_frames_are_not_surfaced()
{
    printf("transport frames stay hidden\n");

    harness h;

    h.rx(0x18ECFF21, {32, 20, 0, 3, 0xFF, 0xF1, 0xFE, 0x00});
    h.rx(0x1CEBFF21, {1, 1, 2, 3, 4, 5, 6, 7});

    message_t m;

    check(!h.eng.pop(m), "no partial message and no raw transport frames");

    printf("  announcement and data packets are consumed\n");
}

/* A peer that grants one packet at a time -- python-can-j1939's own default, so a case that really
 * occurs -- must get exactly what it asked for and no more. */
void test_rts_cts_transmit_split()
{
    printf("connection-mode transmit, split grants\n");

    harness h;

    h.claim(0xF9);

    const std::vector<uint8_t> payload = ramp(20);

    check(h.eng.submit(0x00EF00, 6, 0xF9, 0x21, false, payload.data(), payload.size())
              == result_t::ok,
          "transfer accepted");

    h.advance(0);
    check(h.sent.size() == 1, "request to send goes out first");
    check(at(h.sent, 0).id == 0x18EC21F9, "request identifier keeps the message priority");
    check(body_is(at(h.sent, 0), {16, 20, 0, 3, 3, 0x00, 0xEF, 0x00}), "request body");

    h.sent.clear();
    h.rx(0x1CECF921, {17, 1, 1, 0xFF, 0xFF, 0x00, 0xEF, 0x00});
    h.advance(0);
    check(h.sent.size() == 1, "one packet granted, one packet sent");
    check(at(h.sent, 0).id == 0x1CEB21F9, "data packet identifier");
    check(body_is(at(h.sent, 0), {1, 1, 2, 3, 4, 5, 6, 7}), "first packet");

    h.advance(300);
    check(h.sent.size() == 1, "and nothing more until the next grant");

    h.sent.clear();
    h.rx(0x1CECF921, {17, 2, 2, 0xFF, 0xFF, 0x00, 0xEF, 0x00});
    h.advance(0);
    check(h.sent.size() == 2, "two more granted, two more sent");
    check(body_is(at(h.sent, 0), {2, 8, 9, 10, 11, 12, 13, 14}), "second packet");
    check(body_is(at(h.sent, 1), {3, 15, 16, 17, 18, 19, 20, 0xFF}), "third packet padded");

    h.sent.clear();
    h.rx(0x1CECF921, {19, 20, 0, 3, 0xFF, 0x00, 0xEF, 0x00});
    h.advance(2000);
    check(h.sent.empty(), "the acknowledgement closes the session cleanly");

    printf("  grants of one then two, then acknowledged\n");
}

/* A grant of zero packets is a request to wait. We honour it, and T4 bounds how long. */
void test_rts_cts_hold()
{
    printf("connection-mode hold\n");

    harness h;

    h.claim(0xF9);

    const std::vector<uint8_t> payload = ramp(20);

    h.eng.submit(0x00EF00, 6, 0xF9, 0x21, false, payload.data(), payload.size());
    h.advance(0);
    h.sent.clear();

    h.rx(0x1CECF921, {17, 0, 0xFF, 0xFF, 0xFF, 0x00, 0xEF, 0x00});
    h.advance(400);
    check(h.sent.empty(), "held, so nothing is sent");

    h.rx(0x1CECF921, {17, 3, 1, 0xFF, 0xFF, 0x00, 0xEF, 0x00});
    h.advance(0);
    check(h.sent.size() == 3, "the transfer resumes on the next grant");

    printf("  a zero grant pauses, a real grant resumes\n");
}

/* A grant asking to resume at an earlier packet must rewind the sender. python-can-j1939 reads this
 * field and then ignores it, which silently sends the wrong data after any loss. */
void test_cts_rewind()
{
    printf("connection-mode rewind\n");

    harness h;

    h.claim(0xF9);

    const std::vector<uint8_t> payload = ramp(20);

    h.eng.submit(0x00EF00, 6, 0xF9, 0x21, false, payload.data(), payload.size());
    h.advance(0);
    h.sent.clear();

    h.rx(0x1CECF921, {17, 3, 1, 0xFF, 0xFF, 0x00, 0xEF, 0x00});
    h.advance(0);
    check(h.sent.size() == 3, "all three packets sent");

    /* The peer says it missed packet 2 and wants the rest again from there. */
    h.sent.clear();
    h.rx(0x1CECF921, {17, 2, 2, 0xFF, 0xFF, 0x00, 0xEF, 0x00});
    h.advance(0);
    check(h.sent.size() == 2, "two packets resent");
    check(body_is(at(h.sent, 0), {2, 8, 9, 10, 11, 12, 13, 14}), "resumed at packet 2, not packet 4");
    check(body_is(at(h.sent, 1), {3, 15, 16, 17, 18, 19, 20, 0xFF}), "and then packet 3");

    printf("  the sender rewinds to the requested packet\n");
}

/* Receiving a connection-mode transfer: answer the request, collect the packets, acknowledge. */
void test_rts_cts_receive()
{
    printf("connection-mode receive\n");

    harness h;

    h.claim(0xF9);

    h.rx(0x18ECF921, {16, 20, 0, 3, 0xFF, 0x00, 0xEF, 0x00});
    check(h.sent.size() == 1, "answered immediately, well inside Tr");
    check(at(h.sent, 0).id == 0x1CEC21F9, "clear-to-send identifier");
    check(body_is(at(h.sent, 0), {17, 3, 1, 0xFF, 0xFF, 0x00, 0xEF, 0x00}), "clear to send three");

    h.sent.clear();
    h.rx(0x1CEBF921, {1, 1, 2, 3, 4, 5, 6, 7});
    h.rx(0x1CEBF921, {2, 8, 9, 10, 11, 12, 13, 14});
    h.rx(0x1CEBF921, {3, 15, 16, 17, 18, 19, 20, 0xFF});

    check(h.sent.size() == 1, "acknowledged once complete");
    check(body_is(at(h.sent, 0), {19, 20, 0, 3, 0xFF, 0x00, 0xEF, 0x00}), "end of message acknowledge");

    message_t m;

    check(h.eng.pop(m), "message delivered");
    check(m.pgn == 0x00EF00, "parameter group");
    check(m.sa == 0x21, "source address");
    check(m.da == 0xF9, "addressed to the address we hold");
    check(!m.bam, "reported as connection mode, not broadcast");
    check(m.data.size() == 20 && m.data[19] == 20, "payload");

    printf("  clear to send, collect, acknowledge\n");
}

/* A request aimed at somebody else must not be answered. */
void test_rts_for_another_node()
{
    printf("connection-mode request for another node\n");

    harness h;

    h.claim(0xF9);
    h.rx(0x18EC3321, {16, 20, 0, 3, 0xFF, 0x00, 0xEF, 0x00});
    h.advance(2000);

    check(h.sent.empty(), "no answer to a transfer aimed elsewhere");

    printf("  ignored\n");
}

/* Duplicates and reordering are survivable because packets are placed by sequence number. The
 * reference implementation appends in arrival order and would corrupt or truncate the message. */
void test_duplicate_and_reordered_packets()
{
    printf("duplicate and reordered packets\n");

    harness h;

    h.rx(0x18ECFF21, {32, 20, 0, 3, 0xFF, 0xF1, 0xFE, 0x00});
    h.rx(0x1CEBFF21, {1, 1, 2, 3, 4, 5, 6, 7});
    h.rx(0x1CEBFF21, {1, 1, 2, 3, 4, 5, 6, 7});
    h.rx(0x1CEBFF21, {3, 15, 16, 17, 18, 19, 20, 0xFF});
    h.rx(0x1CEBFF21, {2, 8, 9, 10, 11, 12, 13, 14});

    message_t m;

    check(h.eng.pop(m), "still one message");
    check(m.data.size() == 20, "still twenty bytes");

    bool intact = true;

    for (size_t i = 0; i < m.data.size(); i++)
    {
        intact = intact && m.data[i] == (uint8_t)(i + 1);
    }

    check(intact, "and the bytes are in the right order");

    printf("  a duplicate is ignored and a reorder is placed correctly\n");
}

/* A packet number outside the announced range is nonsense; the session is torn down rather than
 * writing past the buffer. */
void test_packet_number_out_of_range()
{
    printf("packet number out of range\n");

    harness h;

    h.claim(0xF9);
    h.rx(0x18ECF921, {16, 20, 0, 3, 0xFF, 0x00, 0xEF, 0x00});
    h.sent.clear();

    h.rx(0x1CEBF921, {9, 1, 2, 3, 4, 5, 6, 7});

    check(h.sent.size() == 1, "aborted");
    check(at(h.sent, 0).data[0] == 255 && at(h.sent, 0).data[1] == 250, "abort, reason other");

    message_t m;

    check(!h.eng.pop(m), "nothing delivered");

    printf("  session aborted, nothing delivered\n");
}

/* An announcement whose size and packet count disagree is refused before anything is allocated. */
void test_inconsistent_announcement()
{
    printf("inconsistent announcement\n");

    harness h;

    h.claim(0xF9);
    h.rx(0x18ECF921, {16, 20, 0, 9, 0xFF, 0x00, 0xEF, 0x00});
    check(h.sent.size() == 1, "refused");
    check(at(h.sent, 0).data[0] == 255 && at(h.sent, 0).data[1] == 2, "abort, out of resources");

    h.sent.clear();
    h.rx(0x18ECF921, {16, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0xEF, 0x00});
    check(h.sent.size() == 1, "an oversized request is refused too");

    h.sent.clear();
    h.rx(0x18ECF921, {16, 20, 0, 3});
    check(h.sent.empty(), "a short frame is dropped, not interpreted");

    printf("  size checked before anything is allocated\n");
}

/* A second request from a peer we are already serving gets the busy abort J1939/21 asks for. */
void test_second_request_is_busy()
{
    printf("second request from the same peer\n");

    harness h;

    h.claim(0xF9);
    h.rx(0x18ECF921, {16, 20, 0, 3, 0xFF, 0x00, 0xEF, 0x00});
    h.sent.clear();

    h.rx(0x18ECF921, {16, 20, 0, 3, 0xFF, 0x00, 0xEF, 0x00});

    check(h.sent.size() == 1, "refused");
    check(at(h.sent, 0).data[0] == 255 && at(h.sent, 0).data[1] == 1, "abort, already in a session");

    printf("  abort with reason 1\n");
}

/* Two peers broadcasting at once must both get through: the session key is the address pair, so
 * they do not collide. */
void test_concurrent_sessions()
{
    printf("concurrent sessions\n");

    harness h;

    h.rx(0x18ECFF21, {32, 14, 0, 2, 0xFF, 0xF1, 0xFE, 0x00});
    h.rx(0x18ECFF33, {32, 14, 0, 2, 0xFF, 0xF1, 0xFE, 0x00});
    h.rx(0x1CEBFF21, {1, 1, 1, 1, 1, 1, 1, 1});
    h.rx(0x1CEBFF33, {1, 2, 2, 2, 2, 2, 2, 2});
    h.rx(0x1CEBFF21, {2, 1, 1, 1, 1, 1, 1, 1});
    h.rx(0x1CEBFF33, {2, 2, 2, 2, 2, 2, 2, 2});

    message_t first;
    message_t second;

    check(h.eng.pop(first) && h.eng.pop(second), "both messages assembled");
    check(first.sa == 0x21 && first.data[0] == 1, "the first peer's data");
    check(second.sa == 0x33 && second.data[0] == 2, "the second peer's data");

    printf("  two broadcasts from different nodes interleave safely\n");
}

/* Timeouts: a connection-mode transfer says so, a broadcast dies quietly. */
void test_timeouts()
{
    printf("timeouts\n");

    harness cmdt;

    cmdt.claim(0xF9);
    cmdt.rx(0x18ECF921, {16, 20, 0, 3, 0xFF, 0x00, 0xEF, 0x00});
    cmdt.sent.clear();
    cmdt.advance(1300);

    check(cmdt.sent.size() == 1, "connection-mode receive times out");
    check(at(cmdt.sent, 0).data[0] == 255 && at(cmdt.sent, 0).data[1] == 3, "abort, timeout");

    harness bam;

    bam.rx(0x18ECFF21, {32, 20, 0, 3, 0xFF, 0xF1, 0xFE, 0x00});
    bam.rx(0x1CEBFF21, {1, 1, 2, 3, 4, 5, 6, 7});
    bam.advance(800);

    message_t m;

    check(bam.sent.empty(), "a broadcast is never aborted");
    check(!bam.eng.pop(m), "and the partial message is discarded");

    harness tx;

    tx.claim(0xF9);

    const std::vector<uint8_t> payload = ramp(20);

    tx.eng.submit(0x00EF00, 6, 0xF9, 0x21, false, payload.data(), payload.size());
    tx.advance(0);
    tx.sent.clear();
    tx.advance(1300);

    check(tx.sent.size() == 1, "a transfer with no grant times out");
    check(at(tx.sent, 0).data[0] == 255 && at(tx.sent, 0).data[1] == 3, "abort, timeout");

    printf("  T1/T2 abort, T3 abort, broadcasts discarded silently\n");
}

/* An abort from the peer stops us dead, whatever we were doing. The reference only honours it while
 * waiting for a grant, so it keeps transmitting into a closed session. */
void test_abort_mid_burst()
{
    printf("abort during a burst\n");

    harness h;

    h.claim(0xF9);

    const std::vector<uint8_t> payload = ramp(1785);

    h.eng.submit(0x00EF00, 6, 0xF9, 0x21, false, payload.data(), payload.size());
    h.advance(0);
    h.rx(0x1CECF921, {17, 16, 1, 0xFF, 0xFF, 0x00, 0xEF, 0x00});
    h.advance(0);
    h.sent.clear();

    h.rx(0x1CECF921, {255, 1, 0xFF, 0xFF, 0xFF, 0x00, 0xEF, 0x00});
    h.advance(2000);

    check(h.sent.empty(), "nothing more is transmitted after the abort");

    /* And the session slot is free again. */
    h.eng.submit(0x00EF00, 6, 0xF9, 0x21, false, payload.data(), 20);
    h.advance(0);
    check(h.sent.size() == 1, "a new transfer starts straight away");

    printf("  transmission stops and the session is released\n");
}

/* The largest message the RP1210 J1939 field can carry, in both directions. */
void test_maximum_size()
{
    printf("maximum message size\n");

    harness h;

    h.claim(0xF9);

    const std::vector<uint8_t> payload = ramp(1785);

    check(h.eng.submit(0x00FEF1, 6, 0xF9, 0xFF, true, payload.data(), payload.size())
              == result_t::ok,
          "1785 bytes accepted");

    h.advance(0);
    check(body_is(at(h.sent, 0), {32, 0xF9, 0x06, 255, 0xFF, 0xF1, 0xFE, 0x00}),
          "255 packets announced");

    for (int i = 0; i < 255; i++)
    {
        h.advance(50);
    }

    check(h.sent.size() == 256, "one announcement and 255 packets");
    check(at(h.sent, 255).data[0] == 255, "last packet is number 255");
    check(at(h.sent, 255).data[7] == (uint8_t)(1785 & 0xFF), "last packet is full, no padding");

    std::vector<uint8_t> payload_too_long(1786, 0);

    check(h.eng.submit(0x00FEF1, 6, 0xF9, 0xFF, true, payload_too_long.data(),
                       payload_too_long.size())
              == result_t::too_long,
          "1786 bytes refused");

    printf("  1785 bytes in 255 packets, 1786 refused\n");
}

/* A multi-packet transfer needs an address of our own; a single frame does not. */
void test_address_needed_for_transport()
{
    printf("address requirement\n");

    harness h;
    const std::vector<uint8_t> big = ramp(20);
    const std::vector<uint8_t> small = ramp(8);

    check(h.eng.submit(0x00EF00, 6, 0xF9, 0x21, false, big.data(), big.size())
              == result_t::address_never_claimed,
          "a transfer without an address is refused");

    check(h.eng.submit(0x00EF00, 6, 0xF9, 0x21, false, small.data(), small.size()) == result_t::ok,
          "a single frame is sent anyway, as the specification requires");

    printf("  transport needs a claimed address, a single frame does not\n");
}

/* ---------------------------------------------------------------- address claim */

void test_address_claim()
{
    printf("address claim\n");

    /* An address in the service tool range is usable at once. */
    harness immediate;

    check(immediate.eng.begin_claim(0xF9, test_name) == result_t::ok, "claim accepted");
    check(immediate.eng.claim_settled(), "249 needs no contention window");
    check(immediate.sent.size() == 1, "the claim goes out");
    check(at(immediate.sent, 0).id == 0x18EEFFF9, "claim identifier");
    check(body_is(at(immediate.sent, 0),
                  {135, 214, 82, 83, 130, 201, 254, 82}),
          "the NAME travels unchanged, low byte first");

    /* An address in the arbitrary range has to wait. */
    harness waited;

    waited.eng.begin_claim(0x80, test_name);
    check(!waited.eng.claim_settled(), "128..247 waits");
    waited.advance(249);
    check(!waited.eng.claim_settled(), "still waiting just before 250 ms");
    waited.advance(1);
    check(waited.eng.claim_settled(), "settled after 250 ms");
    check(waited.eng.claimed_address() == 0x80, "and the address is ours");

    printf("  immediate for 0..127 and 248..253, 250 ms for the arbitrary range\n");
}

void test_address_contention()
{
    printf("address contention\n");

    const uint8_t lower[8] = {135, 214, 82, 83, 130, 111, 254, 82};
    const uint8_t higher[8] = {135, 214, 82, 83, 130, 222, 254, 82};

    /* A contender with a lower NAME takes the address; we say we cannot claim. */
    harness lose;

    lose.claim(0xF9);
    lose.rx(0x18EEFFF9, {lower[0], lower[1], lower[2], lower[3], lower[4], lower[5], lower[6],
                         lower[7]});

    check(lose.eng.claim_state() == claim_state_t::lost, "address conceded");
    check(lose.sent.size() >= 1, "cannot-claim sent");
    check(at(lose.sent, 0).id == 0x18EEFFFE, "cannot claim comes from the null address");

    /* A contender with a higher NAME loses, and we restate our claim. */
    harness win;

    win.claim(0xF9);
    win.rx(0x18EEFFF9, {higher[0], higher[1], higher[2], higher[3], higher[4], higher[5], higher[6],
                        higher[7]});

    check(win.eng.claim_state() == claim_state_t::claimed, "address kept");
    check(win.sent.size() == 1 && at(win.sent, 0).id == 0x18EEFFF9, "our claim is restated");

    /* The same NAME is our own claim echoed back, or a genuine duplicate. Conceding would be wrong. */
    harness same;

    same.claim(0xF9);
    same.rx(0x18EEFFF9, {135, 214, 82, 83, 130, 201, 254, 82});

    check(same.eng.claim_state() == claim_state_t::claimed, "an identical NAME changes nothing");
    check(same.sent.empty(), "and provokes no answer");

    printf("  lower wins, higher yields, equal is our own echo\n");
}

void test_address_request()
{
    printf("request for address claimed\n");

    harness h;

    h.claim(0xF9);

    /* A global request. */
    h.rx(0x18EAFF21, {0x00, 0xEE, 0x00});
    check(h.sent.size() == 1 && at(h.sent, 0).id == 0x18EEFFF9, "answered a global request");

    /* One addressed to us. */
    h.sent.clear();
    h.rx(0x18EAF921, {0x00, 0xEE, 0x00});
    check(h.sent.size() == 1, "answered a directed request");

    /* One addressed to somebody else. */
    h.sent.clear();
    h.rx(0x18EA3321, {0x00, 0xEE, 0x00});
    check(h.sent.empty(), "ignored a request aimed elsewhere");

    /* Answering is the driver's own duty, so the application's filter must not silence it. */
    h.eng.set_gate(gate_t::none);
    h.sent.clear();
    h.rx(0x18EAFF21, {0x00, 0xEE, 0x00});
    check(h.sent.size() == 1, "answered even with every filter discarded");

    printf("  global and directed answered, others ignored, filters irrelevant\n");
}

void test_address_release()
{
    printf("address release\n");

    harness h;

    h.claim(0xF9);

    check(h.eng.release_claim(0x80) == result_t::address_never_claimed, "wrong address refused");
    check(h.eng.release_claim(0xF9) == result_t::ok, "released");
    check(h.eng.claim_state() == claim_state_t::none, "no longer claimed");

    h.sent.clear();
    h.rx(0x18EAFF21, {0x00, 0xEE, 0x00});
    check(h.sent.empty(), "and we no longer answer for it");

    check(h.eng.release_claim(0xF9) == result_t::address_never_claimed,
          "releasing twice is refused");

    printf("  released, silent afterwards\n");
}

/* ---------------------------------------------------------------- filtering */

void test_filtering()
{
    printf("filtering\n");

    /* Nothing at all until the application asks. */
    harness closed;

    closed.eng.set_gate(gate_t::none);
    closed.rx(0x0CF00400, {1, 2, 3, 4, 5, 6, 7, 8});

    message_t m;

    check(!closed.eng.pop(m), "nothing before a filter is set");

    /* By parameter group. */
    harness by_pgn;

    by_pgn.eng.set_gate(gate_t::none);

    filter_t f;

    f.flags = filter_pgn;
    f.pgn = 0x00F004;
    by_pgn.eng.add_filter(f);

    by_pgn.rx(0x0CF00400, {1, 2, 3, 4, 5, 6, 7, 8});
    by_pgn.rx(0x0CF00500, {1, 2, 3, 4, 5, 6, 7, 8});
    by_pgn.rx(0x1CF00400, {1, 2, 3, 4, 5, 6, 7, 8});

    check(by_pgn.eng.pop(m) && m.pgn == 0x00F004, "the wanted group arrives");
    check(by_pgn.eng.pop(m) && m.priority == 7,
          "priority is not part of a parameter group filter");
    check(!by_pgn.eng.pop(m), "and nothing else does");

    /* By source. */
    harness by_sa;

    by_sa.eng.set_gate(gate_t::none);

    filter_t s;

    s.flags = filter_source;
    s.sa = 0x21;
    by_sa.eng.add_filter(s);

    by_sa.rx(0x0CF00421, {1});
    by_sa.rx(0x0CF00433, {1});

    check(by_sa.eng.pop(m) && m.sa == 0x21, "the wanted source arrives");
    check(!by_sa.eng.pop(m), "and no other");

    /* A parameter group filter on a peer-to-peer group must not constrain the destination. */
    harness pdu1;

    pdu1.eng.set_gate(gate_t::none);

    filter_t p;

    p.flags = filter_pgn;
    p.pgn = 0x00EF00;
    pdu1.eng.add_filter(p);

    pdu1.rx(0x18EF8021, {1});
    pdu1.rx(0x18EF3321, {1});

    check(pdu1.eng.pop(m) && pdu1.eng.pop(m), "both destinations pass a group-only filter");

    /* Exclusive. */
    harness excl;

    excl.eng.set_filter_exclusive(true);

    filter_t e;

    e.flags = filter_pgn;
    e.pgn = 0x00F004;
    excl.eng.add_filter(e);

    excl.rx(0x0CF00400, {1});
    excl.rx(0x18FECA00, {1});

    check(excl.eng.pop(m) && m.pgn == 0x00FECA, "the listed group is blocked, the rest passes");
    check(!excl.eng.pop(m), "only the one");

    printf("  group, source, destination, inclusive and exclusive\n");
}

/* A filter set by the application must not starve the transport layer: the driver has to keep
 * seeing the transport frames even when the application only wants the assembled result. */
void test_filter_does_not_break_transport()
{
    printf("filters do not break transport\n");

    harness h;

    h.eng.set_gate(gate_t::none);

    filter_t f;

    f.flags = filter_pgn;
    f.pgn = 0x00FEF1;
    h.eng.add_filter(f);

    h.rx(0x18ECFF21, {32, 20, 0, 3, 0xFF, 0xF1, 0xFE, 0x00});
    h.rx(0x1CEBFF21, {1, 1, 2, 3, 4, 5, 6, 7});
    h.rx(0x1CEBFF21, {2, 8, 9, 10, 11, 12, 13, 14});
    h.rx(0x1CEBFF21, {3, 15, 16, 17, 18, 19, 20, 0xFF});

    message_t m;

    check(h.eng.pop(m) && m.data.size() == 20, "the assembled broadcast passes the filter");

    /* And a group the application did not ask for is assembled and then dropped, not leaked. */
    h.rx(0x18ECFF21, {32, 20, 0, 3, 0xFF, 0xF2, 0xFE, 0x00});
    h.rx(0x1CEBFF21, {1, 1, 2, 3, 4, 5, 6, 7});
    h.rx(0x1CEBFF21, {2, 8, 9, 10, 11, 12, 13, 14});
    h.rx(0x1CEBFF21, {3, 15, 16, 17, 18, 19, 20, 0xFF});

    check(!h.eng.pop(m), "an unwanted group is dropped after reassembly");

    printf("  reassembly happens below the filter\n");
}

/* Echo reports the assembled message once, not the transport frames that carried it. */
void test_echo()
{
    printf("echo\n");

    harness h;

    h.claim(0xF9);
    h.eng.set_echo(true);

    const std::vector<uint8_t> payload = ramp(20);

    h.eng.submit(0x00FEF1, 6, 0xF9, 0xFF, true, payload.data(), payload.size());

    /* Feed our own frames back, as the device does when transmit acknowledgements are on. */
    for (int i = 0; i < 4; i++)
    {
        h.advance(50);

        for (const raw_frame_t &f : h.sent)
        {
            h.eng.feed(f, true);
        }

        h.sent.clear();
    }

    message_t m;

    check(h.eng.pop(m), "one echo");
    check(m.echo, "flagged as our own");
    check(m.bam, "and as carried by broadcast");
    check(m.data.size() == 20, "the whole message, not a packet");
    check(!h.eng.pop(m), "exactly one, not one per frame");

    printf("  one assembled echo, not four frame echoes\n");
}

} // namespace

int main()
{
    /* Unbuffered, so a crash still shows which group was running. */
    setvbuf(stdout, nullptr, _IONBF, 0);

    test_id_vectors();
    test_pdu2_ignores_destination();
    test_pdu1_normalisation();
    test_exhaustive_round_trip();
    test_identifier_bits();
    test_name_rules();
    test_claim_ranges();

    test_bam_transmit();
    test_bam_interpacket_time();
    test_bam_receive();
    test_transport_frames_are_not_surfaced();
    test_rts_cts_transmit_split();
    test_rts_cts_hold();
    test_cts_rewind();
    test_rts_cts_receive();
    test_rts_for_another_node();
    test_duplicate_and_reordered_packets();
    test_packet_number_out_of_range();
    test_inconsistent_announcement();
    test_second_request_is_busy();
    test_concurrent_sessions();
    test_timeouts();
    test_abort_mid_burst();
    test_maximum_size();
    test_address_needed_for_transport();

    test_address_claim();
    test_address_contention();
    test_address_request();
    test_address_release();

    test_filtering();
    test_filter_does_not_break_transport();
    test_echo();

    printf("\n%s\n", failures == 0 ? "all J1939 tests passed" : "J1939 tests FAILED");

    return (failures == 0) ? 0 : 1;
}
