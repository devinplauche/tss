foreach(t envelope config config_service lifecycle live conformance extended threading)
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
    if(t STREQUAL "extended" OR t STREQUAL "threading")
        target_link_libraries(face_tss_test_${t} PRIVATE Threads::Threads)
    endif()
    add_test(NAME c-${t} COMMAND face_tss_test_${t})
    set_tests_properties(c-${t} PROPERTIES TIMEOUT 120)
endforeach()

# Typed interface test: compiles the codegen'd PositionReport wrapper
# (flat schema), the Telemetry wrapper (nested table, vector, enum), the
# Event wrapper (unions, vectors of tables/strings, nested vectors,
# explicit field ids) from c/schemas/kitchen_sink.fbs, and the Advanced
# wrapper (nested string vectors, triple-nested scalars, bare enums,
# mixed unions, vectors of unions) from c/schemas/advanced.fbs.
add_executable(face_tss_test_typed
    test_typed.c
    ${CMAKE_SOURCE_DIR}/c/generated/positionreport_typed.c
    ${CMAKE_SOURCE_DIR}/c/generated/telemetry_typed.c
    ${CMAKE_SOURCE_DIR}/c/generated/event_typed.c
    ${CMAKE_SOURCE_DIR}/c/generated/advanced_typed.c)
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

# C++ binding test
add_executable(face_tss_test_cpp_binding
    ${CMAKE_SOURCE_DIR}/cpp/tests/test_cpp_binding.cpp)
target_link_libraries(face_tss_test_cpp_binding PRIVATE FaceTSS::TSS_CPP)
add_test(NAME c-cpp-binding COMMAND face_tss_test_cpp_binding)
set_tests_properties(c-cpp-binding PROPERTIES TIMEOUT 60)
