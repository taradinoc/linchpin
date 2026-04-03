/* Copyright 2026 Tara McGrew. See LICENSE for details. */

/*
 * lpst_run_dispatch_real.c — Floating-point arithmetic for the VM.
 *
 * The VM stores a double-precision float as a sequence of 8 payload words
 * (16 bytes) inside an aggregate.  The first eight bytes hold the IEEE 754
 * double in little-endian byte order; the remaining bytes are padding.
 *
 * On POSIX (and other platforms with a working <math.h>) the standard
 * library functions are used directly.
 *
 * On Atari ST / Amiga, <math.h> transcendental functions are unavailable,
 * so this file provides a software implementation for log() and exp().
 * The implementation uses the standard range-reduction technique:
 *   log(x)  = log2(x) / log2(e)  with argument reduced to [sqrt(2)/2, sqrt(2)]
 *   exp(x)  = 2^(x/ln2)          with argument reduced to [-0.5, 0.5]
 * The LPST_DOUBLE_* macros manipulate the IEEE 754 bit representation
 * directly through the lpst_double_bits union.
 */
#include "lpst_run_internal.h"

#if !LPST_PLATFORM_ATARI && !LPST_PLATFORM_AMIGA
#include <math.h>
#endif

typedef enum lpst_real_binary_op {
    LPST_REAL_ADD,
    LPST_REAL_SUBTRACT,
    LPST_REAL_MULTIPLY,
    LPST_REAL_DIVIDE
} lpst_real_binary_op;

#if LPST_PLATFORM_ATARI || LPST_PLATFORM_AMIGA
#define LPST_DOUBLE_SIGN_MASK 0x8000000000000000ULL
#define LPST_DOUBLE_EXPONENT_MASK 0x7FF0000000000000ULL
#define LPST_DOUBLE_FRACTION_MASK 0x000FFFFFFFFFFFFFULL
#define LPST_DOUBLE_HIDDEN_BIT 0x0010000000000000ULL

#define LPST_SQRT2 1.41421356237309504880
#define LPST_LN2 0.69314718055994530942
#define LPST_INV_LN2 1.44269504088896340736
#define LPST_LOG_MAX_INPUT 709.78271289338397310
#define LPST_LOG_MIN_INPUT -745.13321910194110842

typedef union lpst_double_bits {
    double value;
    uint64_t bits;
} lpst_double_bits;

static double round_to_nearest_integral(double value);

static uint64_t get_double_bits(double value)
{
    lpst_double_bits encoded;

    encoded.value = value;
    return encoded.bits;
}

static double get_double_from_bits(uint64_t bits)
{
    lpst_double_bits decoded;

    decoded.bits = bits;
    return decoded.value;
}

static int is_double_zero(double value)
{
    return (get_double_bits(value) & ~LPST_DOUBLE_SIGN_MASK) == 0;
}

static int is_double_nan(double value)
{
    uint64_t bits = get_double_bits(value);

    return (bits & LPST_DOUBLE_EXPONENT_MASK) == LPST_DOUBLE_EXPONENT_MASK
        && (bits & LPST_DOUBLE_FRACTION_MASK) != 0;
}

static int is_double_infinite(double value)
{
    uint64_t bits = get_double_bits(value);

    return (bits & LPST_DOUBLE_EXPONENT_MASK) == LPST_DOUBLE_EXPONENT_MASK
        && (bits & LPST_DOUBLE_FRACTION_MASK) == 0;
}

static int is_double_negative(double value)
{
    return (get_double_bits(value) & LPST_DOUBLE_SIGN_MASK) != 0;
}

static double make_double_nan(void)
{
    return get_double_from_bits(0x7FF8000000000000ULL);
}

static double make_double_infinity(int negative)
{
    return get_double_from_bits((negative ? LPST_DOUBLE_SIGN_MASK : 0)
        | LPST_DOUBLE_EXPONENT_MASK);
}

