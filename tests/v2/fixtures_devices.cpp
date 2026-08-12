// fixtures_devices.cpp (maize-451): the port space, the machine block, and the console class.
//
// Most fixtures here drive the device surface object of a freshly constructed machine rather
// than executing port instructions, because what they assert is the CONTRACT AT RESET: the state
// device-surface.md and boot.md require to hold at the instant the first instruction executes.
// Running an instruction first would move the machine off the state under test.
//
// Two fixtures do execute real instructions, and they are the ones about the instructions rather
// than about the devices: that port_in and port_out reach the port space at all, and that both
// carry the privileged-operation guard.

#include <string>
#include <vector>

#include "device_v2.h"
#include "fixture_support.h"

namespace maize::v2::test {
namespace {

constexpr std::uint64_t kBase = 0x100;

// The ports this card populates, spelled here the way asm/v2/devices.mzasm spells them for a
// guest, so a fixture and a guest program disagree loudly rather than quietly.
constexpr std::uint16_t kMachineId = 0x0000;
constexpr std::uint16_t kMachinePresence = 0x0001;
constexpr std::uint16_t kConsoleId = 0x0010;
constexpr std::uint16_t kConsoleStatus = 0x0011;
constexpr std::uint16_t kConsoleControl = 0x0012;
constexpr std::uint16_t kConsoleData = 0x0013;

std::string console_text(const DeviceSurfaceV2& surface) {
    const std::vector<std::uint8_t>& bytes = surface.console_output();
    return std::string(bytes.begin(), bytes.end());
}

}  // namespace

V2_FIXTURE(device_machine_block_identification_and_presence) {
    // A probe of one port establishes that a Maize port space is present at all and which base
    // version it answers for, and a probe of the next says which classes are there.
    Machine machine;
    DeviceSurfaceV2& ports = machine.interpreter().device_surface();

    V2_CHECK_EQ(ports.port_in(kMachineId), 0x0000000002004D32ull);
    V2_CHECK_EQ(ports.port_in(kMachinePresence), 0x0000000000000002ull);

    // The console's identification carries its class code in the low quarter-word and is
    // nonzero, which is all presence detection needs. The second quarter-word, the class
    // contract version, is NOT asserted here and must not be: OQ-1 is open, the value the build
    // ships is a placeholder nobody chose, and a fixture pinning it would turn a guess into a
    // ruling that a later card would have to argue with a test to change.
    const std::uint64_t console_id = ports.port_in(kConsoleId);
    V2_CHECK_EQ(console_id & 0xFFFFu, 1u);
    V2_CHECK(console_id != 0);

    // device-surface.md's own cross-check: the set bits of the bitmap are exactly the classes
    // whose identification port reads nonzero. This is what makes the machine unable to lie
    // about itself in either direction, and it is checked over the whole class table rather than
    // over the one class this build populates.
    const std::uint64_t bitmap = ports.port_in(kMachinePresence);
    for (unsigned code = 1; code <= device_class::kHighestClassCode; ++code) {
        const bool claimed = ((bitmap >> code) & 1u) != 0;
        const std::uint64_t identification =
            ports.port_in(static_cast<std::uint16_t>(code * kPortsPerBlock));
        V2_CHECK(claimed == (identification != 0));
    }
}

V2_FIXTURE(device_unpopulated_ports_read_zero_and_discard_write) {
    // Read-zero-and-discard is what makes presence detection work without a trap handler, and it
    // has to hold for five different reasons that all reach the same behaviour: a reserved offset
    // in the machine block, a reserved offset in a POPULATED class block, a class the base
    // assigns but this machine does not carry, the range reserved for later specification work,
    // and the implementation range. A machine that answered on any of them would have a
    // detection mechanism that lies.
    Machine machine;
    DeviceSurfaceV2& ports = machine.interpreter().device_surface();

    const std::uint16_t probes[] = {
        0x0002, 0x000F,  // reserved within the populated machine block
        0x0014, 0x001F,  // reserved within the populated console block
        0x0020, 0x0025,  // keyboard: assigned by the base, absent here
        0x0030, 0x003F,  // timer: assigned by the base, absent here (D-1)
        0x0040, 0x0050, 0x0060, 0x0070,  // the remaining optional classes, all absent
        0x0080, 0x7FFF,  // reserved for later specification work
        0x8000, 0xFFFF,  // the implementation range
    };

    for (std::uint16_t port : probes) {
        V2_CHECK_EQ(ports.port_in(port), 0u);
        ports.port_out(port, 0xDEADBEEFCAFEF00Dull);
        V2_CHECK_EQ(ports.port_in(port), 0u);
    }

    // No write above reached the one device that is present. A console that had accepted a byte
    // from a reserved offset of its own block would pass every read check above.
    V2_CHECK(ports.console_output().empty());
}

V2_FIXTURE(device_console_reset_state) {
    // boot.md: every present device is in its reset state at the instant the first instruction
    // executes, so this reads a machine that has run nothing.
    Machine machine;
    DeviceSurfaceV2& ports = machine.interpreter().device_surface();

    const std::uint64_t status = ports.port_in(kConsoleStatus);

    // Output-ready and nothing else. Checked as a whole word first, so a bit this build never
    // names cannot appear unnoticed, and then bit by bit so a failure says which one.
    V2_CHECK_EQ(status, status_mask(console_status_bit::kOutputReady));
    V2_CHECK((status & status_mask(console_status_bit::kInputAvailable)) == 0);
    V2_CHECK((status & status_mask(1)) == 0);  // busy, named at the skeleton level
    V2_CHECK((status & status_mask(2)) == 0);  // invalid-request, likewise
    V2_CHECK((status & status_mask(console_status_bit::kOutputReady)) != 0);
    V2_CHECK((status & status_mask(console_status_bit::kEndOfInput)) == 0);
    V2_CHECK((status & status_mask(console_status_bit::kOverrun)) == 0);

    V2_CHECK_EQ(ports.port_in(kConsoleControl), 0u);
    V2_CHECK(!machine.interpreter().device_surface().console().interrupt_enabled());
    V2_CHECK(ports.console_output().empty());
}

V2_FIXTURE(device_console_output_accumulates_bytes) {
    Machine machine;
    DeviceSurfaceV2& ports = machine.interpreter().device_surface();

    // Every transfer is a whole word and the console takes the low byte of it. The high bits are
    // set to something loud here so a device that emitted the wrong byte of the word, or that
    // truncated at the wrong width, fails rather than coincidentally agreeing.
    const std::string text = "hello, maize\n";
    for (char c : text) {
        const std::uint64_t byte = static_cast<std::uint64_t>(static_cast<std::uint8_t>(c));
        ports.port_out(kConsoleData, 0xFFFFFFFFFFFFFF00ull | byte);
    }

    V2_CHECK(console_text(ports) == text);
    if (console_text(ports) != text) {
        record_failure("the console buffered '" + console_text(ports) + "' rather than '" + text +
                       "'");
    }

    // Output-ready is permanently set on this console, so no write above could have been lost
    // and the overrun bit is still clear.
    V2_CHECK((ports.port_in(kConsoleStatus) & status_mask(console_status_bit::kOverrun)) == 0);
}

V2_FIXTURE(device_console_input_is_permanently_absent) {
    // D-2: no host input source is wired to the console in this build, so input-available stays
    // clear and end-of-input is never asserted. Reading offset 3 with input-available clear
    // yields zero and consumes nothing, which is the contract's own defined behaviour rather
    // than a gap in it. This fixture is what pins the decision: a later build that starts
    // claiming end-of-input has to come back and change a test that says it never does.
    Machine machine;
    DeviceSurfaceV2& ports = machine.interpreter().device_surface();

    for (int i = 0; i < 256; ++i) {
        V2_CHECK_EQ(ports.port_in(kConsoleData), 0u);
        const std::uint64_t status = ports.port_in(kConsoleStatus);
        V2_CHECK((status & status_mask(console_status_bit::kInputAvailable)) == 0);
        V2_CHECK((status & status_mask(console_status_bit::kEndOfInput)) == 0);
    }
}

V2_FIXTURE(device_console_acknowledge_clears_transient_bits_only) {
    // The acknowledge contract is two rules in one sentence: a bit named in the written value is
    // cleared, and a bit the device holds true remains set. A test that only acknowledged a
    // latched bit would pass on an implementation that cleared everything.
    Machine machine;
    DeviceSurfaceV2& ports = machine.interpreter().device_surface();
    ConsoleDeviceV2& console = ports.console();

    // Overrun cannot be reached from a guest program in this build, because output-ready is
    // permanently set, so the fixture stands it up on the device object directly. A real
    // terminal with backpressure will reach it for real, and the contract has to already be
    // right when it does.
    console.host_latch_status_bit(console_status_bit::kOverrun);
    const std::uint64_t before = ports.port_in(kConsoleStatus);
    V2_CHECK((before & status_mask(console_status_bit::kOverrun)) != 0);
    V2_CHECK((before & status_mask(console_status_bit::kOutputReady)) != 0);

    // Acknowledge both bits at once. Overrun goes, because nothing holds it true; output-ready
    // stays, because the console can still accept a byte.
    ports.port_out(kConsoleStatus,
                   status_mask(console_status_bit::kOverrun) |
                       status_mask(console_status_bit::kOutputReady));
    const std::uint64_t after = ports.port_in(kConsoleStatus);
    V2_CHECK((after & status_mask(console_status_bit::kOverrun)) == 0);
    V2_CHECK((after & status_mask(console_status_bit::kOutputReady)) != 0);
    V2_CHECK_EQ(after, status_mask(console_status_bit::kOutputReady));

    // The other half of the overrun contract: a write with output-ready clear discards the byte
    // and sets the bit, rather than buffering the byte anyway or trapping.
    console.host_set_output_ready(false);
    const std::size_t buffered = ports.console_output().size();
    ports.port_out(kConsoleData, static_cast<std::uint64_t>('x'));
    V2_CHECK_EQ(ports.console_output().size(), buffered);
    const std::uint64_t overrun = ports.port_in(kConsoleStatus);
    V2_CHECK((overrun & status_mask(console_status_bit::kOverrun)) != 0);
    V2_CHECK((overrun & status_mask(console_status_bit::kOutputReady)) == 0);
}

V2_FIXTURE(device_interrupt_control_reads_back_what_it_stores) {
    // Interrupt control is read and write, bit 0 enables the class's line, and the rest are
    // reserved and read zero. Nothing in this build consumes bit 0's value, since there is no
    // trap machinery to assert a line into, but storing and returning it faithfully is the port
    // contract and a class whose enable bit does not round-trip is not conforming.
    Machine machine;
    DeviceSurfaceV2& ports = machine.interpreter().device_surface();

    ports.port_out(kConsoleControl, 1u);
    V2_CHECK_EQ(ports.port_in(kConsoleControl), 1u);

    ports.port_out(kConsoleControl, 0u);
    V2_CHECK_EQ(ports.port_in(kConsoleControl), 0u);

    // A reserved bit written on its own stores nothing and reads back zero.
    ports.port_out(kConsoleControl, 2u);
    V2_CHECK_EQ(ports.port_in(kConsoleControl), 0u);

    // Every bit at once: bit 0 lands, and the sixty-three reserved bits are discarded rather
    // than trapped, so the read is 1 and not the value written.
    ports.port_out(kConsoleControl, ~std::uint64_t{0});
    V2_CHECK_EQ(ports.port_in(kConsoleControl), 1u);

    ports.port_out(kConsoleControl, 0u);
    V2_CHECK_EQ(ports.port_in(kConsoleControl), 0u);
}

V2_FIXTURE(port_instructions_reach_the_port_space) {
    // The two instructions themselves, executed rather than called: a byte written through
    // port_out arrives at the console, and a word read through port_in arrives in the
    // destination register. The port register's upper 48 bits are ignored rather than checked,
    // so this loads one with a loud value and expects port $0000 anyway.
    Machine machine;
    Encoder program(kBase);
    program.op_r_i1(op::kMoveZb, reg(2), kConsoleData)
        .op_r_i1(op::kMoveZb, reg(3), static_cast<std::uint8_t>('h'))
        .op_r_r(op::kPortOut, reg(3), reg(2))
        .op_r_r(op::kPortIn, reg(4), reg(5))
        .halt();
    machine.load(program);
    machine.set(4, 0x00000000DEAD0000ull);  // names port $0000; the high bits are ignored

    const StepResult result = machine.run();
    expect_halted(result, "a program that writes a byte and reads the machine identification");
    V2_CHECK_EQ(machine.get(5), 0x0000000002004D32ull);
    V2_CHECK(console_text(machine.interpreter().device_surface()) == "h");
}

V2_FIXTURE(port_in_port_out_privileged) {
    // Both port instructions are privileged, and executing either at user level raises the
    // privileged-operation trap and PERFORMS NO TRANSFER. Cause 4's auxiliary word is the
    // offending opcode byte here rather than a register number, since neither instruction names
    // a control and status register.
    //
    // No guest-visible path to user level exists in this build (trap_return is maize-420), so
    // the fixture uses the host-side privilege setter. Writing the guard and leaving it
    // unexercised, which is what the halt guard has had to do until now, produces a check
    // nobody can tell apart from a missing one.
    {
        Machine machine;
        Encoder program(kBase);
        program.op_r_r(op::kPortIn, reg(4), reg(5));
        machine.load(program);
        machine.set(4, kMachineId);
        machine.set(5, 0xA5A5A5A5A5A5A5A5ull);
        machine.interpreter().host_set_privilege(Privilege::User);

        const StepResult result = machine.step();
        expect_trap(result, cause::kPrivilegedOperation, 0, op::kPortIn, kBase,
                    "port_in at user level");
        V2_CHECK_EQ(machine.get(5), 0xA5A5A5A5A5A5A5A5ull);  // the destination is untouched
    }

    {
        Machine machine;
        Encoder program(kBase);
        program.op_r_r(op::kPortOut, reg(3), reg(2));
        machine.load(program);
        machine.set(2, kConsoleData);
        machine.set(3, static_cast<std::uint64_t>('h'));
        machine.interpreter().host_set_privilege(Privilege::User);

        const StepResult result = machine.step();
        expect_trap(result, cause::kPrivilegedOperation, 0, op::kPortOut, kBase,
                    "port_out at user level");
        V2_CHECK(machine.interpreter().device_surface().console_output().empty());
    }

    // The same two instructions at supervisor level do not trap, so the fixture is testing the
    // privilege check rather than an instruction that traps unconditionally.
    {
        Machine machine;
        Encoder program(kBase);
        program.op_r_r(op::kPortIn, reg(4), reg(5)).halt();
        machine.load(program);
        machine.set(4, kMachineId);
        const StepResult result = machine.run();
        expect_halted(result, "port_in at supervisor level");
        V2_CHECK_EQ(machine.get(5), 0x0000000002004D32ull);
    }
}

}  // namespace maize::v2::test
