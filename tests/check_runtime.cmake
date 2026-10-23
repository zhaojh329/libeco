# The production binary and shared library must not require a C++ runtime.
foreach(binary IN ITEMS "${BINARY}" "${LIBRARY}")
    if(NOT binary)
        continue()
    endif()

    execute_process(COMMAND "${READELF}" -d "${binary}"
        RESULT_VARIABLE status OUTPUT_VARIABLE dynamic ERROR_VARIABLE error)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "readelf failed for ${binary}: ${error}")
    endif()
    if(dynamic MATCHES "NEEDED[^\n]*(libstdc[+][+]|libc[+][+]|libc[+][+]abi|libsupc[+][+])")
        message(FATAL_ERROR "C++ runtime dependency in ${binary}: ${dynamic}")
    endif()
endforeach()
