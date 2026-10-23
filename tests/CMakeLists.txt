# SPDX-License-Identifier: MIT
if(NOT CMAKE_READELF)
    find_program(CMAKE_READELF NAMES readelf)
    if(NOT CMAKE_READELF)
        message(FATAL_ERROR "readelf is required for runtime dependency tests")
    endif()
endif()

add_test(NAME runtime COMMAND ${CMAKE_COMMAND}
    -DREADELF=${CMAKE_READELF} -DLIBRARY=$<TARGET_FILE:eco_shared>
    -P ${CMAKE_CURRENT_SOURCE_DIR}/check_runtime.cmake)

set(ECO_TESTS mutex core wait_kinds wait_fd timers cond faults io events watch sources process process_faults process_signals)

foreach(name IN LISTS ECO_TESTS)
    add_executable(test_${name} test_${name}.cpp)
    if(name STREQUAL "mutex" OR name STREQUAL "faults" OR name STREQUAL "watch" OR name STREQUAL "process_faults" OR name STREQUAL "process_signals")
        target_link_libraries(test_${name} PRIVATE eco_static)
    else()
        target_link_libraries(test_${name} PRIVATE eco_shared)
    endif()
    set_target_properties(test_${name} PROPERTIES LINKER_LANGUAGE C)
    target_compile_definitions(test_${name} PRIVATE _GNU_SOURCE)
    target_compile_options(test_${name} PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG -fno-exceptions -fno-rtti)
    add_test(NAME ${name} COMMAND test_${name})
    set_tests_properties(${name} PROPERTIES TIMEOUT 20)
endforeach()

target_include_directories(test_sources PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_include_directories(test_timers PRIVATE ${PROJECT_SOURCE_DIR}/src)

find_package(Threads REQUIRED)
target_include_directories(test_mutex PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(test_mutex PRIVATE Threads::Threads -Wl,--wrap=calloc -Wl,--wrap=realloc)
target_link_libraries(test_process PRIVATE Threads::Threads)
target_link_libraries(test_process_signals PRIVATE Threads::Threads -Wl,--wrap=write)

target_link_libraries(test_faults PRIVATE
    -Wl,--wrap=malloc -Wl,--wrap=free -Wl,--wrap=calloc -Wl,--wrap=realloc
    -Wl,--wrap=epoll_ctl -Wl,--wrap=epoll_wait
    -Wl,--wrap=clock_gettime -Wl,--wrap=write)

target_link_libraries(test_watch PRIVATE -Wl,--wrap=epoll_ctl -Wl,--wrap=epoll_wait)

target_link_libraries(test_process_faults PRIVATE
    -Wl,--wrap=calloc -Wl,--wrap=free -Wl,--wrap=realloc
    -Wl,--wrap=pipe2 -Wl,--wrap=eventfd -Wl,--wrap=epoll_ctl
    -Wl,--wrap=sigaction -Wl,--wrap=sigprocmask
    -Wl,--wrap=posix_spawn_file_actions_init -Wl,--wrap=posix_spawnattr_init
    -Wl,--wrap=posix_spawn_file_actions_adddup2 -Wl,--wrap=posix_spawn_file_actions_addclose
    -Wl,--wrap=posix_spawnp -Wl,--wrap=read -Wl,--wrap=waitpid)

add_test(NAME caller_runtime COMMAND ${CMAKE_COMMAND}
    -DREADELF=${CMAKE_READELF} -DBINARY=$<TARGET_FILE:test_wait_fd>
    -P ${CMAKE_CURRENT_SOURCE_DIR}/check_runtime.cmake)

add_test(NAME process_runtime COMMAND ${CMAKE_COMMAND}
    -DREADELF=${CMAKE_READELF} -DBINARY=$<TARGET_FILE:test_process>
    -P ${CMAKE_CURRENT_SOURCE_DIR}/check_runtime.cmake)

add_executable(eco_memory memory.cpp)
target_link_libraries(eco_memory PRIVATE eco_shared)
set_target_properties(eco_memory PROPERTIES LINKER_LANGUAGE C)
target_compile_options(eco_memory PRIVATE -fno-exceptions -fno-rtti)

add_test(NAME mutex_runtime COMMAND ${CMAKE_COMMAND}
    -DREADELF=${CMAKE_READELF} -DBINARY=$<TARGET_FILE:test_mutex>
    -P ${CMAKE_CURRENT_SOURCE_DIR}/check_runtime.cmake)
