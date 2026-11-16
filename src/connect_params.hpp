#ifndef CANSUB_RP1210_CONNECT_PARAMS_HPP
#define CANSUB_RP1210_CONNECT_PARAMS_HPP

/* The protocol string of RP1210_ClientConnect, e.g. "CAN:Baud=500,DataBaud=2000".
 *
 * Free of libcansub, the device and Windows, so it is unit tested on its own. Whether the device
 * can actually produce the requested bit timing is checked by the caller, which has the SDK. */

#include <cstddef>
#include <cstdint>
#include <string>

namespace rp1210
{

/* Finds a key in the argument list and copies its value. Keys are matched case-insensitively and
 * only at a token boundary, so "Baud=" does not match a vendor key ending in the same letters. */
bool find_token(const char *protocol, const char *key, char *value, size_t value_size);

/* The value of a Baud= token, in bit/s: 1000 and below is kBaud, anything above is bit/s. Rates
 * above 1 Mbit/s are refused, since that is the CAN maximum for the nominal phase. */
bool bitrate_from_text(const char *text, uint32_t &bitrate);

/* What the protocol string asks for. Every option beyond Baud= is opt-in: a string without them
 * gives classic CAN at an 80 % sample point in normal mode, as before they existed. */
struct connect_params_t
{
    /* A bare protocol name means 250k, the J1939 rate and the RP1210A-era CAN default. */
    uint32_t bitrate = 250000;

    /* Baud=Auto, J1939 only. */
    bool autodetect = false;

    /* DataBaud=, CAN only. 0 is classic CAN. */
    uint32_t data_bitrate = 0;

    /* SamplePoint= and DataSamplePoint=, in 0.01 % units (8000 = 80 %). */
    uint32_t sample_point = 8000;
    uint32_t data_sample_point = 8000;

    /* ListenOnly=, CAN only. */
    bool listen_only = false;
};

enum class params_error_t
{
    none,

    /* A bit timing option (Baud=, DataBaud=, SamplePoint=, DataSamplePoint=) is malformed or out
     * of range, or DataSamplePoint= comes without DataBaud=. */
    bad_baud,

    /* Another option is malformed. */
    bad_option,

    /* An option the protocol does not offer, such as DataBaud= on J1939. */
    not_for_protocol
};

/* Parses the options after the colon. Unknown keys are ignored, as RP1210 allows vendor keys.
 * detail names the offending option for GetLastErrorMsg and the trace. */
params_error_t parse_connect_params(const char *protocol, bool j1939, connect_params_t &params,
                                    std::string &detail);

} // namespace rp1210

#endif
