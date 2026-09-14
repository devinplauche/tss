add_executable(face_tss_pubsub pubsub_demo.c)
target_include_directories(face_tss_pubsub PRIVATE ${CMAKE_SOURCE_DIR}/c/include)
target_link_libraries(face_tss_pubsub PRIVATE FaceTSS::TSS)
