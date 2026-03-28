#include "vm.h"
#include "safe_alloc.h"
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define BIGINT_LIMB_BITS 32
#define BIGINT_DEC_BASE 1000000000u
#define BIGINT_DEC_BASE_DIGITS 9

static size_t bigint_trim_count(const uint32_t* limbs, size_t count) {
    while (count > 0 && limbs[count - 1] == 0) {
        count--;
    }
    return count;
}

static ObjBigInt* bigint_from_limbs(int sign, uint32_t* limbs, size_t count) {
    if (limbs && count > 0) {
        count = bigint_trim_count(limbs, count);
    }

    if (count == 0) {
        if (limbs) free(limbs);
        limbs = NULL;
        sign = 0;
    } else {
        sign = sign < 0 ? -1 : 1;
        size_t size = count * sizeof(uint32_t);
        limbs = (uint32_t*)safe_realloc(limbs, size);
    }

    ObjBigInt* bi = (ObjBigInt*)safe_malloc(sizeof(ObjBigInt));
    bi->ref_count = 1;
    bi->sign = sign;
    bi->limbs = limbs;
    bi->count = count;
    return bi;
}

static ObjBigInt* bigint_copy(const ObjBigInt* src) {
    if (!src || src->count == 0) {
        return bigint_from_limbs(0, NULL, 0);
    }
    uint32_t* limbs = (uint32_t*)safe_malloc(src->count * sizeof(uint32_t));
    memcpy(limbs, src->limbs, src->count * sizeof(uint32_t));
    return bigint_from_limbs(src->sign, limbs, src->count);
}

static int bigint_compare_abs_limbs(const uint32_t* a, size_t a_count, const uint32_t* b, size_t b_count) {
    if (a_count < b_count) return -1;
    if (a_count > b_count) return 1;
    for (size_t i = a_count; i > 0; i--) {
        uint32_t av = a[i - 1];
        uint32_t bv = b[i - 1];
        if (av < bv) return -1;
        if (av > bv) return 1;
    }
    return 0;
}

int obj_bigint_compare_abs(const ObjBigInt* a, const ObjBigInt* b) {
    if (!a || !b) return 0;
    return bigint_compare_abs_limbs(a->limbs, a->count, b->limbs, b->count);
}

static uint32_t* bigint_add_abs_limbs(const uint32_t* a, size_t a_count,
                                     const uint32_t* b, size_t b_count,
                                     size_t* out_count) {
    size_t max_count = a_count > b_count ? a_count : b_count;
    uint32_t* out = (uint32_t*)safe_malloc((max_count + 1) * sizeof(uint32_t));
    uint64_t carry = 0;

    for (size_t i = 0; i < max_count; i++) {
        uint64_t sum = carry;
        if (i < a_count) sum += a[i];
        if (i < b_count) sum += b[i];
        out[i] = (uint32_t)sum;
        carry = sum >> BIGINT_LIMB_BITS;
    }

    if (carry) {
        out[max_count] = (uint32_t)carry;
        *out_count = max_count + 1;
    } else {
        *out_count = max_count;
    }

    *out_count = bigint_trim_count(out, *out_count);
    return out;
}

static uint32_t* bigint_sub_abs_limbs(const uint32_t* a, size_t a_count,
                                     const uint32_t* b, size_t b_count,
                                     size_t* out_count) {
    uint32_t* out = (uint32_t*)safe_malloc(a_count * sizeof(uint32_t));
    uint64_t borrow = 0;

    for (size_t i = 0; i < a_count; i++) {
        uint64_t av = a[i];
        uint64_t bv = (i < b_count) ? (uint64_t)b[i] : 0;
        uint64_t sub = bv + borrow;
        borrow = av < sub ? 1 : 0;
        out[i] = (uint32_t)(av - sub);
    }

    *out_count = bigint_trim_count(out, a_count);
    return out;
}

static uint32_t* bigint_mul_abs_limbs(const uint32_t* a, size_t a_count,
                                     const uint32_t* b, size_t b_count,
                                     size_t* out_count) {
    if (a_count == 0 || b_count == 0) {
        *out_count = 0;
        return NULL;
    }

    size_t len = a_count + b_count;
    uint32_t* out = (uint32_t*)safe_calloc(len + 1, sizeof(uint32_t));

    for (size_t i = 0; i < a_count; i++) {
        uint64_t carry = 0;
        for (size_t j = 0; j < b_count; j++) {
            uint64_t cur = (uint64_t)out[i + j];
            cur += (uint64_t)a[i] * (uint64_t)b[j];
            cur += carry;
            out[i + j] = (uint32_t)cur;
            carry = cur >> BIGINT_LIMB_BITS;
        }

        size_t k = i + b_count;
        while (carry != 0 && k < len + 1) {
            uint64_t cur = (uint64_t)out[k] + carry;
            out[k] = (uint32_t)cur;
            carry = cur >> BIGINT_LIMB_BITS;
            k++;
        }
    }

    size_t count = bigint_trim_count(out, len + 1);
    if (count == 0) {
        free(out);
        *out_count = 0;
        return NULL;
    }

    out = (uint32_t*)safe_realloc(out, count * sizeof(uint32_t));
    *out_count = count;
    return out;
}

