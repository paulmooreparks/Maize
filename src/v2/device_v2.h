// device_v2.h (maize-451): the port space, the common class skeleton, and the console class.
//
// device-surface.md fixes a port space of 65,536 ports that is disjoint from memory, reached by
// `port_in` and `port_out` and by nothing else. Seven device classes each own sixteen
// consecutive ports at a base of class code times sixteen, and block $0000 through $000F is the
// machine block rather than a class.
//
// THIS BUILD SHIPS THE CONSOLE CLASS AND NOTHING ELSE (D-1). device-surface.md's class table
// requires the console AND the timer of a machine claiming full conformance, so this machine
// does not claim it. It does not lie about itself either: the presence bitmap is COMPUTED from
// which classes are actually populated, so a class this build omits reads absent, exactly as a
// genuinely optional class an implementation chose to leave out would. That is the same
// disclosed-partial-build pattern memory_v2.h's D-3 note and interpreter_v2.cpp's D-2 note
// already follow.
//
// THE SKELETON IS THE REUSABLE PIECE. Offsets 0, 1 and 2 of every class block mean the same
// three things in every class: identification, status-and-acknowledge, and interrupt control.
// DeviceClassV2 implements those three once and hands a class two hooks for the rest, so
// maize-421's other six classes inherit the shape rather than restate it. What is uniform is the
// OFFSETS, not the bit meanings above bit 2: each class contract names its own status bits, and
// the console's are not the timer's.
//
// ACKNOWLEDGEABLE VERSUS HELD STATUS BITS. The acknowledge contract reads "every status bit set
// in the written value is cleared, and a bit the device holds true remains set", which is two
// kinds of bit rather than one, and the whole of the split is the question ONE ACKNOWLEDGE ASKS:
// after the guest writes this bit, does the device still consider it true?
//
// An acknowledgeable bit records that something happened, and once the guest has been told, the
// device has nothing left to report, so the write clears it for good. The console's overrun bit
// is one: the guest now knows a byte was lost, and no further reading of the device makes that
// event un-reported. These live in `acknowledgeable_status_`.
//
// A held bit reports a condition the device considers true, so it reappears on the next read
// however hard the guest acknowledges it. These are recomputed by `held_status_bits()`.
//
// THE TEST IS "DOES THE DEVICE STILL CONSIDER IT TRUE", NOT "WAS IT SET BY AN EVENT", and the two
// readings disagree on exactly one console bit. End-of-input is set by an event, which makes it
// look acknowledgeable, and device-surface.md:248 says it "latches once the input stream is
// exhausted and stays set thereafter, so software distinguishes a byte that is not there yet from
// a byte that will never come". A bit an acknowledge could clear for good cannot draw that
// distinction: the guest would acknowledge it, read zero, and be told a byte might still come
// when the stream is over. So end-of-input is HELD, beside output-ready, behind a flag saying
// the stream is exhausted. It was on the wrong side of this split when the card first shipped
// (maize-451 code review), and the sentence above is the rule that put it there.
//
// The spec's own word for end-of-input is "latches", which is why `latched` is not the name of
// the other category here: borrowing it would put the one bit the spec calls latched in the
// field named after it, and the miscategorisation would read as correct forever.

#ifndef MAIZE_V2_DEVICE_V2_H
#define MAIZE_V2_DEVICE_V2_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace maize::v2 {

// A port identifier is 16 bits, and the low nibble is the offset within a class block.
inline constexpr unsigned kPortsPerBlock = 16;

constexpr unsigned port_class_code(std::uint16_t port) {
    return static_cast<unsigned>(port) / kPortsPerBlock;
}

constexpr std::uint16_t port_offset(std::uint16_t port) {
    return static_cast<std::uint16_t>(port % kPortsPerBlock);
}

namespace device_class {

// The class codes of device-surface.md's class table. Class 0 is not a class: block $0000 is the
// machine block. The codes are listed in full even though this build populates one of them,
// because the presence bitmap and the port dispatch both range over the whole table and a code
// spelled only where it is implemented is a code the next class has to re-derive.
inline constexpr unsigned kMachineBlock = 0;
inline constexpr unsigned kConsole = 1;
inline constexpr unsigned kKeyboard = 2;
inline constexpr unsigned kTimer = 3;
inline constexpr unsigned kBlockStorage = 4;
inline constexpr unsigned kFramebuffer = 5;
inline constexpr unsigned kNetwork = 6;
inline constexpr unsigned kEntropy = 7;
inline constexpr unsigned kHighestClassCode = kEntropy;

}  // namespace device_class

