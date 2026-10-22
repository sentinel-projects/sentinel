# Exercise the actual helper with CMake as a harmless, portable fake signer.
string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef fixture_id)
set(fixture "${TEST_ROOT}/package-signing-${fixture_id}")
set(packages "${fixture}/packages with spaces")
set(signer "${fixture}/signer.cmake")
set(receipt "${fixture}/signed.txt")
file(MAKE_DIRECTORY "${packages}")
file(WRITE "${packages}/sentinel-desktop-fixture.exe" "synthetic installer")
file(WRITE "${packages}/sentinel-desktop-fixture.msi" "synthetic installer")

function(run_signing result output)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DBINARY_DIR=${packages}"
            "-DSIGNTOOL_EXECUTABLE=${CMAKE_COMMAND}"
            "-DSIGN_ARGS=-DSIGN_RECEIPT=${receipt};-P;${signer}"
            -P "${HELPER}"
        RESULT_VARIABLE code OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr
    )
    set(${result} "${code}" PARENT_SCOPE)
    set(${output} "${stdout}${stderr}" PARENT_SCOPE)
endfunction()

file(WRITE "${signer}" "message(FATAL_ERROR \"Synthetic signing failure\")\n")
run_signing(code output)
if("${code}" STREQUAL "0" OR NOT output MATCHES "Failed to sign package")
    file(REMOVE_RECURSE "${fixture}")
    message(FATAL_ERROR "Signer failure was not propagated: ${code}\n${output}")
endif()

file(WRITE "${signer}" [=[
math(EXPR last_argument "${CMAKE_ARGC} - 1")
file(APPEND "${SIGN_RECEIPT}" "${CMAKE_ARGV${last_argument}}\n")
]=])
run_signing(code output)
if(NOT "${code}" STREQUAL "0")
    file(REMOVE_RECURSE "${fixture}")
    message(FATAL_ERROR "Successful signing failed: ${output}")
endif()
file(STRINGS "${receipt}" signed)
list(LENGTH signed signed_count)
if(NOT signed_count EQUAL 2)
    file(REMOVE_RECURSE "${fixture}")
    message(FATAL_ERROR "Expected both installer formats to be signed: ${signed}")
endif()
foreach(package IN LISTS signed)
    if(NOT EXISTS "${package}" OR NOT package MATCHES "\\.(exe|msi)$")
        file(REMOVE_RECURSE "${fixture}")
        message(FATAL_ERROR "Signer received an invalid package argument: ${package}")
    endif()
endforeach()
file(REMOVE_RECURSE "${fixture}")
