/* Hardware-free tests of the glue between libcansub and the official SDK: frame conversion in both
 * directions, the error-type renumbering, host classification, the API version the SDK accepts,
 * and bit timing from the SDK's calculator. No device, no network. */

#include <cstdio>
#include <cstring>

#include "cansub/bit_timing.hpp"
#include "cansub/discover.hpp"
#include "sdk/convert.hpp"

using namespace cansub;

namespace
{

int failures = 0;
int passes = 0;

void check(bool condition, const char *what)
{
    if (condition)
    {
        passes++;
    }
    else
    {
        failures++;
        printf("FAIL: %s\n", what);
    }
}

cansub_frame error_frame(uint8_t type)
{
    cansub_frame raw = {};

    raw.flags = CANSUB_FRAME_FLAG_ERROR;
    raw.length = 1;
    raw.dlc = 1;
    raw.data[0] = type;
    raw.timestamp_us = 42;

    return raw;
}

void test_api_version()
{
    check(cansub_version() == CANSUB_VERSION, "SDK header and library versions agree");
    check(sdk::api_supported(sdk::required_api), "SDK accepts the API version named in messages");
    check(!sdk::api_supported("04.00"), "API 04.00 refused");
    check(!sdk::api_supported(""), "empty version refused");
    check(!sdk::api_supported("05.0"), "malformed version refused");

    device_info_t info;

    info.api = "05.00";
    check(info.supported(), "discovery: 05.00 supported");
    info.api = "04.00";
    check(!info.supported(), "discovery: 04.00 not supported");
    info.api.clear();
    check(info.supported(), "discovery: missing api record left to bus::open");
}

void test_error_types()
{
    /* The SDK numbers BIT=1..CRC=5; frame.hpp keeps the wire / python-can-cansub numbering. */
    struct
    {
        uint8_t sdk;
        error_type_t expected;
        uint32_t id;
    } cases[] = {
        {CANSUB_ERROR_FRAME_TYPE_BIT_ERROR, error_type_t::bit, 0},
        {CANSUB_ERROR_FRAME_TYPE_ACK_ERROR, error_type_t::ack, 1},
        {CANSUB_ERROR_FRAME_TYPE_FORM_ERROR, error_type_t::form, 2},
        {CANSUB_ERROR_FRAME_TYPE_STUFF_ERROR, error_type_t::stuff, 3},
        {CANSUB_ERROR_FRAME_TYPE_CRC_ERROR, error_type_t::crc, 4},
        {CANSUB_ERROR_FRAME_TYPE_OTHER_ERROR, error_type_t::other, 0xFF},
        {CANSUB_ERROR_FRAME_TYPE_NO_ERROR, error_type_t::other, 0xFF},
    };

    for (const auto &c : cases)
    {
        const frame_t frame = sdk::frame_from_sdk(error_frame(c.sdk));
        char what[96];

        snprintf(what, sizeof(what), "error type %u maps to %s", c.sdk,
                 error_type_string(c.expected));
        check(frame.is_error && frame.error == c.expected, what);

        snprintf(what, sizeof(what), "error type %u gives id %u as python-can-cansub does", c.sdk,
                 c.id);
        check(frame.id == c.id, what);
        check(frame.len == 0 && frame.us == 42, "error frame has no payload, keeps timestamp");
    }
}

void test_from_sdk()
{
    cansub_frame raw = {};

    raw.timestamp_us = 1767225600000000ull;
    raw.arbitration_id = 0x18FEF100;
    raw.flags = CANSUB_FRAME_FLAG_IDE | CANSUB_FRAME_FLAG_TX;
    raw.length = 8;
    raw.dlc = 8;

    for (uint8_t i = 0; i < 8; i++)
    {
        raw.data[i] = (uint8_t)(0xA0 + i);
    }

    frame_t frame = sdk::frame_from_sdk(raw);

    check(frame.id == 0x18FEF100 && frame.extended, "extended id");
    check(frame.tx && !frame.fd && !frame.rtr && !frame.is_error, "flags");
    check(frame.len == 8 && frame.data[7] == 0xA7, "payload");
    check(frame.us == 1767225600000000ull, "timestamp");

    cansub_frame rtr = {};

    rtr.arbitration_id = 0x123;
    rtr.flags = CANSUB_FRAME_FLAG_RTR;
    rtr.length = 4;
    rtr.dlc = 4;
    rtr.data[0] = 0x55; /* not carried by an RTR frame */
    frame = sdk::frame_from_sdk(rtr);
    check(frame.rtr && frame.len == 4 && frame.data[0] == 0, "RTR: requested length, no payload");

    cansub_frame fd = {};

    fd.arbitration_id = 0x7FF;
    fd.flags = CANSUB_FRAME_FLAG_FDF | CANSUB_FRAME_FLAG_BRS | CANSUB_FRAME_FLAG_ESI;
    fd.length = 64;
    fd.dlc = 15;
    fd.data[63] = 0x99;
    frame = sdk::frame_from_sdk(fd);
    check(frame.fd && frame.brs && frame.esi && frame.len == 64 && frame.data[63] == 0x99,
          "CAN FD 64 bytes with BRS and ESI");
}

void test_to_sdk()
{
    const uint8_t payload[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    cansub_frame raw;

    frame_t frame = frame_t::make(0x123, payload, 8);

    check(sdk::frame_to_sdk(frame, raw), "classic 8 bytes accepted");
    check(raw.arbitration_id == 0x123 && raw.dlc == 8 && raw.length == 8 && raw.flags == 0,
          "classic fields");
    check(memcmp(raw.data, payload, 8) == 0, "classic payload");

    frame = frame_t::make(0x1ABCDEF, payload, 3, true);
    check(sdk::frame_to_sdk(frame, raw) && raw.flags == CANSUB_FRAME_FLAG_IDE && raw.dlc == 3,
          "extended 3 bytes");

    frame.tx = true;
    check(sdk::frame_to_sdk(frame, raw) && !(raw.flags & CANSUB_FRAME_FLAG_TX),
          "receive-only tx flag dropped when sending");

    frame = frame_t::make(0x800, payload, 1);
    check(!sdk::frame_to_sdk(frame, raw), "11-bit id out of range refused");

    uint8_t big[64] = {};
    frame = frame_t::make(0x100, big, 12);
    check(!sdk::frame_to_sdk(frame, raw), "classic 12 bytes refused");

    frame.fd = true;
    check(sdk::frame_to_sdk(frame, raw) && raw.dlc == 9 && raw.length == 12
              && (raw.flags & CANSUB_FRAME_FLAG_FDF),
          "CAN FD 12 bytes is DLC 9");

    frame = frame_t::make(0x100, big, 10);
    frame.fd = true;
    check(!sdk::frame_to_sdk(frame, raw), "CAN FD 10 bytes is not a valid length");

    frame = frame_t::make(0x100, payload, 2);
    frame.brs = true; /* classic: ignored, not an error */
    check(sdk::frame_to_sdk(frame, raw) && raw.flags == 0, "BRS on a classic frame dropped");

    frame = frame_t::make(0x321, payload, 5);
    frame.rtr = true;
    check(sdk::frame_to_sdk(frame, raw) && (raw.flags & CANSUB_FRAME_FLAG_RTR) && raw.dlc == 5
              && raw.data[0] == 0,
          "RTR keeps the requested length and sends no payload");

    frame = frame_t();
    frame.is_error = true;
    check(!sdk::frame_to_sdk(frame, raw), "error frames cannot be sent");
}

void test_hosts()
{
    check(sdk::is_ipv4_literal("10.216.248.1"), "IPv4 literal");
    check(sdk::is_ipv4_literal("192.168.0.255"), "IPv4 literal with 255");
    check(!sdk::is_ipv4_literal("192.168.0.256"), "octet above 255");
    check(!sdk::is_ipv4_literal("10.1.1"), "three octets");
    check(!sdk::is_ipv4_literal("10.1.1.1.1"), "five octets");
    check(!sdk::is_ipv4_literal("10..1.1"), "empty octet");
    check(!sdk::is_ipv4_literal("1a8788d1-usb.local"), "hostname");
    check(!sdk::is_ipv4_literal(""), "empty");

    check(sdk::accept_any_device_for("10.216.248.1"), "IP host: root certificate only");
    check(!sdk::accept_any_device_for("97622d1f-eth.local"), "hostname: name is verified");
}

void test_bit_timing()
{
    const uint32_t sp = bit_timing_t::default_sample_point;
    const uint32_t nominal[] = {125000, 250000, 500000, 1000000};

    for (uint32_t rate : nominal)
    {
        bit_timing_t timing;
        bit_timing_t data;
        char what[96];

        snprintf(what, sizeof(what), "nominal %u: exact rate at 80.0%%", rate);
        check(bit_timing_t::calculate(rate, sp, 1000000, sp, timing, data)
                  && timing.bitrate() == rate && timing.sample_point_tenths() == 800
                  && timing.sjw >= 1 && timing.sjw <= timing.seg2,
              what);

        printf("  nominal %7u: brp %u seg1 %u seg2 %u sjw %u\n", rate, timing.brp, timing.seg1,
               timing.seg2, timing.sjw);
    }

    const uint32_t data_rates[] = {500000, 1000000, 2000000, 4000000, 5000000, 8000000};

    for (uint32_t rate : data_rates)
    {
        bit_timing_t timing;
        bit_timing_t data;
        char what[96];

        /* At 5 Mbit/s a bit is only 16 quanta of the 80 MHz clock, so 80.0% cannot be hit exactly
         * (the SDK gives 81.25%). Allow 2.5%. */
        snprintf(what, sizeof(what), "data %u: exact rate, sample point 80%% +/- 2.5%%", rate);
        check(bit_timing_t::calculate(500000, sp, rate, sp, timing, data)
                  && data.bitrate() == rate && data.sample_point_tenths() >= 775
                  && data.sample_point_tenths() <= 825,
              what);
    }

    /* Common CAN FD combinations with the sample points set explicitly. */
    struct
    {
        uint32_t nominal, nominal_sp, data, data_sp;
    } const combos[] = {
        {500000, 8000, 2000000, 8000},
        {500000, 8750, 2000000, 7500},
        {1000000, 8000, 4000000, 7500},
        {250000, 8750, 1000000, 8000},
        {1000000, 7500, 8000000, 7000},
    };

    for (const auto &c : combos)
    {
        bit_timing_t timing;
        bit_timing_t data;
        char what[128];

        snprintf(what, sizeof(what), "%u at %u + data %u at %u: rates exact, sample points within 1%%",
                 c.nominal, c.nominal_sp, c.data, c.data_sp);

        const bool found =
            bit_timing_t::calculate(c.nominal, c.nominal_sp, c.data, c.data_sp, timing, data);
        const int nominal_error = (int)timing.sample_point_tenths() * 10 - (int)c.nominal_sp;
        const int data_error = (int)data.sample_point_tenths() * 10 - (int)c.data_sp;

        check(found && timing.bitrate() == c.nominal && data.bitrate() == c.data
                  && nominal_error >= -100 && nominal_error <= 100 && data_error >= -100
                  && data_error <= 100,
              what);
    }

    bit_timing_t timing;
    bit_timing_t data;

    check(!bit_timing_t::calculate(666000, sp, 1000000, sp, timing, data),
          "666 kbit/s cannot be generated");
    check(!bit_timing_t::calculate(0, sp, 1000000, sp, timing, data), "0 bit/s refused");
    check(!bit_timing_t::calculate(1000000, sp, 500000, sp, timing, data),
          "data rate below nominal refused");
    check(!bit_timing_t::calculate(500000, 10001, 2000000, sp, timing, data),
          "sample point above 100% refused");
}

void test_results()
{
    check(sdk::status_from_result(CANSUB_RESULT_OK) == status_t::ok, "result: OK");
    check(sdk::status_from_result(CANSUB_RESULT_CONNECTION_REJECTED) == status_t::channel_busy,
          "result: rejected connection is a busy channel");
    check(sdk::status_from_result(CANSUB_RESULT_CONNECTION_TLS) == status_t::tls_failed, "result: TLS");
    check(sdk::status_from_result(CANSUB_RESULT_CONNECTION_PROTOCOL) == status_t::protocol_error,
          "result: protocol");
    check(sdk::status_from_result(CANSUB_RESULT_CONNECTION_LOST) == status_t::connect_failed,
          "result: connection lost");
    check(sdk::status_from_result(CANSUB_RESULT_INVALID_ARG) == status_t::invalid_argument,
          "result: invalid argument");
    check(sdk::status_from_result(-12345) == status_t::internal_error, "result: unknown code");
}

} // namespace

int main()
{
    test_api_version();
    test_results();
    test_error_types();
    test_from_sdk();
    test_to_sdk();
    test_hosts();
    test_bit_timing();

    printf("%d passed, %d failed\n", passes, failures);

    return failures == 0 ? 0 : 1;
}
