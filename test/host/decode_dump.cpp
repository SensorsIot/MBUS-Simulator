// Development tool, not a test — see test/host/CMakeLists.txt.
//
// Reads a whitespace-separated hex dump and prints what dlms_parser makes of it.
// Its job is to answer "is the harness calling the oracle correctly?" against a
// known-good real meter capture, so that when test_encode fails we know whether
// the encoder or the harness is at fault.
//
//   ./decode_dump path/to/capture.hex

#include <dlms_parser/dlms_parser.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

int main(int argc, char **argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: decode_dump <hexfile>\n");
        return 2;
    }

    std::ifstream in(argv[1]);
    if (!in) {
        std::fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }

    std::vector<uint8_t> buf;
    std::string token;
    while (in >> token) {
        buf.push_back(static_cast<uint8_t>(std::stoul(token, nullptr, 16)));
    }
    std::printf("%zu bytes\n", buf.size());

    size_t captures = 0;
    dlms_parser::DlmsParser parser([&captures](const dlms_parser::AxdrCapture &c) {
        char obis[32];
        char value[128];
        ++captures;
        std::printf("  [%2u] class=%-3u obis=%-16s type=%-13s value=%s\n", c.elem_idx, c.class_id,
                    c.obis.to_string(obis).data(), dlms_parser::to_string(c.value_type),
                    c.value_as_string(value).data());
    });
    parser.load_default_patterns();

    const dlms_parser::ParseResult result = parser.parse(buf);
    std::printf("parse: count=%zu bytes_consumed=%zu callbacks=%zu\n", result.count,
                result.bytes_consumed, captures);
    return result.count > 0 ? 0 : 1;
}