namespace skeleton_offset {

// The three offsets every class block lays out identically.
inline constexpr std::uint16_t kIdentification = 0;   // read only
inline constexpr std::uint16_t kStatus = 1;           // read: status; write: acknowledge
inline constexpr std::uint16_t kInterruptControl = 2;  // read and write
inline constexpr std::uint16_t kFirstClassSpecific = 3;

}  // namespace skeleton_offset

namespace console_status_bit {

// device-surface.md's Console section names bits 0, 3, 4 and 5. Bits 1 (busy) and 2
// (invalid-request) are named at the skeleton level and the console never sets either, so they
// read zero permanently here.
inline constexpr unsigned kInputAvailable = 0;
inline constexpr unsigned kOutputReady = 3;
inline constexpr unsigned kEndOfInput = 4;
inline constexpr unsigned kOverrun = 5;

}  // namespace console_status_bit

namespace skeleton_status_bit {

// device-surface.md, "The common class skeleton": bit 0 is the class's primary ready or pending
// condition, bit 1 is busy, and bit 2 is invalid-request. Those three mean the same thing in
// every class, which is why they are here rather than repeated per class.
inline constexpr unsigned kPrimaryCondition = 0;
inline constexpr unsigned kBusy = 1;
inline constexpr unsigned kInvalidRequest = 2;

}  // namespace skeleton_status_bit

namespace timer_status_bit {

// device-surface.md's Timer section names bit 0, expiry-pending, and it is the interrupt
// condition. Bit 2, invalid-request, is the skeleton's and the timer is the one class in this
// build that can actually set it: a period of zero with counting enabled is an invalid request.
inline constexpr unsigned kExpiryPending = skeleton_status_bit::kPrimaryCondition;
inline constexpr unsigned kInvalidRequest = skeleton_status_bit::kInvalidRequest;

}  // namespace timer_status_bit

namespace timer_offset {

inline constexpr std::uint16_t kPeriod = 3;          // read and write, nanoseconds
inline constexpr std::uint16_t kMode = 4;            // read and write
inline constexpr std::uint16_t kMonotonicCount = 5;  // read only, nanoseconds since power-on

// The mode word: bit 0 enables counting and bit 1 selects periodic rather than one-shot.
inline constexpr std::uint64_t kModeCountingEnabled = 0x1;
inline constexpr std::uint64_t kModePeriodic = 0x2;
inline constexpr std::uint64_t kModeDefinedMask = 0x3;

}  // namespace timer_offset

constexpr std::uint64_t status_mask(unsigned bit) { return std::uint64_t{1} << bit; }

// Every class identification word carries a "class contract version" in its second
// quarter-word (device-surface.md, "The common class skeleton", "The class contract version").
// Erratum 2.0.2 settled OQ-1: the version is a single 16-bit counter, assigned per class, that
// starts at 1 and increments for an additive change to that class's contract; an incompatible
// change takes a new class code and starts again at 1 instead of incrementing. No class this
// specification defines revises its contract, so every class's counter reads 1 permanently.
inline constexpr std::uint64_t kClassContractVersionInitial = 0x0001;

// The machine block's identification word (device-surface.md, "The class table"): the literal
// $4D32 in the low quarter-word, the BASE specification version in the second, major in bits 31
// through 24 and minor in bits 23 through 16. Base 2.0 puts $0200 there. The 2.0.2 erratum level
// this build is written against does not appear: versioning.md's Errata section says an erratum
// level is not named in a conformance claim, which names the base version.
inline constexpr std::uint64_t kMachineMagic = 0x4D32;
inline constexpr std::uint64_t kBaseSpecificationVersion = 0x0200;

