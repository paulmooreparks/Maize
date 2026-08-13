// trap_v2.h (maize-418, delivery on maize-464): the trap record and the shapes trap-model.md
// fixes around it.
//
// The record is what a condition produces: a cause, a subcode, an auxiliary word, and the
// captured program counter. Delivery is what the machine then does with it, and since maize-464
// that is the real thing rather than a report to the host: the vector fetch, the four-word
// frame, the privilege change, the jump to the handler, and the two halts that replace delivery
// when the machine cannot reach a handler. interpreter_v2.cpp owns that sequence.
//
// The captured program counter in this record is already class-resolved. A fault carries the
// faulting instruction's own address and a trap in the narrow sense carries the address of the
// following instruction, so nothing downstream of the raise site has to know which class the
// cause belongs to (trap-model.md, "Fault, trap, and interrupt").

#ifndef MAIZE_V2_TRAP_V2_H
#define MAIZE_V2_TRAP_V2_H

#include <cstdint>

namespace maize::v2 {

// trap-model.md, "The cause enumeration". Only the causes this build can raise are named.
// Causes 8, 9 and 10 (the page faults) became reachable on maize-465, which brought the Sv48
// translation whose six rejections raise them; in bare mode they remain unreachable by
// construction, since translation is not performed at all and an access outside populated
// physical memory raises cause 11 instead.
//
// FOUR NUMBERS ARE NOT HERE ON PURPOSE, and the gaps are the point rather than an omission.
// Causes 5 and 6 are held dark so that a handler table carried over from Maize v1, which spent
// those numbers on a segment-bounds violation and a stack fault, cannot silently alias an old
// cause onto a new one (trap-model.md, "The cause enumeration"). Causes 12 through 31 carry the
// same guarantee for future synchronous causes. Nothing in this machine constructs any of them,
// and reserved_cause_is_never_delivered in fixtures_traps.cpp is what makes that a tested claim
// rather than a described one.
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

// "Causes 0 through 31 are synchronous, causes 32 through 255 are external interrupts, and the
// device-surface chapter assigns interrupt causes to sources." The two constants below are the
// whole of the line-to-cause mapping trap-model.md's "External interrupts" section fixes for all
// time: the cause number of a device interrupt is 32 plus the device's interrupt line index, and
// the line index equals the device class code (maize-466).
inline constexpr std::uint8_t kFirstExternalInterrupt = 32;
inline constexpr std::uint8_t kConsoleInterrupt = kFirstExternalInterrupt + 1;  // class 1
inline constexpr std::uint8_t kTimerInterrupt = kFirstExternalInterrupt + 3;    // class 3
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

// trap-model.md, "The frame". Four words, in this order, and NO general-purpose register. Maize
// v1 pushed thirteen registers into every handler whether the handler wanted them or not; v2
// pushes none and has no instruction that saves a fixed register set, so a handler that needs a
// register saves it itself. The frame is the entire hardware-visible cost of entering a handler.
namespace trap_frame {
inline constexpr std::uint64_t kBytes = 32;
inline constexpr std::uint64_t kPcOffset = 0;
inline constexpr std::uint64_t kStatusOffset = 8;
inline constexpr std::uint64_t kCauseOffset = 16;
inline constexpr std::uint64_t kAuxOffset = 24;
}  // namespace trap_frame

// trap-model.md, "The cause word": the cause number in bits 7:0, the subcode in bits 15:8, and
// bits 63:16 written as zero by every conforming machine, so a handler that tests the whole word
// against a constant behaves the same everywhere.
constexpr std::uint64_t encode_cause_word(std::uint8_t cause_number, std::uint8_t subcode_number) {
    return static_cast<std::uint64_t>(cause_number) |
           (static_cast<std::uint64_t>(subcode_number) << 8);
}

// trap-model.md, "Vectored dispatch": 256 entries of 8 bytes each, 2 KiB in all, with entry `c`
// at the base plus `c` times 8. The base is a control and status register rather than v1's fixed
// $1000, and its low 11 bits are required to be zero so the table never straddles more pages
// than it has to.
namespace vector_table {
inline constexpr std::uint64_t kEntryBytes = 8;
inline constexpr std::uint64_t kEntryCount = 256;
inline constexpr std::uint64_t kBytes = kEntryBytes * kEntryCount;

constexpr std::uint64_t entry_address(std::uint64_t base, std::uint8_t cause_number) {
    return base + static_cast<std::uint64_t>(cause_number) * kEntryBytes;
}
}  // namespace vector_table

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
