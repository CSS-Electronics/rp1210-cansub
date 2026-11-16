/* Tests for the RP1210_ClientConnect protocol string, no hardware needed.
 *
 * Whether the device can generate a parsed bit timing is the SDK's call and is covered by the bit
 * timing tests in test_glue and by the hardware suite. */

#include <cstdio>
#include <string>

#include "connect_params.hpp"

using namespace rp1210;

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

params_error_t parse(const char *text, connect_params_t &params, bool j1939 = false)
{
    std::string detail;

    return parse_connect_params(text, j1939, params, detail);
}

void test_find_token()
{
    char value[16];

    check(find_token("CAN:Baud=500", "Baud", value, sizeof(value))
              && std::string(value) == "500",
          "find_token: simple");
    check(find_token("CAN:baud=500", "Baud", value, sizeof(value)), "find_token: case");
    check(find_token("CAN: Channel=1; Baud=250", "Baud", value, sizeof(value))
              && std::string(value) == "250",
          "find_token: separators");
    check(!find_token("CAN:DataBaud=2000", "Baud", value, sizeof(value)),
          "find_token: Baud does not match DataBaud");
    check(!find_token("CAN:DataSamplePoint=75", "SamplePoint", value, sizeof(value)),
          "find_token: SamplePoint does not match DataSamplePoint");
    check(!find_token("CAN", "Baud", value, sizeof(value)), "find_token: bare name");
}

void test_bitrate_from_text()
{
    uint32_t rate = 0;

    check(bitrate_from_text("500", rate) && rate == 500000, "Baud 500 = kBaud");
    check(bitrate_from_text("500000", rate) && rate == 500000, "Baud 500000 = bit/s");
    check(bitrate_from_text("1000", rate) && rate == 1000000, "Baud 1000 = 1 Mbit/s");
    check(bitrate_from_text("800", rate) && rate == 800000, "Baud 800 accepted here");
    check(bitrate_from_text("83333", rate) && rate == 83333, "Baud 83333 bit/s accepted here");
    check(bitrate_from_text("2000", rate) && rate == 2000, "Baud 2000 = 2000 bit/s (RP1210 rule)");
    check(!bitrate_from_text("2000000", rate), "Baud above 1 Mbit/s refused");
    check(!bitrate_from_text("0", rate), "Baud 0 refused");
    check(!bitrate_from_text("abc", rate), "Baud text refused");
}

void test_defaults()
{
    connect_params_t params;

    check(parse("CAN", params) == params_error_t::none && params.bitrate == 250000
              && params.data_bitrate == 0 && params.sample_point == 8000
              && params.data_sample_point == 8000 && !params.listen_only && !params.autodetect,
          "bare CAN: classic 250k, 80 %, normal mode");
    check(parse("CAN:Baud=500", params) == params_error_t::none && params.bitrate == 500000
              && params.data_bitrate == 0,
          "Baud only stays classic");
    check(parse("CAN:Baud=500,Vendor=1", params) == params_error_t::none,
          "unknown keys ignored");
}

void test_fd()
{
    connect_params_t params;

    check(parse("CAN:Baud=500,DataBaud=2000", params) == params_error_t::none
              && params.bitrate == 500000 && params.data_bitrate == 2000000,
          "DataBaud 2000 = 2 Mbit/s");
    check(parse("CAN:Baud=1000,DataBaud=4000000", params) == params_error_t::none
              && params.data_bitrate == 4000000,
          "DataBaud in bit/s");
    check(parse("CAN:Baud=500,DataBaud=500", params) == params_error_t::none
              && params.data_bitrate == 500000,
          "DataBaud 500 = 500 kbit/s");
    check(parse("CAN:DataBaud=10000", params) == params_error_t::none
              && params.data_bitrate == 10000000,
          "DataBaud 10000 = 10 Mbit/s (cut-off)");
    check(parse("CAN:DataBaud=10001", params) == params_error_t::none
              && params.data_bitrate == 10001,
          "DataBaud above the cut-off is bit/s");
    check(parse("CAN:DataBaud=0", params) == params_error_t::bad_baud, "DataBaud 0 refused");
    check(parse("CAN:DataBaud=2M", params) == params_error_t::bad_baud, "DataBaud 2M refused");
    check(parse("CAN:DataBaud=", params) == params_error_t::bad_baud, "DataBaud empty refused");
    check(parse("J1939:Baud=250,DataBaud=2000", params, true)
              == params_error_t::not_for_protocol,
          "DataBaud on J1939 refused");
}