// A device class: the common skeleton at offsets 0 through 2, plus two hooks for the rest of the
// block. A class that defines no port above offset 2 inherits reserved-in-a-populated-block
// behaviour for the whole tail without writing anything.
class DeviceClassV2 {
  public:
    // The contract version is a constructor parameter rather than one shared constant read
    // straight out of identification(), so a class that ever needs a version of its own can
    // carry it without a base-class change. It defaults to kClassContractVersionInitial because
    // erratum 2.0.2 fixes every class's counter at 1 for as long as base 2.0 does not revise and
    // no extension may assign or alter a device class; the seam remains so that a class assigned
    // by later specification work is not blocked on a base-class change to get its own counter.
    explicit DeviceClassV2(unsigned class_code,
                           std::uint64_t contract_version = kClassContractVersionInitial)
        : class_code_(class_code), contract_version_(contract_version) {}
    virtual ~DeviceClassV2() = default;

    DeviceClassV2(const DeviceClassV2&) = delete;
    DeviceClassV2& operator=(const DeviceClassV2&) = delete;

    unsigned class_code() const { return class_code_; }

    // Identification: class code in the low quarter-word, class contract version in the second,
    // remaining bits zero. Nonzero by construction, which is what presence detection reads.
    //
    // This is not virtual and does not need to be. The LAYOUT is universal, fixed for every class
    // by device-surface.md's skeleton table, and a class that could override the layout could
    // break presence detection for everyone. What varies per class is the VERSION, and that comes
    // in through the constructor.
    std::uint64_t identification() const {
        return static_cast<std::uint64_t>(class_code_) | (contract_version_ << 16);
    }

    // The acknowledgeable bits plus whatever the class holds true at this instant.
    std::uint64_t status() const { return acknowledgeable_status_ | held_status_bits(); }

    bool interrupt_enabled() const { return interrupt_enabled_; }

    // Whether this class is asserting its interrupt line right now (maize-466). device-surface.md,
    // "Device interrupts": "A device asserts its line when the condition named in its class
    // contract becomes true and its interrupt-enable port bit is set."
    //
    // THE TWO ENABLES ARE DIFFERENT ENABLES AND THIS ONE IS THE DEVICE'S. The port bit below
    // gates whether the device asserts at all; the interrupt-enable register in the control and
    // status space gates whether the machine DELIVERS an asserted cause. A machine that confuses
    // them leaves a masked condition unrecorded, which the same section forbids outright: "A
    // device whose interrupt-enable bit is clear still records the condition in its status port,
    // so every class is fully usable by polling."
    bool interrupt_asserted() const { return interrupt_enabled_ && interrupt_condition(); }

    // A read of a port in this class's block. Not const: a class read can consume, which is the
    // console's offset 3 and the keyboard's and the entropy device's.
    //
    // OFFSETS 0 THROUGH 2 ARE DELIBERATELY SEALED. Neither this nor port_write is virtual, so no
    // subclass can observe or alter the skeleton's three ports, and that is the point: the
    // skeleton is what lets one driver probe, poll and mask any class, and a class that could
    // redefine those three offsets could break that for every reader of it. A class extends the
    // surface at offset 3 and above.
    //
    // The one seam beneath them is `on_acknowledge`, added on maize-466 when the timer became
    // the first class to need it and shaped exactly as the note that stood here predicted: a
    // protected hook called AFTER the clear, rather than a virtual port_write that would let a
    // class redefine the acknowledge itself.
    std::uint64_t port_read(std::uint16_t offset) {
        switch (offset) {
            case skeleton_offset::kIdentification: return identification();
            case skeleton_offset::kStatus: return status();
            case skeleton_offset::kInterruptControl: return interrupt_enabled_ ? 1u : 0u;
            default: return read_class_port(offset);
        }
    }

