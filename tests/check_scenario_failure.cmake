execute_process(COMMAND "${PROGRAM}" --input "${INPUT}"
    RESULT_VARIABLE code OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 10)
if(NOT "${code}" STREQUAL "1")
    message(FATAL_ERROR "Expected scenario failure exit 1, received ${code}: ${error}")
endif()
if(NOT error MATCHES "line 2: Invalid integer: invalid")
    message(FATAL_ERROR "Missing precise parse error: ${error}")
endif()
