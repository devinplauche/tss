add_executable(face_tss_pubsub pubsub_demo.c)
target_include_directories(face_tss_pubsub PRIVATE ${CMAKE_SOURCE_DIR}/c/include)
target_link_libraries(face_tss_pubsub PRIVATE FaceTSS::TSS)

# Typed demo: compiles the codegen'd PositionReport wrapper.
add_executable(face_tss_typed_pubsub
    typed_pubsub_demo.c
    ${CMAKE_SOURCE_DIR}/c/generated/positionreport_typed.c)
target_include_directories(face_tss_typed_pubsub PRIVATE
    ${CMAKE_SOURCE_DIR}/c/include
    ${CMAKE_SOURCE_DIR}/c/generated)
target_link_libraries(face_tss_typed_pubsub PRIVATE FaceTSS::TSS)
# The generated wrapper calls the flatcc runtime directly.
target_link_libraries(face_tss_typed_pubsub PRIVATE ${FLATCCRT_TARGET})
if(NOT FACETSS_USE_SYSTEM_FLATCC)
    target_include_directories(face_tss_typed_pubsub PRIVATE
        ${flatcc_SOURCE_DIR}/include)
endif()
