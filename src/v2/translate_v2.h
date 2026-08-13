// translate_v2.h (maize-465): Sv48 address translation and the translation cache.
//
// privileged-architecture.md, "Address translation" through "The translation cache", is the
// whole content of this file. The page-table format is unchanged from Maize v1, deliberately,
// so that a kernel's paging code ports across without a rewrite; the walk below is therefore
// the one part of this file a reader can recognize from v1. Everything that REJECTS is where
// v2 differs, and each rejection is written out on its own rather than folded into a single
// "the entry is no good" path, because each one names a condition the chapter names.
//
// Four things here are easy to implement almost-correctly, and all four are built deliberately.
//
// A LEAF WITH W SET AND R CLEAR IS REJECTED. It is a reserved encoding, not a write-only page,
// and a machine that honors it hands a kernel a mapping the architecture says does not exist.
//
// A MISALIGNED SUPERPAGE IS REJECTED, NOT ALIGNED DOWN. A level-1 leaf whose address field has
// any nonzero bit below bit 21 is invalid. Masking those bits off instead would translate the
// access to a page the kernel never named, silently.
//
// THE ENTRY HAS A READ DISCIPLINE. The machine reads V, R, W, X, U and the address field, and
// reads G, A and D and the software bits NOT AT ALL, and writes no part of any entry ever. A
// machine that honors a bit the chapter ignores is as wrong as one that ignores a bit the
// chapter honors, so this file never names bit 5, 6, 7 or 11:8 except to say it does not.
//
// A CACHED TRANSLATION IS RE-CHECKED, NOT TRUSTED. The cache stores the leaf's permission bits
// and every use re-runs the same permission test the walk ran, against the access kind and the
// CURRENT privilege level. That is why a privilege change invalidates nothing and why a page
// the kernel touched does not thereby become reachable from user mode.
//
// What is NOT here: the paging-root register's value validation, which is maize-463's and lives
// in csr_v2.h's value_is_acceptable, and trap delivery, which is maize-464's and lives in
// interpreter_v2.cpp. This file produces trap RECORDS and never delivers one.

#ifndef MAIZE_V2_TRANSLATE_V2_H
#define MAIZE_V2_TRANSLATE_V2_H

#include <array>
#include <cstdint>

#include "csr_v2.h"
#include "memory_v2.h"
#include "trap_v2.h"

