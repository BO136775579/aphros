get_filename_component(name ${CMAKE_CURRENT_SOURCE_DIR} NAME)

set(T t.${name}_util)
add_executable(${T} dump_util.cpp)
target_link_libraries(${T} aphros)
add_test_current(NAME dump_util COMMAND ./test util)

set(T t.${name}_diff)
add_executable(${T} dump_diff.cpp)
target_link_libraries(${T} aphros)

set(T t.${name}_gen)
add_executable(${T} dump_gen.cpp)
target_link_libraries(${T} aphros)

add_test_current(NAME dump_h5 COMMAND ./test h5)