    void port_write(std::uint16_t offset, std::uint64_t value) {
        switch (offset) {
            case skeleton_offset::kIdentification:
                // Read-only. Writing a read-only port is discarded.
                return;
            case skeleton_offset::kStatus:
                // Acknowledge. Clearing only the acknowledgeable bits is the whole of it: a held
                // bit is not stored in this field at all, so it reappears on the next read
                // exactly as "a bit the device holds true remains set" requires.
                acknowledgeable_status_ &= ~value;
                // The side effect an acknowledge can carry, called AFTER the clear (maize-466).
                // The seam is here because the timer needs it: device-surface.md's Timer section
                // says acknowledging expiry-pending "re-arms a periodic timer for the next expiry
                // and leaves a one-shot timer disarmed", which is a consequence of the
                // acknowledge rather than a redefinition of what an acknowledge means, so the
                // three skeleton offsets stay sealed and the hook runs beneath them.
                on_acknowledge(value);
                return;
            case skeleton_offset::kInterruptControl:
                // Bit 0 enables the class's interrupt line. Bits 1 through 63 are reserved, and
                // writing a reserved bit is discarded rather than trapped.
                interrupt_enabled_ = (value & 1u) != 0;
                return;
            default:
                write_class_port(offset, value);
                return;
        }
    }

    // Host-side, reachable from no instruction. A fixture uses this to stand up an acknowledgeable
    // condition whose real source this build does not wire, so the acknowledge contract can be
    // tested on a bit that genuinely clears.
    void host_raise_acknowledgeable_bit(unsigned bit) { acknowledgeable_status_ |= status_mask(bit); }

  protected:
    // The bits this class holds true right now, recomputed on every read.
    virtual std::uint64_t held_status_bits() const { return 0; }

    // Is the condition this class's contract names as its interrupt condition true right now
    // (maize-466)? Independent of the port-level enable bit, which interrupt_asserted() applies
    // on top, and independent of whether the machine would deliver the cause at all.
    virtual bool interrupt_condition() const { return false; }

    // What an acknowledge does beyond clearing bits. `written` is the value the guest wrote, so
    // an override sees which bits it acknowledged and not merely that it acknowledged something.
    virtual void on_acknowledge(std::uint64_t written) { (void)written; }

    // Offsets 3 through 15. The default is reserved-in-a-populated-block, which reads zero and
    // discards writes, observably identical to an unpopulated port and required to be.
    virtual std::uint64_t read_class_port(std::uint16_t offset) {
        (void)offset;
        return 0;
    }

    virtual void write_class_port(std::uint16_t offset, std::uint64_t value) {
        (void)offset;
        (void)value;
    }

    // Bits an acknowledge clears for good. A bit the device would still consider true afterwards
    // does not belong here; it belongs in held_status_bits().
    std::uint64_t acknowledgeable_status_ = 0;

  private:
    unsigned class_code_;
    std::uint64_t contract_version_;
    bool interrupt_enabled_ = false;
};

// The console: a byte-at-a-time input stream and a byte-at-a-time output stream at offset 3.
//
// NO HOST INPUT SOURCE IS WIRED IN THIS BUILD, and a queue a fixture fills is not one. maize-451
// filed that as D-2 when a console with no way at all to receive a byte was the whole story;
// maize-466 needs the interrupt condition the console owns, so the queue below exists and
// host_push_input is the only thing that fills it. Nothing reachable from a guest instruction
// puts a byte in it, so a program run under mzvm still sees input-available permanently clear,
// which is the contract's own defined behaviour for an input stream that has nothing waiting
// rather than a gap in it. A later card wires real stdin into host_push_input and changes
// nothing else here.
//
// WHERE THE BIT GOES WHEN STDIN ARRIVES, since that card will read this paragraph rather than
// re-derive it. End-of-input is HELD, not acknowledgeable, so it belongs in held_status_bits()
// behind `input_exhausted_` and NOT in acknowledgeable_status_. The spec's word for it is
// "latches", and it "stays set thereafter" precisely so a guest can tell a byte that is not
// there yet from a byte that will never come, which an acknowledge must not be able to undo.
// The stdin card sets `input_exhausted_` when the host stream ends and never clears it. That
// path is already exercised: host_set_input_exhausted stands the condition up today and
// device_console_acknowledge_clears_transient_bits_only proves an acknowledge cannot clear it.
//
// INPUT-AVAILABLE IS HELD TOO, and for a different reason: it is true exactly while a byte is
// waiting, so consuming the last byte makes it false again. device-surface.md's Console section
// says so directly, "Reading offset 3 consumes the byte and clears the condition when no further
// byte is waiting", which is what makes the console's interrupt line level-sensitive: a guest
// that takes the interrupt and does not read the byte sees the same cause pending again at the
// next boundary, and that is the re-raise the trap-model chapter describes rather than a
// double delivery.
//
// Output goes into a buffer rather than to a real stream, so the interpreter and its in-process
// fixtures carry no I/O dependency. mzvm_main.cpp reads the buffer after run() and is the only
// thing in the tree that touches a real stdout.
class ConsoleDeviceV2 : public DeviceClassV2 {
  public:
    ConsoleDeviceV2() : DeviceClassV2(device_class::kConsole) {}

