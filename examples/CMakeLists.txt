# SPDX-License-Identifier: MIT
add_executable(eco_echo echo.cpp)
target_link_libraries(eco_echo PRIVATE eco_shared)
set_target_properties(eco_echo PROPERTIES LINKER_LANGUAGE C)
target_compile_options(eco_echo PRIVATE -Wall -Wextra -Wpedantic -fno-exceptions -fno-rtti)