static uint32_t* bigint_mul_small(const uint32_t* a, size_t a_count, uint32_t m, size_t* out_count) {
    if (a_count == 0 || m == 0) {
        *out_count = 0;
        return NULL;
    }

    uint32_t* out = (uint32_t*)safe_malloc((a_count + 1) * sizeof(uint32_t));
    uint64_t carry = 0;
    for (size_t i = 0; i < a_count; i++) {
        uint64_t cur = (uint64_t)a[i] * (uint64_t)m + carry;
        out[i] = (uint32_t)cur;
        carry = cur >> BIGINT_LIMB_BITS;
    }

    if (carry) {
        out[a_count] = (uint32_t)carry;
        *out_count = a_count + 1;
    } else {
        *out_count = a_count;
    }

    return out;
}

static void bigint_divmod_small(const uint32_t* a, size_t a_count, uint32_t d,
                                uint32_t** q_out, size_t* q_count, uint32_t* r_out) {
    bool want_q = (q_out != NULL && q_count != NULL);

    uint32_t* q = NULL;
    if (want_q && a_count > 0) {
        q = (uint32_t*)safe_calloc(a_count, sizeof(uint32_t));
    }

    uint64_t rem = 0;
    for (size_t i = a_count; i > 0; i--) {
        uint64_t cur = (rem << BIGINT_LIMB_BITS) | (uint64_t)a[i - 1];
        uint32_t q_digit = (uint32_t)(cur / d);
        if (q) q[i - 1] = q_digit;
        rem = cur % d;
    }

    size_t trimmed = q ? bigint_trim_count(q, a_count) : 0;
    if (trimmed == 0 && q) {
        free(q);
        q = NULL;
    }

    if (want_q) {
        *q_out = q;
        *q_count = trimmed;
    } else if (q) {
        free(q);
    }
    if (r_out) *r_out = (uint32_t)rem;
}

static bool bigint_sub_mul(uint32_t* u, const uint32_t* v, size_t n, size_t j, uint64_t qhat) {
    uint64_t carry = 0;
    int64_t borrow = 0;
    const int64_t base = (int64_t)(UINT64_C(1) << BIGINT_LIMB_BITS);

    for (size_t i = 0; i < n; i++) {
        uint64_t p = qhat * (uint64_t)v[i] + carry;
        carry = p >> BIGINT_LIMB_BITS;
        uint32_t p_low = (uint32_t)p;

        int64_t t = (int64_t)u[j + i] - (int64_t)p_low - borrow;
        if (t < 0) {
            t += base;
            borrow = 1;
        } else {
            borrow = 0;
        }
        u[j + i] = (uint32_t)t;
    }

    int64_t t = (int64_t)u[j + n] - (int64_t)carry - borrow;
    if (t < 0) {
        u[j + n] = (uint32_t)(t + base);
        return true;
    }

    u[j + n] = (uint32_t)t;
    return false;
}

static void bigint_add_back(uint32_t* u, size_t u_count, const uint32_t* v, size_t n, size_t j) {
    uint64_t carry = 0;
    for (size_t i = 0; i < n; i++) {
        uint64_t sum = (uint64_t)u[j + i] + (uint64_t)v[i] + carry;
        u[j + i] = (uint32_t)sum;
        carry = sum >> BIGINT_LIMB_BITS;
    }

    size_t k = j + n;
    while (carry != 0 && k < u_count) {
        uint64_t sum = (uint64_t)u[k] + carry;
        u[k] = (uint32_t)sum;
        carry = sum >> BIGINT_LIMB_BITS;
        k++;
    }
}

