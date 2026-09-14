foreach(t envelope config lifecycle live conformance extended)
    add_executable(face_tss_test_${t} test_${t}.c)
    target_include_directories(face_tss_test_${t} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/c/include)
    target_link_libraries(face_tss_test_${t} PRIVATE FaceTSS::TSS)
    if(NOT FACETSS_USE_SYSTEM_NNG)
        target_include_directories(face_tss_test_${t} PRIVATE
            ${nng_SOURCE_DIR}/include)
    endif()
    if(NOT FACETSS_USE_SYSTEM_FLATCC)
        target_include_directories(face_tss_test_${t} PRIVATE
            ${flatcc_SOURCE_DIR}/include)
    endif()
    if(t STREQUAL "live")
        target_link_libraries(face_tss_test_${t} PRIVATE nng)
    endif()
    add_test(NAME c-${t} COMMAND face_tss_test_${t})
    set_tests_properties(c-${t} PROPERTIES TIMEOUT 120)
endforeach()

# Typed interface test: compiles the codegen'd PositionReport wrapper
# (flat schema) and the Telemetry wrapper (nested table, vector, enum).
add_executable(face_tss_test_typed
    test_typed.c
    ${CMAKE_SOURCE_DIR}/c/generated/positionreport_typed.c
    ${CMAKE_SOURCE_DIR}/c/generated/telemetry_typed.c)
target_include_directories(face_tss_test_typed PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/c/include
    ${CMAKE_SOURCE_DIR}/c/generated)
target_link_libraries(face_tss_test_typed PRIVATE FaceTSS::TSS)
# The generated wrapper calls the flatcc runtime directly.
target_link_libraries(face_tss_test_typed PRIVATE ${FLATCCRT_TARGET})
if(NOT FACETSS_USE_SYSTEM_NNG)
    target_include_directories(face_tss_test_typed PRIVATE
        ${nng_SOURCE_DIR}/include)
endif()
if(NOT FACETSS_USE_SYSTEM_FLATCC)
    target_include_directories(face_tss_test_typed PRIVATE
        ${flatcc_SOURCE_DIR}/include)
endif()
add_test(NAME c-typed COMMAND face_tss_test_typed)
set_tests_properties(c-typed PROPERTIES TIMEOUT 120)
