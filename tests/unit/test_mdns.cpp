/* mDNS message parsing, with no network involved.
 *
 * This code reads unsolicited UDP packets from whatever is on the local network, inside a DLL that
 * lives in a third-party process, so the hostile cases matter as much as the happy one: a
 * compression pointer that loops, a length that runs off the end, a truncated packet. */

#include <cstdio>
#include <cstring>
#include <vector>

#include "transport/mdns.hpp"

using namespace cansub;

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

void put16(std::vector<uint8_t> &out, uint16_t value)
{
    out.push_back((uint8_t)(value >> 8));
    out.push_back((uint8_t)(value & 0xFF));
}

void put_name(std::vector<uint8_t> &out, const char *name)
{
    const char *start = name;

    for (;;)
    {
        const char *dot = strchr(start, '.');
        const size_t length = dot ? (size_t)(dot - start) : strlen(start);

        out.push_back((uint8_t)length);
        out.insert(out.end(), start, start + length);

        if (!dot)
        {
            break;
        }

        start = dot + 1;
    }

    out.push_back(0);
}

void put_header(std::vector<uint8_t> &out, uint16_t answers)
{
    put16(out, 0);      /* transaction id */
    put16(out, 0x8400); /* response, authoritative */
    put16(out, 0);      /* questions */
    put16(out, answers);
    put16(out, 0);
    put16(out, 0);
}

/* A response shaped like the one a CANsub actually sends: PTR, SRV, TXT and A together. */
std::vector<uint8_t> cansub_response()
{
    std::vector<uint8_t> out;

    put_header(out, 4);

    put_name(out, "_cansub._tcp.local");
    put16(out, mdns::type_ptr);
    put16(out, 1);
    put16(out, 0);
    put16(out, 120);
    {
        std::vector<uint8_t> rdata;

        put_name(rdata, "97622d1f-usb._cansub._tcp.local");
        put16(out, (uint16_t)rdata.size());
        out.insert(out.end(), rdata.begin(), rdata.end());
    }

    put_name(out, "97622d1f-usb._cansub._tcp.local");
    put16(out, mdns::type_srv);
    put16(out, 1);
    put16(out, 0);
    put16(out, 120);
    {
        std::vector<uint8_t> rdata;

        put16(rdata, 0); /* priority */
        put16(rdata, 0); /* weight */
        put16(rdata, 443);
        put_name(rdata, "97622d1f-usb.local");
        put16(out, (uint16_t)rdata.size());
        out.insert(out.end(), rdata.begin(), rdata.end());
    }

    put_name(out, "97622d1f-usb._cansub._tcp.local");
    put16(out, mdns::type_txt);
    put16(out, 1);
    put16(out, 0);
    put16(out, 120);
    {
        std::vector<uint8_t> rdata;
        const char *items[] = {"api=04.00", "channels=2"};

        for (const char *item : items)
        {
            rdata.push_back((uint8_t)strlen(item));
            rdata.insert(rdata.end(), item, item + strlen(item));
        }

        put16(out, (uint16_t)rdata.size());
        out.insert(out.end(), rdata.begin(), rdata.end());
    }

    put_name(out, "97622d1f-usb.local");
    put16(out, mdns::type_a);
    put16(out, 1);
    put16(out, 0);
    put16(out, 120);
    put16(out, 4);
    out.push_back(10);
    out.push_back(216);
    out.push_back(248);
    out.push_back(1);

    return out;
}

void test_query()
{
    printf("query\n");

    const std::vector<uint8_t> q = mdns::query("_cansub._tcp.local", mdns::type_ptr);

    check(q.size() == 12 + 20 + 4, "length: header, three labels, type and class");
    check(q[0] == 0 && q[1] == 0, "transaction id is zero, as multicast DNS expects");
    check(q[5] == 1, "one question");
    check(q[12] == 7 && memcmp(&q[13], "_cansub", 7) == 0, "first label");
    check(q[q.size() - 3] == mdns::type_ptr, "type PTR");
    check(q[q.size() - 1] == 1, "class IN");

    printf("  one PTR question for the service\n");
}

