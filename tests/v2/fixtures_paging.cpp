// fixtures_paging.cpp (maize-465): Sv48 translation, what it rejects, and the translation cache.
//
// privileged-architecture.md, "Address translation" through "The translation cache", is what
// these fixtures test. The page-table format is unchanged from Maize v1 and the walk over it is
// the one part of the chapter a v1 implementation could be lifted for, so the fixtures here
// spend most of their weight on the parts that CANNOT be lifted:
//
//   1. The six rejections, each asserted by cause AND subcode rather than by "something
//      faulted". A machine that rejects a write-only leaf with the wrong subcode is wrong in a
//      way that only a handler discovers, and a handler is exactly what reads the subcode.
//   2. The entry's read discipline. A leaf carrying garbage in G, A, D and the software bits
//      translates identically to one carrying zeros, and no entry is written by any access.
//   3. The cache, whose three invalidating events are stated and whose permission re-check is
//      stated, and whose behaviour in every OTHER respect the chapter declares nondeterministic.
//
// TWO KINDS OF ASSERTION LIVE IN THIS FILE, AND THE DIFFERENCE MATTERS.
//
// Most fixtures assert conformance: a machine that fails them is not a Maize v2 machine. One,
// the_translation_cache_neither_over_flushes_nor_under_flushes, asserts THIS implementation's
// caching through counters no guest instruction can read. It is not a conformance test and must
// never become one, because the chapter says outright that a machine which caches nothing
// conforms and that an un-invalidated edit may be observed either way. That leaves a real gap:
// nothing a conformance suite may assert can tell an over-flushing machine from a correct one.
// The counters close it for this machine only, and the fixture says so where it stands.
//
// NUMBERS ARE ASSERTED IN PLAIN DIGITS in sv48_geometry_and_entry_bits_are_the_chapters_numbers.
// Every other fixture uses the named constants, which is readable and which also means those
// fixtures would pass unchanged if a constant were given the wrong value. The digits fixture is
// the one that would not, and it is the reason the others may use names.

#include <cstdio>
#include <vector>

#include "fixture_support.h"

namespace maize::v2::test {
namespace {

// The address map these fixtures run on. Populated memory is one contiguous region in this
// build, so every physical address below is inside it except where a fixture deliberately names
// one that is not.
constexpr std::uint64_t kMemoryBytes = 0x20000;   // 128 KiB
constexpr std::uint64_t kProgramBase = 0x0100;
constexpr std::uint64_t kHandlerBase = 0x0600;
constexpr std::uint64_t kVectorTable = 0x1000;    // 2 KiB aligned, through $17FF
constexpr std::uint64_t kTrapStackTop = 0x2000;
constexpr std::uint64_t kRootTable = 0x8000;      // 4 KiB aligned, as every page table must be
constexpr std::uint64_t kFreeTables = 0x9000;     // tables the mapper allocates, upward
constexpr std::uint64_t kDataPage = 0x1E000;      // a physical page a fixture maps and touches
constexpr std::uint64_t kOtherPage = 0x1F000;     // a second one, for the cache fixtures

// The virtual addresses the fixtures map. Deliberately far from the identity-mapped low memory,
// so a machine that ignored the walk and returned the virtual address unchanged would name
// unpopulated physical memory and fail loudly rather than quietly passing.
constexpr std::uint64_t kTestVirtual = 0x0000000040000000ull;   // 1 GiB, level-2 index 1
constexpr std::uint64_t kSecondVirtual = 0x0000000040001000ull;  // the next 4 KiB page along
// A virtual address whose level-3 index is 1 rather than 0, so every table on its path is its
// own. The identity map of low memory sits under level-3 index 0, and a fixture that clears or
// rewrites an entry at level 3 would otherwise unmap the program it is running.
constexpr std::uint64_t kFarVirtual = 0x0000008000000000ull;  // 512 GiB

constexpr std::uint64_t kSentinel = 0x0123456789ABCDEFull;
constexpr std::uint64_t kReplacement = 0xFEDCBA9876543210ull;

// The permission sets, spelled as the chapter spells them.
constexpr std::uint64_t kLeafRWX = pte::kValid | pte::kReadable | pte::kWritable |
                                   pte::kExecutable;
constexpr std::uint64_t kLeafRWXU = kLeafRWX | pte::kUserAccessible;

// A page-table builder. It allocates the intermediate tables a mapping needs, one 4 KiB page at
// a time, and hands back the physical address of the leaf entry it wrote so a fixture can edit
// that entry later without re-deriving where it went.
class PageTables {
  public:
    PageTables(MemoryV2& memory, std::uint64_t root_table, std::uint64_t free_base)
        : memory_(&memory), root_table_(root_table), next_free_(free_base) {}

    // The value a kernel writes to paging_root: the root table's address in bits 63:12, and
    // mode 1 in bits 3:0.
    std::uint64_t root_value() const { return root_table_ | paging_root::kModeSv48; }
    std::uint64_t root_table() const { return root_table_; }

    // Map one page at `leaf_level` (0 for 4 KiB, 1 for 2 MiB, 2 for 1 GiB, 3 for 512 GiB) and
    // return the physical address of the leaf entry.
    std::uint64_t map(std::uint64_t virtual_address, std::uint64_t physical, std::uint64_t bits,
                      unsigned leaf_level = 0) {
        std::uint64_t table = root_table_;
        for (unsigned level = sv48::kLevels; level-- > leaf_level;) {
            const std::uint64_t entry_address = entry_in(table, virtual_address, level);
            if (level == leaf_level) {
                memory_->write_little_endian(entry_address, sv48::kEntryBytes, physical | bits);
                return entry_address;
            }
            std::uint64_t entry =
                memory_->read_little_endian(entry_address, sv48::kEntryBytes);
            if ((entry & pte::kValid) == 0u) {
                const std::uint64_t fresh = allocate_table();
                entry = fresh | pte::kValid;  // a non-leaf: V set, R, W and X all clear
                memory_->write_little_endian(entry_address, sv48::kEntryBytes, entry);
            }
            table = entry & pte::kAddressMask;
        }
        return 0;
    }

    // The physical address of the entry a given level's index selects in a given table.
    static std::uint64_t entry_in(std::uint64_t table, std::uint64_t virtual_address,
                                  unsigned level) {
        const std::uint64_t index =
            (virtual_address >> sv48::index_shift(level)) & sv48::kIndexMask;
        return table + index * sv48::kEntryBytes;
    }

    std::uint64_t allocate_table() {
        const std::uint64_t table = next_free_;
        next_free_ += sv48::kTableBytes;
        for (std::uint64_t offset = 0; offset < sv48::kTableBytes; offset += sv48::kEntryBytes) {
            memory_->write_little_endian(table + offset, sv48::kEntryBytes, 0);
        }
        return table;
    }

  private:
    MemoryV2* memory_;
    std::uint64_t root_table_;
    std::uint64_t next_free_;
};

// A machine with page tables, a kernel's trap state, and a program that turns translation on.
//
// The low memory is identity-mapped, so the program, the vector table, the trap stack and the
// page tables themselves are reachable at the same addresses before and after the paging_root
// write. That is what lets a fixture enable translation in the middle of a program and keep
// executing, and it is also how a real kernel does it.
class Paged {
  public:
    Paged() : machine_(static_cast<std::size_t>(kMemoryBytes)),
              tables_(machine_.memory(), kRootTable, kFreeTables),
              program_(kProgramBase) {
        // Every table starts zeroed because populated memory does, so the root needs no
        // preparation beyond existing.
    }

    Machine& machine() { return machine_; }
    Encoder& program() { return program_; }
    PageTables& tables() { return tables_; }
    CsrFileV2& csr() { return machine_.interpreter().csr(); }
    TranslatorV2& translator() { return machine_.interpreter().translator(); }

    // Identity-map [0, kMemoryBytes) with 4 KiB pages.
    void identity_map(std::uint64_t bits = kLeafRWX) {
        for (std::uint64_t page = 0; page < kMemoryBytes; page += sv48::page_bytes(0)) {
            tables_.map(page, page, bits);
        }
    }

    // Identity-map the low memory through ONE leaf at the named level, which is how the
    // superpage fixtures cover a program, its stack and its tables with a single entry.
    void identity_map_superpage(unsigned level, std::uint64_t bits = kLeafRWX) {
        tables_.map(0, 0, bits, level);
    }

