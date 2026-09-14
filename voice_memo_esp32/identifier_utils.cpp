#include "identifier_utils.h"

#include <cctype>
#include <cstddef>
#include <string>

namespace voice_memo_firmware {
namespace {

bool is_valid_identifier(const char* value, size_t max_length) {
    if (value == nullptr) {
        return false;
    }

    const size_t length = std::char_traits<char>::length(value);
    if (length == 0 || length > max_length) {
        return false;
    }

    const char first = value[0];
    if (!std::isalnum(static_cast<unsigned char>(first))) {
        return false;
    }

    for (size_t i = 1; i < length; ++i) {
        const char c = value[i];
        if (!std::isalnum(static_cast<unsigned char>(c)) &&
            c != '.' && c != '_' && c != '-') {
            return false;
        }
    }
    return true;
}

}  // namespace

bool is_valid_device_id(const char* value) {
    return is_valid_identifier(value, 63);
}

bool is_valid_recording_id(const char* value) {
    return is_valid_identifier(value, 127);
}

}  // namespace voice_memo_firmware