static void bigint_divmod_abs_limbs(const uint32_t* a, size_t a_count,
                                    const uint32_t* b, size_t b_count,
                                    uint32_t** q_out, size_t* q_count,
                                    uint32_t** r_out, size_t* r_count,
                                    bool* div_by_zero) {
    bool want_q = (q_out != NULL && q_count != NULL);
    bool want_r = (r_out != NULL && r_count != NULL);

    if (div_by_zero) *div_by_zero = false;
    if (b_count == 0) {
        if (div_by_zero) *div_by_zero = true;
        if (want_q) {
            *q_out = NULL;
            *q_count = 0;
        }
        if (want_r) {
            *r_out = NULL;
            *r_count = 0;
        }
        return;
    }

    if (a_count == 0) {
        if (want_q) {
            *q_out = NULL;
            *q_count = 0;
        }
        if (want_r) {
            *r_out = NULL;
            *r_count = 0;
        }
        return;
    }

    int cmp = bigint_compare_abs_limbs(a, a_count, b, b_count);
    if (cmp < 0) {
        if (want_q) {
            *q_out = NULL;
            *q_count = 0;
        }
        if (want_r) {
            uint32_t* r = (uint32_t*)safe_malloc(a_count * sizeof(uint32_t));
            memcpy(r, a, a_count * sizeof(uint32_t));
            *r_out = r;
            *r_count = a_count;
        }
        return;
    }
    if (cmp == 0) {
        if (want_q) {
            uint32_t* q = (uint32_t*)safe_malloc(sizeof(uint32_t));
            q[0] = 1;
            *q_out = q;
            *q_count = 1;
        }
        if (want_r) {
            *r_out = NULL;
            *r_count = 0;
        }
        return;
    }

    if (b_count == 1) {
        uint32_t* q = NULL;
        size_t q_len = 0;
        uint32_t rem = 0;
        bigint_divmod_small(a, a_count, b[0], want_q ? &q : NULL, want_q ? &q_len : NULL, &rem);
        uint32_t* r = NULL;
        size_t r_len = 0;
        if (want_r && rem != 0) {
            r = (uint32_t*)safe_malloc(sizeof(uint32_t));
            r[0] = rem;
            r_len = 1;
        }
        if (want_q) {
            *q_out = q;
            *q_count = q_len;
        }
        if (want_r) {
            *r_out = r;
            *r_count = r_len;
        }
        return;
    }

    // Fast path: small quotients when a and b have the same limb count.
    // This avoids O(n^2) division and large intermediate allocations.
    if ((a_count == b_count || a_count == (b_count + 1)) && (want_q || want_r)) {
        const uint32_t SMALL_Q_LIMIT = 16;
        uint32_t bh = b[b_count - 1];
        if (bh != 0) {
            uint64_t q_est = 0;
            if (a_count == b_count) {
                q_est = (uint64_t)a[a_count - 1] / (uint64_t)bh;
            } else {
                uint64_t numerator = ((uint64_t)a[a_count - 1] << BIGINT_LIMB_BITS) | (uint64_t)a[a_count - 2];
                q_est = numerator / (uint64_t)bh;
            }
            if (q_est < SMALL_Q_LIMIT) {
                uint32_t* rem = (uint32_t*)safe_malloc(a_count * sizeof(uint32_t));
                memcpy(rem, a, a_count * sizeof(uint32_t));
                size_t rem_count = a_count;

                uint32_t q = 0;
                while (q < SMALL_Q_LIMIT) {
                    int rem_cmp = bigint_compare_abs_limbs(rem, rem_count, b, b_count);
                    if (rem_cmp < 0) {
                        break;
                    }

                    // rem -= b (in place); rem_count shrinks as needed.
                    uint64_t borrow = 0;
                    for (size_t i = 0; i < rem_count; i++) {
                        uint64_t av = rem[i];
                        uint64_t bv = (i < b_count) ? (uint64_t)b[i] : 0;
                        uint64_t sub = bv + borrow;
                        borrow = av < sub ? 1 : 0;
                        rem[i] = (uint32_t)(av - sub);
                    }
                    rem_count = bigint_trim_count(rem, rem_count);
                    q++;
                }

                if (bigint_compare_abs_limbs(rem, rem_count, b, b_count) < 0) {
                    if (want_q) {
                        if (q == 0) {
                            *q_out = NULL;
                            *q_count = 0;
                        } else {
                            uint32_t* q_limbs = (uint32_t*)safe_malloc(sizeof(uint32_t));
                            q_limbs[0] = q;
                            *q_out = q_limbs;
                            *q_count = 1;
                        }
                    }

                    if (want_r) {
                        if (rem_count == 0) {
                            free(rem);
                            *r_out = NULL;
                            *r_count = 0;
                        } else {
                            if (rem_count < a_count) {
                                rem = (uint32_t*)safe_realloc(rem, rem_count * sizeof(uint32_t));
                            }
                            *r_out = rem;
                            *r_count = rem_count;
                        }
                    } else {
                        free(rem);
                    }

                    return;
                }

                free(rem);
            }
        }
    }

    uint32_t d = (uint32_t)((UINT64_C(1) << BIGINT_LIMB_BITS) / ((uint64_t)b[b_count - 1] + 1u));
    size_t u_count = 0;
    uint32_t* u = bigint_mul_small(a, a_count, d, &u_count);
    if (u_count == a_count) {
        u = (uint32_t*)safe_realloc(u, (a_count + 1) * sizeof(uint32_t));
        u[a_count] = 0;
        u_count = a_count + 1;
    }

    size_t v_count = 0;
    uint32_t* v = bigint_mul_small(b, b_count, d, &v_count);
    if (v_count != b_count) {
        v = (uint32_t*)safe_realloc(v, b_count * sizeof(uint32_t));
        v_count = b_count;
    }

    if (u_count <= v_count) {
        u = (uint32_t*)safe_realloc(u, (v_count + 1) * sizeof(uint32_t));
        for (size_t i = u_count; i < v_count + 1; i++) {
            u[i] = 0;
        }
        u_count = v_count + 1;
    }

    size_t m = u_count - v_count - 1;
    uint32_t* q = want_q ? (uint32_t*)safe_calloc(m + 1, sizeof(uint32_t)) : NULL;

    for (size_t j = m + 1; j-- > 0;) {
        uint64_t ujn = u[j + v_count];
        uint64_t ujn1 = u[j + v_count - 1];
        uint64_t ujn2 = u[j + v_count - 2];

        uint64_t numerator = (ujn << BIGINT_LIMB_BITS) | ujn1;
        uint64_t qhat = numerator / v[v_count - 1];
        uint64_t rhat = numerator % v[v_count - 1];

        if (qhat > UINT32_MAX) {
            qhat = UINT32_MAX;
            rhat += v[v_count - 1];
        }

        while (qhat * (uint64_t)v[v_count - 2] > (rhat << BIGINT_LIMB_BITS) + ujn2) {
            qhat--;
            rhat += v[v_count - 1];
            if (rhat >= (UINT64_C(1) << BIGINT_LIMB_BITS)) {
                break;
            }
        }

        if (bigint_sub_mul(u, v, v_count, j, qhat)) {
            qhat--;
            bigint_add_back(u, u_count, v, v_count, j);
        }

        if (q) q[j] = (uint32_t)qhat;
    }

    size_t q_len = 0;
    if (q) {
        q_len = bigint_trim_count(q, m + 1);
        if (q_len == 0) {
            free(q);
            q = NULL;
        }
    }

    uint32_t* r = NULL;
    size_t r_len = 0;
    if (want_r) {
        if (d == 1) {
            r_len = bigint_trim_count(u, v_count);
            if (r_len > 0) {
                r = (uint32_t*)safe_malloc(r_len * sizeof(uint32_t));
                memcpy(r, u, r_len * sizeof(uint32_t));
            }
        } else {
            uint32_t* r_norm = (uint32_t*)safe_malloc(v_count * sizeof(uint32_t));
            memcpy(r_norm, u, v_count * sizeof(uint32_t));
            uint32_t* r_denorm = NULL;
            size_t r_denorm_len = 0;
            uint32_t rem = 0;
            bigint_divmod_small(r_norm, v_count, d, &r_denorm, &r_denorm_len, &rem);
            free(r_norm);
            (void)rem;
            r = r_denorm;
            r_len = r_denorm_len;
        }
    }

    free(u);
    free(v);

    if (want_q) {
        *q_out = q;
        *q_count = q_len;
    } else if (q) {
        free(q);
    }

    if (want_r) {
        *r_out = r;
        *r_count = r_len;
    } else if (r) {
        free(r);
    }
}

