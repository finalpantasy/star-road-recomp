# Included after the game object's response file has been parsed.
set(glue "${CMAKE_CURRENT_SOURCE_DIR}/decomp")
set(workspace "${CMAKE_CURRENT_SOURCE_DIR}/../decomp")
set(target_dir "${CMAKE_CURRENT_BINARY_DIR}/cargo")
set(archive "${target_dir}/release/smrpg_decomp.lib")
find_program(CARGO cargo REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_custom_command(OUTPUT "${archive}"
  COMMAND "${CARGO}" build --release --locked --offline --target-dir "${target_dir}"
  WORKING_DIRECTORY "${workspace}"
  DEPENDS "${workspace}/Cargo.toml" "${workspace}/Cargo.lock" "${workspace}/rust-toolchain.toml"
          "${workspace}/smrpg_decomp/Cargo.toml" "${workspace}/smrpg_decomp/src/lib.rs"
  VERBATIM)
add_custom_target(decomp_archive DEPENDS "${archive}")
add_dependencies(smrpg_boot decomp_archive)
target_link_libraries(smrpg_boot PRIVATE "${archive}")

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${glue}/leaves.def")
file(STRINGS "${glue}/leaves.def" rows)
set(chunks "")
foreach(row IN LISTS rows)
  if(NOT row MATCHES "^[SC]?LEAF[(](main_[0-9]+),")
    message(FATAL_ERROR "Invalid swap manifest row: ${row}")
  endif()
  list(APPEND chunks "${CMAKE_MATCH_1}")
endforeach()
list(REMOVE_DUPLICATES chunks)
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
    COMMAND "${Python3_EXECUTABLE}" -I "${glue}/wrap_chunk.py" "${original}" "${glue}/leaves.def" "${wrapped}"
    DEPENDS "${original}" "${glue}/wrap_chunk.py" "${glue}/leaves.def"
    VERBATIM)
  set_source_files_properties("${wrapped}" PROPERTIES COMPILE_OPTIONS "-w;-fno-exceptions")
  target_sources(smrpg_boot PRIVATE "${wrapped}")
endforeach()
target_sources(smrpg_boot PRIVATE "${glue}/swap.c")
target_compile_definitions(smrpg_boot PRIVATE SMRPG_DECOMP_BUILT=1)
