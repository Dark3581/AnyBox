#include "Inflate.hpp"

#include <zlib.h>

#include <limits>

namespace abx::xcrdapi {

bool InflateZlib(const std::uint8_t* source, std::size_t sourceBytes, std::uint8_t* destination, std::size_t destinationBytes, std::size_t& written, std::string& error) {
    written = 0;
    if (sourceBytes > (std::numeric_limits<uInt>::max)() || destinationBytes > (std::numeric_limits<uInt>::max)()) {
        error = "a compressed block larger than 4 GB";
        return false;
    }
    z_stream stream{};
    if (inflateInit(&stream) != Z_OK) {
        error = "inflateInit failed";
        return false;
    }
    stream.next_in = const_cast<Bytef*>(source);
    stream.avail_in = static_cast<uInt>(sourceBytes);
    stream.next_out = destination;
    stream.avail_out = static_cast<uInt>(destinationBytes);
    const int result = inflate(&stream, Z_FINISH);
    written = stream.total_out;
    inflateEnd(&stream);
    if (result != Z_STREAM_END) {
        error = std::string("zlib inflate returned ") + std::to_string(result) + (stream.msg != nullptr ? std::string(": ") + stream.msg : std::string());
        return false;
    }
    return true;
}

}
