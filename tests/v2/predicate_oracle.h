// predicate_oracle.h (maize-418): the ten compare and branch predicates, pinned independently.
//
// WHY THIS FILE EXISTS. The first version of the predicate fixtures computed their expected
// value by calling evaluate_predicate, which is the production function the interpreter
// dispatches to for all thirty predicate opcodes. An expectation computed by calling the thing
// under test moves with it, so those assertions held in both worlds: permuting the case arms in
// interpreter_v2.cpp left every operand pair green, and AC-9's claim that a branch agrees with
// the compare of the same predicate was true by construction rather than by test.
//
// The oracle here is pinned to the specification instead, in two independent layers, and
// neither layer calls anything the interpreter uses.
//
// LAYER 1, the pin table. Nine operand pairs, each carrying ten hand-written answers in the
// predicate order appendix A.5 assigns. The answers were worked out by hand from
// instruction-reference-integer.md's Operation lines, not computed. Read as a column per
// predicate, the nine answers give each predicate a distinct nine-bit signature, so ANY
// permutation of the ten predicates is caught rather than only the ones that happen to differ
// on a value somebody thought to test. Four of the pairs are chosen where the signed and the
// unsigned answers diverge, which is where a mis-mapped arm hides.
//
// LAYER 2, a second transcription. oracle_predicate below spells the ten relations out again,
// independently of interpreter_v2.cpp, for the exhaustive value sweeps. This is the same device
// fixtures_decode.cpp uses on the length table and it exists for the same reason: two
// transcriptions of one appendix that agree everywhere is evidence, and one transcription
// checked against itself is not.

#ifndef MAIZE_V2_TESTS_PREDICATE_ORACLE_H
#define MAIZE_V2_TESTS_PREDICATE_ORACLE_H

#include <cstdint>

namespace maize::v2::test {

inline constexpr unsigned kPredicateCount = 10;

// The order of appendix A.5, which the register compares ($40..$49), the immediate compares
// ($4A..$53) and the branches ($60..$69) all share.
inline constexpr const char* kPredicateNames[kPredicateCount] = {
    "eq", "ne", "lt_signed", "le_signed", "gt_signed",
    "ge_signed", "lt_unsigned", "le_unsigned", "gt_unsigned", "ge_unsigned",
};

struct PredicatePin {
    std::uint64_t left;
    std::uint64_t right;
    // Ten characters, '1' or '0', in the predicate order above. Hand-written.
    const char* answers;
    // True when the right operand is the sign-extension of a 32-bit literal, so the immediate
    // compare form can present this same pair. The pairs that turn on the most negative word or
    // the largest positive word cannot be reached from a 32-bit literal and say so here.
    bool reachable_by_immediate;
    // The 32-bit literal that sign-extends to `right`, meaningful only when reachable.
    std::uint32_t immediate;
};

//                                        eq ne lts les gts ges ltu leu gtu geu
inline constexpr PredicatePin kPredicatePins[] = {
    // Equal at zero. Every "or equal" predicate holds and no strict one does.
    {0x0000000000000000ull, 0x0000000000000000ull, "1001010101", true, 0x00000000u},
    // 0 below 1, and the two signednesses agree.
    {0x0000000000000000ull, 0x0000000000000001ull, "0111001100", true, 0x00000001u},
    // 1 above 0, the mirror of the pair above.
    {0x0000000000000001ull, 0x0000000000000000ull, "0100110011", true, 0x00000000u},
    // DIVERGENT. All-ones is -1 signed, so it is below 1; it is the largest unsigned value, so
    // it is above 1. This one pair separates every signed predicate from its unsigned twin.
    {0xFFFFFFFFFFFFFFFFull, 0x0000000000000001ull, "0111000011", true, 0x00000001u},
    // DIVERGENT. The most negative word against the largest positive one: below signed, above
    // unsigned.
    {0x8000000000000000ull, 0x7FFFFFFFFFFFFFFFull, "0111000011", false, 0x00000000u},
    // DIVERGENT. The mirror of the pair above.
    {0x7FFFFFFFFFFFFFFFull, 0x8000000000000000ull, "0100111100", false, 0x00000000u},
    // Equal at the top of the range, so equality is checked somewhere other than zero.
    {0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull, "1001010101", true, 0xFFFFFFFFu},
    // The most negative word against -1: below on both readings, so it does NOT diverge, which
    // is what keeps a divergent pair from being the only evidence either way.
    {0x8000000000000000ull, 0xFFFFFFFFFFFFFFFFull, "0111001100", true, 0xFFFFFFFFu},
    // DIVERGENT. 1 against -1: above signed, below unsigned.
    {0x0000000000000001ull, 0xFFFFFFFFFFFFFFFFull, "0100111100", true, 0xFFFFFFFFu},
};

inline constexpr unsigned kPredicatePinCount =
    sizeof(kPredicatePins) / sizeof(kPredicatePins[0]);

inline bool pinned_answer(const PredicatePin& pin, unsigned predicate) {
    return pin.answers[predicate] == '1';
}

// The second transcription. Written from instruction-reference-integer.md's ten Operation
// lines, deliberately without consulting the interpreter's own dispatch.
inline bool oracle_predicate(unsigned predicate, std::uint64_t left, std::uint64_t right) {
    const std::int64_t signed_left = static_cast<std::int64_t>(left);
    const std::int64_t signed_right = static_cast<std::int64_t>(right);
    switch (predicate) {
        case 0: return left == right;
        case 1: return !(left == right);
        case 2: return signed_left < signed_right;
        case 3: return signed_left < signed_right || left == right;
        case 4: return signed_right < signed_left;
        case 5: return signed_right < signed_left || left == right;
        case 6: return left < right;
        case 7: return left < right || left == right;
        case 8: return right < left;
        case 9: return right < left || left == right;
        default: return false;
    }
}

}  // namespace maize::v2::test

#endif  // MAIZE_V2_TESTS_PREDICATE_ORACLE_H