    // Setup instructions, counted so a fixture can run them all and then step the one
    // instruction it is actually asserting on.
    void emit_move(unsigned number, std::uint64_t value) {
        program_.op_r_i8(op::kMoveW, reg(number), value);
        ++setup_steps_;
    }

    void emit_csr_write(std::uint16_t number, std::uint64_t value, unsigned via = 1) {
        emit_move(via, value);
        program_.op_r_i2(op::kCsrWrite, reg(via), number);
        ++setup_steps_;
    }

    void emit_csr_read(unsigned number, std::uint16_t csr_number) {
        program_.op_r_i2(op::kCsrRead, reg(number), csr_number);
        ++setup_steps_;
    }

    // The instruction that turns translation on, and the whole of what a kernel does to switch
    // address spaces.
    void emit_enable() { emit_csr_write(csr::kPagingRoot, tables_.root_value()); }

    void emit_enter_user() { emit_csr_write(csr::kStatus, 0x0); }

    // A load emitted as part of the setup, for a fixture whose setup has to touch memory before
    // the instruction it is actually asserting on.
    void emit_load(unsigned base, unsigned destination) {
        program_.op_r_r(op::kLoad, reg(base), reg(destination));
        ++setup_steps_;
    }

    // A vector-table entry, written straight into physical memory the way a boot loader would.
    void install_handler(std::uint8_t cause_number, std::uint64_t address) {
        machine_.memory().write_little_endian(
            vector_table::entry_address(kVectorTable, cause_number), 8, address);
    }

    void install_all_handlers(std::uint64_t address) {
        for (unsigned number = 0; number < vector_table::kEntryCount; ++number) {
            install_handler(static_cast<std::uint8_t>(number), address);
        }
    }

    void load_image(const Encoder& image) {
        const bool ok = machine_.memory().load_image(image.base_address(), image.bytes().data(),
                                                     image.bytes().size());
        V2_CHECK(ok);
    }

    // The address the next emitted instruction will occupy, which is the address a fixture
    // records for the instruction it expects to fault.
    std::uint64_t here() const { return program_.current_address(); }

    // Emit the trap state a handler needs, before anything else. Written through csr_write so
    // each register's own value validation runs on the way in.
    void emit_kernel_preamble() {
        emit_csr_write(csr::kTrapVectorBase, kVectorTable);
        emit_csr_write(csr::kTrapStack, kTrapStackTop);
    }

    void start() {
        machine_.load(program_);
    }

    // Run every setup instruction, so the next step() is the instruction under test.
    void run_setup() {
        for (unsigned i = 0; i < setup_steps_; ++i) {
            const StepResult result = machine_.step();
            if (result.status != StepStatus::Advanced) {
                record_failure("paging fixture setup did not complete");
                return;
            }
        }
    }

    StepResult step() { return machine_.step(); }