void test_sample_points()
{
    connect_params_t params;

    check(parse("CAN:Baud=500,SamplePoint=87.5", params) == params_error_t::none
              && params.sample_point == 8750,
          "SamplePoint 87.5 = 8750");
    check(parse("CAN:SamplePoint=62.25", params) == params_error_t::none
              && params.sample_point == 6225,
          "SamplePoint two decimals");
    check(parse("CAN:SamplePoint=80", params) == params_error_t::none
              && params.sample_point == 8000,
          "SamplePoint whole number");
    check(parse("CAN:Baud=500,DataBaud=2000,DataSamplePoint=75", params)
                  == params_error_t::none
              && params.data_sample_point == 7500 && params.sample_point == 8000,
          "DataSamplePoint 75");
    check(parse("J1939:Baud=250,SamplePoint=87.5", params, true) == params_error_t::none
              && params.sample_point == 8750,
          "SamplePoint allowed on J1939");

    check(parse("CAN:SamplePoint=100", params) == params_error_t::bad_baud, "100 % refused");
    check(parse("CAN:SamplePoint=0", params) == params_error_t::bad_baud, "0 % refused");
    check(parse("CAN:SamplePoint=87.555", params) == params_error_t::bad_baud,
          "three decimals refused");
    check(parse("CAN:SamplePoint=0.875", params) == params_error_t::bad_baud,
          "a fraction such as 0.875 refused (three decimals)");
    check(parse("CAN:SamplePoint=87.", params) == params_error_t::bad_baud,
          "trailing dot refused");
    check(parse("CAN:SamplePoint=87%", params) == params_error_t::bad_baud,
          "percent sign refused");
    check(parse("CAN:SamplePoint=-80", params) == params_error_t::bad_baud,
          "negative refused");
    check(parse("CAN:Baud=500,DataSamplePoint=75", params) == params_error_t::bad_baud,
          "DataSamplePoint without DataBaud refused");
    check(parse("J1939:DataSamplePoint=75", params, true) == params_error_t::not_for_protocol,
          "DataSamplePoint on J1939 refused");
}

void test_listen_only()
{
    connect_params_t params;

    check(parse("CAN:ListenOnly=1", params) == params_error_t::none && params.listen_only,
          "ListenOnly=1");
    check(parse("CAN:ListenOnly=true", params) == params_error_t::none && params.listen_only,
          "ListenOnly=true");
    check(parse("CAN:ListenOnly=0", params) == params_error_t::none && !params.listen_only,
          "ListenOnly=0");
    check(parse("CAN:ListenOnly=maybe", params) == params_error_t::bad_option,
          "ListenOnly bad value refused");
    check(parse("J1939:ListenOnly=1", params, true) == params_error_t::not_for_protocol,
          "ListenOnly on J1939 refused");
}

void test_baud()
{
    connect_params_t params;

    check(parse("J1939:Baud=Auto", params, true) == params_error_t::none && params.autodetect,
          "Baud=Auto on J1939");
    check(parse("CAN:Baud=Auto", params) == params_error_t::bad_baud, "Baud=Auto on CAN refused");
    check(parse("CAN:Baud=2000000", params) == params_error_t::bad_baud,
          "nominal above 1 Mbit/s refused");
    check(parse("J1939:Baud=x", params, true) == params_error_t::bad_baud,
          "J1939 bad Baud refused");
}

} // namespace

int main()
{
    test_find_token();
    test_bitrate_from_text();
    test_defaults();
    test_fd();
    test_sample_points();
    test_listen_only();
    test_baud();

    if (failures != 0)
    {
        printf("test_connect_params: %d failure(s)\n", failures);

        return 1;
    }

    printf("test_connect_params: all passed\n");

    return 0;
}
