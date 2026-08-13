// csr_v2.h (maize-463): privilege levels and the control-and-status-register space.
//
// privileged-architecture.md, "Privilege levels" through "The base registers", is the whole
// content of this file. Two things in it are easy to implement almost-correctly, and both are
// built here deliberately rather than discovered later.
//
// THE FOUR ACCESS RULES APPLY IN A FIXED ORDER, AND THE ORDER IS OBSERVABLE. The chapter's
// "The number layout" section numbers them and then states outright that a read-only,
// supervisor-numbered register accessed at user level takes rule 2 and never reaches rule 3.
// access() below is therefore four sequential early returns in the chapter's own order, not a
// set of conditions gathered into one expression, because the order is the specified behaviour
// and a reordering has to break a test rather than pass a differently-shaped one.
//
// AN UNIMPLEMENTED NUMBER TRAPS. Maize v1 read an undefined control-register index as zero and
// discarded a write to it, and conformance.md names that convention as the likeliest thing a
// port carries forward by accident. Rule 4 here is the whole of the difference: every number
// outside the eighteen in the base table, including every unallocated extension index in
// $1000..$1FFF, raises the illegal-operand trap with subcode 3, on read and on write alike.
//
// What is NOT here, and is not this card: the trap frame and vectored delivery (maize-464), the
// translation the paging root selects and the cache the flush counter below stands in for
// (maize-465), and what makes an interrupt-pending bit set (maize-466). Those cards name
// registers this file already holds, which is why this file came first.

#ifndef MAIZE_V2_CSR_V2_H
#define MAIZE_V2_CSR_V2_H

#include <array>
#include <cstdint>

#include "trap_v2.h"