namespace maize::v2 {

// Which of the three page-fault causes an access raises when its translation fails. The
// enumerator values are not the cause numbers: the mapping is stated once, in
// page_fault_cause() below, so a reader sees it rather than inferring it from an enum's
// underlying values.
enum class AccessKind : std::uint8_t { Fetch, Load, Store };

// trap-model.md's cause table, split by access kind: 8 for an instruction fetch, 9 for a load
// or a block-memory read, 10 for a store or a block-memory write.
constexpr std::uint8_t page_fault_cause(AccessKind kind) {
    switch (kind) {
        case AccessKind::Fetch: return cause::kPageFaultFetch;
        case AccessKind::Load: return cause::kPageFaultLoad;
        case AccessKind::Store: return cause::kPageFaultStore;
    }
    return cause::kPageFaultLoad;
}

// The two page-fault subcodes, privileged-architecture.md "What translation rejects": 0 means
// no valid mapping was found and 1 means a mapping was found and the access violates it.
namespace page_fault_subcode {
inline constexpr std::uint8_t kNoMapping = 0;
inline constexpr std::uint8_t kPermission = 1;
}  // namespace page_fault_subcode

// The page-table entry, privileged-architecture.md "The page-table entry". Five bits and one
// field, and no name for anything else in the word.
namespace pte {
inline constexpr std::uint64_t kValid = std::uint64_t{1} << 0;
inline constexpr std::uint64_t kReadable = std::uint64_t{1} << 1;
inline constexpr std::uint64_t kWritable = std::uint64_t{1} << 2;
inline constexpr std::uint64_t kExecutable = std::uint64_t{1} << 3;
inline constexpr std::uint64_t kUserAccessible = std::uint64_t{1} << 4;
// The permission bits an entry carries, which are also exactly what a cached translation
// keeps: the three access permissions and the user-accessible bit.
inline constexpr std::uint64_t kPermissionMask =
    kReadable | kWritable | kExecutable | kUserAccessible;
// Bits 63 through 12 of a physical address. The low 12 bits of the field are the entry's other
// bits and are never part of the address, so extracting the address is a mask rather than a
// shift pair.
inline constexpr std::uint64_t kAddressMask = ~std::uint64_t{0xFFF};
}  // namespace pte

// Sv48 geometry, privileged-architecture.md "Sv48 translation".
namespace sv48 {
inline constexpr unsigned kLevels = 4;                 // levels 3, 2, 1 and 0
inline constexpr unsigned kPageOffsetBits = 12;        // bits 11:0 of the virtual address
inline constexpr unsigned kIndexBits = 9;              // 512 entries per table
inline constexpr std::uint64_t kIndexMask = 0x1FFu;
inline constexpr std::uint64_t kEntryBytes = 8;
inline constexpr std::uint64_t kEntriesPerTable = 512;
inline constexpr std::uint64_t kTableBytes = kEntryBytes * kEntriesPerTable;  // one 4 KiB page
// Bits 63 through 48 take no part in translation and no canonical-form check rejects them, so
// every address the walk sees is first reduced to its low 48 bits.
inline constexpr std::uint64_t kTranslatedMask = (std::uint64_t{1} << 48) - 1;

// The shift of the index field a given level selects on: 39 at level 3, 30 at level 2, 21 at
// level 1 and 12 at level 0.
constexpr unsigned index_shift(unsigned level) {
    return kPageOffsetBits + kIndexBits * level;
}

// The size, in bytes, of the page a leaf at this level maps: 4 KiB, 2 MiB, 1 GiB, 512 GiB.
constexpr std::uint64_t page_bytes(unsigned level) {
    return std::uint64_t{1} << index_shift(level);
}

// The offset bits a leaf at this level takes from the virtual address, which is also the mask a
// superpage's physical address field must have clear to be aligned.
constexpr std::uint64_t page_offset_mask(unsigned level) {
    return page_bytes(level) - 1;
}
}  // namespace sv48

// What one translation decided. `ok` false carries the trap record the chapter names for the
// condition, with the cause, the subcode and the auxiliary word set and the captured program
// counter left to the raise site, which is the only place that knows it.
struct TranslationResult {
    bool ok = false;
    std::uint64_t physical = 0;
    TrapV2 trap{};
};

// Does a leaf carrying these permission bits admit this access at this privilege level? The
// walk and the cache both call this, which is the whole of why a cached translation cannot come
// to disagree with the entry it was built from.
constexpr bool leaf_permits(std::uint64_t permissions, AccessKind kind, Privilege level) {
    if (level == Privilege::User && (permissions & pte::kUserAccessible) == 0u) {
        return false;
    }
    switch (kind) {
        case AccessKind::Fetch: return (permissions & pte::kExecutable) != 0u;
        case AccessKind::Load: return (permissions & pte::kReadable) != 0u;
        case AccessKind::Store: return (permissions & pte::kWritable) != 0u;
    }
    return false;
}

class TranslatorV2 {
  public:
    // How many translations the cache holds. The chapter makes the cache architecturally
    // invisible, so this number is an implementation choice and nothing observable rests on it:
    // a machine that caches nothing is fully conforming, and so is one that caches everything.
    static constexpr unsigned kCapacity = 32;

