add_executable(deferred_resource_queue_test
    deferred_resource_queue_test.cpp
)

target_compile_features(deferred_resource_queue_test PRIVATE cxx_std_17)
target_include_directories(deferred_resource_queue_test PRIVATE
    ${PROJECT_SOURCE_DIR}
)
set_target_properties(deferred_resource_queue_test PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(deferred_resource_queue_test PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
endif()

add_test(
    NAME app.deferred_resource_queue
    COMMAND deferred_resource_queue_test
)

set_tests_properties(app.deferred_resource_queue PROPERTIES TIMEOUT 5)