    static constexpr std::uint16_t kDataOffset = 3;

    const std::vector<std::uint8_t>& output() const { return output_; }

    // Host-side, reachable from no instruction. A console whose output can always accept a byte
    // holds output-ready permanently set, which is conforming and is what this build's in-memory
    // buffer is. A fixture clears it to reach the overrun path, which no guest program can reach
    // here but a real terminal with backpressure will reach for real.
    void host_set_output_ready(bool ready) { output_ready_ = ready; }

    // Host-side, reachable from no instruction. The stdin card sets this when the host stream
    // ends; today it exists so a fixture can prove that end-of-input, once true, survives an
    // acknowledge. Without it the categorisation above would be an untested claim, and it is a
    // claim that got itself wrong once already.
    void host_set_input_exhausted(bool exhausted) { input_exhausted_ = exhausted; }

    // Host-side, reachable from no instruction (maize-466). Deliver one byte to the input stream,
    // which is what a real console does when a key reaches it. This is the ordinary console-input
    // injection the interrupt fixtures use, and it is the only thing in the tree that makes
    // input-available true.
    void host_push_input(std::uint8_t byte) { input_.push_back(byte); }

    // How many delivered bytes the guest has not consumed yet. Host-side and for assertions; a
    // guest sees this only as the input-available status bit.
    std::size_t host_pending_input() const { return input_.size() - input_read_; }

  protected:
    std::uint64_t held_status_bits() const override {
        std::uint64_t held = 0;
        if (input_available()) {
            held |= status_mask(console_status_bit::kInputAvailable);
        }
        if (output_ready_) {
            held |= status_mask(console_status_bit::kOutputReady);
        }
        // Held rather than acknowledgeable: once the stream is exhausted the device still
        // considers this true, forever, and no acknowledge may take it back.
        if (input_exhausted_) {
            held |= status_mask(console_status_bit::kEndOfInput);
        }
        return held;
    }

    // device-surface.md, Console: "The interrupt condition is input-available."
    bool interrupt_condition() const override { return input_available(); }

    std::uint64_t read_class_port(std::uint16_t offset) override {
        if (offset == kDataOffset) {
            if (!input_available()) {
                // "Reading offset 3 when input-available is clear yields zero and consumes
                // nothing." That is the contract, not a stub.
                return 0;
            }
            // Zero-extended, and consumed. Consuming the last byte clears input-available, which
            // drops the interrupt line with it.
            return input_[input_read_++];
        }
        return 0;  // reserved within a populated block
    }

    void write_class_port(std::uint16_t offset, std::uint64_t value) override {
        if (offset != kDataOffset) {
            return;  // reserved within a populated block
        }
        if (!output_ready_) {
            // Writing offset 3 while output-ready is clear discards the byte and sets the
            // overrun bit, so a program that paces itself on output-ready loses nothing and one
            // that does not can see that it lost something. Acknowledgeable: once the guest has
            // been told a byte was lost, the device has nothing further to report.
            acknowledgeable_status_ |= status_mask(console_status_bit::kOverrun);
            return;
        }
        output_.push_back(static_cast<std::uint8_t>(value & 0xFF));
    }

  private:
    bool input_available() const { return input_read_ < input_.size(); }

    std::vector<std::uint8_t> output_;
    // Delivered bytes and how far the guest has read. A read index rather than an erase keeps
    // every delivered byte for a fixture to look back at, and the queue is small by construction.
    std::vector<std::uint8_t> input_;
    std::size_t input_read_ = 0;
    bool output_ready_ = true;
    bool input_exhausted_ = false;  // nothing in this build ever sets it
};

