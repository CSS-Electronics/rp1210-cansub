#ifndef CANSUB_JSON_HPP
#define CANSUB_JSON_HPP

/* Just enough JSON for the CANsub REST API: flat objects of strings, numbers and bools, one level
 * of nesting for the bit-timing blocks, and arrays of integers. Deliberately not a general parser.
 * If the API grows to need one, replace this file rather than extend it. */

#include <cstdint>
#include <string>
#include <vector>

namespace cansub::json
{

/* Extracts a value by key from a flat or one-level-nested object. Returns false if absent. */
bool find_string(const std::string &text, const std::string &key, std::string &out);
bool find_int(const std::string &text, const std::string &key, int64_t &out);
bool find_bool(const std::string &text, const std::string &key, bool &out);

/* Returns the raw text of an object-valued key, so nested blocks can be queried separately. */
bool find_object(const std::string &text, const std::string &key, std::string &out);

/* Parses a top-level array of integers, e.g. the channel and filter id lists. */
bool parse_int_array(const std::string &text, std::vector<int> &out);

/* Unquotes a bare JSON string response, e.g. "05.00" from GET /api/version. */
bool parse_string(const std::string &text, std::string &out);

/* Building. Values are appended in insertion order; the API does not care. */
class builder
{
public:
    builder &key(const std::string &name);
    builder &value(const std::string &text);
    builder &value(int64_t number);
    builder &value(bool flag);
    builder &object_begin();
    builder &object_end();

    /* Convenience for the flat "key": value case. */
    builder &field(const std::string &name, const std::string &text);
    builder &field(const std::string &name, int64_t number);
    builder &field(const std::string &name, bool flag);

    std::string str() const;

private:
    void separate();

    std::string out_;
};

} // namespace cansub::json

#endif
