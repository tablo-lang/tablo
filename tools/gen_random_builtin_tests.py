from pathlib import Path
import math

MASK64 = (1 << 64) - 1

def splitmix64(x):
    x = (x + 0x9E3779B97F4A7C15) & MASK64
    z = x
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9 & MASK64
    z = (z ^ (z >> 27)) * 0x94D049BB133111EB & MASK64
    return (z ^ (z >> 31)) & MASK64, x

def sfc64_init(seed):
    a, seed = splitmix64(seed)
    b, seed = splitmix64(seed)
    c, seed = splitmix64(seed)
    d = 1
    ctx = [a, b, c, d]
    for _ in range(12):
        sfc64_next(ctx)
    return ctx

def rotl64(x, k):
    return ((x << k) & MASK64) | (x >> (64 - k))

def sfc64_next(ctx):
    a, b, c, d = ctx
    tmp = (a + b + d) & MASK64
    d = (d + 1) & MASK64
    a = (b ^ (b >> 11)) & MASK64
    b = (c + ((c << 3) & MASK64)) & MASK64
    c = (rotl64(c, 24) + tmp) & MASK64
    ctx[0], ctx[1], ctx[2], ctx[3] = a, b, c, d
    return tmp

def sfc64_double(ctx):
    return ((sfc64_next(ctx) >> 11) * (1.0 / 9007199254740992.0))

def rng_fill_bytes(ctx, n):
    out = bytearray(n)
    offset = 0
    while offset < n:
        r = sfc64_next(ctx)
        chunk = min(8, n - offset)
        for i in range(chunk):
            out[offset + i] = (r >> (8 * (7 - i))) & 0xFF
        offset += chunk
    return out

def bigint_bit_length_upper(value):
    if value == 0:
        return 0
    base = 1_000_000_000
    limbs = []
    v = value
    while v > 0:
        limbs.append(v % base)
        v //= base
    top = limbs[-1]
    bits = top.bit_length()
    total = (len(limbs) - 1) * 30 + bits
    if total > 0:
        total += 1
    return total

def random_bigint_bits(ctx, bits):
    if bits <= 0:
        return 0
    byte_len = (bits + 7) // 8
    data = rng_fill_bytes(ctx, byte_len)
    extra = byte_len * 8 - bits
    if extra > 0:
        data[0] &= (0xFF >> extra)
    return int.from_bytes(data, 'big')

def random_bigint_range(ctx, min_val, max_val):
    if min_val > max_val:
        min_val, max_val = max_val, min_val
    if min_val == max_val:
        return min_val
    span = max_val - min_val + 1
    bits = bigint_bit_length_upper(span)
    while True:
        cand = random_bigint_bits(ctx, bits)
        if cand < span:
            return min_val + cand

def random_u64_range(ctx, span):
    if span == 0:
        return sfc64_next(ctx)
    limit = MASK64 - (MASK64 % span)
    while True:
        r = sfc64_next(ctx)
        if r < limit:
            return r % span

def to_int64(u):
    if u >= (1 << 63):
        u -= (1 << 64)
    return u

def random_int_range(ctx, min_val, max_val):
    if max_val < min_val:
        min_val, max_val = max_val, min_val
    span = (max_val - min_val + 1) & MASK64
    if span == 0:
        return to_int64(sfc64_next(ctx))
    r = random_u64_range(ctx, span)
    return to_int64(r) + min_val

def fmt_double(x):
    if math.isnan(x):
        return "nan"
    if math.isinf(x):
        return "-inf" if x < 0 else "inf"
    if x == int(x):
        return f"{x:.1f}"
    return f"{x:g}"

seed = 123456789
ctx = sfc64_init(seed)

lines = []
lines.append(fmt_double(sfc64_double(ctx)))
lines.append(fmt_double(sfc64_double(ctx)))
lines.append(str(random_int_range(ctx, -5, 5)))
lines.append(str(random_int_range(ctx, 10, 20)))
lines.append(fmt_double(1.0 + (2.0 - 1.0) * sfc64_double(ctx)))
lines.append(str(random_bigint_bits(ctx, 16)))
lines.append(str(random_bigint_range(ctx, 1000, 5000)))