// The timer, class 3 (maize-466). device-surface.md says outright that it "is the device the
// conformance suite uses to exercise interrupt delivery end to end", which is why the class lands
// with the interrupt card rather than waiting for the rest of the device surface: without a
// device that can assert a line, every interrupt fixture would have to reach past the guest and
// set a pending bit by hand, and a delivery path tested only that way is a path no device has
// ever driven.
//
// TIME IS COUNTED IN RETIRED INSTRUCTIONS, WHICH IS BOTH CONFORMING AND THE ONLY DETERMINISTIC
// CHOICE. The chapter states the period and the monotonic count in nanoseconds, then says "The
// resolution a machine actually delivers is an implementation property, and the monotonic count
// advances in whatever increment that resolution produces. Nothing in this contract requires a
// tick to arrive on time, only that expiry, status, and the monotonic count agree with each
// other." A machine that read a host clock would satisfy the letter of that and violate
// execution-model.md's determinism guarantee and conformance.md's ban on a test that depends on
// how long anything takes, because the same image would expire the timer at a different
// instruction on a faster host. Counting the machine's own retired instructions makes expiry a
// function of architectural state, so a conformance binary that arms the timer and counts
// instructions gets the same answer on every machine that makes the same choice, and gets an
// answer that is at least reproducible on any machine at all.
inline constexpr std::uint64_t kNanosecondsPerInstruction = 1000;

class TimerDeviceV2 : public DeviceClassV2 {
  public:
    TimerDeviceV2() : DeviceClassV2(device_class::kTimer) {}

    std::uint64_t monotonic_nanoseconds() const { return monotonic_ns_; }

    // Advance the machine's own sense of time and expire the timer if the interval elapsed. The
    // interpreter calls this once per retired instruction; nothing a guest executes calls it.
    void advance_time(std::uint64_t nanoseconds) {
        monotonic_ns_ += nanoseconds;
        if (armed_ && monotonic_ns_ >= next_expiry_ns_) {
            // Expiry-pending is ACKNOWLEDGEABLE rather than held, and the split matters here more
            // than anywhere else in the file: the whole of the timer's re-arm contract is that
            // the guest's acknowledge is what clears the condition, so a bit the device
            // recomputed on every read would drop the expiry the instant the interval passed and
            // an interrupt would be lost between the expiry and the handler's first instruction.
            acknowledgeable_status_ |= status_mask(timer_status_bit::kExpiryPending);
            // An expiry disarms. A periodic timer is re-armed by the acknowledge, not by the
            // expiry, which is what keeps a periodic timer from queueing expiries behind a
            // handler that has not run yet.
            armed_ = false;
        }
    }

    // How many nanoseconds until this device could next make its condition true, or none when
    // nothing is scheduled. wait_for_interrupt is the caller: a suspended machine retires no
    // instruction, so without something to advance time to, the wait could never end.
    bool nanoseconds_until_expiry(std::uint64_t& out) const {
        if (!armed_) {
            return false;
        }
        out = next_expiry_ns_ > monotonic_ns_ ? next_expiry_ns_ - monotonic_ns_ : 0;
        return true;
    }

  protected:
    // device-surface.md, Timer: "Status bit 0 is expiry-pending, and it is the interrupt
    // condition." Read off the acknowledgeable field rather than recomputed, for the reason
    // advance_time states.
    bool interrupt_condition() const override {
        return (acknowledgeable_status_ & status_mask(timer_status_bit::kExpiryPending)) != 0;
    }

    // "Writing the acknowledge port clears it, which re-arms a periodic timer for the next expiry
    // and leaves a one-shot timer disarmed."
    void on_acknowledge(std::uint64_t written) override {
        if ((written & status_mask(timer_status_bit::kExpiryPending)) == 0) {
            return;  // the guest acknowledged something else
        }
        // No period_ != 0 guard here, because counting being enabled already means the period is
        // nonzero: the mode port refuses to enable counting against a zero period, and the period
        // port disables counting when a zero is written into it, so the two writers between them
        // hold the invariant and a third check here would be a guard nothing can reach.
        if (counting_enabled() && periodic()) {
            arm();
        }
    }