static int bigint_digits_u32(uint32_t value) {
    if (value >= 100000000) return 9;
    if (value >= 10000000) return 8;
    if (value >= 1000000) return 7;
    if (value >= 100000) return 6;
    if (value >= 10000) return 5;
    if (value >= 1000) return 4;
    if (value >= 100) return 3;
    if (value >= 10) return 2;
    return 1;
}

static size_t bigint_divmod_small_inplace(uint32_t* limbs, size_t count, uint32_t d, uint32_t* r_out) {
    if (d == 0) {
        if (r_out) *r_out = 0;
        return 0;
    }

    uint64_t rem = 0;
    for (size_t i = count; i > 0; i--) {
        uint64_t cur = (rem << BIGINT_LIMB_BITS) | (uint64_t)limbs[i - 1];
        limbs[i - 1] = (uint32_t)(cur / d);
        rem = cur % d;
    }

    if (r_out) *r_out = (uint32_t)rem;
    return bigint_trim_count(limbs, count);
}

static void bigint_mul_add_small_inplace(uint32_t** limbs_io, size_t* count_io, uint32_t m, uint32_t add) {
    if (!limbs_io || !count_io) return;

    uint32_t* limbs = *limbs_io;
    size_t count = *count_io;

    if (count == 0) {
        if (add == 0) return;
        limbs = (uint32_t*)safe_malloc(sizeof(uint32_t));
        limbs[0] = add;
        *limbs_io = limbs;
        *count_io = 1;
        return;
    }

    uint64_t carry = add;
    for (size_t i = 0; i < count; i++) {
        uint64_t cur = (uint64_t)limbs[i] * (uint64_t)m + carry;
        limbs[i] = (uint32_t)cur;
        carry = cur >> BIGINT_LIMB_BITS;
    }

    if (carry) {
        limbs = (uint32_t*)safe_realloc(limbs, (count + 1) * sizeof(uint32_t));
        limbs[count++] = (uint32_t)carry;
    }

    count = bigint_trim_count(limbs, count);
    if (count == 0) {
        free(limbs);
        limbs = NULL;
    } else {
        limbs = (uint32_t*)safe_realloc(limbs, count * sizeof(uint32_t));
    }

    *limbs_io = limbs;
    *count_io = count;
}

size_t obj_bigint_decimal_digits(const ObjBigInt* bigint) {
    if (!bigint || bigint->count == 0) return 1;

    size_t work_count = bigint->count;
    uint32_t* work = (uint32_t*)safe_malloc(work_count * sizeof(uint32_t));
    memcpy(work, bigint->limbs, work_count * sizeof(uint32_t));

    size_t chunk_count = 0;
    uint32_t ms_chunk = 0;
    while (work_count > 0) {
        uint32_t rem = 0;
        work_count = bigint_divmod_small_inplace(work, work_count, BIGINT_DEC_BASE, &rem);
        ms_chunk = rem;
        chunk_count++;
    }

    free(work);

    if (chunk_count == 0) return 1;
    size_t digits = (chunk_count - 1) * BIGINT_DEC_BASE_DIGITS;
    digits += (size_t)bigint_digits_u32(ms_chunk);
    return digits;
}

