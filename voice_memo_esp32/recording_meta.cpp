#include "recording_meta.h"

#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>

#include "identifier_utils.h"

namespace voice_memo_firmware {
namespace {

void appendEscaped(std::string& out, const std::string& value) {
    out.push_back('"');
    for (char c : value) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20U) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04X",
                                  static_cast<unsigned int>(static_cast<unsigned char>(c)));
                    out += buffer;
                } else {
                    out.push_back(c);
                }
                break;
        }
    }
    out.push_back('"');
}

void appendField(std::string& out, const char* key, const std::string& value, bool quoted) {
    out += "  \"";
    out += key;
    out += "\": ";
    if (quoted) {
        appendEscaped(out, value);
    } else {
        out += value;
    }
    out += ",\n";
}

bool isSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

bool parseString(const std::string& text, size_t& index, std::string* out) {
    if (index >= text.size() || text[index] != '"') {
        return false;
    }
    ++index;
    std::string value;
    while (index < text.size()) {
        const char c = text[index++];
        if (c == '"') {
            *out = value;
            return true;
        }
        if (c == '\\') {
            if (index >= text.size()) {
                return false;
            }
            const char escape = text[index++];
            switch (escape) {
                case '"':
                    value.push_back('"');
                    break;
                case '\\':
                    value.push_back('\\');
                    break;
                case '/':
                    value.push_back('/');
                    break;
                case 'n':
                    value.push_back('\n');
                    break;
                case 'r':
                    value.push_back('\r');
                    break;
                case 't':
                    value.push_back('\t');
                    break;
                case 'b':
                    value.push_back('\b');
                    break;
                case 'f':
                    value.push_back('\f');
                    break;
                case 'u': {
                    // Only the escapes this writer can emit are accepted
                    // (\u00XX); anything else is treated as corrupt.
                    if (index + 4 > text.size()) {
                        return false;
                    }
                    unsigned int code = 0;
                    for (int i = 0; i < 4; ++i) {
                        const char digit = text[index + static_cast<size_t>(i)];
                        code <<= 4U;
                        if (digit >= '0' && digit <= '9') {
                            code |= static_cast<unsigned int>(digit - '0');
                        } else if (digit >= 'a' && digit <= 'f') {
                            code |= static_cast<unsigned int>(digit - 'a' + 10);
                        } else if (digit >= 'A' && digit <= 'F') {
                            code |= static_cast<unsigned int>(digit - 'A' + 10);
                        } else {
                            return false;
                        }
                    }
                    index += 4;
                    if (code > 0x7FU) {
                        return false;
                    }
                    value.push_back(static_cast<char>(code));
                    break;
                }
                default:
                    return false;
            }
            continue;
        }
        value.push_back(c);
    }
    return false;
}

bool parseNumber(const std::string& text, size_t& index, std::string* out) {
    const size_t start = index;
    if (index < text.size() && (text[index] == '-' || text[index] == '+')) {
        ++index;
    }
    bool any = false;
    while (index < text.size() && text[index] >= '0' && text[index] <= '9') {
        ++index;
        any = true;
    }
    if (!any) {
        return false;
    }
    *out = text.substr(start, index - start);
    return true;
}

struct ParsedField {
    std::string value;
    bool quoted = false;
};

// Accepts exactly one flat JSON object whose values are string or number
// literals. Anything else (nested object/array, booleans, trailing garbage) is
// rejected, so a partially written sidecar can never be interpreted.
bool parseFlatObject(const std::string& text, std::vector<std::pair<std::string, ParsedField> >* out) {
    size_t index = 0;
    while (index < text.size() && isSpace(text[index])) {
        ++index;
    }
    if (index >= text.size() || text[index] != '{') {
        return false;
    }
    ++index;

    for (;;) {
        while (index < text.size() && isSpace(text[index])) {
            ++index;
        }
        if (index >= text.size()) {
            return false;
        }
        if (text[index] == '}') {
            ++index;
            break;
        }

        std::string key;
        if (!parseString(text, index, &key)) {
            return false;
        }
        while (index < text.size() && isSpace(text[index])) {
            ++index;
        }
        if (index >= text.size() || text[index] != ':') {
            return false;
        }
        ++index;
        while (index < text.size() && isSpace(text[index])) {
            ++index;
        }
        if (index >= text.size()) {
            return false;
        }

        ParsedField field;
        if (text[index] == '"') {
            field.quoted = true;
            if (!parseString(text, index, &field.value)) {
                return false;
            }
        } else if (text[index] == '-' || text[index] == '+' ||
                   (text[index] >= '0' && text[index] <= '9')) {
            if (!parseNumber(text, index, &field.value)) {
                return false;
            }
        } else {
            return false;
        }
        out->push_back(std::make_pair(key, field));

        while (index < text.size() && isSpace(text[index])) {
            ++index;
        }
        if (index >= text.size()) {
            return false;
        }
        if (text[index] == ',') {
            ++index;
            continue;
        }
        if (text[index] == '}') {
            ++index;
            break;
        }
        return false;
    }

    while (index < text.size() && isSpace(text[index])) {
        ++index;
    }
    return index == text.size();
}