static double make_double_zero(int negative)
{
    return get_double_from_bits(negative ? LPST_DOUBLE_SIGN_MASK : 0);
}

static double scale_double_binary(double value, int exponent_adjust)
{
    lpst_double_bits encoded;
    uint64_t sign;
    uint64_t fraction;
    int exponent;

    encoded.value = value;
    sign = encoded.bits & LPST_DOUBLE_SIGN_MASK;
    exponent = (int)((encoded.bits & LPST_DOUBLE_EXPONENT_MASK) >> 52);
    fraction = encoded.bits & LPST_DOUBLE_FRACTION_MASK;

    if (exponent == 0x7FF || (exponent == 0 && fraction == 0)) {
        return value;
    }

    if (exponent == 0) {
        while ((fraction & LPST_DOUBLE_HIDDEN_BIT) == 0) {
            fraction <<= 1;
            exponent_adjust--;
        }

        fraction &= LPST_DOUBLE_FRACTION_MASK;
        exponent = 1;
    }

    exponent += exponent_adjust;

    if (exponent >= 0x7FF) {
        return make_double_infinity(sign != 0);
    }

    if (exponent <= 0) {
        if (exponent <= -52) {
            return make_double_zero(sign != 0);
        }

        fraction |= LPST_DOUBLE_HIDDEN_BIT;
        fraction >>= (unsigned int)(1 - exponent);
        return get_double_from_bits(sign | fraction);
    }

    return get_double_from_bits(sign | ((uint64_t)exponent << 52) | fraction);
}

static double normalize_log_mantissa(double value, int *exponent_out)
{
    lpst_double_bits encoded;
    int exponent;
    double mantissa;

    encoded.value = value;
    exponent = (int)((encoded.bits & LPST_DOUBLE_EXPONENT_MASK) >> 52);

    if (exponent == 0) {
        encoded.value = scale_double_binary(value, 54);
        exponent = (int)((encoded.bits & LPST_DOUBLE_EXPONENT_MASK) >> 52) - 54;
    }

    exponent -= 1023;
    encoded.bits = (encoded.bits & LPST_DOUBLE_FRACTION_MASK) | ((uint64_t)1023 << 52);
    mantissa = encoded.value;

    if (mantissa > LPST_SQRT2) {
        mantissa *= 0.5;
        exponent++;
    }

    *exponent_out = exponent;
    return mantissa;
}

static double software_log(double value)
{
    double mantissa;
    double y;
    double y_squared;
    double sum;
    double term;
    int exponent;
    int denominator;

    if (is_double_nan(value)) {
        return make_double_nan();
    }

    if (is_double_zero(value)) {
        return make_double_infinity(1);
    }

    if (value < 0 || (is_double_infinite(value) && is_double_negative(value))) {
        return make_double_nan();
    }

    if (is_double_infinite(value)) {
        return value;
    }

    mantissa = normalize_log_mantissa(value, &exponent);
    y = (mantissa - 1.0) / (mantissa + 1.0);
    y_squared = y * y;
    sum = y;
    term = y;

    for (denominator = 3; denominator <= 19; denominator += 2) {
        term *= y_squared;
        sum += term / (double)denominator;
    }

    return (double)exponent * LPST_LN2 + 2.0 * sum;
}

static double software_exp(double value)
{
    double sum;
    double term;
    double remainder;
    int exponent;
    int index;

    if (is_double_nan(value)) {
        return value;
    }

    if (is_double_infinite(value)) {
        return is_double_negative(value) ? 0.0 : value;
    }

    if (value >= LPST_LOG_MAX_INPUT) {
        return make_double_infinity(0);
    }

    if (value <= LPST_LOG_MIN_INPUT) {
        return 0.0;
    }

    exponent = (int)round_to_nearest_integral(value * LPST_INV_LN2);
    remainder = value - (double)exponent * LPST_LN2;
    sum = 1.0;
    term = 1.0;

    for (index = 1; index <= 12; index++) {
        term *= remainder / (double)index;
        sum += term;
    }

    return scale_double_binary(sum, exponent);
}
#else
static int is_double_nan(double value)
{
    return isnan(value) != 0;
}