bool obj_bigint_is_even(const ObjBigInt* bigint) {
    if (!bigint) return false;
    if (bigint->count == 0) return true;
    return (bigint->limbs[0] & 1u) == 0u;
}

static int bigint_hex_digit_value(char c) {
    if (c >= '0' && c <= '9') return (int)(c - '0');
    if (c >= 'a' && c <= 'f') return (int)(c - 'a') + 10;
    if (c >= 'A' && c <= 'F') return (int)(c - 'A') + 10;
    return -1;
}

static bool bigint_parse_string(const char* str, int* sign_out, uint32_t** limbs_out, size_t* count_out) {
    if (!str || !sign_out || !limbs_out || !count_out) return false;

    const char* p = str;
    int sign = 1;
    if (*p == '-') {
        sign = -1;
        p++;
    } else if (*p == '+') {
        p++;
    }
    if (*p == '\0') return false;

    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
        if (*p == '\0') return false;

        for (const char* c = p; *c; c++) {
            if (bigint_hex_digit_value(*c) < 0) return false;
        }

        while (*p == '0' && p[1] != '\0') {
            p++;
        }

        if (*p == '0' && p[1] == '\0') {
            *sign_out = 0;
            *limbs_out = NULL;
            *count_out = 0;
            return true;
        }

        size_t digits = strlen(p);
        size_t limb_count = (digits + 7) / 8;
        uint32_t* limbs = (uint32_t*)safe_malloc(limb_count * sizeof(uint32_t));

        size_t pos = digits;
        size_t count = 0;
        while (pos > 0) {
            size_t start = pos >= 8 ? pos - 8 : 0;
            uint32_t limb = 0;
            for (size_t i = start; i < pos; i++) {
                limb = (limb << 4) | (uint32_t)bigint_hex_digit_value(p[i]);
            }
            limbs[count++] = limb;
            pos = start;
        }

        if (count == 0) {
            free(limbs);
            *sign_out = 0;
            *limbs_out = NULL;
            *count_out = 0;
            return true;
        }

        *sign_out = sign < 0 ? -1 : 1;
        *limbs_out = limbs;
        *count_out = count;
        return true;
    }

    for (const char* c = p; *c; c++) {
        if (!isdigit((unsigned char)*c)) return false;
    }

    while (*p == '0' && p[1] != '\0') {
        p++;
    }

    if (*p == '0' && p[1] == '\0') {
        *sign_out = 0;
        *limbs_out = NULL;
        *count_out = 0;
        return true;
    }

    size_t len = strlen(p);
    size_t first_digits = len % BIGINT_DEC_BASE_DIGITS;
    if (first_digits == 0) first_digits = BIGINT_DEC_BASE_DIGITS;

    uint32_t* limbs = NULL;
    size_t count = 0;

    size_t offset = 0;
    while (offset < len) {
        size_t chunk_len = offset == 0 ? first_digits : BIGINT_DEC_BASE_DIGITS;
        uint32_t chunk = 0;
        for (size_t i = 0; i < chunk_len; i++) {
            chunk = chunk * 10u + (uint32_t)(p[offset + i] - '0');
        }
        bigint_mul_add_small_inplace(&limbs, &count, BIGINT_DEC_BASE, chunk);
        offset += chunk_len;
    }

    if (count == 0) {
        *sign_out = 0;
        *limbs_out = NULL;
        *count_out = 0;
        return true;
    }

    *sign_out = sign < 0 ? -1 : 1;
    *limbs_out = limbs;
    *count_out = count;
    return true;
}

ObjBigInt* obj_bigint_from_string(const char* str) {
    int sign = 0;
    uint32_t* limbs = NULL;
    size_t count = 0;
    if (!bigint_parse_string(str, &sign, &limbs, &count)) {
        return bigint_from_limbs(0, NULL, 0);
    }
    return bigint_from_limbs(sign, limbs, count);
}

bool obj_bigint_try_from_string(const char* str, ObjBigInt** out) {
    if (!out) return false;
    *out = NULL;

    int sign = 0;
    uint32_t* limbs = NULL;
    size_t count = 0;
    if (!bigint_parse_string(str, &sign, &limbs, &count)) {
        return false;
    }

    *out = bigint_from_limbs(sign, limbs, count);
    return true;
}

ObjBigInt* obj_bigint_from_int64(int64_t value) {
    if (value == 0) {
        return bigint_from_limbs(0, NULL, 0);
    }

    uint64_t abs_val = value < 0 ? (uint64_t)(-(value + 1)) + 1 : (uint64_t)value;
    uint32_t limbs[2];
    size_t count = 0;
    while (abs_val > 0) {
        limbs[count++] = (uint32_t)abs_val;
        abs_val >>= BIGINT_LIMB_BITS;
    }

    uint32_t* out = (uint32_t*)safe_malloc(count * sizeof(uint32_t));
    memcpy(out, limbs, count * sizeof(uint32_t));
    return bigint_from_limbs(value < 0 ? -1 : 1, out, count);
}

void obj_bigint_retain(ObjBigInt* bigint) {
    if (bigint) bigint->ref_count++;
}

