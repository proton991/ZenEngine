"""Original positive-binary32 sum cases, with an exact rational oracle."""
from fractions import Fraction
from pathlib import Path
import json
import random
import struct

WIDTH = 4096


def rational(bits):
    return Fraction.from_float(struct.unpack('<f', struct.pack('<I', bits))[0])


def rounded(total):
    # Binary search the representable values and compare exact rational distances.
    if total >= Fraction(2**128 - 2**103):
        return 0x7f800000
    low, high = 0, 0x7f7fffff
    while low < high:
        middle = (low + high + 1) // 2
        if rational(middle) <= total:
            low = middle
        else:
            high = middle - 1
    if low == 0x7f7fffff or rational(low) == total:
        return low
    below, above = total - rational(low), rational(low + 1) - total
    return low if below < above or (below == above and low % 2 == 0) else low + 1


def main():
    cases = []

    def add(name, values):
        assert len(values) <= WIDTH
        values = list(values) + [0] * (WIDTH - len(values))
        finite = []
        flags = 0
        for bits in values:
            magnitude = bits & 0x7fffffff
            if magnitude:
                if bits >> 31 or magnitude > 0x7f800000:
                    flags |= 0x80000000
                elif magnitude == 0x7f800000:
                    flags |= 0x40000000
                else:
                    finite.append(rational(bits))
        total = sum(finite, Fraction())
        integer = total * 2**149
        assert integer.denominator == 1
        words = [(integer.numerator >> (32 * i)) & 0xffffffff for i in range(10)]
        assert words[-1] < 0x40000000
        words[-1] |= flags
        result = 0x7fc00000 if flags & 0x80000000 else 0x7f800000 if flags else rounded(total)
        cases.append((name, words + [result], values))

    add('zero-and-negative-zero', [0, 0x80000000] * (WIDTH // 2))
    add('ones', [0x3f800000] * WIDTH)
    add('minimum-subnormals', [1] * WIDTH)
    add('subnormal-to-normal', [0x7fffff, 1])
    for exponent in range(1, 255, 4):
        base = exponent << 23
        ulp = rational(base + 1) - rational(base)
        half = rounded(ulp / 2)
        add(f'tie-even-e{exponent}', [base, half])
        add(f'tie-odd-e{exponent}', [base + 1, half])
        add(f'sticky-e{exponent}', [base, half, 1])
    for highest in [31, 63, 95, 127, 149, 159, 191, 223, 255, 276]:
        powers = [rounded(Fraction(2)**(bit-149)) for bit in range(highest+1)]
        add(f'carry-through-bit-{highest}', powers + [1])
    add('largest-finite', [0x7f7fffff])
    add('overflow-rounding-boundary', [0x7f7fffff, rounded(Fraction(2)**103)])
    add('overflow-sum', [0x7f7fffff] * WIDTH)
    add('positive-infinity', [0x7f800000, 0x3f800000])
    add('nan-dominates-infinity', [0x7f800000, 0x7fc12345])
    add('invalid-negative', [0xbf800000, 0x3f800000])
    rng = random.Random(20261024)
    for index in range(64):
        exponents = range(1, 240) if index % 2 else range(100, 107)
        add(f'random-{index}', [(rng.choice(exponents) << 23) | rng.getrandbits(23) for _ in range(WIDTH)])

    path = Path(__file__).with_name('guide_sum_cases.bin')
    header = struct.pack('<4I', 0x3153475a, len(cases), WIDTH, 11)
    payload = b''.join(struct.pack('<' + 'I' * len(expected), *expected) for _, expected, _ in cases)
    payload += b''.join(struct.pack('<' + 'I' * WIDTH, *values) for _, _, values in cases)
    path.write_bytes(header + payload)
    path.with_suffix('.json').write_text(json.dumps({'oracle': 'Exact Fraction sum; nearest-even via rational binary search',
        'samples_per_case': WIDTH, 'cases': [name for name, _, _ in cases]}, indent=2) + '\n', encoding='utf-8')
    print(f'{len(cases)} cases, {len(header + payload)} bytes')


if __name__ == '__main__':
    main()
