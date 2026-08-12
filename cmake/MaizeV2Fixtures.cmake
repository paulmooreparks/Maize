# MaizeV2Fixtures.cmake (maize-418): the v2 interpreter's fixtures as real CTest tests.
#
# mazm v2 does not exist yet, so every fixture here is hand-assembled bytes emitted through
# tests/v2/encode_v2.h and run against the interpreter in-process. The runner takes one fixture
# name on its command line, and each name below is registered as its own add_test() entry, so a
# failure names the specific behaviour that broke rather than a bundled pass or fail. That
# follows the per-fixture registration convention MaizeCTest.cmake already uses for the
# guest-toolchain suite.
#
# The list below and the C++ registry have to agree, and neither can quietly drift: a fixture
# added in C++ but missing here would run under nothing, and a name here with no fixture behind
# it would pass vacuously. The v2_fixture_registry test closes that loop by handing this exact
# list to the runner and asking it to compare against what registered itself.

add_executable(mzvm_v2_fixtures
  ${MAIZE_V2_SOURCES}
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/v2/fixture_support.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/v2/fixtures_decode.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/v2/fixtures_integer.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/v2/fixtures_control.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/v2/fixtures_memory.cpp")
target_include_directories(mzvm_v2_fixtures PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src/v2"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/v2")
set_property(TARGET mzvm_v2_fixtures PROPERTY CXX_STANDARD 20)

if (MAIZE_SANITIZE)
  target_compile_options(mzvm_v2_fixtures PRIVATE ${_maize_san_flags})
  target_link_options(mzvm_v2_fixtures    PRIVATE -fsanitize=address,undefined)
endif()

set(MAIZE_V2_FIXTURES
  decode_length_table_matches_appendix_a
  decode_reserved_and_escape_bytes_trap
  decode_plain_slot_rejects_nonzero_form
  decode_sliced_slot_form_range
  out_of_scope_opcodes_are_a_host_diagnostic
  encoding_worked_examples
  decode_next_instruction_address_precedes_operands
  constants_and_moves
  pc_add_reads_the_following_instruction_address
  half_word_operations_zero_extend
  shift_counts_are_masked
  divide_and_remainder_trap_without_writing
  carry_chain_matches_wide_addition
  arithmetic_and_logic_word_forms
  compares_write_one_or_zero
  branches_agree_with_compares
  jump_call_and_return
  select_leaves_the_destination_alone
  halt_stops_the_machine
  loads_and_stores_at_every_width
  memory_faults_report_the_lowest_inaccessible_address
  a_trapping_access_writes_nothing
  positional_extract_and_insert
  bitfield_extract_and_insert
  block_memory_operand_validity
  block_memory_completion_and_overlap
  block_memory_restart_invariant)

foreach(_fixture ${MAIZE_V2_FIXTURES})
  add_test(NAME "v2_${_fixture}" COMMAND mzvm_v2_fixtures "${_fixture}")
  set_tests_properties("v2_${_fixture}" PROPERTIES LABELS "v2" TIMEOUT 60)
endforeach()

string(REPLACE ";" "," _maize_v2_fixture_csv "${MAIZE_V2_FIXTURES}")
add_test(NAME "v2_fixture_registry" COMMAND mzvm_v2_fixtures --verify-names "${_maize_v2_fixture_csv}")
set_tests_properties("v2_fixture_registry" PROPERTIES LABELS "v2" TIMEOUT 60)

# maize-422: the mzasm suite. Unlike the interpreter fixtures above, most of these drive the
# SHIPPED mzasm binary and read what it wrote back off disk, because several acceptance criteria
# are about the binary's filesystem behaviour rather than about a library call. The binary's path
# and the repository root therefore ride on each test's command line: the repository root is
# where the specification chapters live, and appendix-a-opcode-map.md is the conformance oracle.
#
# decode_v2.cpp is linked in as the shape-and-length oracle for the corpus fixture. It was
# written on maize-418, independently of everything here, and is untouched by this card, which
# is the whole reason it can serve as one.
add_executable(mzasm_tests
  "${CMAKE_CURRENT_SOURCE_DIR}/src/v2/decode_v2.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/v2/appendix_a.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/v2/mzasm_test_support.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/v2/mzasm_conformance.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/v2/mzasm_corpus.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/v2/mzasm_language.cpp")
target_include_directories(mzasm_tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src/v2"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/v2")
set_property(TARGET mzasm_tests PROPERTY CXX_STANDARD 20)
add_dependencies(mzasm_tests mzasm mzvm)

if (MAIZE_SANITIZE)
  target_compile_options(mzasm_tests PRIVATE ${_maize_san_flags})
  target_link_options(mzasm_tests    PRIVATE -fsanitize=address,undefined)
endif()

set(MAIZE_MZASM_FIXTURES
  appendix_a_parse_is_total
  mnemonic_table_matches_appendix
  opcode_table_matches_appendix
  band_summary_matches_rows
  mnemonic_and_opcode_tables_agree
  csr_include_matches_the_privileged_architecture_table
  corpus_covers_every_assigned_opcode
  encoding_chapter_worked_examples
  assembler_chapter_worked_example
  register_names_are_reserved_words
  field_fit_is_dual_reading_at_every_boundary
  move_family_selection_stops_where_the_chapter_says
  the_assembler_synthesizes_nothing
  check_touches_nothing_and_a_failure_removes_stale_output
  include_resolves_csr_names_and_reports_a_cycle
  flat_output_takes_the_mzi_suffix
  mzvm_runs_what_mzasm_wrote
  nothing_in_the_v2_assembler_names_the_v1_suffix
  the_task_scanner_stops_at_the_next_task)

foreach(_fixture ${MAIZE_MZASM_FIXTURES})
  add_test(NAME "v2_mzasm_${_fixture}"
           COMMAND mzasm_tests "${_fixture}" "$<TARGET_FILE:mzasm>" "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties("v2_mzasm_${_fixture}" PROPERTIES LABELS "v2" TIMEOUT 300)
endforeach()

# The CMake list and the C++ registry have to agree: a fixture added in C++ but missing here
# would run under nothing, and a name here with no fixture behind it would pass vacuously.
string(REPLACE ";" "," _maize_mzasm_fixture_csv "${MAIZE_MZASM_FIXTURES}")
add_test(NAME "v2_mzasm_fixture_registry"
         COMMAND mzasm_tests --verify-names "${_maize_mzasm_fixture_csv}")
set_tests_properties("v2_mzasm_fixture_registry" PROPERTIES LABELS "v2" TIMEOUT 60)