    // Translate one virtual address for one access kind at one privilege level.
    //
    // `memory` is const because a walk READS page tables and the machine never writes any part
    // of any entry. The accessed and dirty bits are software-managed, so there is no update
    // step here to get wrong, and the const is what makes that structural rather than a promise.
    TranslationResult translate(const MemoryV2& memory, std::uint64_t paging_root_value,
                                Privilege level, AccessKind kind,
                                std::uint64_t virtual_address) {
        if ((paging_root_value & paging_root::kModeMask) == paging_root::kModeBare) {
            // Bare mode: the physical address IS the virtual address, no page table is read, and
            // no page fault is possible. An address outside populated memory still raises the
            // physical-memory fault, but that is the caller's judgement, not this function's.
            TranslationResult result;
            result.ok = true;
            result.physical = virtual_address;
            return result;
        }

        const std::uint64_t translated = virtual_address & sv48::kTranslatedMask;

        if (const CachedTranslation* hit = lookup(translated); hit != nullptr) {
            ++hits_;
            return finish(*hit, translated, virtual_address, kind, level);
        }

        ++walks_;
        // The walk begins at the root field with the low 12 bits taken as zero, and reads the
        // entry the level-3 index selects.
        std::uint64_t table = paging_root_value & pte::kAddressMask;
        for (unsigned walk_level = sv48::kLevels; walk_level-- > 0;) {
            const std::uint64_t index = (translated >> sv48::index_shift(walk_level)) & sv48::kIndexMask;
            const std::uint64_t entry_address = table + index * sv48::kEntryBytes;

            // A page-table read is a PHYSICAL access and is never itself translated, so a read
            // naming an address outside populated memory raises cause 11 rather than a page
            // fault, and its auxiliary word is that physical address.
            std::uint64_t inaccessible = 0;
            if (!memory.check_range(entry_address, sv48::kEntryBytes, inaccessible)) {
                return fault(cause::kPhysicalMemoryFault, 0, inaccessible);
            }
            const std::uint64_t entry =
                memory.read_little_endian(entry_address, sv48::kEntryBytes);

            // Reject 1. V clear names no mapping, at any level.
            if ((entry & pte::kValid) == 0u) {
                return page_fault(kind, page_fault_subcode::kNoMapping, virtual_address);
            }

            const bool is_leaf =
                (entry & (pte::kReadable | pte::kWritable | pte::kExecutable)) != 0u;
            if (!is_leaf) {
                // Reject 4. A non-leaf at level 0 has nothing left to descend to.
                if (walk_level == 0) {
                    return page_fault(kind, page_fault_subcode::kNoMapping, virtual_address);
                }
                table = entry & pte::kAddressMask;
                continue;
            }

            // Reject 2. W set with R clear is a reserved encoding, and the machine rejects it
            // as an invalid entry rather than honoring it as a write-only page.
            if ((entry & pte::kWritable) != 0u && (entry & pte::kReadable) == 0u) {
                return page_fault(kind, page_fault_subcode::kNoMapping, virtual_address);
            }

            const std::uint64_t offset_mask = sv48::page_offset_mask(walk_level);
            const std::uint64_t physical_base = entry & pte::kAddressMask;

            // Reject 3. A superpage leaf whose physical address field has any nonzero bit below
            // its level's page boundary is misaligned, and the machine rejects it rather than
            // aliasing it to an aligned address. At level 0 the mask is the 12 bits the address
            // field does not cover, so the test is vacuous there and is written to apply at
            // every level rather than being guarded by one.
            if ((physical_base & offset_mask) != 0u) {
                return page_fault(kind, page_fault_subcode::kNoMapping, virtual_address);
            }

            CachedTranslation entry_record;
            entry_record.virtual_base = translated & ~offset_mask;
            entry_record.physical_base = physical_base;
            entry_record.offset_mask = offset_mask;
            entry_record.permissions = entry & pte::kPermissionMask;
            insert(entry_record);

            // Rejects 5 and 6, the permission violations, run through the same test a cache hit
            // runs, which is what keeps the two from drifting apart.
            return finish(entry_record, translated, virtual_address, kind, level);
        }

        // Unreachable: the level-0 iteration either returns a leaf or takes reject 4.
        return page_fault(kind, page_fault_subcode::kNoMapping, virtual_address);
    }

    // Invalidating event 2. Discards every cached translation.
    void invalidate_all() { valid_.fill(false); }

