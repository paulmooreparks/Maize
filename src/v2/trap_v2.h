// trap_v2.h (maize-418): the trap record this build surfaces to its host.
//
// SCAFFOLD BOUNDARY, and it must never be mistaken for conformant trap delivery. The real
// v2 trap machinery (the four-word frame, the vector table, the trap-stack and status control
// and status registers, trap_return) is maize-420 and none of it exists here. What exists is a
// value: step() either advances, or hands back the cause, subcode, auxiliary word and faulting
// instruction address that trap-model.md's cause table fixes for the condition, and the
// machine stops advancing. Nothing is pushed and nothing is vectored, so the only consumer of
// a trap in this build is the host that called step().
//
// The cause numbers, the subcode numbers and the auxiliary-word contents ARE the specified
// ones, because those are what maize-420 will deliver through the real frame and what a
// conformance binary reads. Only the delivery is scaffolding.

#ifndef MAIZE_V2_TRAP_V2_H
#define MAIZE_V2_TRAP_V2_H

#include <cstdint>

namespace maize::v2 {

// trap-model.md, "The cause enumeration". Only the causes this build can raise are named.
// Causes 8, 9 and 10 (the page faults) are unreachable here: this interpreter runs bare mode,
// where translation is not performed at all, so an access outside populated physical memory
// raises cause 11 and nothing raises a page fault (boot.md, D-3).
namespace cause {
inline constexpr std::uint8_t kIllegalInstruction = 0;
inline constexpr std::uint8_t kIllegalOperand = 1;
inline constexpr std::uint8_t kDivideError = 2;
inline constexpr std::uint8_t kBreakpoint = 3;
inline constexpr std::uint8_t kPrivilegedOperation = 4;
inline constexpr std::uint8_t kSyscall = 7;
inline constexpr std::uint8_t kPageFaultFetch = 8;
inline constexpr std::uint8_t kPageFaultLoad = 9;
inline constexpr std::uint8_t kPageFaultStore = 10;
inline constexpr std::uint8_t kPhysicalMemoryFault = 11;
}  // namespace cause

// trap-model.md, "Subcodes". Every cause not listed there writes a subcode of zero.
namespace subcode {
inline constexpr std::uint8_t kOperandForm = 0;         // cause 1: undefined operand form field
inline constexpr std::uint8_t kInvalidImmediate = 1;    // cause 1: an immediate the entry rejects
inline constexpr std::uint8_t kReservedRoundingMode = 2;  // cause 1: a reserved frm encoding
inline constexpr std::uint8_t kUnimplementedCsr = 3;    // cause 1: a well-formed number with no register
inline constexpr std::uint8_t kReadOnlyCsr = 4;         // cause 1: a write to a read-only number
inline constexpr std::uint8_t kBlockMemoryOperands = 5;  // cause 1: aliased slot or r0 in one
inline constexpr std::uint8_t kInvalidCsrValue = 6;     // cause 1: a value the register rejects
inline constexpr std::uint8_t kReservedCsrPrivilege = 7;  // cause 1: a reserved privilege field
inline constexpr std::uint8_t kDivideByZero = 0;        // cause 2
inline constexpr std::uint8_t kQuotientOverflow = 1;    // cause 2
}  // namespace subcode

struct TrapV2 {
    std::uint8_t cause = 0;
    std::uint8_t subcode = 0;
    std::uint64_t aux = 0;  // the per-cause auxiliary word of trap-model.md's cause table
    // The address of the faulting instruction's FIRST byte, never an operand byte, never an
    // immediate, and never the address of the following instruction. A faulting instruction is
    // restartable and has to re-decode identically (instruction-encoding.md, step 6).
    std::uint64_t pc = 0;
};

}  // namespace maize::v2

#endif  // MAIZE_V2_TRAP_V2_H