static int is_double_infinite(double value)
{
    return isinf(value) != 0;
}

static double software_log(double value)
{
    return log(value);
}

static double software_exp(double value)
{
    return exp(value);
}
#endif

static uint16_t format_ext34_scalar(uint16_t arg0, uint16_t arg1, uint16_t arg2, uint16_t arg3)
{
    (void)arg1;
    (void)arg2;
    (void)arg3;
    return arg0;
}

static double read_big_endian_real64(const lpst_exec_state *state, uint16_t handle)
{
    union {
        double value;
        uint8_t bytes[8];
    } decoded;
    uint8_t buffer[8];
    int index;

    for (index = 0; index < 8; index++) {
        buffer[index] = read_aggregate_payload_byte(state, handle, index);
    }

#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
    for (index = 0; index < 8; index++) {
        decoded.bytes[index] = buffer[index];
    }
#else
    for (index = 0; index < 8; index++) {
        decoded.bytes[index] = buffer[7 - index];
    }
#endif

    return decoded.value;
}

static void write_big_endian_real64(lpst_exec_state *state, uint16_t handle, double value)
{
    union {
        double value;
        uint8_t bytes[8];
    } encoded;
    int index;

    encoded.value = value;

#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
    for (index = 0; index < 8; index++) {
        write_aggregate_payload_byte(state, handle, index, encoded.bytes[index]);
    }
#else
    for (index = 0; index < 8; index++) {
        write_aggregate_payload_byte(state, handle, index, encoded.bytes[7 - index]);
    }
#endif
}

static uint16_t execute_binary_real_operation(lpst_exec_state *state, lpst_real_binary_op op)
{
    uint16_t destination_handle = lpst_exec_pop(state);
    uint16_t right_handle = lpst_exec_pop(state);
    uint16_t left_handle = lpst_exec_pop(state);
    double left = read_big_endian_real64(state, left_handle);
    double right = read_big_endian_real64(state, right_handle);
    double result;

    switch (op) {
    case LPST_REAL_ADD:
        result = left + right;
        break;

    case LPST_REAL_SUBTRACT:
        result = left - right;
        break;

    case LPST_REAL_MULTIPLY:
        result = left * right;
        break;

    case LPST_REAL_DIVIDE:
        result = left / right;
        break;

    default:
        result = 0;
        break;
    }

    write_big_endian_real64(state, destination_handle, result);
    return destination_handle;
}

static uint16_t execute_unary_real_operation(lpst_exec_state *state, double (*op)(double))
{
    uint16_t destination_handle = lpst_exec_pop(state);
    uint16_t source_handle = lpst_exec_pop(state);
    double value = read_big_endian_real64(state, source_handle);

    write_big_endian_real64(state, destination_handle, op(value));
    return destination_handle;
}

static double abs_double(double value)
{
    return value < 0 ? -value : value;
}

static double round_to_nearest_integral(double value)
{
    if (value >= 0) {
        return (double)(long long)(value + 0.5);
    }

    return (double)(long long)(value - 0.5);
}

static double truncate_toward_zero_double(double value)
{
    double rounded = round_to_nearest_integral(value);

    if (value >= 0) {
        return rounded > value ? rounded - 1.0 : rounded;
    }

    return rounded < value ? rounded + 1.0 : rounded;
}

static int is_nearly_integral(double value)
{
    double rounded = round_to_nearest_integral(value);
    return abs_double(value - rounded) < 1e-9;
}

static int append_decimal_char(char *out, size_t out_size, size_t *pos, char ch)
{
    if (*pos + 1 >= out_size) {
        return 0;
    }

    out[*pos] = ch;
    (*pos)++;
    out[*pos] = '\0';
    return 1;
}

static int append_decimal_text(char *out, size_t out_size, size_t *pos, const char *text)
{
    while (*text != '\0') {
        if (!append_decimal_char(out, out_size, pos, *text++)) {
            return 0;
        }
    }

    return 1;
}