bool toU32(const ParsedField& field, uint32_t* out) {
    if (field.quoted || field.value.empty()) {
        return false;
    }
    // Explicitly reject a sign. strtoul() accepts "-1" and wraps it to
    // ULONG_MAX, which is 0xFFFFFFFF on the ESP32 (32-bit long) and would then
    // pass a naive range check as a legitimate huge value.
    if (field.value[0] == '-' || field.value[0] == '+') {
        return false;
    }
    char* end = nullptr;
    // strtoull (not strtoul): on the ESP32 `unsigned long` is 32 bits, so
    // strtoul would saturate at ULONG_MAX and the bound check would be dead
    // code, silently accepting an out-of-range value.
    const unsigned long long value = std::strtoull(field.value.c_str(), &end, 10);
    if (end == nullptr || *end != '\0') {
        return false;
    }
    if (value > 0xFFFFFFFFULL) {
        return false;
    }
    *out = static_cast<uint32_t>(value);
    return true;
}

}  // namespace

std::string recording_meta_to_json(const RecordingMeta& meta) {
    char buffer[24];
    std::string out;
    out.reserve(320);
    out += "{\n";

    std::snprintf(buffer, sizeof(buffer), "%lu", static_cast<unsigned long>(meta.schema_version));
    appendField(out, "schema_version", buffer, false);

    appendField(out, "recording_id", meta.recording_id, true);
    appendField(out, "device_id", meta.device_id, true);

    std::snprintf(buffer, sizeof(buffer), "%lu", static_cast<unsigned long>(meta.tag_index));
    appendField(out, "tag_index", buffer, false);

    std::snprintf(buffer, sizeof(buffer), "%lu", static_cast<unsigned long>(meta.duration_ms));
    appendField(out, "duration_ms", buffer, false);

    std::snprintf(buffer, sizeof(buffer), "%lu", static_cast<unsigned long>(meta.wav_bytes));
    appendField(out, "wav_bytes", buffer, false);

    std::snprintf(buffer, sizeof(buffer), "%lu", static_cast<unsigned long>(meta.sample_rate));
    appendField(out, "sample_rate", buffer, false);

    std::snprintf(buffer, sizeof(buffer), "%lu", static_cast<unsigned long>(meta.created_utc));
    appendField(out, "created_utc", buffer, false);

    // The crc32 key is written only when a CRC was actually computed. Writing
    // "crc32": 0 for an unknown CRC would come back as has_crc = true (the key
    // being present is the flag) and make VM_SD_VERIFY_CRC_ON_BOOT compare the
    // real hash against 0 and quarantine a perfectly good recording.
    if (meta.has_crc) {
        std::snprintf(buffer, sizeof(buffer), "%lu", static_cast<unsigned long>(meta.crc32));
        appendField(out, "crc32", buffer, false);
    }

    std::snprintf(buffer, sizeof(buffer), "%lu", static_cast<unsigned long>(meta.attempt_count));
    appendField(out, "attempt_count", buffer, false);

    std::snprintf(buffer, sizeof(buffer), "%lu", static_cast<unsigned long>(meta.sequence));
    appendField(out, "sequence", buffer, false);

    // Replace the final ",\n" of the last field with "\n}\n".
    out.erase(out.size() - 2);
    out += "\n}\n";
    return out;
}

bool recording_meta_from_json(const std::string& json, RecordingMeta& out) {
    std::vector<std::pair<std::string, ParsedField> > fields;
    if (!parseFlatObject(json, &fields)) {
        return false;
    }

    RecordingMeta parsed;
    bool have_recording_id = false;
    bool have_wav_bytes = false;

    for (size_t i = 0; i < fields.size(); ++i) {
        const std::string& key = fields[i].first;
        const ParsedField& field = fields[i].second;

        if (key == "schema_version") {
            if (!toU32(field, &parsed.schema_version)) {
                return false;
            }
        } else if (key == "recording_id") {
            if (!field.quoted) {
                return false;
            }
            parsed.recording_id = field.value;
            have_recording_id = true;
        } else if (key == "device_id") {
            if (!field.quoted) {
                return false;
            }
            parsed.device_id = field.value;
        } else if (key == "tag_index") {
            if (!toU32(field, &parsed.tag_index)) {
                return false;
            }
        } else if (key == "duration_ms") {
            if (!toU32(field, &parsed.duration_ms)) {
                return false;
            }
        } else if (key == "wav_bytes") {
            if (!toU32(field, &parsed.wav_bytes)) {
                return false;
            }
            have_wav_bytes = true;
        } else if (key == "sample_rate") {
            if (!toU32(field, &parsed.sample_rate)) {
                return false;
            }
        } else if (key == "created_utc") {
            if (!toU32(field, &parsed.created_utc)) {
                return false;
            }
        } else if (key == "crc32") {
            if (!toU32(field, &parsed.crc32)) {
                return false;
            }
            // The key being present *is* the "a CRC was computed" signal; a
            // legitimate CRC of 0 is still a computed CRC.
            parsed.has_crc = true;
        } else if (key == "attempt_count") {
            if (!toU32(field, &parsed.attempt_count)) {
                return false;
            }
        } else if (key == "sequence") {
            if (!toU32(field, &parsed.sequence)) {
                return false;
            }
        }
        // Unknown keys are ignored on purpose: a newer writer can add fields
        // without breaking an older reader.
    }

    if (!have_recording_id || !have_wav_bytes) {
        return false;
    }
    if (!is_valid_recording_id(parsed.recording_id.c_str())) {
        return false;
    }
    if (parsed.wav_bytes == 0) {
        return false;
    }
    if (parsed.schema_version > kRecordingMetaSchemaVersion) {
        return false;
    }

    out = parsed;
    return true;
}

}  // namespace voice_memo_firmware