    std::uint64_t read_class_port(std::uint16_t offset) override {
        switch (offset) {
            case timer_offset::kPeriod: return period_;
            case timer_offset::kMode: return mode_;
            case timer_offset::kMonotonicCount: return monotonic_ns_;
            default: return 0;  // reserved within a populated block
        }
    }

    void write_class_port(std::uint16_t offset, std::uint64_t value) override {
        switch (offset) {
            case timer_offset::kPeriod:
                // "Writing the period while counting is enabled takes effect at the next expiry
                // rather than immediately, so a running periodic timer is reprogrammed without
                // losing a tick." The register reads back what was written either way; what is
                // deferred is the interval in flight, which arm() re-reads when it next runs.
                period_ = value;
                if (!counting_enabled()) {
                    armed_ = false;
                    return;
                }
                if (value == 0) {
                    // "A period of zero with counting enabled is an invalid request: the device
                    // sets the invalid-request status bit, leaves counting disabled, and raises no
                    // trap." THE RULE IS A CONDITION ON STATE, NOT ON ONE PORT (maize-466 cycle 2).
                    // The mode port is one route into that state and this is the other, so the
                    // response has to be identical on both or a guest reaches an invalid state by
                    // the unguarded route and the device reports nothing wrong. The deferral rule
                    // in the paragraph above does not soften it: deferring the refusal to the next
                    // expiry would leave the register reading a counting timer that the device has
                    // already decided to refuse, and the guest with nothing to read that says so.
                    refuse_counting_with_a_zero_period();
                }
                return;
            case timer_offset::kMode: {
                // Reserved bits above bit 1 are discarded rather than trapped, which is the
                // skeleton's rule for every device register.
                const std::uint64_t requested = value & timer_offset::kModeDefinedMask;
                if ((requested & timer_offset::kModeCountingEnabled) != 0 && period_ == 0) {
                    // The mode-port route into a zero period with counting enabled. Counting is
                    // left disabled, so the counting bit does not reach the mode register and a
                    // guest that reads the register back sees the refusal. The periodic bit the
                    // guest asked for is kept, because the contract refuses the combination rather
                    // than the whole write.
                    mode_ = requested;
                    refuse_counting_with_a_zero_period();
                    return;
                }
                mode_ = requested;
                if (counting_enabled()) {
                    // A write to the MODE port is an explicit programming action, so it starts a
                    // fresh interval whatever the register held before. The deferral the chapter
                    // states belongs to the PERIOD port, "Writing the period while counting is
                    // enabled takes effect at the next expiry rather than immediately", and
                    // deferring a mode write too would leave a guest that re-programmed a
                    // one-shot after its expiry with a timer that never fires again.
                    arm();
                } else {
                    armed_ = false;
                }
                return;
            }
            default:
                return;  // the monotonic count is read-only, and the rest is reserved
        }
    }

  private:
    bool counting_enabled() const {
        return (mode_ & timer_offset::kModeCountingEnabled) != 0;
    }
    bool periodic() const { return (mode_ & timer_offset::kModePeriodic) != 0; }

    // The single response the contract states for a period of zero with counting enabled, applied
    // by both routes that can reach that state. Sets the invalid-request status bit, leaves
    // counting disabled, raises no trap. Every caller has already stored whatever the guest wrote
    // to its own register, so a read-back shows both what was asked for and what was refused.
    void refuse_counting_with_a_zero_period() {
        acknowledgeable_status_ |= status_mask(timer_status_bit::kInvalidRequest);
        mode_ &= ~timer_offset::kModeCountingEnabled;
        armed_ = false;
    }

    // Start an interval from now. The period is read HERE rather than latched at the write, which
    // is what makes a period written mid-interval take effect at the next expiry.
    void arm() {
        next_expiry_ns_ = monotonic_ns_ + period_;
        armed_ = true;
    }

    std::uint64_t period_ = 0;
    std::uint64_t mode_ = 0;
    std::uint64_t monotonic_ns_ = 0;
    std::uint64_t next_expiry_ns_ = 0;
    bool armed_ = false;
};