static int append_decimal_uint64(char *out, size_t out_size, size_t *pos, uint64_t value)
{
    char digits[32];
    size_t count = 0;

    if (value == 0) {
        return append_decimal_char(out, out_size, pos, '0');
    }

    while (value > 0 && count < sizeof(digits)) {
        digits[count++] = (char)('0' + (value % 10u));
        value /= 10u;
    }

    while (count > 0) {
        if (!append_decimal_char(out, out_size, pos, digits[--count])) {
            return 0;
        }
    }

    return 1;
}

static size_t trim_trailing_decimal_zeros(char *buffer, size_t length)
{
    while (length > 0 && buffer[length - 1] == '0') {
        length--;
    }

    if (length > 0 && buffer[length - 1] == '.') {
        length--;
    }

    buffer[length] = '\0';
    return length;
}

static int format_fixed_decimal_text(double value, int scale, char *out, size_t out_size)
{
    unsigned char digits[32];
    uint64_t whole;
    double whole_double;
    double fractional;
    size_t pos = 0;
    int index;

    if (out == NULL || out_size == 0) {
        return 0;
    }

    out[0] = '\0';

    if (scale < 0) {
        scale = 0;
    }
    if (scale > 30) {
        scale = 30;
    }

    if (value < 0) {
        if (!append_decimal_char(out, out_size, &pos, '-')) {
            return 0;
        }
        value = -value;
    }

    whole_double = truncate_toward_zero_double(value);
    if (whole_double < 0) {
        whole_double = 0;
    }

    whole = (uint64_t)whole_double;
    fractional = value - whole_double;
    if (fractional < 0) {
        fractional = 0;
    } else if (fractional >= 1.0) {
        fractional = 0.9999999999999999;
    }

    for (index = 0; index < scale; index++) {
        double digit_value;
        int digit;

        fractional *= 10.0;
        digit_value = truncate_toward_zero_double(fractional);
        digit = (int)digit_value;
        if (digit < 0) {
            digit = 0;
        } else if (digit > 9) {
            digit = 9;
        }

        digits[index] = (unsigned char)digit;
        fractional -= digit_value;
        if (fractional < 0) {
            fractional = 0;
        }
    }

    fractional *= 10.0;
    if ((int)fractional >= 5) {
        if (scale == 0) {
            whole++;
        } else {
            for (index = scale - 1; index >= 0; index--) {
                if (digits[index] < 9) {
                    digits[index]++;
                    break;
                }

                digits[index] = 0;
            }

            if (index < 0) {
                whole++;
            }
        }
    }

    if (!append_decimal_uint64(out, out_size, &pos, whole)) {
        return 0;
    }

    if (scale > 0) {
        if (!append_decimal_char(out, out_size, &pos, '.')) {
            return 0;
        }

        for (index = 0; index < scale; index++) {
            if (!append_decimal_char(out, out_size, &pos, (char)('0' + digits[index]))) {
                return 0;
            }
        }
    }

    return (int)pos;
}