# randomFillInt
arr = [0, 0, 0, 0]
for i in range(len(arr)):
    arr[i] = random_int_range(ctx, -3, 3)
lines.append("[" + ", ".join(str(x) for x in arr) + "]")

# randomFillDouble
arr_d = [0.0, 0.0, 0.0]
for i in range(len(arr_d)):
    arr_d[i] = 1.0 + (2.0 - 1.0) * sfc64_double(ctx)
lines.append("[" + ", ".join(fmt_double(x) for x in arr_d) + "]")

# randomFillBigIntBits
arr_b = [0, 0]
for i in range(len(arr_b)):
    arr_b[i] = random_bigint_bits(ctx, 12)
lines.append("[" + ", ".join(str(x) for x in arr_b) + "]")

# randomFillBigIntRange
arr_br = [0, 0, 0]
for i in range(len(arr_br)):
    arr_br[i] = random_bigint_range(ctx, 100, 200)
lines.append("[" + ", ".join(str(x) for x in arr_br) + "]")

expected = "\n".join(lines) + "\n1\n"

tblo_source = """func main(): void {
    randomSeed(123456789);
    println(random());
    println(random());
    println(randomInt(-5, 5));
    println(randomInt(10, 20));
    println(randomDouble(1.0, 2.0));
    println(randomBigIntBits(16));
    println(randomBigIntRange(1000n, 5000n));

    var iarr: array<int> = [0, 0, 0, 0];
    randomFillInt(iarr, -3, 3);
    println(iarr);

    var darr: array<double> = [0.0, 0.0, 0.0];
    randomFillDouble(darr, 1.0, 2.0);
    println(darr);

    var barr: array<bigint> = [0n, 0n];
    randomFillBigIntBits(barr, 12);
    println(barr);

    var brarr: array<bigint> = [0n, 0n, 0n];
    randomFillBigIntRange(brarr, 100n, 200n);
    println(brarr);

    var ok: int = 1;
    var s: double = secureRandom();
    if (s < 0.0 || s >= 1.0) { ok = 0; }
    var si: int = secureRandomInt(-10, 10);
    if (si < -10 || si > 10) { ok = 0; }
    var sd: double = secureRandomDouble(5.0, 6.0);
    if (sd < 5.0 || sd > 6.0) { ok = 0; }
    var sb: bigint = secureRandomBigIntBits(8);
    if (sb < 0n || sb > 255n) { ok = 0; }
    var sbr: bigint = secureRandomBigIntRange(100n, 150n);
    if (sbr < 100n || sbr > 150n) { ok = 0; }

    var iarr2: array<int> = [0, 0, 0];
    secureRandomFillInt(iarr2, 1, 3);
    foreach (x in iarr2) {
        if (x < 1 || x > 3) { ok = 0; }
    }

    var darr2: array<double> = [0.0, 0.0];
    secureRandomFillDouble(darr2, -1.0, 1.0);
    foreach (x in darr2) {
        if (x < -1.0 || x > 1.0) { ok = 0; }
    }

    var barr2: array<bigint> = [0n, 0n];
    secureRandomFillBigIntBits(barr2, 10);
    foreach (x in barr2) {
        if (x < 0n || x > 1023n) { ok = 0; }
    }

    var brarr2: array<bigint> = [0n, 0n];
    secureRandomFillBigIntRange(brarr2, 10n, 20n);
    foreach (x in brarr2) {
        if (x < 10n || x > 20n) { ok = 0; }
    }

    println(ok);
}
"""

Path("tests/native_integration_tests/random_builtin_tests.tblo").write_text(tblo_source, encoding="ascii")
Path("tests/native_integration_tests/expected_output/random_builtin_tests.expected").write_text(expected, encoding="ascii")
print("wrote random_builtin_tests")
