#pragma once
// CLEMSA Mastercode MV12 36-bit transmit model.
// Command: 10;CLEMSA-MV12;ID=0ABF;SWITCH=1;CMD=ON;
// ID: encode each DIP as down(-)=00, middle(0)=10, up(+)=11; pack switch 1 into bits 1:0 
//     through switch 8 into bits 15:14, then write as 4 hex digits.
// Optional tuning: LEADING=6610;CHIP=65;SLOT=3120;GUARD=16000;
// LEADING is four hex digits. Some remotes use different values (e.g. B7E0).
// Timings are independent values in microseconds.
// Distributed under the RFLink Gateway license; retain License.txt.

#ifdef PLUGIN_TX_078

#include <stdint.h>
#include <stddef.h>
#include <string.h>

namespace RFLink { namespace ClemsaMV12 {

static constexpr uint8_t BIT_COUNT = 36;

struct Request {
    uint16_t serial = 0;
    uint16_t leading = 0x6610;
    uint16_t chipUs = 65;
    uint16_t slotUs = 3120; // 48 chips at the default CHIP; independently tunable.
    uint16_t guardUs = 16000;
    uint8_t button = 1;
    uint8_t frames = 10; // Total frames, not additional repeats.
    uint8_t repeat = 1;
    uint16_t durationMs = 0; // 0 = disabled
};

// Parser field masks. ID and DIP intentionally share one bit because they are
// mutually exclusive.
static constexpr unsigned FIELD_ID_DIP = 1U;
static constexpr unsigned FIELD_SWITCH = 2U;
static constexpr unsigned FIELD_CMD = 4U;
static constexpr unsigned FIELD_LEADING = 8U;
static constexpr unsigned FIELD_CHIP = 16U;
static constexpr unsigned FIELD_SLOT = 32U;
static constexpr unsigned FIELD_GUARD = 64U;
static constexpr unsigned FIELD_FRAMES = 128U;
static constexpr unsigned FIELD_REPEAT = 256U;
static constexpr unsigned FIELD_DURATION = 512U;

inline bool equal(const char *text, size_t length, const char *expected) {
    if (strlen(expected) != length) return false;
    for (size_t i = 0; i < length; ++i) {
        char c = text[i];
        if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
        if (c != expected[i]) return false;
    }
    return true;
}

inline bool matches(const char *command) {
    if (!command || strncmp(command, "10;", 3) != 0) return false;
    const char *end = strchr(command + 3, ';');
    const size_t length = end ? size_t(end - command - 3) : strlen(command + 3);
    return equal(command + 3, length, "CLEMSA-MV12");
}

inline bool number(const char *text, size_t length, unsigned base, uint32_t &value) {
    if (!length || length > 8) return false;
    value = 0;
    for (size_t i = 0; i < length; ++i) {
        const char c = text[i];
        unsigned digit;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else return false;
        if (digit >= base || value > (UINT32_MAX - digit) / base) return false;
        value = value * base + digit;
    }
    return true;
}

inline const char *validate(const Request &r) {
    for (unsigned i = 0; i < 8; ++i)
        if (((r.serial >> (2 * i)) & 3) == 1) return "INVALID_DIP_PAIR";

    if (r.button != 1 && r.button != 2)
        return "SWITCH_MUST_BE_1_OR_2";

    if (r.frames < 1 || r.frames > 12)
        return "FRAMES_RANGE_1_12";

    if (r.repeat < 1 || r.repeat > 64)
        return "REPEAT_RANGE_1_64";

    if (r.durationMs != 0 &&
        (r.durationMs < 100 || r.durationMs > 10000))
        return "DURATION_RANGE_100_10000_MS";

    if (r.chipUs < 50 || r.chipUs > 90)
        return "CHIP_RANGE_50_90";

    if (r.slotUs < 2800 || r.slotUs > 3600)
        return "SLOT_RANGE_2800_3600";

    if (r.guardUs < 1500 || r.guardUs > 20000)
        return "GUARD_RANGE_1500_20000";

    // A logical 1 uses 16 ON pulses. Its active span from the first rising
    // edge through the final falling edge is 31 chips, so there must still be
    // a non-zero LOW remainder inside the slot.
    if (r.slotUs <= 31U * r.chipUs)
        return "SLOT_MUST_EXCEED_31_CHIPS";

    return nullptr;
}

inline const char *parse(const char *command, Request &output) {
    if (!matches(command)) return "NOT_MV12";
    if (strlen(command) > 255) return "COMMAND_TOO_LONG";
    Request r;
    unsigned seen = 0;
    const char *separator = strchr(command + 3, ';');
    if (!separator) return "EXPECTED_SEMICOLON";
    const char *p = separator + 1;
    while (*p) {
        const char *end = strchr(p, ';');
        if (!end || end == p) return "EXPECTED_FIELD_AND_SEMICOLON";
        const char *eq = static_cast<const char *>(memchr(p, '=', end - p));
        if (!eq || eq == p || eq + 1 == end) return "EXPECTED_KEY_VALUE";
        const size_t keyLength = eq - p;
        const char *v = eq + 1;
        const size_t n = end - v;
        uint32_t value = 0;
        unsigned field = 0;
        if (equal(p, keyLength, "ID")) {
            field = FIELD_ID_DIP;
            if (n != 4 || !number(v, n, 16, value)) return "ID_REQUIRES_4_HEX_DIGITS";
            r.serial = value;
        } else if (equal(p, keyLength, "DIP")) {
            field = FIELD_ID_DIP; // ID and DIP are mutually exclusive.
            if (n != 8) return "DIP_REQUIRES_8_SWITCHES";
            r.serial = 0;
            for (unsigned i = 0; i < 8; ++i) {
                if (v[i] != '-' && v[i] != '0' && v[i] != '+') return "INVALID_DIP_SYMBOL";
                r.serial |= uint16_t(v[i] == '+' ? 3 : v[i] == '0' ? 2 : 0) << (2 * i);
            }
        } else if (equal(p, keyLength, "SWITCH")) {
            field = FIELD_SWITCH;
            if (n != 1 || (v[0] != '1' && v[0] != '2')) return "SWITCH_MUST_BE_1_OR_2";
            r.button = v[0] - '0';
        } else if (equal(p, keyLength, "CMD")) {
            field = FIELD_CMD;
            if (!equal(v, n, "ON")) return "CMD_MUST_BE_ON";
        } else if (equal(p, keyLength, "LEADING")) {
            field = FIELD_LEADING;
            if (n != 4 || !number(v, n, 16, value)) return "LEADING_REQUIRES_4_HEX_DIGITS";
            r.leading = value;
        } else {
            if (equal(p, keyLength, "CHIP")) field = FIELD_CHIP;
            else if (equal(p, keyLength, "SLOT")) field = FIELD_SLOT;
            else if (equal(p, keyLength, "GUARD")) field = FIELD_GUARD;
            else if (equal(p, keyLength, "FRAMES")) field = FIELD_FRAMES;
            else if (equal(p, keyLength, "REPEAT")) field = FIELD_REPEAT;
            else if (equal(p, keyLength, "DURATION")) field = FIELD_DURATION;
            else return "UNKNOWN_FIELD";
            const uint32_t maxValue =
                (field == FIELD_FRAMES || field == FIELD_REPEAT) ? 255U : 65535U;
            if (!number(v, n, 10, value) || value > maxValue)
                return "INVALID_INTEGER";
            if (field == FIELD_CHIP) r.chipUs = value;
            if (field == FIELD_SLOT) r.slotUs = value;
            if (field == FIELD_GUARD) r.guardUs = value;
            if (field == FIELD_FRAMES) r.frames = value;
            if (field == FIELD_REPEAT) r.repeat = value;
            if (field == FIELD_DURATION) r.durationMs = value;
        }
        if (seen & field) return "DUPLICATE_FIELD";
        seen |= field;
        p = end + 1;
    }

    if ((seen & (FIELD_ID_DIP | FIELD_SWITCH | FIELD_CMD)) !=
        (FIELD_ID_DIP | FIELD_SWITCH | FIELD_CMD))
        return "REQUIRES_ID_OR_DIP_SWITCH_AND_CMD";

    if ((seen & FIELD_REPEAT) && (seen & FIELD_DURATION))
        return "REPEAT_AND_DURATION_MUTUALLY_EXCLUSIVE";

    if (const char *error = validate(r)) return error;
    output = r;
    return nullptr;
}

inline uint64_t payload(const Request &r) {
    const uint64_t buttonBits = r.button == 1 ? 2ULL : 1ULL;

    // MSB first: leading16 | serial16 | button2 | suffix00.
    // The tested remote uses 0x6610, rather than the Flipper example's 0xB7E0.
    return (uint64_t(r.leading) << 20) |
           (uint64_t(r.serial) << 4) |
           (buttonBits << 2);
}

inline size_t pairCount(const Request &r) {
    const uint64_t data = payload(r);
    size_t count = 0;
    for (int bit = BIT_COUNT - 1; bit >= 0; --bit)
        count += ((data >> bit) & 1) ? 16 : 8;
    return count * r.frames;
}

inline uint32_t durationUs(const Request &r) {
    return (uint32_t(BIT_COUNT) * r.slotUs + r.guardUs) * r.frames;
}

// Emit explicit RF ON/OFF pairs. The last OFF in a symbol includes its idle
// remainder, so no extra chip is accidentally added to the slot. The final
// symbol includes an additional guard (not a replacement for its normal LOW).
template <typename Emit> bool encode(const Request &r, Emit emit) {
    if (validate(r)) return false;
    const uint64_t data = payload(r);

    for (unsigned frame = 0; frame < r.frames; ++frame) {
        for (int bit = BIT_COUNT - 1; bit >= 0; --bit) {
            const unsigned cycles = ((data >> bit) & 1) ? 16 : 8;
            for (unsigned cycle = 0; cycle < cycles; ++cycle) {
                uint16_t off = r.chipUs;
                if (cycle + 1 == cycles) {
                    off = static_cast<uint16_t>(
                        r.slotUs - (2 * cycles - 1) * r.chipUs);
                    if (bit == 0)
                        off = static_cast<uint16_t>(off + r.guardUs);
                }
                if (!emit(r.chipUs, off)) return false;
            }
        }
    }
    return true;
}

}} // namespace RFLink::ClemsaMV12

#endif // PLUGIN_TX_078