static int format_general_decimal_text(double value, char *out, size_t out_size)
{
    unsigned char digits[16];
    double normalized;
    int exponent = 0;
    int index;
    int next_digit;
    size_t pos = 0;

    if (out == NULL || out_size == 0) {
        return 0;
    }

    out[0] = '\0';

    if (value == 0) {
        return append_decimal_char(out, out_size, &pos, '0') ? 1 : 0;
    }

    if (value < 0) {
        if (!append_decimal_char(out, out_size, &pos, '-')) {
            return 0;
        }
        value = -value;
    }

    normalized = value;
    while (normalized >= 10.0) {
        normalized /= 10.0;
        exponent++;
    }
    while (normalized < 1.0) {
        normalized *= 10.0;
        exponent--;
    }

    for (index = 0; index < 15; index++) {
        int digit = (int)truncate_toward_zero_double(normalized);

        if (digit < 0) {
            digit = 0;
        } else if (digit > 9) {
            digit = 9;
        }

        digits[index] = (unsigned char)digit;
        normalized = (normalized - digit) * 10.0;
        if (normalized < 0) {
            normalized = 0;
        }
    }

    next_digit = (int)truncate_toward_zero_double(normalized);
    if (next_digit >= 5) {
        for (index = 14; index >= 0; index--) {
            if (digits[index] < 9) {
                digits[index]++;
                break;
            }

            digits[index] = 0;
        }

        if (index < 0) {
            digits[0] = 1;
            for (index = 1; index < 15; index++) {
                digits[index] = 0;
            }
            exponent++;
        }
    }

    if (exponent >= -4 && exponent < 15) {
        char fractional[32];
        size_t fractional_len = 0;

        if (exponent >= 0) {
            for (index = 0; index <= exponent; index++) {
                char digit_char = index < 15 ? (char)('0' + digits[index]) : '0';

                if (!append_decimal_char(out, out_size, &pos, digit_char)) {
                    return 0;
                }
            }

            for (index = exponent + 1; index < 15; index++) {
                fractional[fractional_len++] = (char)('0' + digits[index]);
            }
        } else {
            if (!append_decimal_char(out, out_size, &pos, '0')) {
                return 0;
            }

            fractional[fractional_len++] = '.';
            for (index = 0; index < -exponent - 1; index++) {
                fractional[fractional_len++] = '0';
            }
            for (index = 0; index < 15; index++) {
                fractional[fractional_len++] = (char)('0' + digits[index]);
            }
        }

        if (fractional_len > 0) {
            fractional[fractional_len] = '\0';
            fractional_len = trim_trailing_decimal_zeros(fractional, fractional_len);
            if (fractional_len > 0) {
                if (fractional[0] != '.') {
                    if (!append_decimal_char(out, out_size, &pos, '.')) {
                        return 0;
                    }
                }
                if (!append_decimal_text(out, out_size, &pos, fractional[0] == '.' ? fractional + 1 : fractional)) {
                    return 0;
                }
            }
        }

        return (int)pos;
    }

    if (!append_decimal_char(out, out_size, &pos, (char)('0' + digits[0]))) {
        return 0;
    }

    {
        char fractional[32];
        size_t fractional_len = 0;

        for (index = 1; index < 15; index++) {
            fractional[fractional_len++] = (char)('0' + digits[index]);
        }
        fractional[fractional_len] = '\0';
        fractional_len = trim_trailing_decimal_zeros(fractional, fractional_len);
        if (fractional_len > 0) {
            if (!append_decimal_char(out, out_size, &pos, '.')) {
                return 0;
            }
            if (!append_decimal_text(out, out_size, &pos, fractional)) {
                return 0;
            }
        }
    }

    if (!append_decimal_char(out, out_size, &pos, 'E')) {
        return 0;
    }

    if (exponent < 0) {
        if (!append_decimal_char(out, out_size, &pos, '-')) {
            return 0;
        }
        exponent = -exponent;
    }

    return append_decimal_uint64(out, out_size, &pos, (uint64_t)exponent) ? (int)pos : 0;
}