void obj_bigint_release(ObjBigInt* bigint) {
    if (!bigint) return;
    bigint->ref_count--;
    if (bigint->ref_count <= 0) {
        obj_bigint_free(bigint);
    }
}

void obj_bigint_free(ObjBigInt* bigint) {
    if (!bigint) return;
    if (bigint->limbs) free(bigint->limbs);
    free(bigint);
}

int obj_bigint_compare(const ObjBigInt* a, const ObjBigInt* b) {
    if (!a || !b) return 0;
    if (a->sign != b->sign) return (a->sign < b->sign) ? -1 : 1;
    if (a->sign == 0) return 0;
    int cmp = bigint_compare_abs_limbs(a->limbs, a->count, b->limbs, b->count);
    return a->sign < 0 ? -cmp : cmp;
}

ObjBigInt* obj_bigint_add(const ObjBigInt* a, const ObjBigInt* b) {
    if (!a || !b) return bigint_from_limbs(0, NULL, 0);
    if (a->sign == 0) return bigint_copy(b);
    if (b->sign == 0) return bigint_copy(a);

    if (a->sign == b->sign) {
        size_t out_count = 0;
        uint32_t* limbs = bigint_add_abs_limbs(a->limbs, a->count, b->limbs, b->count, &out_count);
        return bigint_from_limbs(a->sign, limbs, out_count);
    }

    int cmp = bigint_compare_abs_limbs(a->limbs, a->count, b->limbs, b->count);
    if (cmp == 0) {
        return bigint_from_limbs(0, NULL, 0);
    }

    if (cmp > 0) {
        size_t out_count = 0;
        uint32_t* limbs = bigint_sub_abs_limbs(a->limbs, a->count, b->limbs, b->count, &out_count);
        return bigint_from_limbs(a->sign, limbs, out_count);
    }

    size_t out_count = 0;
    uint32_t* limbs = bigint_sub_abs_limbs(b->limbs, b->count, a->limbs, a->count, &out_count);
    return bigint_from_limbs(b->sign, limbs, out_count);
}

ObjBigInt* obj_bigint_sub(const ObjBigInt* a, const ObjBigInt* b) {
    if (!a || !b) return bigint_from_limbs(0, NULL, 0);
    ObjBigInt neg_b = *b;
    neg_b.sign = -neg_b.sign;
    return obj_bigint_add(a, &neg_b);
}

ObjBigInt* obj_bigint_mul(const ObjBigInt* a, const ObjBigInt* b) {
    if (!a || !b) return bigint_from_limbs(0, NULL, 0);
    if (a->sign == 0 || b->sign == 0) return bigint_from_limbs(0, NULL, 0);

    if (a->count == 1) {
        size_t out_count = 0;
        uint32_t* limbs = bigint_mul_small(b->limbs, b->count, a->limbs[0], &out_count);
        int sign = a->sign * b->sign;
        return bigint_from_limbs(sign, limbs, out_count);
    }
    if (b->count == 1) {
        size_t out_count = 0;
        uint32_t* limbs = bigint_mul_small(a->limbs, a->count, b->limbs[0], &out_count);
        int sign = a->sign * b->sign;
        return bigint_from_limbs(sign, limbs, out_count);
    }

    size_t out_count = 0;
    uint32_t* limbs = bigint_mul_abs_limbs(a->limbs, a->count, b->limbs, b->count, &out_count);
    int sign = a->sign * b->sign;
    return bigint_from_limbs(sign, limbs, out_count);
}

bool obj_bigint_mul_small_inplace(ObjBigInt* a, uint32_t m) {
    if (!a) return false;
    if (a->sign == 0 || a->count == 0) return true;

    if (m == 0) {
        if (a->limbs) free(a->limbs);
        a->limbs = NULL;
        a->count = 0;
        a->sign = 0;
        return true;
    }

    if (m == 1) return true;

    bigint_mul_add_small_inplace(&a->limbs, &a->count, m, 0);
    if (a->count == 0) {
        a->sign = 0;
    }
    return true;
}

ObjBigInt* obj_bigint_div(const ObjBigInt* a, const ObjBigInt* b, bool* div_by_zero) {
    if (!a || !b) return bigint_from_limbs(0, NULL, 0);
    if (div_by_zero) *div_by_zero = false;
    if (b->sign == 0) {
        if (div_by_zero) *div_by_zero = true;
        return bigint_from_limbs(0, NULL, 0);
    }
    if (a->sign == 0) return bigint_from_limbs(0, NULL, 0);

    uint32_t* q_limbs = NULL;
    size_t q_count = 0;
    bigint_divmod_abs_limbs(a->limbs, a->count, b->limbs, b->count,
                            &q_limbs, &q_count, NULL, NULL, div_by_zero);

    int sign = (q_count == 0) ? 0 : a->sign * b->sign;
    return bigint_from_limbs(sign, q_limbs, q_count);
}

