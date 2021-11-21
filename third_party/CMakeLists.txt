message ("pzany, ${CMAKE_CURRENT_SOURCE_DIR}")

add_custom_command(OUTPUT "a"
                          "b"
                   COMMAND make
                   ARGS -j4
                   WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/<subdir>
)