namespace maize::v2 {

// privileged-architecture.md, "Privilege levels", and trap-model.md, "The status word": the
// enumerators carry the architectural encoding of the status word's privilege field, so the
// field and the enum need no translation table between them and cannot drift apart.
enum class Privilege : std::uint8_t { User = 0, Supervisor = 1 };

// The eighteen base registers, privileged-architecture.md "The base registers". These numbers
// are the same ones asm/v2/csr.mzasm gives the assembler, and the mzasm conformance fixture
// already checks that file against the chapter's table, so a number that disagrees with the
// chapter is caught at two independent sites.
namespace csr {
inline constexpr std::uint16_t kFcsr = 0x0000;
inline constexpr std::uint16_t kFeatureBitmap = 0x2000;
inline constexpr std::uint16_t kStatus = 0x4000;
inline constexpr std::uint16_t kTrapStack = 0x4001;
inline constexpr std::uint16_t kTrapVectorBase = 0x4002;
inline constexpr std::uint16_t kPagingRoot = 0x4003;
inline constexpr std::uint16_t kInterruptEnable0 = 0x4004;
inline constexpr std::uint16_t kInterruptEnable1 = 0x4005;
inline constexpr std::uint16_t kInterruptEnable2 = 0x4006;
inline constexpr std::uint16_t kInterruptEnable3 = 0x4007;
inline constexpr std::uint16_t kSyscallProvider = 0x4008;
inline constexpr std::uint16_t kScratch = 0x4009;
inline constexpr std::uint16_t kInterruptPending0 = 0x6000;
inline constexpr std::uint16_t kInterruptPending1 = 0x6001;
inline constexpr std::uint16_t kInterruptPending2 = 0x6002;
inline constexpr std::uint16_t kInterruptPending3 = 0x6003;
inline constexpr std::uint16_t kHaltCause = 0x6004;
inline constexpr std::uint16_t kBootInfo = 0x6005;
inline constexpr unsigned kBaseRegisterCount = 18;
}  // namespace csr

// The number layout, privileged-architecture.md "The number layout":
//
//     bits 15:14   privilege   %00 user, %01 supervisor, %10 and %11 reserved
//     bit     13   read-only   1 marks the register read-only
//     bits 12:0    index       the register's index within the space
//
// The rules are arithmetic on the number rather than a lookup, which is the point of putting
// them in the number at all.
constexpr unsigned csr_privilege_field(std::uint16_t number) {
    return static_cast<unsigned>(number >> 14);
}

constexpr bool csr_read_only_bit(std::uint16_t number) {
    return ((number >> 13) & 1u) != 0u;
}

constexpr std::uint16_t csr_index_field(std::uint16_t number) {
    return static_cast<std::uint16_t>(number & 0x1FFFu);
}

// Index allocation, "Index allocation": $0000..$0FFF is the base and $1000..$1FFF is extension
// space, allocated in blocks of $100 by the extension registry. This build implements no
// extension, so every extension index is a well-formed unimplemented number and traps under
// rule 4 exactly like any other. The predicate is here because the distinction is a real one a
// later card will need, not because anything branches on it today.
constexpr bool csr_index_is_extension(std::uint16_t number) {
    return csr_index_field(number) >= 0x1000u;
}

// The status word, trap-model.md "The status word":
//
//     bits  1:0   priv      %00 user, %01 supervisor, %10 and %11 reserved
//     bit     2   ie        external-interrupt enable
//     bits 63:3   reserved, read as zero and written as zero
namespace status_word {
inline constexpr std::uint64_t kPrivilegeMask = 0x3u;
inline constexpr std::uint64_t kInterruptEnableBit = 0x4u;
inline constexpr std::uint64_t kDefinedMask = 0x7u;
// The reset value the chapter states: supervisor level, external interrupts disabled.
inline constexpr std::uint64_t kResetValue = 0x1u;
}  // namespace status_word

// The halt-cause layout, trap-model.md "No handler installed":
//
//     bits  7:0   cause     the cause number of the condition that halted the machine
//     bits 15:8   subcode   that condition's subcode
//     bits 17:16  kind      0 = the halt instruction executed, 1 = no handler installed,
//                           2 = double fault
//     bits 63:18  reserved, read as zero
//
// Kind 0 records the `halt` instruction, which has no cause and no subcode, so both fields read
// zero there and two machines cannot differ in what they leave behind.
namespace halt_cause {
inline constexpr unsigned kKindHaltInstruction = 0;
inline constexpr unsigned kKindNoHandler = 1;
inline constexpr unsigned kKindDoubleFault = 2;

constexpr std::uint64_t encode(unsigned kind, std::uint8_t cause_number,
                               std::uint8_t subcode_number) {
    return static_cast<std::uint64_t>(cause_number) |
           (static_cast<std::uint64_t>(subcode_number) << 8) |
           (static_cast<std::uint64_t>(kind) << 16);
}
}  // namespace halt_cause

// The paging-root layout, privileged-architecture.md "The paging-root register". This card owns
// the register and the validity of a value written to it; maize-465 owns what the machine then
// does with it.
namespace paging_root {
inline constexpr std::uint64_t kModeMask = 0xFu;
inline constexpr std::uint64_t kReservedMask = 0xFF0u;  // bits 11:4, written as zero
inline constexpr std::uint64_t kModeBare = 0u;
inline constexpr std::uint64_t kModeSv48 = 1u;
}  // namespace paging_root

// What one access decided. `ok` false carries the trap the chapter names for the condition, and
// nothing in the file changed. `ok` true carries the register's value from BEFORE the access,
// which is what csr_read delivers and what csr_swap banks in rd.
struct CsrOutcome {
    bool ok = false;
    std::uint8_t cause = 0;
    std::uint8_t subcode = 0;
    std::uint64_t aux = 0;
    std::uint64_t prior = 0;
    // "Every write to paging_root flushes every cached translation, whether or not the write
    // changes the value." This card owns the register and therefore owns the fact that the
    // flush was requested; maize-465 owns the cache that will consume it.
    bool flushed_translations = false;
};

class CsrFileV2 {
  public:
    CsrFileV2() { reset(); }

    // boot.md and "The base registers": every writable register holds zero at reset except
    // status, which holds $1. The read-only registers hold what the machine has to report, and
    // in this build that is zero for all of them: no extension is implemented, so the feature
    // bitmap is empty, and the boot-information block is maize-421, so there is no address to
    // report yet. A host that has one populates it below before the first instruction executes.
    void reset() {
        fcsr_ = 0;
        feature_bitmap_ = 0;
        status_ = status_word::kResetValue;
        trap_stack_ = 0;
        trap_vector_base_ = 0;
        paging_root_ = 0;
        interrupt_enable_.fill(0);
        syscall_provider_ = 0;
        scratch_ = 0;
        interrupt_pending_.fill(0);
        halt_cause_ = 0;
        boot_info_ = 0;
        translation_flushes_ = 0;
    }

