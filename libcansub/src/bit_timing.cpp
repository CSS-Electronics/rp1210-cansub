#include "cansub/bit_timing.hpp"

#include "cansub/cansub_ffi.h"

namespace cansub
{

namespace
{

/* The device's default sample point, in the SDK's 0.01 % units. */
constexpr uint32_t sample_point = 8000;

bit_timing_t from_segment(const cansub_bittiming_segment &segment)
{
    bit_timing_t timing;

    timing.brp = segment.brp;
    timing.seg1 = segment.seg1;
    timing.seg2 = segment.seg2;
    timing.sjw = segment.sjw;

    return timing;
}

} // namespace

/* Both phases come from the SDK's calculator. It always computes the two together and needs the
 * data rate to be at least the nominal one, so each function asks for the same rate in both
 * phases and keeps the half it was asked for.
 *
 * The SDK picks the smallest prescaler, so its registers differ from the device's factory values
 * (250 kbit/s gives {1, 255, 64, 64} rather than {4, 63, 16, 16}) at the same bit rate and sample
 * point. Both are valid. */
bool bit_timing_t::from_bitrate(uint32_t bitrate, bit_timing_t &timing)
{
    cansub_bittiming result = {};

    if (cansub_calculate_bittiming(&result, bitrate, sample_point, bitrate, sample_point)
        != CANSUB_RESULT_OK)
    {
        return false;
    }

    timing = from_segment(result.nominal);

    return true;
}

bool bit_timing_t::data_from_bitrate(uint32_t bitrate, bit_timing_t &timing)
{
    cansub_bittiming result = {};

    if (cansub_calculate_bittiming(&result, bitrate, sample_point, bitrate, sample_point)
        != CANSUB_RESULT_OK)
    {
        return false;
    }

    timing = from_segment(result.data);

    return true;
}

} // namespace cansub
