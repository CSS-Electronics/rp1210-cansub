#include "connect_params.hpp"

#include <cstdlib>
#include <cstring>

namespace rp1210
{

namespace
{

/* A whole unsigned decimal number, nothing else. */
bool parse_uint(const char *text, uint32_t &value)
{
    if (*text == '\0')
    {
        return false;
    }

    uint64_t result = 0;

    for (const char *p = text; *p != '\0'; p++)
    {
        if (*p < '0' || *p > '9')
        {
            return false;
        }

        result = result * 10 + (uint64_t)(*p - '0');

        if (result > 0xFFFFFFFFull)
        {
            return false;
        }
    }

    value = (uint32_t)result;

    return true;
}

/* A sample point in percent with up to two decimals ("80", "87.5", "62.25"), as 0.01 % units.
 * Parsed by hand so "87.5" is exactly 8750, with no floating-point rounding. */
bool parse_sample_point(const char *text, uint32_t &value)
{
    uint32_t whole = 0;
    uint32_t hundredths = 0;
    int digits = 0;
    const char *p = text;

    for (; *p >= '0' && *p <= '9'; p++)
    {
        whole = whole * 10 + (uint32_t)(*p - '0');

        if (++digits > 3)
        {
            return false;
        }
    }

    if (digits == 0)
    {
        return false;
    }

    if (*p == '.')
    {
        p++;

        int decimals = 0;

        for (; *p >= '0' && *p <= '9'; p++)
        {
            if (++decimals > 2)
            {
                return false;
            }

            hundredths = hundredths * 10 + (uint32_t)(*p - '0');
        }

        if (decimals == 0)
        {
            return false;
        }

        if (decimals == 1)
        {
            hundredths *= 10;
        }
    }

    if (*p != '\0')
    {
        return false;
    }

    const uint32_t result = whole * 100 + hundredths;

    /* A sample point sits inside the bit, so 0 % and 100 % are not sample points. */
    if (result == 0 || result >= 10000)
    {
        return false;
    }

    value = result;

    return true;
}

bool parse_bool(const char *text, bool &value)
{
    if (strcmp(text, "1") == 0 || _stricmp(text, "true") == 0 || _stricmp(text, "yes") == 0)
    {
        value = true;

        return true;
    }

    if (strcmp(text, "0") == 0 || _stricmp(text, "false") == 0 || _stricmp(text, "no") == 0)
    {
        value = false;

        return true;
    }

    return false;
}

} // namespace

bool find_token(const char *protocol, const char *key, char *value, size_t value_size)
{
    const char *p = strchr(protocol, ':');

    if (p == nullptr)
    {
        return false;
    }

    const size_t key_len = strlen(key);

    p++;

    while (*p != '\0')
    {
        while (*p == ' ' || *p == ',' || *p == ';')
        {
            p++;
        }

        if (*p == '\0')
        {
            break;
        }

        const char *token = p;

        while (*p != '\0' && *p != ',' && *p != ';')
        {
            p++;
        }

        if ((size_t)(p - token) > key_len && _strnicmp(token, key, key_len) == 0
            && token[key_len] == '=')
        {
            size_t i = 0;
            const char *v = token + key_len + 1;

            while (v < p && i + 1 < value_size)
            {
                value[i++] = *v++;
            }

            value[i] = '\0';

            return true;
        }
    }

    return false;
}

/* RP1210E section 14.10.5 allows two spellings of the same thing: a kBaud value ("125", "250",
 * "500", "666", "1000") or a bit rate "between 1001-1000000". So 1000 and below is kBaud and
 * anything above is bit/s, which is why "Baud=500" and "Baud=500000" both mean 500 kbit/s. Some
 * applications send the bit/s spelling, and reading only the kBaud one made every connection attempt
 * from them fail with ERR_CAN_BAUD_SET_NONSTANDARD.
 *
 * Any rate up to 1 Mbit/s passes here. Whether the CANsub can generate it (666 kBaud, for one, it
 * cannot) is the caller's check, so a rate is refused rather than rounded to something the
 * application did not ask for. */
bool bitrate_from_text(const char *text, uint32_t &bitrate)
{
    const long value = atol(text);

    if (value <= 0)
    {
        return false;
    }

    const uint32_t bits = (value <= 1000) ? ((uint32_t)value * 1000) : (uint32_t)value;

    if (bits > 1000000)
    {
        return false;
    }

    bitrate = bits;

    return true;
}

/* The CAN FD options are vendor keys: RP1210 has no CAN FD, so no application sends them by
 * accident, and leaving them out keeps the classic behaviour. */
params_error_t parse_connect_params(const char *protocol, bool j1939, connect_params_t &params,
                                    std::string &detail)
{
    params = connect_params_t();
    detail.clear();

    char value[32];

    if (find_token(protocol, "Baud", value, sizeof(value)))
    {
        if (_stricmp(value, "Auto") == 0)
        {
            /* Only J1939 offers detection here, which is what CANAutoBaud=FALSE in the INI tells an
             * application about the CAN protocol. */
            if (!j1939)
            {
                detail = "Baud=Auto is J1939 only";

                return params_error_t::bad_baud;
            }

            params.autodetect = true;
        }
        else if (!bitrate_from_text(value, params.bitrate))
        {
            detail = std::string("Baud=") + value + " is not a CAN bit rate";

            return params_error_t::bad_baud;
        }
    }

    if (find_token(protocol, "DataBaud", value, sizeof(value)))
    {
        if (j1939)
        {
            detail = "DataBaud is CAN only";

            return params_error_t::not_for_protocol;
        }

        /* Like Baud=, a small number is kbit/s, but data rates reach into Mbit/s, so the cut-off
         * is 10000: "2000" is 2 Mbit/s and "2000000" is too. */
        uint32_t rate = 0;

        if (!parse_uint(value, rate) || rate == 0)
        {
            detail = std::string("DataBaud=") + value + " is not a bit rate";

            return params_error_t::bad_baud;
        }

        params.data_bitrate = (rate <= 10000) ? rate * 1000 : rate;
    }

    if (find_token(protocol, "SamplePoint", value, sizeof(value))
        && !parse_sample_point(value, params.sample_point))
    {
        detail = std::string("SamplePoint=") + value + " is not a percentage below 100";

        return params_error_t::bad_baud;
    }

    if (find_token(protocol, "DataSamplePoint", value, sizeof(value)))
    {
        if (j1939)
        {
            detail = "DataSamplePoint is CAN only";

            return params_error_t::not_for_protocol;
        }

        if (!parse_sample_point(value, params.data_sample_point))
        {
            detail = std::string("DataSamplePoint=") + value + " is not a percentage below 100";

            return params_error_t::bad_baud;
        }

        /* Silently ignoring it would leave the application believing it had set something. */
        if (params.data_bitrate == 0)
        {
            detail = "DataSamplePoint needs DataBaud";

            return params_error_t::bad_baud;
        }
    }

    if (find_token(protocol, "ListenOnly", value, sizeof(value)))
    {
        /* A J1939 client sends on its own (address claim, transport protocol handshakes), which a
         * listen-only channel cannot do. */
        if (j1939)
        {
            detail = "ListenOnly is CAN only";

            return params_error_t::not_for_protocol;
        }

        if (!parse_bool(value, params.listen_only))
        {
            detail = std::string("ListenOnly=") + value + " is not 0 or 1";

            return params_error_t::bad_option;
        }
    }

    return params_error_t::none;
}

} // namespace rp1210