    // Is this number one of the eighteen the base defines? Every other well-formed number is
    // unimplemented and traps under rule 4.
    static constexpr bool is_implemented(std::uint16_t number) {
        switch (number) {
            case csr::kFcsr:
            case csr::kFeatureBitmap:
            case csr::kStatus:
            case csr::kTrapStack:
            case csr::kTrapVectorBase:
            case csr::kPagingRoot:
            case csr::kInterruptEnable0:
            case csr::kInterruptEnable1:
            case csr::kInterruptEnable2:
            case csr::kInterruptEnable3:
            case csr::kSyscallProvider:
            case csr::kScratch:
            case csr::kInterruptPending0:
            case csr::kInterruptPending1:
            case csr::kInterruptPending2:
            case csr::kInterruptPending3:
            case csr::kHaltCause:
            case csr::kBootInfo:
                return true;
            default:
                return false;
        }
    }

    // One access, checked and performed. csr_read passes is_write false; csr_write and csr_swap
    // both pass true with the same value, because "a csr_swap is checked exactly as a csr_write
    // to the same number" and the two differ only in what the instruction does with `prior`.
    //
    // The four early returns below are privileged-architecture.md's four rules in its own
    // order, and the order is the specified behaviour.
    CsrOutcome access(std::uint16_t number, Privilege level, bool is_write,
                      std::uint64_t value) {
        // Rule 1. A reserved privilege encoding names no level the machine can check an access
        // against, so it traps at EVERY level, supervisor included, before the level is
        // consulted at all.
        const unsigned required = csr_privilege_field(number);
        if (required > static_cast<unsigned>(Privilege::Supervisor)) {
            return trap(cause::kIllegalOperand, subcode::kReservedCsrPrivilege, number);
        }

        // Rule 2. An access from below the level the number names is a privileged operation,
        // and cause 4's auxiliary word for a control-and-status-register access is the register
        // number rather than an opcode byte (trap-model.md, cause 4 row).
        if (static_cast<unsigned>(level) < required) {
            return trap(cause::kPrivilegedOperation, 0, number);
        }

        // Rule 3. A write to a number whose read-only bit is set, and the register keeps its
        // value. This is reached only when rule 2 let the access through, which is exactly what
        // makes the chapter's ordering example observable.
        if (is_write && csr_read_only_bit(number)) {
            return trap(cause::kIllegalOperand, subcode::kReadOnlyCsr, number);
        }

        // Rule 4. Well formed, and this machine does not implement it. v1 read zero here.
        if (!is_implemented(number)) {
            return trap(cause::kIllegalOperand, subcode::kUnimplementedCsr, number);
        }

        // The register's own value-validation rules. They belong to the register rather than to
        // the number, so they run only once the number's four rules have all passed.
        if (is_write && !value_is_acceptable(number, value)) {
            return trap(cause::kIllegalOperand, subcode::kInvalidCsrValue, value);
        }

        CsrOutcome outcome;
        outcome.ok = true;
        outcome.prior = read_raw(number);
        if (is_write) {
            store(number, value);
            if (number == csr::kPagingRoot) {
                ++translation_flushes_;
                outcome.flushed_translations = true;
            }
        }
        return outcome;
    }

    // Is this a value the status register accepts? trap-model.md, "The status word": a value
    // naming a reserved privilege encoding, or setting any reserved bit, raises the
    // illegal-operand trap with subcode 6 and changes nothing.
    //
    // Public and static because `trap_return` asks the same question of the status word it finds
    // on the frame, and the chapter says outright that subcode 6 "covers the status word a
    // trap_return frame supplies as well, since that word is written into the status register".
    // One rule, one implementation, so a csr_write and a trap_return cannot come to disagree
    // about which words are legal.
    static constexpr bool status_value_is_acceptable(std::uint64_t value) {
        return (value & ~status_word::kDefinedMask) == 0u &&
               (value & status_word::kPrivilegeMask) <=
                   static_cast<std::uint64_t>(Privilege::Supervisor);
    }

    // The live privilege level, which is the status register's privilege field and nothing
    // else. There is no second copy of it to keep in step.
    Privilege privilege() const {
        return static_cast<Privilege>(status_ & status_word::kPrivilegeMask);
    }

    bool interrupts_enabled() const {
        return (status_ & status_word::kInterruptEnableBit) != 0u;
    }