    // Invalidating event 3. Discards any cached translation for the page containing this virtual
    // address. D-3 read "the page" as the extent of the leaf that was actually cached, up to a
    // full superpage, because privileged-architecture.md never names a 4 KiB granule for a cache
    // entry and a cached translation comes from a leaf that may span 2 MiB, 1 GiB or 512 GiB.
    // That reading needed no interpretive licence in the end: instruction-reference-control.md
    // lines 672 through 678 settles it in the instruction's own entry, saying "The low bits of
    // the address within the page are ignored, so any address in the page names the page" and
    // then "A machine may discard more translations than the instruction names, up to and
    // including all of them." Discarding the whole superpage is permitted outright, so no
    // granularity below the leaf can ever be required of any machine.
    void invalidate_address(std::uint64_t virtual_address) {
        const std::uint64_t translated = virtual_address & sv48::kTranslatedMask;
        for (unsigned i = 0; i < kCapacity; ++i) {
            if (valid_[i] && (translated & ~entries_[i].offset_mask) == entries_[i].virtual_base) {
                valid_[i] = false;
            }
        }
    }

    // Implementation-visible counters, reachable from no instruction. The cache is
    // architecturally invisible, so nothing a conformance suite asserts may rest on these; they
    // exist so THIS machine's caching can be tested for over-flushing and under-flushing, which
    // the chapter's own rules cannot distinguish (see fixtures_paging.cpp).
    std::uint64_t walks() const { return walks_; }
    std::uint64_t hits() const { return hits_; }
    unsigned cached_count() const {
        unsigned count = 0;
        for (unsigned i = 0; i < kCapacity; ++i) {
            if (valid_[i]) {
                ++count;
            }
        }
        return count;
    }

  private:
    struct CachedTranslation {
        std::uint64_t virtual_base = 0;   // the mapped page's base, in the low 48 bits
        std::uint64_t physical_base = 0;  // the leaf's address field
        std::uint64_t offset_mask = 0;    // the mapped page's size minus one
        std::uint64_t permissions = 0;    // R, W, X and U, in their page-table-entry positions
    };

    static TranslationResult fault(std::uint8_t cause_number, std::uint8_t subcode_number,
                                   std::uint64_t aux) {
        TranslationResult result;
        result.ok = false;
        result.trap.cause = cause_number;
        result.trap.subcode = subcode_number;
        result.trap.aux = aux;
        return result;
    }

    static TranslationResult page_fault(AccessKind kind, std::uint8_t subcode_number,
                                        std::uint64_t virtual_address) {
        return fault(page_fault_cause(kind), subcode_number, virtual_address);
    }

    // The last step of a translation, shared by the walk and by a cache hit: check the leaf's
    // permissions against this access and this privilege level, then form the physical address
    // from the leaf's address field and the virtual address's offset bits.
    static TranslationResult finish(const CachedTranslation& record, std::uint64_t translated,
                                    std::uint64_t virtual_address, AccessKind kind,
                                    Privilege level) {
        if (!leaf_permits(record.permissions, kind, level)) {
            return page_fault(kind, page_fault_subcode::kPermission, virtual_address);
        }
        TranslationResult result;
        result.ok = true;
        result.physical = record.physical_base | (translated & record.offset_mask);
        return result;
    }

    const CachedTranslation* lookup(std::uint64_t translated) const {
        for (unsigned i = 0; i < kCapacity; ++i) {
            if (valid_[i] && (translated & ~entries_[i].offset_mask) == entries_[i].virtual_base) {
                return &entries_[i];
            }
        }
        return nullptr;
    }

    void insert(const CachedTranslation& record) {
        entries_[next_] = record;
        valid_[next_] = true;
        next_ = (next_ + 1) % kCapacity;
    }

    std::array<CachedTranslation, kCapacity> entries_{};
    std::array<bool, kCapacity> valid_{};
    unsigned next_ = 0;
    std::uint64_t walks_ = 0;
    std::uint64_t hits_ = 0;
};

}  // namespace maize::v2

#endif  // MAIZE_V2_TRANSLATE_V2_H