ObjBigInt* obj_bigint_mod(const ObjBigInt* a, const ObjBigInt* b, bool* div_by_zero) {
    if (!a || !b) return bigint_from_limbs(0, NULL, 0);
    if (div_by_zero) *div_by_zero = false;
    if (b->sign == 0) {
        if (div_by_zero) *div_by_zero = true;
        return bigint_from_limbs(0, NULL, 0);
    }
    if (a->sign == 0) return bigint_from_limbs(0, NULL, 0);

    uint32_t* r_limbs = NULL;
    size_t r_count = 0;
    bigint_divmod_abs_limbs(a->limbs, a->count, b->limbs, b->count,
                            NULL, NULL, &r_limbs, &r_count, div_by_zero);

    int sign = (r_count == 0) ? 0 : a->sign;
    return bigint_from_limbs(sign, r_limbs, r_count);
}

ObjBigInt* obj_bigint_negate(const ObjBigInt* a) {
    if (!a) return bigint_from_limbs(0, NULL, 0);
    if (a->sign == 0) return bigint_copy(a);
    uint32_t* limbs = (uint32_t*)safe_malloc(a->count * sizeof(uint32_t));
    memcpy(limbs, a->limbs, a->count * sizeof(uint32_t));
    return bigint_from_limbs(-a->sign, limbs, a->count);
}

static uint32_t bigint_abs_limb_at(const ObjBigInt* bi, size_t index) {
    if (!bi || bi->count == 0 || !bi->limbs) return 0;
    return index < bi->count ? bi->limbs[index] : 0;
}

static uint32_t bigint_twos_complement_limb_at(const ObjBigInt* bi, size_t index, uint64_t* carry_inout) {
    uint32_t limb = bigint_abs_limb_at(bi, index);
    if (!bi || bi->sign >= 0) return limb;

    uint64_t sum = (uint64_t)(~limb) + (carry_inout ? *carry_inout : 0);
    if (carry_inout) {
        *carry_inout = sum >> 32;
    }
    return (uint32_t)sum;
}

static ObjBigInt* bigint_from_twos_complement_limbs(uint32_t* limbs, size_t count) {
    if (!limbs || count == 0) {
        return bigint_from_limbs(0, NULL, 0);
    }

    bool negative = (limbs[count - 1] & 0x80000000u) != 0u;
    if (!negative) {
        return bigint_from_limbs(1, limbs, count);
    }

    uint64_t carry = 1;
    for (size_t i = 0; i < count; i++) {
        uint64_t sum = (uint64_t)(~limbs[i]) + carry;
        limbs[i] = (uint32_t)sum;
        carry = sum >> 32;
    }

    return bigint_from_limbs(-1, limbs, count);
}

typedef enum {
    BIGINT_BITOP_AND,
    BIGINT_BITOP_OR,
    BIGINT_BITOP_XOR
} BigIntBitOp;

static ObjBigInt* bigint_bitwise_binary_op(const ObjBigInt* a, const ObjBigInt* b, BigIntBitOp op) {
    size_t a_count = a ? a->count : 0;
    size_t b_count = b ? b->count : 0;
    size_t out_count = (a_count > b_count ? a_count : b_count) + 1;

    uint32_t* out = (uint32_t*)safe_calloc(out_count, sizeof(uint32_t));

    uint64_t carry_a = (a && a->sign < 0) ? 1 : 0;
    uint64_t carry_b = (b && b->sign < 0) ? 1 : 0;

    for (size_t i = 0; i < out_count; i++) {
        uint32_t la = bigint_twos_complement_limb_at(a, i, &carry_a);
        uint32_t lb = bigint_twos_complement_limb_at(b, i, &carry_b);

        switch (op) {
            case BIGINT_BITOP_AND:
                out[i] = la & lb;
                break;
            case BIGINT_BITOP_OR:
                out[i] = la | lb;
                break;
            case BIGINT_BITOP_XOR:
                out[i] = la ^ lb;
                break;
            default:
                out[i] = 0;
                break;
        }
    }

    return bigint_from_twos_complement_limbs(out, out_count);
}

ObjBigInt* obj_bigint_bit_and(const ObjBigInt* a, const ObjBigInt* b) {
    return bigint_bitwise_binary_op(a, b, BIGINT_BITOP_AND);
}

ObjBigInt* obj_bigint_bit_or(const ObjBigInt* a, const ObjBigInt* b) {
    return bigint_bitwise_binary_op(a, b, BIGINT_BITOP_OR);
}

ObjBigInt* obj_bigint_bit_xor(const ObjBigInt* a, const ObjBigInt* b) {
    return bigint_bitwise_binary_op(a, b, BIGINT_BITOP_XOR);
}

ObjBigInt* obj_bigint_bit_not(const ObjBigInt* a) {
    size_t a_count = a ? a->count : 0;
    size_t out_count = a_count + 1;
    uint32_t* out = (uint32_t*)safe_calloc(out_count, sizeof(uint32_t));

    uint64_t carry = (a && a->sign < 0) ? 1 : 0;
    for (size_t i = 0; i < out_count; i++) {
        uint32_t limb_tc = bigint_twos_complement_limb_at(a, i, &carry);
        out[i] = ~limb_tc;
    }

    return bigint_from_twos_complement_limbs(out, out_count);
}

