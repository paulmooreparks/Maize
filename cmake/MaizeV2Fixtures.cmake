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
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/v2/fixtures_memory.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/v2/fixtures_devices.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/v2/fixtures_privilege.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/v2/fixtures_paging.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/v2/fixtures_traps.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/v2/fixtures_interrupts.cpp")
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
  block_memory_restart_invariant
  device_machine_block_identification_and_presence
  device_unpopulated_ports_read_zero_and_discard_write
  device_console_reset_state
  device_console_output_accumulates_bytes
  device_console_input_is_permanently_absent
  device_console_acknowledge_clears_transient_bits_only
  device_interrupt_control_reads_back_what_it_stores
  port_instructions_reach_the_port_space
  port_in_port_out_privileged
  csr_access_rules_apply_in_the_chapters_order
  csr_unimplemented_numbers_trap_rather_than_reading_zero
  csr_read_has_no_side_effect
  csr_swap_exchanges_atomically
  csr_swap_traps_exactly_as_csr_write
  csr_value_validation_is_per_register
  csr_reset_state
  scratch_register_contract
  privileged_instructions_are_privileged_at_user_level
  user_level_reads_its_own_floating_point_flags
  trap_cause_enumeration_delivers_exact_values
  trap_subcodes_are_the_documented_ones
  page_fault_causes_deliver_through_the_same_mechanism
  physical_memory_fault_reports_the_offending_physical_address
  trap_frame_layout_and_trap_stack_discipline
  frame_status_word_is_the_interrupted_context
  cause_word_packs_cause_and_subcode
  auxiliary_word_is_zero_where_the_table_says_zero
  vectored_dispatch_follows_the_chapters_order
  no_handler_installed_halts_with_kind_one
  double_fault_halts_with_the_original_cause
  nested_trap_lands_beneath_the_outer_frame
  trap_return_is_privileged_and_validates_before_committing
  trap_return_restores_and_resumes
  trap_return_fault_while_popping_is_an_ordinary_fault
  registers_survive_a_trap_untouched
  syscall_boundary_carries_number_and_arguments
  fault_restart_leaves_no_partial_effect
  block_memory_fault_restart_through_a_real_handler
  reserved_cause_is_never_delivered
  sv48_geometry_and_entry_bits_are_the_chapters_numbers
  bare_mode_translates_every_address_to_itself
  sv48_walk_reads_the_indices_the_chapter_names
  sv48_translation_carries_a_program_and_its_data
  superpages_map_their_whole_range_at_every_level
  translation_rejects_an_invalid_entry_with_subcode_zero
  translation_rejects_a_permission_violation_with_subcode_one
  the_machine_reads_only_the_bits_the_chapter_names
  a_page_table_read_outside_memory_is_a_physical_memory_fault
  a_page_fault_is_delivered_and_the_instruction_runs_again
  writing_the_paging_root_flushes_every_cached_translation
  tlb_invalidate_discards_the_translations_it_names
  tlb_maintenance_is_privileged_and_faults_at_nothing
  a_cached_translation_is_rechecked_on_every_use
  the_translation_cache_neither_over_flushes_nor_under_flushes
  a_fetch_page_fault_beats_an_interrupt_deliverable_at_the_same_boundary
  a_block_interrupt_and_the_page_fault_after_it_compose_and_lose_nothing
  interrupt_cause_numbers_and_register_layout_are_the_specified_ones
  interrupt_enable_zero_rejects_the_non_maskable_synchronous_causes
  pending_is_set_while_the_cause_is_masked_at_the_cpu
  lowest_numbered_deliverable_cause_wins
  interrupt_lands_between_instructions_never_inside_one
  interrupt_during_block_copy_restarts_and_copies_every_byte_once
  one_expiry_is_delivered_exactly_once_and_acknowledged_before_return
  wait_for_interrupt_retires_before_it_delivers
  masked_completion_of_a_wait_is_not_a_delivery
  a_pending_but_disabled_cause_never_wakes_the_machine
  wait_for_interrupt_at_user_level_faults_without_suspending
  an_interrupt_handler_preserves_every_general_register
  timer_contract_arms_expires_and_rearms_as_the_class_says
  timer_refuses_a_zero_period_written_while_counting_is_already_enabled
  timer_period_written_mid_interval_takes_effect_at_the_next_expiry
  console_asserts_its_line_only_while_a_byte_is_waiting
  a_wait_with_nothing_armed_suspends_rather_than_spinning)

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
  include_paths_normalize_identically_at_both_sites
  flat_output_takes_the_mzi_suffix
  mzvm_runs_what_mzasm_wrote
  mzvm_prints_hello_world
  mzvm_refuses_out_of_range_numeric_arguments
  mzvm_leading_whitespace_cannot_hide_a_minus_sign
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