void test_real_response()
{
    printf("a CANsub response\n");

    const std::vector<uint8_t> message = cansub_response();
    std::vector<mdns::record_t> records;

    check(mdns::parse(message.data(), message.size(), records), "parses");
    check(records.size() == 4, "four records");

    const mdns::record_t *ptr = nullptr;
    const mdns::record_t *srv = nullptr;
    const mdns::record_t *txt = nullptr;
    const mdns::record_t *a = nullptr;

    for (const mdns::record_t &record : records)
    {
        if (record.type == mdns::type_ptr) { ptr = &record; }
        if (record.type == mdns::type_srv) { srv = &record; }
        if (record.type == mdns::type_txt) { txt = &record; }
        if (record.type == mdns::type_a) { a = &record; }
    }

    check(ptr != nullptr && ptr->name == "_cansub._tcp.local", "PTR name");
    check(ptr != nullptr && ptr->target == "97622d1f-usb._cansub._tcp.local", "PTR target");
    check(srv != nullptr && srv->target == "97622d1f-usb.local", "SRV host");
    check(srv != nullptr && srv->port == 443, "SRV port");
    check(txt != nullptr && txt->txt.size() == 2, "two TXT items");
    check(txt != nullptr && txt->txt.size() == 2 && txt->txt[0] == "api=04.00", "api record");
    check(txt != nullptr && txt->txt.size() == 2 && txt->txt[1] == "channels=2", "channels record");
    check(a != nullptr && a->target == "10.216.248.1", "A address");

    printf("  PTR, SRV, TXT and A all understood\n");
}

/* Names may end in a pointer to an earlier name. Handling it is required; following it in circles
 * is a hang. */
void test_compression()
{
    printf("name compression\n");

    std::vector<uint8_t> out;

    put_header(out, 1);

    const size_t name_at = out.size();

    put_name(out, "_cansub._tcp.local");
    put16(out, mdns::type_ptr);
    put16(out, 1);
    put16(out, 0);
    put16(out, 120);
    {
        /* "abc" followed by a pointer back to the name above. */
        std::vector<uint8_t> rdata;

        rdata.push_back(3);
        rdata.push_back('a');
        rdata.push_back('b');
        rdata.push_back('c');
        rdata.push_back((uint8_t)(0xC0 | ((name_at >> 8) & 0x3F)));
        rdata.push_back((uint8_t)(name_at & 0xFF));

        put16(out, (uint16_t)rdata.size());
        out.insert(out.end(), rdata.begin(), rdata.end());
    }

    std::vector<mdns::record_t> records;

    check(mdns::parse(out.data(), out.size(), records), "parses");
    check(records.size() == 1, "one record");
    check(records.size() == 1 && records[0].target == "abc._cansub._tcp.local",
          "suffix followed through the pointer");

    printf("  a pointer to an earlier name is followed\n");
}

/* None of these may hang, read out of bounds, or be reported as valid. */
void test_malformed()
{
    printf("malformed input\n");

    {
        std::vector<mdns::record_t> records;

        check(!mdns::parse(nullptr, 0, records), "null buffer");
    }

    {
        const uint8_t truncated[6] = {0, 0, 0x84, 0, 0, 1};
        std::vector<mdns::record_t> records;

        check(!mdns::parse(truncated, sizeof(truncated), records), "shorter than a header");
    }

    {
        /* A pointer to itself. The loop this would cause is why jumps must go backwards only. */
        std::vector<uint8_t> out;

        put_header(out, 1);

        const size_t here = out.size();

        out.push_back((uint8_t)(0xC0 | ((here >> 8) & 0x3F)));
        out.push_back((uint8_t)(here & 0xFF));

        std::vector<mdns::record_t> records;

        check(!mdns::parse(out.data(), out.size(), records), "a pointer to itself is rejected");
    }

    {
        /* A label claiming more bytes than the message holds. */
        std::vector<uint8_t> out;

        put_header(out, 1);
        out.push_back(200);
        out.push_back('a');
        out.push_back('b');
        out.push_back('c');

        std::vector<mdns::record_t> records;

        check(!mdns::parse(out.data(), out.size(), records), "a label past the end is rejected");
    }

    {
        /* A record whose rdlength runs past the end. */
        std::vector<uint8_t> out;

        put_header(out, 1);
        put_name(out, "x.local");
        put16(out, mdns::type_a);
        put16(out, 1);
        put16(out, 0);
        put16(out, 120);
        put16(out, 4000); /* claims 4000 bytes that are not there */

        std::vector<mdns::record_t> records;

        check(!mdns::parse(out.data(), out.size(), records), "an oversized record is rejected");
    }

    {
        /* Every truncation of a valid message. None may be accepted as complete and none may
         * crash: this is the case that would catch a missing bounds check. */
        const std::vector<uint8_t> good = cansub_response();
        int accepted_whole = 0;

        for (size_t length = 1; length < good.size(); length++)
        {
            std::vector<mdns::record_t> records;

            if (mdns::parse(good.data(), length, records) && records.size() == 4)
            {
                accepted_whole++;
            }
        }

        check(accepted_whole == 0, "no truncation is mistaken for the whole message");
    }

    printf("  null, short, looping, overlong and truncated input all handled\n");
}

} // namespace

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);

    test_query();
    test_real_response();
    test_compression();
    test_malformed();

    printf("\n%s\n", failures == 0 ? "all mDNS tests passed" : "mDNS tests FAILED");

    return (failures == 0) ? 0 : 1;
}