  private:
    Machine machine_;
    PageTables tables_;
    Encoder program_;
    unsigned setup_steps_ = 0;
};

// One rejection case, driven through a real load so the cause, the subcode, the auxiliary word
// and the captured program counter are all asserted at once.
void expect_load_page_fault(const char* what, std::uint8_t expected_subcode,
                            void (*arrange)(Paged&)) {
    Paged paged;
    paged.identity_map();
    arrange(paged);
    paged.emit_enable();
    paged.emit_move(2, kTestVirtual);
    const std::uint64_t faulting = paged.here();
    paged.program().op_r_r(op::kLoad, reg(2), reg(3));
    paged.program().halt();
    paged.start();
    paged.run_setup();

    // Cause 9 is the load's page fault, and the auxiliary word is the faulting VIRTUAL address
    // exactly as the instruction computed it.
    expect_trap(paged.step(), cause::kPageFaultLoad, expected_subcode, kTestVirtual, faulting,
                what);
    // The destination register was not written, because a faulting instruction takes no
    // architectural effect and is meant to run again.
    V2_CHECK_EQ(paged.machine().get(3), 0u);
}

}  // namespace

V2_FIXTURE(sv48_geometry_and_entry_bits_are_the_chapters_numbers) {
    // Every number in this fixture is written in plain digits, on purpose. Everywhere else in
    // the suite a cause, a subcode, a shift or a page size is spelled with the constant that
    // names it, which reads better and which also means the assertion would still pass if the
    // constant itself were wrong: the machine and the expectation would be renamed together.
    // This fixture is what makes that safe, and it is the answer to "if the implementation's
    // copy of this value were wrong, would this line still pass?"

    // "bits 47:39 the level-3 index, 38:30 the level-2 index, 29:21 the level-1 index,
    //  20:12 the level-0 index, 11:0 the offset within the page"
    V2_CHECK_EQ(sv48::index_shift(0), 12u);
    V2_CHECK_EQ(sv48::index_shift(1), 21u);
    V2_CHECK_EQ(sv48::index_shift(2), 30u);
    V2_CHECK_EQ(sv48::index_shift(3), 39u);
    V2_CHECK_EQ(sv48::kIndexMask, 511u);

    // "2 MiB at level 1, 1 GiB at level 2, and 512 GiB at level 3."
    V2_CHECK_EQ(sv48::page_bytes(0), 4096u);
    V2_CHECK_EQ(sv48::page_bytes(1), 2097152u);
    V2_CHECK_EQ(sv48::page_bytes(2), 1073741824u);
    V2_CHECK_EQ(sv48::page_bytes(3), 549755813888u);

    // "A page table occupies one 4 KiB page and holds 512 entries of 8 bytes each."
    V2_CHECK_EQ(sv48::kEntriesPerTable, 512u);
    V2_CHECK_EQ(sv48::kEntryBytes, 8u);
    V2_CHECK_EQ(sv48::kTableBytes, 4096u);
    V2_CHECK_EQ(sv48::kLevels, 4u);

    // "bit 0 V, bit 1 R, bit 2 W, bit 3 X, bit 4 U", and bits 63:12 the physical address.
    V2_CHECK_EQ(pte::kValid, 1u);
    V2_CHECK_EQ(pte::kReadable, 2u);
    V2_CHECK_EQ(pte::kWritable, 4u);
    V2_CHECK_EQ(pte::kExecutable, 8u);
    V2_CHECK_EQ(pte::kUserAccessible, 16u);
    V2_CHECK_EQ(pte::kAddressMask, 0xFFFFFFFFFFFFF000ull);
    // G is bit 5, A is bit 6, D is bit 7 and bits 11:8 are software's. The machine reads none of
    // them, so the mask of what it DOES read has none of them set: 1 + 2 + 4 + 8 + 16 = 31.
    V2_CHECK_EQ(pte::kValid | pte::kPermissionMask, 31u);

    // "bits 3:0 mode, 0 = bare, 1 = Sv48, 2 through 15 reserved; bits 11:4 reserved."
    V2_CHECK_EQ(paging_root::kModeBare, 0u);
    V2_CHECK_EQ(paging_root::kModeSv48, 1u);
    V2_CHECK_EQ(paging_root::kModeMask, 15u);
    V2_CHECK_EQ(paging_root::kReservedMask, 4080u);  // bits 11:4

    // "cause 8 for an instruction fetch, cause 9 for a load, and cause 10 for a store", and
    // subcode 0 for no valid mapping, 1 for a mapping the access violates.
    V2_CHECK_EQ(page_fault_cause(AccessKind::Fetch), 8u);
    V2_CHECK_EQ(page_fault_cause(AccessKind::Load), 9u);
    V2_CHECK_EQ(page_fault_cause(AccessKind::Store), 10u);
    V2_CHECK_EQ(page_fault_subcode::kNoMapping, 0u);
    V2_CHECK_EQ(page_fault_subcode::kPermission, 1u);
    V2_CHECK_EQ(cause::kPhysicalMemoryFault, 11u);
}

V2_FIXTURE(bare_mode_translates_every_address_to_itself) {
    // "In bare mode the physical address equals the virtual address, no page table is consulted,
    // and no access raises a page fault. Bare mode removes the page fault and nothing else."
    //
    // The fixture builds NO page tables at all, which is the sharper form of the claim: an
    // access to an address for which no page table exists is not a fault in bare mode, it is an
    // access to that physical address.
    {
        Paged paged;
        paged.emit_move(2, 0x1234);
        paged.emit_move(3, kSentinel);
        paged.program().op_r_r(op::kStore, reg(3), reg(2));
        paged.program().op_r_r(op::kLoad, reg(2), reg(4));
        paged.program().halt();
        paged.start();
        paged.run_setup();

        V2_CHECK(paged.step().status == StepStatus::Advanced);
        V2_CHECK(paged.step().status == StepStatus::Advanced);
        expect_halted(paged.machine().run(), "bare mode store and load");
        V2_CHECK_EQ(paged.machine().get(4), kSentinel);
        // The store reached physical $1234, which is what "the physical address equals the
        // virtual address" means when read from outside the machine.
        V2_CHECK_EQ(paged.machine().memory().read_little_endian(0x1234, 8), kSentinel);
        // No walk happened, because bare mode consults no page table.
        V2_CHECK_EQ(paged.translator().walks(), 0u);
    }

    // A paging_root holding a root address but mode 0 is still bare. Only the mode field decides.
    {
        Paged paged;
        paged.emit_csr_write(csr::kPagingRoot, 0x8000);  // root recorded, mode 0
        paged.emit_move(2, 0x1234);
        paged.program().op_r_r(op::kLoad, reg(2), reg(4));
        paged.program().halt();
        paged.start();
        paged.run_setup();
        V2_CHECK(paged.step().status == StepStatus::Advanced);
        V2_CHECK_EQ(paged.translator().walks(), 0u);
    }

    // "An access whose physical address lies outside populated memory still raises the
    // physical-memory fault, cause 11", and it is cause 11 rather than any page fault.
    {
        Paged paged;
        paged.emit_move(2, kMemoryBytes);
        const std::uint64_t faulting = paged.here();
        paged.program().op_r_r(op::kLoad, reg(2), reg(4));
        paged.program().halt();
        paged.start();
        paged.run_setup();
        expect_trap(paged.step(), cause::kPhysicalMemoryFault, 0, kMemoryBytes, faulting,
                    "bare mode outside populated memory");
    }
}

V2_FIXTURE(sv48_walk_reads_the_indices_the_chapter_names) {
    // The walk, checked against tables this fixture builds with the chapter's SHIFTS WRITTEN OUT
    // rather than through the mapper the other fixtures use. A machine that indexed level 2 with
    // bits 38:30 of the wrong operand, or that walked the levels in the other order, reads an
    // entry this fixture left invalid and faults.
    //
    // The virtual address below has a different nonzero index at each of the four levels, so no
    // two levels can be confused for one another, and its offset is nonzero so the offset is not
    // confusable with an index either.
    constexpr std::uint64_t kIndex3 = 0x005;
    constexpr std::uint64_t kIndex2 = 0x0C1;
    constexpr std::uint64_t kIndex1 = 0x1A2;
    constexpr std::uint64_t kIndex0 = 0x073;
    constexpr std::uint64_t kOffset = 0x0AB;
    const std::uint64_t address = (kIndex3 << 39) | (kIndex2 << 30) | (kIndex1 << 21) |
                                  (kIndex0 << 12) | kOffset;

    Paged paged;
    PageTables& tables = paged.tables();
    const std::uint64_t level2_table = tables.allocate_table();
    const std::uint64_t level1_table = tables.allocate_table();
    const std::uint64_t level0_table = tables.allocate_table();
    MemoryV2& memory = paged.machine().memory();
    memory.write_little_endian(tables.root_table() + kIndex3 * 8, 8, level2_table | 1);
    memory.write_little_endian(level2_table + kIndex2 * 8, 8, level1_table | 1);
    memory.write_little_endian(level1_table + kIndex1 * 8, 8, level0_table | 1);
    memory.write_little_endian(level0_table + kIndex0 * 8, 8, kDataPage | kLeafRWX);

    TranslatorV2 translator;
    const TranslationResult result = translator.translate(
        memory, tables.root_value(), Privilege::Supervisor, AccessKind::Load, address);
    V2_CHECK(result.ok);
    // The offset is the low 12 bits, carried through untouched.
    V2_CHECK_EQ(result.physical, kDataPage + kOffset);

    // "Bits 63 through 48 of the virtual address are ignored: they take no part in the
    // translation, and no canonical-form check rejects them." A machine that rejected a
    // non-canonical address, or that let those bits into the level-3 index, fails here.
    for (const std::uint64_t high : {0xFFFFull, 0x8000ull, 0x1234ull}) {
        const TranslationResult ignored = translator.translate(
            memory, tables.root_value(), Privilege::Supervisor, AccessKind::Load,
            address | (high << 48));
        V2_CHECK(ignored.ok);
        V2_CHECK_EQ(ignored.physical, kDataPage + kOffset);
    }

    // The root field is bits 63:12 with the low 12 taken as zero, so a paging_root whose mode
    // nibble is set still names the same table. That is already implied above (the root value
    // carries mode 1), and this states it: the walk began at $8000 and not at $8001.
    V2_CHECK_EQ(tables.root_value() & pte::kAddressMask, tables.root_table());
}

V2_FIXTURE(sv48_translation_carries_a_program_and_its_data) {
    // Translation on, a program running out of an identity-mapped low memory, and a store and a
    // load through a 4 KiB page mapped somewhere else entirely. The store's bytes have to land
    // at the PHYSICAL page, which is the whole difference between a machine that translates and
    // one that returns the address it was given.
    Paged paged;
    paged.identity_map();
    paged.tables().map(kTestVirtual, kDataPage, kLeafRWX);
    paged.emit_enable();
    // Read the register back through the guest's own csr_read, immediately after the write that
    // turned translation on. This is the one place in the suite where a paging_root value naming
    // mode 1 is written and read back verbatim: fixtures_privilege.cpp's value-validation cases
    // use bare mode with a root recorded, because a mode-1 write there would translate the very
    // next fetch through a page table that fixture never built. That the register STORES what
    // was written, rather than merely acting on it, is asserted here instead.
    paged.emit_csr_read(5, csr::kPagingRoot);
    paged.emit_move(2, kTestVirtual + 0x18);
    paged.emit_move(3, kSentinel);
    paged.program().op_r_r(op::kStore, reg(3), reg(2));
    paged.program().op_r_r(op::kLoad, reg(2), reg(4));
    paged.program().halt();
    paged.start();
    paged.run_setup();

    V2_CHECK(paged.step().status == StepStatus::Advanced);
    V2_CHECK(paged.step().status == StepStatus::Advanced);
    expect_halted(paged.machine().run(), "translated store and load");
    V2_CHECK_EQ(paged.machine().get(4), kSentinel);
    V2_CHECK_EQ(paged.machine().memory().read_little_endian(kDataPage + 0x18, 8), kSentinel);
    V2_CHECK_EQ(paged.machine().get(5), paged.tables().root_value());
    V2_CHECK_EQ(paged.machine().get(5) & 0xFull, 1u);  // mode 1, Sv48, in plain digits
    // Nothing landed at the virtual address read as a physical one, which is the failure a
    // machine that skipped the walk would show.
    V2_CHECK(paged.translator().walks() > 0u);

    // An access that straddles the page boundary is contiguous in virtual addresses and need not
    // be in physical ones. The two pages below are adjacent virtually and far apart physically,
    // and an 8-byte store across the seam has to land four bytes in each.
    {
        Paged split;
        split.identity_map();
        split.tables().map(kTestVirtual, kDataPage, kLeafRWX);
        split.tables().map(kSecondVirtual, kOtherPage, kLeafRWX);
        split.emit_enable();
        split.emit_move(2, kTestVirtual + 0xFFC);  // four bytes short of the page end
        split.emit_move(3, kSentinel);
        split.program().op_r_r(op::kStore, reg(3), reg(2));
        split.program().op_r_r(op::kLoad, reg(2), reg(4));
        split.program().halt();
        split.start();
        split.run_setup();
        V2_CHECK(split.step().status == StepStatus::Advanced);
        V2_CHECK(split.step().status == StepStatus::Advanced);
        expect_halted(split.machine().run(), "a store across a page boundary");
        V2_CHECK_EQ(split.machine().get(4), kSentinel);
        V2_CHECK_EQ(split.machine().memory().read_little_endian(kDataPage + 0xFFC, 4),
                    kSentinel & 0xFFFFFFFFull);
        V2_CHECK_EQ(split.machine().memory().read_little_endian(kOtherPage, 4), kSentinel >> 32);
    }

    // WHICH ADDRESS A STRADDLING ACCESS REPORTS WHEN THE SECOND PAGE IS NOT THERE.
    //
    // instruction-reference-memory.md lines 115 through 118: "An access that crosses a page
    // boundary is one access for fault purposes rather than two. When more than one byte of the
    // access is inaccessible, the fault reports the lowest inaccessible address the access
    // covers, which makes the reported address a function of the access alone and not of the
    // order in which an implementation touches bytes." The same chapter lists it as a directly
    // testable property at lines 907 through 909: such an access "leaves the destination
    // register and every byte of memory unmodified, and reports the lowest inaccessible address
    // in the access."
    //
    // That rule lives in the instruction-reference chapter rather than in
    // privileged-architecture.md, which is why this card's own chapter reads as silent on it and
    // is not. The reported address is therefore a conformance property, not an implementation
    // choice, and it is pinned here.
    //
    // The expected auxiliary word is written in plain digits rather than derived from
    // kTestVirtual, because deriving it would restate whatever arithmetic the machine did. The
    // access begins at $40000FFC and covers eight bytes, so its first inaccessible byte is the
    // first byte of the unmapped second page, $40001000, and NOT the $40000FFC the instruction
    // computed. A machine reporting the instruction's start address fails these two cases.
    {
        // A load whose second page is invalid. Cause 9, subcode 0 (no valid mapping), and the
        // destination register untouched.
        Paged split;
        split.identity_map();
        split.tables().map(kTestVirtual, kDataPage, kLeafRWX);
        split.tables().map(kSecondVirtual, kOtherPage, 0);  // V clear: no mapping
        split.emit_enable();
        split.emit_move(2, kTestVirtual + 0xFFC);
        const std::uint64_t faulting = split.here();
        split.program().op_r_r(op::kLoad, reg(2), reg(4));
        split.program().halt();
        split.start();
        split.run_setup();

        expect_trap(split.step(), cause::kPageFaultLoad, page_fault_subcode::kNoMapping,
                    0x0000000040001000ull, faulting,
                    "a load whose second page is not present");
        V2_CHECK_EQ(split.machine().get(4), 0u);
    }
    {
        // The same seam under a store. Cause 10, the same address, and every byte of the FIRST
        // page still zero, because a partly-mapped access writes nothing at all.
        Paged split;
        split.identity_map();
        split.tables().map(kTestVirtual, kDataPage, kLeafRWX);
        split.tables().map(kSecondVirtual, kOtherPage, 0);
        split.emit_enable();
        split.emit_move(2, kTestVirtual + 0xFFC);
        split.emit_move(3, kSentinel);
        const std::uint64_t faulting = split.here();
        split.program().op_r_r(op::kStore, reg(3), reg(2));
        split.program().halt();
        split.start();
        split.run_setup();

        expect_trap(split.step(), cause::kPageFaultStore, page_fault_subcode::kNoMapping,
                    0x0000000040001000ull, faulting,
                    "a store whose second page is not present");
        V2_CHECK_EQ(split.machine().memory().read_little_endian(kDataPage + 0xFFC, 4), 0u);
    }
}

V2_FIXTURE(superpages_map_their_whole_range_at_every_level) {
    // "A leaf above level 0 maps a superpage: 2 MiB at level 1, 1 GiB at level 2, and 512 GiB at
    // level 3. The physical address of the translated byte is the leaf's physical address field
    // with the virtual address's offset bits below that level's page boundary substituted in."
    //
    // The first half runs a real program under each of the three, with the leaf's address field
    // zero, so the superpage identity-maps the low memory and the program, its data and its
    // page tables are all inside one entry.
    for (unsigned level = 1; level <= 3; ++level) {
        Paged paged;
        paged.identity_map_superpage(level);
        paged.emit_enable();
        paged.emit_move(2, 0x1240);
        paged.emit_move(3, kSentinel);
        paged.program().op_r_r(op::kStore, reg(3), reg(2));
        paged.program().op_r_r(op::kLoad, reg(2), reg(4));
        paged.program().halt();
        paged.start();
        paged.run_setup();

        char buffer[96];
        std::snprintf(buffer, sizeof(buffer), "a superpage leaf at level %u", level);
        V2_CHECK(paged.step().status == StepStatus::Advanced);
        V2_CHECK(paged.step().status == StepStatus::Advanced);
        expect_halted(paged.machine().run(), buffer);
        check_equal_u64(paged.machine().get(4), kSentinel, buffer, __FILE__, __LINE__);
        check_equal_u64(paged.machine().memory().read_little_endian(0x1240, 8), kSentinel,
                        buffer, __FILE__, __LINE__);
        // One leaf covered every access the program made, so the walk ran exactly once: a
        // machine that mistook a superpage leaf for a non-leaf would have descended instead.
        check_equal_u64(paged.translator().walks(), 1u, buffer, __FILE__, __LINE__);
    }

    // The second half is the offset substitution itself, with a NONZERO leaf address at each
    // level, which no populated physical memory in this build could hold. The arithmetic is what
    // is under test, so it is asserted on the translation rather than on an access: a machine
    // that substituted the wrong number of offset bits lands on the wrong byte.
    struct Case {
        unsigned level;
        std::uint64_t leaf_physical;
        std::uint64_t offset;
    };
    const Case cases[] = {
        {1, 0x0000000180000000ull, 0x1FABCDull},        // 2 MiB: the low 21 bits
        {2, 0x0000004000000000ull, 0x3FABCDEFull},      // 1 GiB: the low 30 bits
        {3, 0x0000800000000000ull, 0x7FABCDEF12ull},    // 512 GiB: the low 39 bits
    };
    for (const Case& one : cases) {
        Paged paged;
        MemoryV2& memory = paged.machine().memory();
        const std::uint64_t base_virtual = std::uint64_t{0x21} << sv48::index_shift(one.level);
        paged.tables().map(base_virtual, one.leaf_physical, kLeafRWX, one.level);

        TranslatorV2 translator;
        const TranslationResult result =
            translator.translate(memory, paged.tables().root_value(), Privilege::Supervisor,
                                 AccessKind::Load, base_virtual + one.offset);
        char buffer[96];
        std::snprintf(buffer, sizeof(buffer), "offset substitution at level %u", one.level);
        check(result.ok, buffer, __FILE__, __LINE__);
        check_equal_u64(result.physical, one.leaf_physical + one.offset, buffer, __FILE__,
                        __LINE__);
    }
}

V2_FIXTURE(translation_rejects_an_invalid_entry_with_subcode_zero) {
    // The four structural rejections, each of which means no valid mapping exists and each of
    // which therefore delivers subcode 0. The subcode is asserted, not merely the fault: a
    // handler distinguishes "map this page" from "kill this process" by reading it.

    // Reject 1, V clear, at every one of the four levels. The mapping is built whole and then
    // the entry at one level is cleared, so exactly one thing differs between the four cases.
    for (unsigned level = 0; level < sv48::kLevels; ++level) {
        Paged paged;
        paged.identity_map();
        paged.tables().map(kFarVirtual, kDataPage, kLeafRWX);

        // Walk down to the named level with the fixture's own arithmetic and clear that entry.
        MemoryV2& memory = paged.machine().memory();
        std::uint64_t table = paged.tables().root_table();
        for (unsigned descending = sv48::kLevels; descending-- > level;) {
            const std::uint64_t entry_address =
                PageTables::entry_in(table, kFarVirtual, descending);
            if (descending == level) {
                memory.write_little_endian(entry_address, 8, 0);
                break;
            }
            table = memory.read_little_endian(entry_address, 8) & pte::kAddressMask;
        }

        paged.emit_enable();
        paged.emit_move(2, kFarVirtual);
        const std::uint64_t faulting = paged.here();
        paged.program().op_r_r(op::kLoad, reg(2), reg(3));
        paged.program().halt();
        paged.start();
        paged.run_setup();

        char buffer[96];
        std::snprintf(buffer, sizeof(buffer), "V clear at level %u", level);
        expect_trap(paged.step(), cause::kPageFaultLoad, page_fault_subcode::kNoMapping,
                    kFarVirtual, faulting, buffer);
    }

    // Reject 2. "A leaf with W set and R clear is a reserved encoding, and the machine rejects it
    // as an invalid entry rather than honoring it as a write-only page." A machine that honored
    // it would let the load below through with no fault at all, or fault with subcode 1.
    expect_load_page_fault("a leaf with W set and R clear", page_fault_subcode::kNoMapping,
                           [](Paged& paged) {
                               paged.tables().map(kTestVirtual, kDataPage,
                                                  pte::kValid | pte::kWritable);
                           });

    // Reject 3. A misaligned superpage, at each of the three levels that can hold one. The
    // offending bit is the lowest one below that level's page boundary, which is the smallest
    // misalignment there is and the one an implementation that masks instead of rejecting is
    // likeliest to swallow.
    for (unsigned level = 1; level <= 3; ++level) {
        Paged paged;
        paged.identity_map();
        // A leaf whose address field is off by one 4 KiB page from its level's boundary.
        const std::uint64_t misaligned = sv48::page_bytes(level) + sv48::page_bytes(0);
        paged.tables().map(kFarVirtual, misaligned, kLeafRWX, level);
        paged.emit_enable();
        paged.emit_move(2, kFarVirtual);
        const std::uint64_t faulting = paged.here();
        paged.program().op_r_r(op::kLoad, reg(2), reg(3));
        paged.program().halt();
        paged.start();
        paged.run_setup();

        char buffer[96];
        std::snprintf(buffer, sizeof(buffer), "a misaligned superpage at level %u", level);
        expect_trap(paged.step(), cause::kPageFaultLoad, page_fault_subcode::kNoMapping,
                    kFarVirtual, faulting, buffer);
    }

    // Reject 4. "A non-leaf entry at level 0 has nothing left to descend to, and the machine
    // treats it as no valid mapping." V set, R, W and X all clear, at the bottom of the walk.
    expect_load_page_fault("a non-leaf entry at level 0", page_fault_subcode::kNoMapping,
                           [](Paged& paged) {
                               paged.tables().map(kTestVirtual, kDataPage, pte::kValid);
                           });
}

V2_FIXTURE(translation_rejects_a_permission_violation_with_subcode_one) {
    // "A leaf that lacks the permission bit the access needs is a permission violation: a fetch
    // needs X, a load needs R, and a store needs W." Subcode 1, meaning a mapping was found and
    // the access violates it, and the cause names which kind of access asked.
    // A load from a leaf with R clear. W is clear too, because a leaf with W set and R clear is
    // a reserved encoding that would be rejected with subcode 0 by an earlier rule, and this
    // case is about the permission rule rather than that one.
    {
        Paged paged;
        paged.identity_map();
        paged.tables().map(kTestVirtual, kDataPage, pte::kValid | pte::kExecutable);
        paged.emit_enable();
        paged.emit_move(2, kTestVirtual);
        const std::uint64_t faulting = paged.here();
        paged.program().op_r_r(op::kLoad, reg(2), reg(3));
        paged.program().halt();
        paged.start();
        paged.run_setup();
        expect_trap(paged.step(), cause::kPageFaultLoad, page_fault_subcode::kPermission,
                    kTestVirtual, faulting, "a load from a leaf without R");
    }

    // A store to a leaf with W clear.
    {
        Paged paged;
        paged.identity_map();
        paged.tables().map(kTestVirtual, kDataPage,
                           pte::kValid | pte::kReadable | pte::kExecutable);
        paged.emit_enable();
        paged.emit_move(2, kTestVirtual);
        paged.emit_move(3, kSentinel);
        const std::uint64_t faulting = paged.here();
        paged.program().op_r_r(op::kStore, reg(3), reg(2));
        paged.program().halt();
        paged.start();
        paged.run_setup();
        expect_trap(paged.step(), cause::kPageFaultStore, page_fault_subcode::kPermission,
                    kTestVirtual, faulting, "a store to a leaf without W");
        // Nothing was written, which is the store's half of trap-writes-nothing.
        V2_CHECK_EQ(paged.machine().memory().read_little_endian(kDataPage, 8), 0u);
    }

    // A fetch from a leaf with X clear. The instruction lives at the mapped page, so the fault
    // is on the fetch itself: the captured program counter and the auxiliary word are both that
    // address, and no opcode was executed.
    {
        Paged paged;
        paged.identity_map();
        paged.tables().map(kTestVirtual, kDataPage,
                           pte::kValid | pte::kReadable | pte::kWritable);
        // The halt goes at the PHYSICAL page the leaf names, since the image loader writes
        // physical memory, and it is never reached: the fetch faults first.
        Encoder physical_image(kDataPage);
        physical_image.halt();
        paged.load_image(physical_image);

        paged.emit_enable();
        paged.emit_move(2, kTestVirtual);
        paged.program().op_r(op::kJumpReg, reg(2));
        paged.program().halt();
        paged.start();
        paged.run_setup();
        V2_CHECK(paged.step().status == StepStatus::Advanced);  // the jump lands
        expect_trap(paged.step(), cause::kPageFaultFetch, page_fault_subcode::kPermission,
                    kTestVirtual, kTestVirtual, "a fetch from a leaf without X");
    }

    // "A leaf with U clear accessed from user level is a permission violation. Supervisor may
    // reach a page whether U is set or clear." Both halves, on one mapping, differing only in
    // the privilege level the access runs at.
    for (unsigned at_user = 0; at_user <= 1; ++at_user) {
        Paged paged;
        paged.identity_map(kLeafRWXU);  // the code the machine runs is reachable from both levels
        paged.tables().map(kTestVirtual, kDataPage, kLeafRWX);  // U clear
        paged.emit_enable();
        paged.emit_move(2, kTestVirtual);
        if (at_user != 0) {
            paged.emit_enter_user();
        }
        const std::uint64_t faulting = paged.here();
        paged.program().op_r_r(op::kLoad, reg(2), reg(3));
        paged.program().halt();
        paged.start();
        paged.run_setup();

        const StepResult result = paged.step();
        if (at_user != 0) {
            expect_trap(result, cause::kPageFaultLoad, page_fault_subcode::kPermission,
                        kTestVirtual, faulting, "a user access to a leaf with U clear");
        } else {
            V2_CHECK(result.status == StepStatus::Advanced);
        }
    }
}

V2_FIXTURE(the_machine_reads_only_the_bits_the_chapter_names) {
    // "The machine reads five bits and one field: V, R, W, X, U, and the physical address. It
    // never reads G, A, D, or the software-available bits, and it never writes any part of any
    // entry."
    //
    // The leaf below carries G, A, D and all four software bits SET, and the same leaf carries
    // them all CLEAR in the second pass. A machine that required A before translating, or that
    // treated D as write permission, or that took the software bits as part of the address, has
    // to differ between the two passes or has to write the entry back.
    for (unsigned pass = 0; pass <= 1; ++pass) {
        const std::uint64_t ignored_bits = pass == 0 ? 0u : 0x0FE0u;  // G, A, D and bits 11:8
        Paged paged;
        paged.identity_map();
        const std::uint64_t leaf =
            paged.tables().map(kTestVirtual, kDataPage, kLeafRWX | ignored_bits);
        const std::uint64_t entry_before = paged.machine().memory().read_little_endian(leaf, 8);

        paged.emit_enable();
        paged.emit_move(2, kTestVirtual);
        paged.emit_move(3, kSentinel);
        paged.program().op_r_r(op::kStore, reg(3), reg(2));
        paged.program().op_r_r(op::kLoad, reg(2), reg(4));
        paged.program().halt();
        paged.start();
        paged.run_setup();

        char buffer[96];
        std::snprintf(buffer, sizeof(buffer), "ignored entry bits, pass %u", pass);
        V2_CHECK(paged.step().status == StepStatus::Advanced);
        V2_CHECK(paged.step().status == StepStatus::Advanced);
        expect_halted(paged.machine().run(), buffer);
        // The same physical address either way: the software bits took no part in it.
        check_equal_u64(paged.machine().get(4), kSentinel, buffer, __FILE__, __LINE__);
        check_equal_u64(paged.machine().memory().read_little_endian(kDataPage, 8), kSentinel,
                        buffer, __FILE__, __LINE__);
        // "it never writes any part of any entry": the accessed and dirty bits are software's,
        // and a store through this mapping left the entry byte for byte as it found it.
        check_equal_u64(paged.machine().memory().read_little_endian(leaf, 8), entry_before,
                        buffer, __FILE__, __LINE__);
    }
}

V2_FIXTURE(a_page_table_read_outside_memory_is_a_physical_memory_fault) {
    // "Page-table reads themselves are physical accesses that are never translated. A page-table
    // read that names a physical address outside populated memory therefore raises the
    // physical-memory fault rather than a page fault."
    //
    // Cause 11 rather than cause 9, and the auxiliary word is the PHYSICAL address of the
    // page-table read rather than the virtual address the instruction computed. A machine that
    // folded this into the page fault would tell a handler to map a page when what actually
    // happened is that the kernel's own tables point off the end of memory.
    {
        // The root itself, out of range.
        Paged paged;
        paged.identity_map();
        paged.emit_csr_write(csr::kPagingRoot, kMemoryBytes | paging_root::kModeSv48);
        // The instruction AFTER the paging_root write is the first thing fetched under the
        // broken root, so the fault arrives on that fetch rather than on any load.
        const std::uint64_t faulting = paged.here();
        paged.emit_move(2, kTestVirtual);
        paged.program().op_r_r(op::kLoad, reg(2), reg(3));
        paged.program().halt();
        paged.start();
        V2_CHECK(paged.step().status == StepStatus::Advanced);  // the move that loads the value
        V2_CHECK(paged.step().status == StepStatus::Advanced);  // the csr_write that enables it
        // The level-3 index of kTestVirtual is zero, so the read lands on the root table's first
        // entry, and the auxiliary word is that PHYSICAL address rather than a virtual one.
        expect_trap(paged.step(), cause::kPhysicalMemoryFault, 0, kMemoryBytes, faulting,
                    "a root table outside populated memory");
    }

    {
        // A level-1 table pointer out of range, reached partway down an otherwise sound walk.
        Paged paged;
        paged.identity_map();
        paged.tables().map(kTestVirtual, kDataPage, kLeafRWX);
        MemoryV2& memory = paged.machine().memory();
        // Follow the chain the mapper built as far as the level-2 entry, and point it off the
        // end of memory. The level-1 index of kTestVirtual is 0, so the read lands on the first
        // entry of the table that is not there.
        std::uint64_t table = paged.tables().root_table();
        const std::uint64_t level3 = PageTables::entry_in(table, kTestVirtual, 3);
        table = memory.read_little_endian(level3, 8) & pte::kAddressMask;
        const std::uint64_t level2 = PageTables::entry_in(table, kTestVirtual, 2);
        memory.write_little_endian(level2, 8, (kMemoryBytes + 0x1000) | pte::kValid);

        paged.emit_enable();
        paged.emit_move(2, kTestVirtual);
        const std::uint64_t faulting = paged.here();
        paged.program().op_r_r(op::kLoad, reg(2), reg(3));
        paged.program().halt();
        paged.start();
        paged.run_setup();
        expect_trap(paged.step(), cause::kPhysicalMemoryFault, 0, kMemoryBytes + 0x1000,
                    faulting, "a page table outside populated memory");
    }

    {
        // Translation succeeded and the PAGE it named is not populated. Still cause 11, and the
        // auxiliary word is the physical address translation produced.
        Paged paged;
        paged.identity_map();
        paged.tables().map(kTestVirtual, kMemoryBytes, kLeafRWX);
        paged.emit_enable();
        paged.emit_move(2, kTestVirtual + 0x40);
        const std::uint64_t faulting = paged.here();
        paged.program().op_r_r(op::kLoad, reg(2), reg(3));
        paged.program().halt();
        paged.start();
        paged.run_setup();
        expect_trap(paged.step(), cause::kPhysicalMemoryFault, 0, kMemoryBytes + 0x40, faulting,
                    "a translation onto unpopulated memory");
    }
}

V2_FIXTURE(a_page_fault_is_delivered_and_the_instruction_runs_again) {
    // The delivery properties the chapter calls normative, all three at once, and then the point
    // of them: a handler that repairs the mapping and returns completes the access.
    //
    //   "The auxiliary word carries the faulting virtual address exactly as the instruction
    //    computed it, including its low bits."
    //   "The captured program counter is the faulting instruction's own address."
    //   "The faulting instruction has taken no architectural effect."
    constexpr std::uint64_t kFaultingOffset = 0x18;

    Paged paged;
    paged.identity_map();
    const std::uint64_t leaf = paged.tables().map(kTestVirtual, kDataPage, kLeafRWX);
    // Map the page invalid to begin with, so the store faults and the handler is what makes it
    // work. The handler writes the leaf entry itself, exactly as a kernel servicing a fault
    // would, and the tables are identity-mapped and writable so it can.
    paged.machine().memory().write_little_endian(leaf, 8, 0);
    paged.install_all_handlers(kHandlerBase);

    Encoder handler(kHandlerBase);
    handler.op_r_i8(op::kMoveW, reg(10), leaf);
    handler.op_r_i8(op::kMoveW, reg(11), kDataPage | kLeafRWX);
    handler.op_r_r(op::kStore, reg(11), reg(10));
    handler.op_r_i8(op::kMoveW, reg(12), kTestVirtual + kFaultingOffset);
    handler.op_r(op::kTlbInvalidateAddress, reg(12));
    handler.op(op::kTrapReturn);
    paged.load_image(handler);

    paged.emit_kernel_preamble();
    paged.emit_enable();
    paged.emit_move(2, kTestVirtual + kFaultingOffset);
    paged.emit_move(3, kSentinel);
    const std::uint64_t faulting = paged.here();
    paged.program().op_r_r(op::kStore, reg(3), reg(2));
    paged.program().halt();
    paged.start();
    paged.run_setup();

    const StepResult result = paged.step();
    expect_trap(result, cause::kPageFaultStore, page_fault_subcode::kNoMapping,
                kTestVirtual + kFaultingOffset, faulting, "a store to an unmapped page");
    expect_disposition(result, TrapDisposition::Delivered, "a store to an unmapped page");
    V2_CHECK_EQ(paged.machine().interpreter().pc(), kHandlerBase);

    // The frame carries the same two words the record did, which is where a handler actually
    // reads them: the auxiliary word is the faulting virtual address INCLUDING its low bits, so
    // the offset survives and a handler learns the byte as well as the page.
    const std::uint64_t frame = kTrapStackTop - trap_frame::kBytes;
    V2_CHECK_EQ(paged.machine().memory().read_little_endian(frame + trap_frame::kAuxOffset, 8),
                kTestVirtual + kFaultingOffset);
    V2_CHECK_EQ(paged.machine().memory().read_little_endian(frame + trap_frame::kPcOffset, 8),
                faulting);
    // "The faulting instruction has taken no architectural effect": no byte of the page it was
    // storing to has changed, and the registers it named still hold what they held.
    V2_CHECK_EQ(paged.machine().memory().read_little_endian(kDataPage + kFaultingOffset, 8), 0u);
    V2_CHECK_EQ(paged.machine().get(2), kTestVirtual + kFaultingOffset);
    V2_CHECK_EQ(paged.machine().get(3), kSentinel);

    // The handler maps the page, invalidates, and returns. The store runs again and completes,
    // which is the entire reason the three properties above are normative.
    expect_halted(paged.machine().run(), "the serviced store");
    V2_CHECK_EQ(paged.machine().memory().read_little_endian(kDataPage + kFaultingOffset, 8),
                kSentinel);
}

V2_FIXTURE(writing_the_paging_root_flushes_every_cached_translation) {
    // "Every write to paging_root flushes every cached translation, whether or not the write
    // changes the value." maize-463 owns the fact that the write ASKS for the flush; this is the
    // cache that answers, and the two halves meet at CsrOutcome::flushed_translations.
    //
    // The test edits a live page-table entry WITHOUT invalidating, which the chapter says leaves
    // the observed translation undetermined, and then writes paging_root, after which it is
    // determined: the new entry, every time.
    for (unsigned same_value = 0; same_value <= 1; ++same_value) {
        Paged paged;
        paged.identity_map();
        const std::uint64_t leaf = paged.tables().map(kTestVirtual, kDataPage, kLeafRWX);
        paged.machine().memory().write_little_endian(kDataPage, 8, kSentinel);
        paged.machine().memory().write_little_endian(kOtherPage, 8, kReplacement);

        paged.emit_enable();
        paged.emit_move(2, kTestVirtual);
        paged.program().op_r_r(op::kLoad, reg(2), reg(3));   // caches the translation
        paged.program().op_r_r(op::kLoad, reg(2), reg(4));   // and uses it again
        paged.program().halt();
        paged.start();
        paged.run_setup();
        V2_CHECK(paged.step().status == StepStatus::Advanced);
        V2_CHECK(paged.step().status == StepStatus::Advanced);
        V2_CHECK_EQ(paged.machine().get(3), kSentinel);

        // Repoint the page at a different physical page, behind the machine's back.
        paged.machine().memory().write_little_endian(leaf, 8, kOtherPage | kLeafRWX);

        // A second program, at the point the first one halted, that writes paging_root and then
        // reads the same virtual address again.
        // The value written is either the one the register already holds, which is the case the
        // chapter calls out by name, or a different root table holding the same mapping, which
        // is what an address-space switch looks like. Both are accepted writes and both flush.
        std::uint64_t root_value = paged.tables().root_value();
        if (same_value == 0) {
            const std::uint64_t alternate = paged.tables().allocate_table();
            for (std::uint64_t offset = 0; offset < sv48::kTableBytes;
                 offset += sv48::kEntryBytes) {
                paged.machine().memory().write_little_endian(
                    alternate + offset, sv48::kEntryBytes,
                    paged.machine().memory().read_little_endian(
                        paged.tables().root_table() + offset, sv48::kEntryBytes));
            }
            root_value = alternate | paging_root::kModeSv48;
        }
        const std::uint64_t resume = paged.machine().interpreter().pc();
        Encoder second(resume);
        second.op_r_i8(op::kMoveW, reg(1), root_value);
        second.op_r_i2(op::kCsrWrite, reg(1), csr::kPagingRoot);
        second.op_r_r(op::kLoad, reg(2), reg(5));
        second.halt();
        paged.load_image(second);
        paged.machine().interpreter().host_resume_at(resume);

        char buffer[96];
        std::snprintf(buffer, sizeof(buffer), "flush on a paging_root write, same value %u",
                      same_value);
        expect_halted(paged.machine().run(), buffer);
        // The new entry, with no tlb_invalidate anywhere in the program.
        check_equal_u64(paged.machine().get(5), kReplacement, buffer, __FILE__, __LINE__);
        // maize-463's half of the seam: two accepted writes reached the register, the one that
        // enabled translation and the one under test, and both counted, whether or not either
        // changed the stored value.
        check_equal_u64(paged.csr().translation_flushes(), 2u, buffer, __FILE__, __LINE__);
    }
}

V2_FIXTURE(tlb_invalidate_discards_the_translations_it_names) {
    // "tlb_invalidate_all discards every cached translation" and "tlb_invalidate_address rs
    // discards any cached translation for the page containing the virtual address in rs."
    //
    // Both are tested the same way, and it is the only way the chapter makes deterministic: edit
    // a live entry, invalidate, and require the NEW entry on the next access. The
    // un-invalidated case is architecturally nondeterministic (the chapter says an access may
    // observe either translation, or one on one access and the other on the next), so nothing
    // here asserts anything about it.
    struct Case {
        const char* what;
        bool all;
        std::uint64_t named_offset;  // for the address form, where within the page rs points
    };
    const Case cases[] = {
        {"tlb_invalidate_all", true, 0},
        {"tlb_invalidate_address at the page base", false, 0},
        // "any address in the affected page", so an address well inside it works exactly as the
        // base does. A machine that compared rs to the cached page's base for equality fails.
        {"tlb_invalidate_address inside the page", false, 0xABC},
    };

    for (const Case& one : cases) {
        Paged paged;
        paged.identity_map();
        const std::uint64_t leaf = paged.tables().map(kTestVirtual, kDataPage, kLeafRWX);
        paged.machine().memory().write_little_endian(kDataPage, 8, kSentinel);
        paged.machine().memory().write_little_endian(kOtherPage, 8, kReplacement);

        paged.emit_enable();
        paged.emit_move(2, kTestVirtual);
        paged.emit_move(6, kTestVirtual + one.named_offset);
        paged.program().op_r_r(op::kLoad, reg(2), reg(3));  // caches the translation
        paged.program().halt();
        paged.start();
        paged.run_setup();
        V2_CHECK(paged.step().status == StepStatus::Advanced);
        check_equal_u64(paged.machine().get(3), kSentinel, one.what, __FILE__, __LINE__);

        paged.machine().memory().write_little_endian(leaf, 8, kOtherPage | kLeafRWX);

        const std::uint64_t resume = paged.machine().interpreter().pc();
        Encoder second(resume);
        if (one.all) {
            second.op(op::kTlbInvalidateAll);
        } else {
            second.op_r(op::kTlbInvalidateAddress, reg(6));
        }
        second.op_r_r(op::kLoad, reg(2), reg(5));
        second.halt();
        paged.load_image(second);
        paged.machine().interpreter().host_resume_at(resume);

        expect_halted(paged.machine().run(), one.what);
        check_equal_u64(paged.machine().get(5), kReplacement, one.what, __FILE__, __LINE__);
    }

    // D-3. privileged-architecture.md says "the page containing the virtual address in rs" and
    // never names a 4 KiB granule for a cache entry, while a cached translation may have come
    // from a leaf spanning 2 MiB, 1 GiB or 512 GiB, so this build reads "the page" as the extent
    // of the leaf that was actually cached. The card recorded that as an interpretive call, and
    // a search of the whole spec set says it is not one: instruction-reference-control.md lines
    // 672 through 678 gives tlb_invalidate_address its own entry, where "any address in the page
    // names the page" and "A machine may discard more translations than the instruction names,
    // up to and including all of them." Discarding the superpage is permitted explicitly.
    //
    // The permission to discard more is also why the counter assertions below say what THIS
    // machine did and could not be read as a requirement on another: a machine that discarded
    // every translation here conforms just as well. The address named is 1 MiB into a 2 MiB
    // leaf, inside the cached page and well outside the 4 KiB granule at its base.
    {
        Paged paged;
        paged.identity_map_superpage(1);
        paged.machine().memory().write_little_endian(0x1240, 8, kSentinel);
        paged.emit_enable();
        paged.emit_move(2, 0x1240);
        paged.emit_move(6, 0x100000);  // 1 MiB in, well past the leaf's first 4 KiB
        paged.program().op_r_r(op::kLoad, reg(2), reg(3));
        paged.program().halt();
        paged.start();
        paged.run_setup();
        V2_CHECK(paged.step().status == StepStatus::Advanced);
        const std::uint64_t walks_before = paged.translator().walks();
        V2_CHECK_EQ(paged.translator().cached_count(), 1u);

        const std::uint64_t resume = paged.machine().interpreter().pc();
        Encoder second(resume);
        second.op_r(op::kTlbInvalidateAddress, reg(6));
        second.halt();
        paged.load_image(second);
        paged.machine().interpreter().host_resume_at(resume);
        V2_CHECK(paged.step().status == StepStatus::Advanced);

        // The superpage's translation is gone, so the next access has to walk again. This is an
        // implementation-visible assertion, and it is the only shape D-3 can be checked in:
        // whether the entry was discarded is invisible to a guest by construction.
        V2_CHECK_EQ(paged.translator().cached_count(), 0u);
        V2_CHECK(paged.translator().walks() == walks_before);
    }
}

V2_FIXTURE(tlb_maintenance_is_privileged_and_faults_at_nothing) {
    // "Both instructions are privileged, and executing either at user level raises the
    // privileged-operation fault."
    for (const std::uint8_t opcode : {op::kTlbInvalidateAll, op::kTlbInvalidateAddress}) {
        Paged paged;
        paged.identity_map(kLeafRWXU);
        paged.tables().map(kTestVirtual, kDataPage, kLeafRWXU);
        paged.emit_enable();
        paged.emit_move(2, kTestVirtual);
        paged.emit_load(2, 3);  // caches a translation while still at supervisor level
        paged.emit_enter_user();
        paged.start();
        paged.run_setup();

        char buffer[96];
        std::snprintf(buffer, sizeof(buffer), "tlb maintenance at user level, opcode $%02X",
                      opcode);
        // Rebuild the tail of the program at the point the setup left off, because the two
        // opcodes have different shapes.
        const std::uint64_t resume = paged.machine().interpreter().pc();
        Encoder tail(resume);
        if (opcode == op::kTlbInvalidateAll) {
            tail.op(op::kTlbInvalidateAll);
        } else {
            tail.op_r(op::kTlbInvalidateAddress, reg(2));
        }
        tail.halt();
        paged.load_image(tail);
        paged.machine().interpreter().host_resume_at(resume);

        const unsigned cached_before = paged.translator().cached_count();
        const StepResult result = paged.step();
        // Cause 4's auxiliary word for an instruction is the offending opcode byte.
        expect_trap(result, cause::kPrivilegedOperation, 0, opcode, resume, buffer);
        // "and performs no invalidation": the trap comes first and the instruction does nothing.
        check_equal_u64(paged.translator().cached_count(), cached_before, buffer, __FILE__,
                        __LINE__);
    }

    // "Neither raises a fault for an address that has no cached translation, for an address that
    // is not mapped at all, or while the machine is in bare mode. A machine that caches nothing
    // satisfies both instructions by doing nothing at all, and it is fully conforming."
    //
    // So the assertion is that each of these completes and advances, and there is deliberately
    // no assertion about what was discarded: a conformance suite must not fail a machine on the
    // grounds that it cached nothing.
    struct Case {
        const char* what;
        bool translate;             // enable Sv48, or stay in bare mode
        std::uint64_t address;
    };
    const Case cases[] = {
        {"an address with no cached translation", true, kSecondVirtual},
        {"an address that is not mapped at all", true, 0x0000700000000000ull},
        {"bare mode", false, kTestVirtual},
    };
    for (const Case& one : cases) {
        Paged paged;
        paged.identity_map();
        paged.tables().map(kTestVirtual, kDataPage, kLeafRWX);
        if (one.translate) {
            paged.emit_enable();
        }
        paged.emit_move(2, one.address);
        paged.start();
        paged.run_setup();

        const std::uint64_t resume = paged.machine().interpreter().pc();
        Encoder tail(resume);
        tail.op_r(op::kTlbInvalidateAddress, reg(2));
        tail.op(op::kTlbInvalidateAll);
        tail.halt();
        paged.load_image(tail);
        paged.machine().interpreter().host_resume_at(resume);

        const StepResult first = paged.step();
        check(first.status == StepStatus::Advanced, one.what, __FILE__, __LINE__);
        const StepResult second = paged.step();
        check(second.status == StepStatus::Advanced, one.what, __FILE__, __LINE__);
        expect_halted(paged.machine().run(), one.what);
    }
}

V2_FIXTURE(a_cached_translation_is_rechecked_on_every_use) {
    // "A cached translation carries the permission bits of the leaf it came from, and the
    // machine re-checks those bits against the access kind and the current privilege level on
    // every use. A change of privilege level therefore requires no invalidation, and a cached
    // user page does not become reachable from user mode because the kernel touched it."
    //
    // This is the one cache rule with a guest-visible consequence, and it is the rule an
    // implementation that stores a bare physical address in the cache gets wrong.
    {
        // The kernel reads a page with U clear, caching it, and then drops to user level with no
        // invalidation of any kind. The user access has to fault.
        Paged paged;
        paged.identity_map(kLeafRWXU);
        paged.tables().map(kTestVirtual, kDataPage, kLeafRWX);  // U clear
        paged.emit_enable();
        paged.emit_move(2, kTestVirtual);
        paged.program().op_r_r(op::kLoad, reg(2), reg(3));  // supervisor: succeeds and caches
        paged.start();
        paged.run_setup();
        V2_CHECK(paged.step().status == StepStatus::Advanced);
        V2_CHECK(paged.translator().cached_count() > 0u);

        const std::uint64_t resume = paged.machine().interpreter().pc();
        Encoder tail(resume);
        tail.op_r_i8(op::kMoveW, reg(1), 0x0);
        tail.op_r_i2(op::kCsrWrite, reg(1), csr::kStatus);  // to user level
        const std::uint64_t faulting = tail.current_address();
        tail.op_r_r(op::kLoad, reg(2), reg(4));
        tail.halt();
        paged.load_image(tail);
        paged.machine().interpreter().host_resume_at(resume);

        V2_CHECK(paged.step().status == StepStatus::Advanced);
        V2_CHECK(paged.step().status == StepStatus::Advanced);
        V2_CHECK(paged.machine().interpreter().privilege() == Privilege::User);
        expect_trap(paged.step(), cause::kPageFaultLoad, page_fault_subcode::kPermission,
                    kTestVirtual, faulting, "a cached kernel page is not user-reachable");
    }

    {
        // The access-kind half of the same rule. A load caches a read-only page; the store that
        // follows uses the same cached translation and has to be rejected by its permission
        // bits, not waved through because the address was already resolved.
        Paged paged;
        paged.identity_map();
        paged.tables().map(kTestVirtual, kDataPage,
                           pte::kValid | pte::kReadable | pte::kExecutable);
        paged.emit_enable();
        paged.emit_move(2, kTestVirtual);
        paged.emit_move(3, kSentinel);
        paged.program().op_r_r(op::kLoad, reg(2), reg(4));
        paged.program().halt();
        paged.start();
        paged.run_setup();
        V2_CHECK(paged.step().status == StepStatus::Advanced);

        const std::uint64_t resume = paged.machine().interpreter().pc();
        Encoder tail(resume);
        tail.op_r_r(op::kStore, reg(3), reg(2));
        tail.halt();
        paged.load_image(tail);
        paged.machine().interpreter().host_resume_at(resume);
        expect_trap(paged.step(), cause::kPageFaultStore, page_fault_subcode::kPermission,
                    kTestVirtual, resume, "a cached read-only page rejects a store");
    }
}

V2_FIXTURE(the_translation_cache_neither_over_flushes_nor_under_flushes) {
    // NOT A CONFORMANCE FIXTURE, and it must not become one.
    //
    // The chapter makes the cache architecturally invisible and says outright that a machine
    // which caches nothing is fully conforming and that an un-invalidated edit may be observed
    // either way. Between those two, nothing a conformance suite is permitted to assert can tell
    // a machine that flushes everything on every event from one that flushes what it says it
    // flushes. That is a real gap, and "flush everything, always" is exactly what an
    // implementation drifts into and then quietly relies on.
    //
    // This fixture closes the gap for THIS machine only, through counters no guest instruction
    // can read. A different conforming machine would fail it, which is why it lives here and not
    // in a conformance suite. The instruction-reference chapter says as much in so many words
    // about the one assertion below that looks most like a conformance claim: of
    // tlb_invalidate_address, instruction-reference-control.md lines 677 through 678 says "A machine may
    // discard more translations than the instruction names, up to and including all of them."
    // So the line that catches a tlb_invalidate_address discarding a page it did not name is a
    // statement about this implementation and is explicitly NOT a requirement on any other.
    Paged paged;
    paged.identity_map();
    paged.tables().map(kTestVirtual, kDataPage, kLeafRWX);
    paged.tables().map(kSecondVirtual, kOtherPage, kLeafRWX);
    paged.emit_enable();
    paged.emit_move(2, kTestVirtual);
    paged.emit_move(6, kSecondVirtual);
    paged.program().op_r_r(op::kLoad, reg(2), reg(3));
    paged.program().halt();
    paged.start();
    paged.run_setup();

    // The first access to the data page walks once. Its eight bytes are one page, so a machine
    // that walked per byte rather than per page would show eight.
    const std::uint64_t before_first = paged.translator().walks();
    V2_CHECK(paged.step().status == StepStatus::Advanced);
    V2_CHECK_EQ(paged.translator().walks() - before_first, 1u);

    // The same access again, with the instruction fetched from an already-cached code page:
    // no walk at all. UNDER-flushing is not what this line catches, a machine that never
    // cached would fail it, and that machine would be conforming. This is the line that makes
    // the two below mean something.
    const std::uint64_t resume = paged.machine().interpreter().pc();
    Encoder repeat(resume);
    repeat.op_r_r(op::kLoad, reg(2), reg(3));
    repeat.op_r(op::kTlbInvalidateAddress, reg(6));  // a DIFFERENT page
    repeat.op_r_r(op::kLoad, reg(2), reg(3));
    repeat.op(op::kTlbInvalidateAll);
    repeat.op_r_r(op::kLoad, reg(2), reg(3));
    repeat.halt();
    paged.load_image(repeat);
    paged.machine().interpreter().host_resume_at(resume);

    std::uint64_t mark = paged.translator().walks();
    V2_CHECK(paged.step().status == StepStatus::Advanced);
    V2_CHECK_EQ(paged.translator().walks() - mark, 0u);

    // tlb_invalidate_address naming a page that is NOT the one under test. An over-flushing
    // machine discards the data page here and walks again on the next line; this machine does
    // not, and that is the assertion.
    V2_CHECK(paged.step().status == StepStatus::Advanced);
    mark = paged.translator().walks();
    V2_CHECK(paged.step().status == StepStatus::Advanced);
    V2_CHECK_EQ(paged.translator().walks() - mark, 0u);

    // tlb_invalidate_all, which discards EVERY translation, including the code page the next
    // instruction is fetched from. So the next load walks twice: once for its own fetch and once
    // for the data page. An under-flushing machine shows fewer.
    V2_CHECK(paged.step().status == StepStatus::Advanced);
    V2_CHECK_EQ(paged.translator().cached_count(), 0u);
    mark = paged.translator().walks();
    V2_CHECK(paged.step().status == StepStatus::Advanced);
    V2_CHECK_EQ(paged.translator().walks() - mark, 2u);
}

}  // namespace maize::v2::test
