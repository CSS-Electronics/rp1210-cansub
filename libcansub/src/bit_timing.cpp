#include "cansub/bit_timing.hpp"

#include "cansub/cansub_ffi.h"

namespace cansub
{

namespace
{

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

bool bit_timing_t::calculate(uint32_t nominal_bitrate, uint32_t nominal_sample_point,
                             uint32_t data_bitrate, uint32_t data_sample_point,
                             bit_timing_t &nominal, bit_timing_t &data)
{
    cansub_bittiming result = {};

    if (cansub_calculate_bittiming(&result, nominal_bitrate, nominal_sample_point, data_bitrate,
                                   data_sample_point)
        != CANSUB_RESULT_OK)
    {
        return false;
    }

    nominal = from_segment(result.nominal);
    data = from_segment(result.data);

    return true;
}

} // namespace cansub
