#ifndef ZEN_GUIDE_SUM
#define ZEN_GUIDE_SUM
// Exact sum of fewer than 2^32 nonnegative finite binary32 samples. Ten words
// cover all significand bits in units of 2^-149, plus the maximum carry. The two
// high bits are reserved for invalid inputs and positive infinity. No float64
// or 64-bit atomics are required. Read only after a workgroup barrier.
const uint GUIDE_SUM_WORDS = 10u;
#ifndef GUIDE_SUM_STORAGE_WORDS
#    define GUIDE_SUM_STORAGE_WORDS GUIDE_SUM_WORDS
#endif
shared uint guideSumWords[GUIDE_SUM_BINS * GUIDE_SUM_STORAGE_WORDS];

void GuideSumClear(uint bin)
{
    for (uint word = 0u; word < GUIDE_SUM_WORDS; ++word)
    {
        guideSumWords[bin * GUIDE_SUM_WORDS + word] = 0u;
    }
}

void GuideSumAddWord(uint base, uint word, uint value)
{
    while (value != 0u && word < GUIDE_SUM_WORDS)
    {
        uint previous = atomicAdd(guideSumWords[base + word], value);
        value         = previous > 0xffffffffu - value ? 1u : 0u;
        ++word;
    }
}

void GuideSumAdd(uint bin, float value)
{
    uint bits      = floatBitsToUint(value);
    uint magnitude = bits & 0x7fffffffu;
    uint base      = bin * GUIDE_SUM_WORDS;
    if (magnitude != 0u)
    {
        uint exponent    = magnitude >> 23u;
        uint significand = magnitude & 0x7fffffu;
        if ((bits & 0x80000000u) != 0u || (exponent == 255u && significand != 0u))
        {
            atomicOr(guideSumWords[base + 9u], 0x80000000u);
        }
        else if (exponent == 255u)
        {
            atomicOr(guideSumWords[base + 9u], 0x40000000u);
        }
        else
        {
            uint shift   = exponent == 0u ? 0u : exponent - 1u;
            significand |= exponent == 0u ? 0u : 0x800000u;
            uint word    = shift / 32u;
            uint bit     = shift % 32u;
            GuideSumAddWord(base, word, significand << bit);
            if (bit != 0u)
            {
                GuideSumAddWord(base, word + 1u, significand >> (32u - bit));
            }
        }
    }
}

float GuideSumValue(uint bin)
{
    uint base   = bin * GUIDE_SUM_WORDS;
    uint flags  = guideSumWords[base + 9u];
    uint result = 0u;
    if ((flags & 0x80000000u) != 0u)
    {
        result = 0x7fc00000u;
    }
    else if ((flags & 0x40000000u) != 0u)
    {
        result = 0x7f800000u;
    }
    else
    {
        int highest = -1;
        for (uint word = 0u; word < GUIDE_SUM_WORDS; ++word)
        {
            uint value = guideSumWords[base + word];
            if (value != 0u)
            {
                highest = int(word * 32u) + findMSB(value);
            }
        }
        if (highest < 23)
        {
            result = guideSumWords[base];
        }
        else
        {
            uint shift       = uint(highest - 23);
            uint word        = shift / 32u;
            uint bit         = shift % 32u;
            uint significand = guideSumWords[base + word] >> bit;
            if (bit != 0u && word + 1u < GUIDE_SUM_WORDS)
            {
                significand |= guideSumWords[base + word + 1u] << (32u - bit);
            }
            if (shift > 0u)
            {
                uint roundWord = (shift - 1u) / 32u;
                uint roundBit  = (shift - 1u) % 32u;
                uint lower     = guideSumWords[base + roundWord] & ((1u << roundBit) - 1u);
                for (uint index = 0u; index < roundWord; ++index)
                {
                    lower |= guideSumWords[base + index];
                }
                bool guard = (guideSumWords[base + roundWord] & (1u << roundBit)) != 0u;
                if (guard && (lower != 0u || (significand & 1u) != 0u))
                {
                    ++significand;
                    if (significand == 0x1000000u)
                    {
                        significand >>= 1u;
                        ++highest;
                    }
                }
            }
            uint exponent = uint(highest - 22);
            result        = exponent >= 255u ? 0x7f800000u : (exponent << 23u) | (significand & 0x7fffffu);
        }
    }
    return uintBitsToFloat(result);
}
#endif