    // How many times a write to the paging root asked for the translation cache to be flushed.
    // There is no cache in this build; maize-465 brings one and consumes the request. The
    // counter exists so this card's half of the seam is observable rather than merely written.
    std::uint64_t translation_flushes() const { return translation_flushes_; }

    // What the MACHINE does to these registers on its own account (maize-464), as opposed to
    // what an instruction asks of them through access() and what a host sets up through the
    // host_ accessors below. Trap delivery, `trap_return` and a halt all write registers without
    // an instruction naming a register number, so none of them can go through access(): the
    // halt-cause register is read-only to software and would fail rule 3, and delivery's status
    // change is not a
    // write of a whole value but a change to two fields with the rest left alone.
    //
    // Each one is named for the sequence step it performs rather than for the register it
    // touches, so a reader of interpreter_v2.cpp's delivery sequence sees the chapter's steps.
    void machine_set_trap_stack(std::uint64_t value) { trap_stack_ = value; }

    // trap-model.md, "Vectored dispatch", step 5: the privilege level goes to supervisor and the
    // interrupt-enable bit is cleared, "leaving every other status bit unchanged". Written as a
    // field update rather than as an assignment of $1 so that a status bit a later extension
    // defines survives a trap without this line being revisited.
    void machine_enter_supervisor() {
        status_ = (status_ & ~(status_word::kPrivilegeMask | status_word::kInterruptEnableBit)) |
                  static_cast<std::uint64_t>(Privilege::Supervisor);
    }

    // trap-model.md, "Returning from a trap": the frame's status word goes into the status
    // register whole, which is what returns the machine to user mode and what re-enables
    // interrupts. The caller validates first, because validation is total and comes first.
    void machine_write_status(std::uint64_t value) { status_ = value; }

    // trap-model.md, "No handler installed": why the machine stopped. Read-only to software, so
    // this is the only writer.
    void machine_record_halt(unsigned kind, std::uint8_t cause_number,
                             std::uint8_t subcode_number) {
        halt_cause_ = halt_cause::encode(kind, cause_number, subcode_number);
    }

    // Host-side, reachable from no instruction, named the way MemoryV2::host_set_size and
    // InterpreterV2::host_set_privilege are named and for the same reason: each stands up a
    // machine state that no guest-visible path into this build can produce.
    void host_set_privilege(Privilege level) {
        status_ = (status_ & ~status_word::kPrivilegeMask) | static_cast<std::uint64_t>(level);
    }
    void host_set_feature_bitmap(std::uint64_t value) { feature_bitmap_ = value; }
    void host_set_boot_info(std::uint64_t value) { boot_info_ = value; }
    void host_set_halt_cause(std::uint64_t value) { halt_cause_ = value; }
    void host_set_interrupt_pending(unsigned array, std::uint64_t value) {
        interrupt_pending_[array] = value;
    }
    // Reads a register without applying any access rule, for a host inspecting a machine.
    std::uint64_t host_read(std::uint16_t number) const { return read_raw(number); }

  private:
    static CsrOutcome trap(std::uint8_t cause_number, std::uint8_t subcode_number,
                           std::uint64_t aux) {
        CsrOutcome outcome;
        outcome.ok = false;
        outcome.cause = cause_number;
        outcome.subcode = subcode_number;
        outcome.aux = aux;
        return outcome;
    }

