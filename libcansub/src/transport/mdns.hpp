#ifndef CANSUB_MDNS_HPP
#define CANSUB_MDNS_HPP

/* Just enough DNS to ask "who is out there" over multicast and understand the answer.
 *
 * Split out from the socket work so it can be tested from a byte buffer, which matters more here
 * than usual: this parses unsolicited packets from the network inside a DLL that lives in somebody
 * else's process. Every read is bounds-checked, compression pointers cannot loop, and a malformed
 * message is reported rather than thrown. */

#include <cstdint>
#include <string>
#include <vector>

namespace cansub
{
namespace mdns
{

constexpr uint16_t type_a = 1;
constexpr uint16_t type_ptr = 12;
constexpr uint16_t type_txt = 16;
constexpr uint16_t type_srv = 33;

struct record_t
{
    std::string name;
    uint16_t type = 0;

    /* PTR: the service instance. SRV: the host. A: the address in dotted-quad form. */
    std::string target;

    uint16_t port = 0; /* SRV only. */
    std::vector<std::string> txt;
};

/* A question for one name and type, with the transaction id left at zero as multicast DNS expects. */
std::vector<uint8_t> query(const std::string &name, uint16_t type);

/* Reads every answer, authority and additional record in a response.
 *
 * Returns false when the message is malformed, in which case whatever was understood before the
 * problem is still in `out`: a truncated packet from one device should not discard another's. */
bool parse(const uint8_t *data, size_t size, std::vector<record_t> &out);

} // namespace mdns
} // namespace cansub

#endif
