// memory_v2.h (maize-418): bare-mode physical memory.
//
// This build implements BARE MODE ONLY (D-3). Sv48 translation is maize-420, and bare mode is
// not a stand-in for it: memory-model.md makes bare mode a real, permanently available mode in
// which an address a load, a store or a block-memory instruction computes IS a physical
// address and the physical memory bounds decide accessibility. Because translation is not
// performed at all, no page table is consulted and no page fault is possible, so causes 8, 9
// and 10 are unreachable in this build and an access outside populated memory raises the
// physical-memory fault, cause 11, with the offending physical address in the auxiliary word.
//
// Populated memory is one contiguous region [0, size). The boot-information block that defines
// the real address map is maize-421; until it lands, whoever constructs the machine says how
// much memory it has.
//
// Addresses wrap modulo 2^64, and wrapping is an ordinary defined outcome rather than a fault:
// an access beginning at $FFFFFFFFFFFFFFFF and covering eight bytes touches seven bytes at the
// top of the address space and the byte at address zero. Every byte of an access is judged on
// its own, which is why the range check walks bytes rather than comparing endpoints.

#ifndef MAIZE_V2_MEMORY_V2_H
#define MAIZE_V2_MEMORY_V2_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace maize::v2 {

class MemoryV2 {
  public:
    explicit MemoryV2(std::size_t size) : bytes_(size, 0) {}

    std::size_t size() const { return bytes_.size(); }

    bool accessible(std::uint64_t address) const {
        return address < static_cast<std::uint64_t>(bytes_.size());
    }

    // Judge a whole access before any of it happens. Returns true when every byte is
    // accessible; otherwise writes the LOWEST inaccessible address the access covers into
    // `lowest_inaccessible` and returns false.
    //
    // The lowest inaccessible address, rather than the first one an implementation happens to
    // touch, is what instruction-reference-memory.md requires: it makes the reported address a
    // function of the access alone and not of the order in which bytes are visited. For an
    // access that wraps past the top of the address space, the numerically lowest inaccessible
    // address is still the reported one, which is what "lowest" says.
    bool check_range(std::uint64_t address, std::uint64_t length,
                     std::uint64_t& lowest_inaccessible) const {
        bool found = false;
        std::uint64_t lowest = 0;
        for (std::uint64_t i = 0; i < length; ++i) {
            const std::uint64_t byte_address = address + i;  // wraps modulo 2^64 by construction
            if (!accessible(byte_address)) {
                if (!found || byte_address < lowest) {
                    lowest = byte_address;
                    found = true;
                }
            }
        }
        if (found) {
            lowest_inaccessible = lowest;
            return false;
        }
        return true;
    }

    // Unchecked byte access. Every caller checks first, either over the whole access (a load or
    // a store, which must write nothing when any byte of the access faults) or per byte (a
    // block-memory instruction, whose restart contract expects a partial transfer to stand).
    std::uint8_t read_byte(std::uint64_t address) const {
        return bytes_[static_cast<std::size_t>(address)];
    }

    void write_byte(std::uint64_t address, std::uint8_t value) {
        bytes_[static_cast<std::size_t>(address)] = value;
    }

    // Little-endian at every width, register to memory and memory to register alike, so the
    // lowest address of a multi-byte access holds the least significant byte.
    std::uint64_t read_little_endian(std::uint64_t address, unsigned width_bytes) const {
        std::uint64_t value = 0;
        for (unsigned i = 0; i < width_bytes; ++i) {
            value |= static_cast<std::uint64_t>(read_byte(address + i)) << (i * 8);
        }
        return value;
    }

    void write_little_endian(std::uint64_t address, unsigned width_bytes, std::uint64_t value) {
        for (unsigned i = 0; i < width_bytes; ++i) {
            write_byte(address + i, static_cast<std::uint8_t>(value >> (i * 8)));
        }
    }

    // Host-side resize. Populated memory is fixed by the address map as far as the guest is
    // concerned, and no instruction can reach this. It exists so a host can stand in for the
    // kernel that services a fault by making a region populated, which is what lets a fixture
    // arm a physical-memory fault partway through a block-memory transfer, inspect the restart
    // state, service the fault, and re-execute. Bytes below the new size keep their values.
    void host_set_size(std::size_t size) { bytes_.resize(size, 0); }

    // Host-side loading. Fixtures and the mzvm entry point place program bytes directly, since
    // the loader and the boot-information block are maize-421.
    bool load_image(std::uint64_t address, const std::uint8_t* data, std::size_t length) {
        std::uint64_t unused = 0;
        if (!check_range(address, length, unused)) {
            return false;
        }
        for (std::size_t i = 0; i < length; ++i) {
            write_byte(address + i, data[i]);
        }
        return true;
    }

  private:
    std::vector<std::uint8_t> bytes_;
};

}  // namespace maize::v2

#endif  // MAIZE_V2_MEMORY_V2_H