    // Six registers carry rules the table had no room for, and each rule is normative. Every
    // one of them rejects with subcode 6, the value trap-model.md assigns to an invalid value
    // written to a control and status register, and the auxiliary word is the offending VALUE
    // rather than the register number.
    static bool value_is_acceptable(std::uint16_t number, std::uint64_t value) {
        switch (number) {
            case csr::kFcsr:
                // floating-point.md, "The floating-point control and status register": bits 63
                // through 8 are reserved and a write of any value other than zero into them
                // raises the trap. A reserved rounding-mode encoding in frm is NOT rejected
                // here; that chapter is explicit that the write succeeds and the next rounding
                // operation is what traps.
                return (value & ~std::uint64_t{0xFF}) == 0u;
            case csr::kStatus:
                return status_value_is_acceptable(value);
            case csr::kTrapStack:
                // The trap stack is full-descending and the frame is four words, so the
                // register requires 16-byte alignment.
                return (value & 0xFu) == 0u;
            case csr::kTrapVectorBase:
                // A 256-entry vector table wants 2 KiB alignment, so the low 11 bits are zero.
                return (value & 0x7FFu) == 0u;
            case csr::kPagingRoot:
                // "The paging-root register": mode 0 is bare and 1 is Sv48, 2 through 15 are
                // reserved, and bits 11:4 are written as zero. v1 forced the reserved bits to
                // zero and treated an unknown mode as bare; v2 rejects both.
                return (value & paging_root::kModeMask) <= paging_root::kModeSv48 &&
                       (value & paging_root::kReservedMask) == 0u;
            case csr::kInterruptEnable0:
                // Bits 0 through 31 are the synchronous causes, which are never maskable, so a
                // write that sets any of them is rejected.
                return (value & 0xFFFFFFFFu) == 0u;
            case csr::kSyscallProvider:
                // Bit 0 selects the provider and every other bit is reserved.
                return (value & ~std::uint64_t{1}) == 0u;
            default:
                // scratch accepts any 64-bit pattern by design, the remaining enable arrays
                // cover asynchronous causes only, and every other base register is read-only
                // and never reaches this function.
                return true;
        }
    }

    std::uint64_t read_raw(std::uint16_t number) const {
        switch (number) {
            case csr::kFcsr: return fcsr_;
            case csr::kFeatureBitmap: return feature_bitmap_;
            case csr::kStatus: return status_;
            case csr::kTrapStack: return trap_stack_;
            case csr::kTrapVectorBase: return trap_vector_base_;
            case csr::kPagingRoot: return paging_root_;
            // "those bits read as zero" is a statement about the read, independent of the
            // statement about the write, so it is enforced on the read rather than inferred
            // from the write rule having rejected every value that could set them.
            case csr::kInterruptEnable0: return interrupt_enable_[0] & ~std::uint64_t{0xFFFFFFFF};
            case csr::kInterruptEnable1: return interrupt_enable_[1];
            case csr::kInterruptEnable2: return interrupt_enable_[2];
            case csr::kInterruptEnable3: return interrupt_enable_[3];
            case csr::kSyscallProvider: return syscall_provider_;
            case csr::kScratch: return scratch_;
            case csr::kInterruptPending0: return interrupt_pending_[0];
            case csr::kInterruptPending1: return interrupt_pending_[1];
            case csr::kInterruptPending2: return interrupt_pending_[2];
            case csr::kInterruptPending3: return interrupt_pending_[3];
            case csr::kHaltCause: return halt_cause_;
            case csr::kBootInfo: return boot_info_;
            default: return 0;
        }
    }

    void store(std::uint16_t number, std::uint64_t value) {
        switch (number) {
            case csr::kFcsr: fcsr_ = value; break;
            case csr::kStatus: status_ = value; break;
            case csr::kTrapStack: trap_stack_ = value; break;
            case csr::kTrapVectorBase: trap_vector_base_ = value; break;
            case csr::kPagingRoot: paging_root_ = value; break;
            case csr::kInterruptEnable0: interrupt_enable_[0] = value; break;
            case csr::kInterruptEnable1: interrupt_enable_[1] = value; break;
            case csr::kInterruptEnable2: interrupt_enable_[2] = value; break;
            case csr::kInterruptEnable3: interrupt_enable_[3] = value; break;
            case csr::kSyscallProvider: syscall_provider_ = value; break;
            case csr::kScratch: scratch_ = value; break;
            default: break;  // read-only numbers never reach here; rule 3 stopped them
        }
    }

    std::uint64_t fcsr_ = 0;
    std::uint64_t feature_bitmap_ = 0;
    std::uint64_t status_ = status_word::kResetValue;
    std::uint64_t trap_stack_ = 0;
    std::uint64_t trap_vector_base_ = 0;
    std::uint64_t paging_root_ = 0;
    std::array<std::uint64_t, 4> interrupt_enable_{};
    std::uint64_t syscall_provider_ = 0;
    std::uint64_t scratch_ = 0;
    std::array<std::uint64_t, 4> interrupt_pending_{};
    std::uint64_t halt_cause_ = 0;
    std::uint64_t boot_info_ = 0;
    std::uint64_t translation_flushes_ = 0;
};

}  // namespace maize::v2

#endif  // MAIZE_V2_CSR_V2_H
