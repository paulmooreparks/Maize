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
// LATCHED VERSUS HELD STATUS BITS. The acknowledge contract reads "every status bit set in the
// written value is cleared, and a bit the device holds true remains set", which is two kinds of
// bit rather than one. A latched bit records that something happened and stays set until the
// guest acknowledges it (overrun, end-of-input). A held bit reports a condition that is true
// right now, so it reappears the instant it is read again however hard the guest acknowledges it
// (the console's output-ready). Splitting them into `status_` and `held_status_bits()` makes the
// acknowledge path one line that cannot get the second half wrong.

#ifndef MAIZE_V2_DEVICE_V2_H
#define MAIZE_V2_DEVICE_V2_H

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

constexpr std::uint64_t status_mask(unsigned bit) { return std::uint64_t{1} << bit; }

// OQ-1, UNANSWERED, OWNED BY THE OPERATOR. Every class identification word carries a "class
// contract version" in its second quarter-word (device-surface.md:172), and NO CHAPTER OF THE
// SPECIFICATION DEFINES A NUMBERING SCHEME FOR IT. It is not the base version, which
// versioning.md fixes at 2.0 forever, and a device class is not an extension either, since
// versioning.md's extension-versioning rules are written for CSR-numbered extensions.
//
// THIS VALUE IS A PLACEHOLDER NOBODY CHOSE. It is $0100 because that is the shape an extension's
// version would take (major 1, minor 0), which is a guess at the answer and not the answer. No
// fixture asserts it, deliberately, so that shipping it cannot turn it into a de-facto ruling by
// way of a test that would have to be edited to change it. maize-421 builds the other six
// classes and needs the real rule before it can give them a version that means anything.
inline constexpr std::uint64_t kClassContractVersionPlaceholder = 0x0100;

// The machine block's identification word (device-surface.md, "The class table"): the literal
// $4D32 in the low quarter-word, the BASE specification version in the second, major in bits 31
// through 24 and minor in bits 23 through 16. Base 2.0 puts $0200 there. The 2.0.1 erratum level
// this build is written against does not appear: versioning.md's Errata section says an erratum
// level is not named in a conformance claim, which names the base version.
inline constexpr std::uint64_t kMachineMagic = 0x4D32;
inline constexpr std::uint64_t kBaseSpecificationVersion = 0x0200;

// A device class: the common skeleton at offsets 0 through 2, plus two hooks for the rest of the
// block. A class that defines no port above offset 2 inherits reserved-in-a-populated-block
// behaviour for the whole tail without writing anything.
class DeviceClassV2 {
  public:
    explicit DeviceClassV2(unsigned class_code) : class_code_(class_code) {}
    virtual ~DeviceClassV2() = default;

    DeviceClassV2(const DeviceClassV2&) = delete;
    DeviceClassV2& operator=(const DeviceClassV2&) = delete;

    unsigned class_code() const { return class_code_; }

    // Identification: class code in the low quarter-word, class contract version in the second,
    // remaining bits zero. Nonzero by construction, which is what presence detection reads.
    std::uint64_t identification() const {
        return static_cast<std::uint64_t>(class_code_) |
               (kClassContractVersionPlaceholder << 16);
    }

    // Latched bits plus whatever the class holds true at this instant.
    std::uint64_t status() const { return status_ | held_status_bits(); }

    bool interrupt_enabled() const { return interrupt_enabled_; }

    // A read of a port in this class's block. Not const: a class read can consume, which is the
    // console's offset 3 and the keyboard's and the entropy device's.
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
                // Acknowledge. Clearing only the latched bits is the whole of it: a held bit is
                // not stored in status_ at all, so it reappears on the next read exactly as
                // "a bit the device holds true remains set" requires.
                status_ &= ~value;
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

    // Host-side, reachable from no instruction. A fixture uses this to stand a latched condition
    // up whose real source this build does not wire, so the acknowledge contract can be tested
    // on a bit that genuinely clears.
    void host_latch_status_bit(unsigned bit) { status_ |= status_mask(bit); }

  protected:
    // The bits this class holds true right now, recomputed on every read.
    virtual std::uint64_t held_status_bits() const { return 0; }

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

    std::uint64_t status_ = 0;  // latched bits only

  private:
    unsigned class_code_;
    bool interrupt_enabled_ = false;
};

// The console: a byte-at-a-time input stream and a byte-at-a-time output stream at offset 3.
//
// D-2: NO HOST INPUT SOURCE IS WIRED IN THIS BUILD. Input-available stays clear and end-of-input
// is never asserted, so a read of offset 3 always yields zero and consumes nothing, which is the
// contract's own defined behaviour for a clear input-available rather than a gap in it.
// device-surface.md never requires an input stream to reach end-of-input; it only says what
// happens when one does. A later card that wires real stdin decides when, if ever, to assert it.
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

  protected:
    std::uint64_t held_status_bits() const override {
        return output_ready_ ? status_mask(console_status_bit::kOutputReady) : 0;
    }

    std::uint64_t read_class_port(std::uint16_t offset) override {
        if (offset == kDataOffset) {
            // Input-available is permanently clear (D-2), so this yields zero and consumes
            // nothing. That is the contract, not a stub.
            return 0;
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
            // that does not can see that it lost something.
            status_ |= status_mask(console_status_bit::kOverrun);
            return;
        }
        output_.push_back(static_cast<std::uint8_t>(value & 0xFF));
    }

  private:
    std::vector<std::uint8_t> output_;
    bool output_ready_ = true;
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

    const std::vector<std::uint8_t>& console_output() const { return console_.output(); }

  private:
    // The one place that says which classes this machine carries. A class code the base assigns
    // but this build does not populate, and a class code the base never assigns at all, both
    // land on nullptr and therefore on the unpopulated behaviour, which is exactly the point:
    // an absent class and a not-yet-built class are indistinguishable from the guest's side.
    DeviceClassV2* device_for(unsigned class_code) {
        switch (class_code) {
            case device_class::kConsole: return &console_;
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
};

}  // namespace maize::v2

#endif  // MAIZE_V2_DEVICE_V2_H