char* obj_bigint_to_string(const ObjBigInt* bigint) {
    if (!bigint || bigint->count == 0) return safe_strdup("0");

    size_t work_count = bigint->count;
    uint32_t* work = (uint32_t*)safe_malloc(work_count * sizeof(uint32_t));
    memcpy(work, bigint->limbs, work_count * sizeof(uint32_t));

    size_t chunk_cap = work_count * 2 + 1;
    uint32_t* chunks = (uint32_t*)safe_malloc(chunk_cap * sizeof(uint32_t));
    size_t chunk_count = 0;

    while (work_count > 0) {
        uint32_t rem = 0;
        work_count = bigint_divmod_small_inplace(work, work_count, BIGINT_DEC_BASE, &rem);
        if (chunk_count == chunk_cap) {
            chunk_cap *= 2;
            chunks = (uint32_t*)safe_realloc(chunks, chunk_cap * sizeof(uint32_t));
        }
        chunks[chunk_count++] = rem;
    }

    free(work);

    if (chunk_count == 0) {
        free(chunks);
        return safe_strdup("0");
    }

    uint32_t ms_chunk = chunks[chunk_count - 1];
    int ms_digits = bigint_digits_u32(ms_chunk);
    size_t digits = (chunk_count - 1) * BIGINT_DEC_BASE_DIGITS + (size_t)ms_digits;
    size_t len = digits + (bigint->sign < 0 ? 1 : 0);

    char* out = (char*)safe_malloc(len + 1);
    char* p = out;

    if (bigint->sign < 0) {
        *p++ = '-';
    }

    snprintf(p, len - (size_t)(p - out) + 1, "%u", ms_chunk);
    p += ms_digits;

    for (size_t i = chunk_count - 1; i > 0; i--) {
        snprintf(p, len - (size_t)(p - out) + 1, "%09u", chunks[i - 1]);
        p += BIGINT_DEC_BASE_DIGITS;
    }

    out[len] = '\0';
    free(chunks);
    return out;
}

char* obj_bigint_to_hex_string(const ObjBigInt* bigint) {
    static const char hex_digits[] = "0123456789abcdef";
    if (!bigint || bigint->count == 0 || bigint->sign == 0) return safe_strdup("0x0");

    uint32_t ms = bigint->limbs[bigint->count - 1];
    int ms_nibbles = 8;
    while (ms_nibbles > 1) {
        uint32_t nibble = (ms >> ((ms_nibbles - 1) * 4)) & 0xFu;
        if (nibble != 0) break;
        ms_nibbles--;
    }

    size_t digit_count = (bigint->count - 1) * 8 + (size_t)ms_nibbles;
    size_t len = digit_count + 2 + (bigint->sign < 0 ? 1 : 0);

    char* out = (char*)safe_malloc(len + 1);
    size_t pos = 0;
    if (bigint->sign < 0) {
        out[pos++] = '-';
    }
    out[pos++] = '0';
    out[pos++] = 'x';

    for (int i = ms_nibbles - 1; i >= 0; i--) {
        out[pos++] = hex_digits[(ms >> (i * 4)) & 0xFu];
    }

    for (size_t li = bigint->count - 1; li > 0; li--) {
        uint32_t limb = bigint->limbs[li - 1];
        out[pos++] = hex_digits[(limb >> 28) & 0xFu];
        out[pos++] = hex_digits[(limb >> 24) & 0xFu];
        out[pos++] = hex_digits[(limb >> 20) & 0xFu];
        out[pos++] = hex_digits[(limb >> 16) & 0xFu];
        out[pos++] = hex_digits[(limb >> 12) & 0xFu];
        out[pos++] = hex_digits[(limb >> 8) & 0xFu];
        out[pos++] = hex_digits[(limb >> 4) & 0xFu];
        out[pos++] = hex_digits[limb & 0xFu];
    }

    out[len] = '\0';
    return out;
}

bool obj_bigint_to_int64(const ObjBigInt* bigint, int64_t* out) {
    if (!bigint || !out) return false;
    if (bigint->sign == 0) {
        *out = 0;
        return true;
    }

    uint64_t limit = bigint->sign < 0 ? (uint64_t)INT64_MAX + 1 : (uint64_t)INT64_MAX;
    uint64_t acc = 0;
    for (size_t i = bigint->count; i > 0; i--) {
        uint64_t limb = bigint->limbs[i - 1];
        if (acc > ((limit - limb) >> BIGINT_LIMB_BITS)) {
            return false;
        }
        acc = (acc << BIGINT_LIMB_BITS) | limb;
    }

    if (bigint->sign < 0) {
        if (acc == limit) {
            *out = INT64_MIN;
        } else {
            *out = -(int64_t)acc;
        }
    } else {
        *out = (int64_t)acc;
    }
    return true;
}

double obj_bigint_to_double(const ObjBigInt* bigint) {
    if (!bigint || bigint->count == 0) return 0.0;
    if (bigint->sign == 0) return 0.0;

    const double limb_base = (double)(UINT64_C(1) << BIGINT_LIMB_BITS);
    double acc = 0.0;
    for (size_t i = bigint->count; i > 0; i--) {
        acc = acc * limb_base + (double)bigint->limbs[i - 1];
        if (isinf(acc)) {
            return bigint->sign < 0 ? -INFINITY : INFINITY;
        }
    }

    return bigint->sign < 0 ? -acc : acc;
}