static int format_ext34_numeric_text(const lpst_exec_state *state,
    uint16_t source_handle,
    uint16_t control_word0,
    uint16_t control_word1,
    char *out,
    size_t out_size)
{
    double value = read_big_endian_real64(state, source_handle);
    int scale = control_word0 == LPST_FALSE_SENTINEL ? -1 : (int)(int16_t)control_word0;
    uint8_t flags = (uint8_t)control_word1;
    int written;

    if (out == NULL || out_size == 0) {
        return 0;
    }

    if (is_double_nan(value)) {
        written = (int)strlen("NaN");
        if (written >= (int)out_size) {
            written = (int)out_size - 1;
        }
        memcpy(out, "NaN", (size_t)written);
        out[written] = '\0';
    } else if (is_double_infinite(value)) {
        const char *literal = value > 0 ? "Infinity" : "-Infinity";
        written = (int)strlen(literal);
        if (written >= (int)out_size) {
            written = (int)out_size - 1;
        }
        memcpy(out, literal, (size_t)written);
        out[written] = '\0';
    } else if (scale >= 0) {
        int clamped_scale = scale;

        if (clamped_scale > 30) {
            clamped_scale = 30;
        }

        written = format_fixed_decimal_text(value, clamped_scale, out, out_size);
        if (written > 1) {
            if (out[0] == '0' && out[1] == '.') {
                memmove(out, out + 1, (size_t)written);
                written--;
            } else if (written > 2 && out[0] == '-' && out[1] == '0' && out[2] == '.') {
                memmove(out + 1, out + 2, (size_t)(written - 1));
                written--;
            }
        }
    } else if ((flags & 0x08u) != 0 || is_nearly_integral(value)) {
        written = format_fixed_decimal_text(round_to_nearest_integral(value), 0, out, out_size);
    } else {
        written = format_general_decimal_text(value, out, out_size);
    }

    if (written < 0) {
        out[0] = '\0';
        return 0;
    }

    if ((size_t)written >= out_size) {
        return (int)(out_size - 1);
    }

    return written;
}

uint16_t execute_real_add(lpst_exec_state *state)
{
    return execute_binary_real_operation(state, LPST_REAL_ADD);
}

uint16_t execute_real_subtract(lpst_exec_state *state)
{
    return execute_binary_real_operation(state, LPST_REAL_SUBTRACT);
}

uint16_t execute_real_multiply(lpst_exec_state *state)
{
    return execute_binary_real_operation(state, LPST_REAL_MULTIPLY);
}

uint16_t execute_real_divide(lpst_exec_state *state)
{
    return execute_binary_real_operation(state, LPST_REAL_DIVIDE);
}

uint16_t execute_real_log(lpst_exec_state *state)
{
    return execute_unary_real_operation(state, software_log);
}

uint16_t execute_real_exp(lpst_exec_state *state)
{
    return execute_unary_real_operation(state, software_exp);
}

void handle_ext34(lpst_exec_state *state)
{
    uint16_t control_word1;
    uint16_t control_word0;
    uint16_t destination_word;
    uint16_t source_word;

    if (state->eval_stack_top < 4) {
        lpst_exec_push(state, LPST_FALSE_SENTINEL);
        return;
    }

    control_word1 = lpst_exec_pop(state);
    control_word0 = lpst_exec_pop(state);
    destination_word = lpst_exec_pop(state);
    source_word = lpst_exec_pop(state);

    if (source_word != 0 && source_word != LPST_FALSE_SENTINEL
        && destination_word != 0 && destination_word != LPST_FALSE_SENTINEL) {
        char formatted[64];
        int formatted_length = format_ext34_numeric_text(state,
            source_word,
            control_word0,
            control_word1,
            formatted,
            sizeof(formatted));
        int capacity = read_aggregate_word(state, destination_word, 0);
        int bytes_to_write = formatted_length < capacity ? formatted_length : capacity;
        int index;

        for (index = 0; index < bytes_to_write; index++) {
            write_aggregate_payload_byte(state, destination_word, index, (uint8_t)formatted[index]);
        }

        if (bytes_to_write < capacity) {
            write_aggregate_payload_byte(state, destination_word, bytes_to_write, 0);
        }

        if (state->trace_enabled) {
            fprintf(stderr,
                    "[FMTREAL] src=0x%04X dst=0x%04X c0=0x%04X c1=0x%04X -> len=%d text=\"%s\"\n",
                    source_word,
                    destination_word,
                    control_word0,
                    control_word1,
                    bytes_to_write,
                    formatted);
        }

        lpst_exec_push(state, (uint16_t)bytes_to_write);
    } else {
        lpst_exec_push(state,
            format_ext34_scalar(source_word, destination_word, control_word0, control_word1));
    }
}
