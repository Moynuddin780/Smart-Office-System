set(SO_TEST_SOURCES
    main.cpp
    test_engine_pool.cpp
    test_preprocess.cpp
    test_yolo26_pose.cpp
    test_scrfd.cpp
    test_onvif.cpp
    test_arcface.cpp
    test_geometry.cpp
    test_tracker.cpp
    test_camera_reader.cpp
    test_camera_rules.cpp
    test_postprocess_pool.cpp
    test_identity.cpp
    test_camera_worker.cpp
    test_attendance_rollup.cpp
    test_modules.cpp
    test_auth.cpp
)

add_executable(so_tests ${SO_TEST_SOURCES})
target_link_libraries(so_tests PRIVATE so_core)
target_include_directories(so_tests PRIVATE ${CMAKE_SOURCE_DIR}/third_party)

# Tests that touch the NPU need the project root as their working directory so
# that relative model paths ("models/...") resolve the same way they do at run
# time. Anything that needs a device is tagged so CI can deselect it.
add_test(NAME so_tests
         COMMAND so_tests
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
