#ifndef CANSUB_BIT_TIMING_HPP
#define CANSUB_BIT_TIMING_HPP

#include <cstdint>

namespace cansub
{

/* CAN bit timing as the device wants it: raw registers, not a bit rate. The CANsub runs an 80 MHz
 * CAN clock, so one bit is (1 + seg1 + seg2) time quanta of brp clocks each. */
struct bit_timing_t
{
    static constexpr uint32_t f_clock = 80000000;

    uint32_t brp = 0;
    uint32_t seg1 = 0;
    uint32_t seg2 = 0;
    uint32_t sjw = 0;

    /* The device's default sample point, in the SDK's 0.01 % units (8000 = 80 %). */
    static constexpr uint32_t default_sample_point = 8000;

    /* Nominal and data-phase timing for a channel, computed together by the SDK
     * (cansub_calculate_bittiming). Sample points are in 0.01 % units. Returns false when either
     * rate cannot be produced within 0.1 %, a sample point is above 100 %, or the data rate is below
     * the nominal one.
     *
     * The device always runs in FD mode, so the phy endpoint needs a data phase even for classic
     * CAN; the caller picks one in that case.
     *
     * The SDK picks the smallest prescaler, so the registers differ from the device's factory
     * values at the same bit rate and sample point: 250 kbit/s gives {1, 255, 64, 64}, while
     * GET /api/can/{n}/phy reports {4, 63, 16, 16} out of the box. Both are valid and interoperate. */
    static bool calculate(uint32_t nominal_bitrate, uint32_t nominal_sample_point,
                          uint32_t data_bitrate, uint32_t data_sample_point,
                          bit_timing_t &nominal, bit_timing_t &data);

    uint32_t bitrate() const
    {
        return f_clock / (brp * (1 + seg1 + seg2));
    }

    /* Sample point in tenths of a percent, to stay in integers. */
    uint32_t sample_point_tenths() const
    {
        return (1000 * (1 + seg1)) / (1 + seg1 + seg2);
    }
};

} // namespace cansub

#endif
