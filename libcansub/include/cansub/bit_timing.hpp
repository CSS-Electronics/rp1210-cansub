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

    /* Nominal timing for a bit rate at the device's default 80% sample point, computed by the SDK
     * (cansub_calculate_bittiming). Returns false for a rate the device cannot produce.
     *
     * The SDK picks the smallest prescaler, so the registers differ from the device's factory
     * values at the same bit rate and sample point: 250 kbit/s gives {1, 255, 64, 64}, while
     * GET /api/can/{n}/phy reports {4, 63, 16, 16} out of the box. Both are valid and interoperate. */
    static bool from_bitrate(uint32_t bitrate, bit_timing_t &timing);

    /* Data-phase timing for CAN FD, from the same SDK calculator. The classic-CAN path still has to
     * send a data block, since the phy endpoint requires it. */
    static bool data_from_bitrate(uint32_t bitrate, bit_timing_t &timing);

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
