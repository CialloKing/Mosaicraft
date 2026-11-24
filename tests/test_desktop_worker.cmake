cmake_minimum_required(VERSION 3.21)

string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef nonce)
set(input "${CMAKE_CURRENT_BINARY_DIR}/desktop-worker-request-${nonce}.json")
file(WRITE "${input}" "{\"op\":\"candidates\",\"position\":0}\n")
execute_process(COMMAND "${CLI}" desktop-worker INPUT_FILE "${input}"
    RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE diagnostics TIMEOUT 30)
file(REMOVE "${input}")

if(WORKER_ENABLED)
    string(JSON ok ERROR_VARIABLE parse_error GET "${output}" ok)
    string(JSON message ERROR_VARIABLE message_error GET "${output}" message)
    if(NOT status STREQUAL "0" OR parse_error OR message_error OR ok OR
       NOT message STREQUAL "generate a preview first")
        message(FATAL_ERROR "Expected a desktop session reply (${status}): ${output}\n${diagnostics}")
    endif()
elseif(NOT status STREQUAL "1" OR NOT diagnostics MATCHES "unknown command: desktop-worker")
    message(FATAL_ERROR "Expected the CLI-only build to reject the worker command (${status}): ${output}\n${diagnostics}")
endif()
