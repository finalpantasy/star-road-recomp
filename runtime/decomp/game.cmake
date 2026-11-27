# Included after the game object's response file has been parsed.
set(glue "${CMAKE_CURRENT_SOURCE_DIR}/decomp")
set(workspace "${CMAKE_CURRENT_SOURCE_DIR}/../decomp")
set(target_dir "${CMAKE_CURRENT_BINARY_DIR}/cargo")
set(archive "${target_dir}/release/smrpg_decomp.lib")
find_program(CARGO cargo REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# Bulk-generated swaps (task-45, tools/decompgen). They derive from the game binary, so they live on A:, not in
# Git: gen.rs (Rust) and gen.def (manifest rows). Missing files are generated once with the default cap.
set(SMRPG_DECOMPGEN_DIR "${SMRPG_WORK}/decompgen" CACHE PATH "output of tools/decompgen/generate.py")
if(NOT EXISTS "${SMRPG_DECOMPGEN_DIR}/gen.def" OR NOT EXISTS "${SMRPG_DECOMPGEN_DIR}/gen.rs")
  execute_process(COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_CURRENT_SOURCE_DIR}/../tools/decompgen/generate.py"
                          --out "${SMRPG_DECOMPGEN_DIR}"
                  RESULT_VARIABLE gen_rc OUTPUT_QUIET)
  if(NOT gen_rc EQUAL 0)
    message(FATAL_ERROR "tools/decompgen/generate.py failed (${gen_rc}); run it by hand to see why")
  endif()
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SMRPG_DECOMPGEN_DIR}/gen.def")
# One manifest for the wrapper script, the compiler and the swap table: hand-written rows, then generated rows.
# Rewritten only when the content changes so an unchanged manifest does not rebuild every wrapped chunk.
file(READ "${glue}/leaves.def" manifest_text)
file(READ "${SMRPG_DECOMPGEN_DIR}/gen.def" generated_text)
set(manifest "${CMAKE_CURRENT_BINARY_DIR}/manifest.def")
set(manifest_new "${manifest_text}${generated_text}")
set(manifest_old "")
if(EXISTS "${manifest}")
  file(READ "${manifest}" manifest_old)
endif()
if(NOT manifest_old STREQUAL manifest_new)
  file(WRITE "${manifest}" "${manifest_new}")
endif()
target_include_directories(smrpg_boot PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")

# Every source module, including subdirectories such as battle/ (task-55), not only lib.rs: a change to any one must rebuild the archive (task-52).
file(GLOB_RECURSE decomp_sources CONFIGURE_DEPENDS "${workspace}/smrpg_decomp/src/*.rs")
add_custom_command(OUTPUT "${archive}"
  COMMAND "${CMAKE_COMMAND}" -E env "SMRPG_DECOMPGEN_DIR=${SMRPG_DECOMPGEN_DIR}"
          "${CARGO}" build --release --locked --offline --target-dir "${target_dir}"
  WORKING_DIRECTORY "${workspace}"
  DEPENDS "${workspace}/Cargo.toml" "${workspace}/Cargo.lock" "${workspace}/rust-toolchain.toml"
          "${workspace}/smrpg_decomp/Cargo.toml" "${workspace}/smrpg_decomp/build.rs"
          ${decomp_sources} "${SMRPG_DECOMPGEN_DIR}/gen.rs"
  VERBATIM)
add_custom_target(decomp_archive DEPENDS "${archive}")
add_dependencies(smrpg_boot decomp_archive)
target_link_libraries(smrpg_boot PRIVATE "${archive}")

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${glue}/leaves.def")
file(STRINGS "${manifest}" rows)
set(chunks "")
foreach(row IN LISTS rows)
  if(NOT row MATCHES "^[SCGNVDPEQO]?LEAF[(](main_[0-9]+),")
    message(FATAL_ERROR "Invalid swap manifest row: ${row}")
  endif()
  list(APPEND chunks "${CMAKE_MATCH_1}")
endforeach()
list(REMOVE_DUPLICATES chunks)
# Sorted (task-58): CMake writes one "Custom options" line per wrapped chunk into flags.make, in this list's order, and every
# object of the target depends on flags.make. Rows come hand-written first, so the first hand-written row for a chunk that
# had only generated rows moved it forward, rewrote flags.make and rebuilt all 400+ objects (task-57: 15 min, task-58 measured
# the same). A sorted list does not change when a row is added to a chunk that already has rows.
list(SORT chunks)
foreach(chunk IN LISTS chunks)
  set(original "${SMRPG_WORK}/build/c/${chunk}.c")
  set(wrapped "${CMAKE_CURRENT_BINARY_DIR}/${chunk}_decomp.c")
  set(old_objects ${_game_objs})
  list(FILTER old_objects INCLUDE REGEX "/${chunk}_[0-9a-f]+[.]o$")
  list(LENGTH old_objects count)
  if(NOT count EQUAL 1)
    message(FATAL_ERROR "Expected one lifted ${chunk} object in ${SMRPG_GAME_OBJECTS}")
  endif()
  list(REMOVE_ITEM _game_objs ${old_objects})
  add_custom_command(OUTPUT "${wrapped}"
    COMMAND "${Python3_EXECUTABLE}" -I "${glue}/wrap_chunk.py" "${original}" "${manifest}" "${wrapped}"
    DEPENDS "${original}" "${glue}/wrap_chunk.py" "${manifest}"
    VERBATIM)
  set_source_files_properties("${wrapped}" PROPERTIES COMPILE_OPTIONS "-w;-fno-exceptions;-g0")   # task-47: unwrapped lifted objects carry no DWARF; -g added 550 KB of 605 KB per chunk
  target_sources(smrpg_boot PRIVATE "${wrapped}")
endforeach()
target_sources(smrpg_boot PRIVATE "${glue}/swap.c" "${glue}/fp_selftest.c")
target_compile_definitions(smrpg_boot PRIVATE SMRPG_DECOMP_BUILT=1)
