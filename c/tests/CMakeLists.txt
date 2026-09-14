foreach(t envelope config lifecycle live)
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