// The whole port space of one machine: the machine block, the populated classes, and the
// read-zero-discard-writes fallback that covers everything else.
class DeviceSurfaceV2 {
  public:
    std::uint64_t port_in(std::uint16_t port) {
        const unsigned class_code = port_class_code(port);
        const std::uint16_t offset = port_offset(port);
        if (class_code == device_class::kMachineBlock) {
            return machine_block_read(offset);
        }
        DeviceClassV2* device = device_for(class_code);
        return device == nullptr ? 0 : device->port_read(offset);
    }

    void port_out(std::uint16_t port, std::uint64_t value) {
        const unsigned class_code = port_class_code(port);
        const std::uint16_t offset = port_offset(port);
        if (class_code == device_class::kMachineBlock) {
            // Both defined ports of the machine block are read-only, and every other offset is
            // reserved. A write to any of them is discarded.
            return;
        }
        DeviceClassV2* device = device_for(class_code);
        if (device != nullptr) {
            device->port_write(offset, value);
        }
    }

    // The bitmap port $0001 reads, and the reason presence detection cannot lie: it is derived
    // from which classes are populated rather than declared beside them.
    std::uint64_t presence_bitmap() {
        std::uint64_t bitmap = 0;
        for (unsigned code = 1; code <= device_class::kHighestClassCode; ++code) {
            if (device_for(code) != nullptr) {
                bitmap |= std::uint64_t{1} << code;
            }
        }
        return bitmap;
    }

    static constexpr std::uint64_t machine_identification() {
        return kMachineMagic | (kBaseSpecificationVersion << 16);
    }

    ConsoleDeviceV2& console() { return console_; }
    const ConsoleDeviceV2& console() const { return console_; }
    TimerDeviceV2& timer() { return timer_; }
    const TimerDeviceV2& timer() const { return timer_; }

    const std::vector<std::uint8_t>& console_output() const { return console_.output(); }

    // Which class codes are asserting their interrupt lines right now, as a bitmask indexed by
    // class code (maize-466). device-surface.md: "Each device class owns exactly one interrupt
    // line, and the line index equals the class code in the class table." The machine turns a
    // line index into a cause number; this function knows nothing about cause numbers, which is
    // what keeps that mapping in the one place trap-model.md assigns it to.
    std::uint64_t asserted_interrupt_lines() const {
        std::uint64_t lines = 0;
        for (unsigned code = 1; code <= device_class::kHighestClassCode; ++code) {
            const DeviceClassV2* device = device_for(code);
            if (device != nullptr && device->interrupt_asserted()) {
                lines |= std::uint64_t{1} << code;
            }
        }
        return lines;
    }

    // Move every device's sense of time forward. Only the timer has one today.
    void advance_time(std::uint64_t nanoseconds) { timer_.advance_time(nanoseconds); }

    // The shortest wait after which some device could assert a line it is not asserting now, or
    // false when no device has anything scheduled. A machine suspended in wait_for_interrupt with
    // this returning false can never wake, which is a state the interpreter reports rather than
    // spins in.
    bool nanoseconds_until_next_device_event(std::uint64_t& out) const {
        return timer_.nanoseconds_until_expiry(out);
    }

  private:
    // The one place that says which classes this machine carries. A class code the base assigns
    // but this build does not populate, and a class code the base never assigns at all, both
    // land on nullptr and therefore on the unpopulated behaviour, which is exactly the point:
    // an absent class and a not-yet-built class are indistinguishable from the guest's side.
    DeviceClassV2* device_for(unsigned class_code) {
        switch (class_code) {
            case device_class::kConsole: return &console_;
            case device_class::kTimer: return &timer_;
            default: return nullptr;
        }
    }

    const DeviceClassV2* device_for(unsigned class_code) const {
        switch (class_code) {
            case device_class::kConsole: return &console_;
            case device_class::kTimer: return &timer_;
            default: return nullptr;
        }
    }

    std::uint64_t machine_block_read(std::uint16_t offset) {
        switch (offset) {
            case 0: return machine_identification();
            case 1: return presence_bitmap();
            default: return 0;  // reserved within the populated machine block
        }
    }

    ConsoleDeviceV2 console_;
    TimerDeviceV2 timer_;
};

}  // namespace maize::v2

#endif  // MAIZE_V2_DEVICE_V2_H
