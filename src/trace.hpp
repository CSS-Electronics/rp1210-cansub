#ifndef CANSUB_RP1210_TRACE_HPP
#define CANSUB_RP1210_TRACE_HPP

/* Call tracing for the RP1210 entry points.
 *
 * A diagnostic application is a black box: it reports "gateway not responding" and says nothing
 * about which call disappointed it. This writes every call, its arguments and its return value to a
 * file, which turns that into a five-minute read.
 *
 * Off unless switched on, and cheap when off: enabled() is one relaxed atomic load, so an untraced
 * build pays nothing measurable. Configured the way RP1210 already defines for this, by DebugFile
 * and DebugLevel in [VendorInformation], with an environment variable as an alternative for anyone
 * who cannot edit a file in the Windows directory.
 *
 *   DebugLevel=0  off (the default)
 *   DebugLevel=1  calls and return values
 *   DebugLevel=2  and the message and command payloads, in hex
 *
 * Every line is flushed, so a log survives the host application crashing or being killed. */

#include <cstddef>
#include <string>

namespace rp1210
{
namespace trace
{

bool enabled();
int level();

/* Where the log went, for reporting back through the version strings. Empty when off. */
const std::string &path();

void printf_line(const char *format, ...);

/* Formats up to `limit` bytes as hex, with an ellipsis when there are more. */
std::string hex(const void *data, size_t size, size_t limit = 32);

} // namespace trace
} // namespace rp1210

/* Statement-like so it reads as a call and costs nothing when tracing is off. */
#define RP1210_TRACE(...)                                                                          \
    do                                                                                             \
    {                                                                                              \
        if (::rp1210::trace::enabled())                                                            \
        {                                                                                          \
            ::rp1210::trace::printf_line(__VA_ARGS__);                                             \
        }                                                                                          \
    } while (0)

#endif
