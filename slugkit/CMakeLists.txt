cmake_minimum_required(VERSION 3.12...3.31)
project(slugkit_client CXX)

option(SLUGKIT_BUILD_TESTS "Build the SlugKit SDK unit tests" OFF)
option(SLUGKIT_BUILD_POOL_POSTGRES "Build the PostgreSQL slug-pool store (needs userver::postgresql)" OFF)

set(SLUGKIT_SRC
    include/slugkit/types.hpp
    include/slugkit/exceptions.hpp
    include/slugkit/secrets.hpp

    include/slugkit/dto/mint.hpp
    include/slugkit/dto/forge.hpp
    include/slugkit/dto/slice.hpp
    include/slugkit/dto/reset.hpp
    include/slugkit/dto/pattern_info.hpp

    include/slugkit/components/client.hpp
    include/slugkit/metrics.hpp

    src/slugkit/components/client.cpp
    src/slugkit/metrics.cpp
    src/slugkit/secrets.cpp

    # Slug pool: mint ahead of demand so request paths never wait on SlugKit.
    include/slugkit/pool/policy.hpp
    include/slugkit/pool/refill.hpp
    include/slugkit/pool/storage.hpp
    include/slugkit/pool/memory_storage.hpp
    include/slugkit/pool/replenisher.hpp

    src/slugkit/pool/policy.cpp
    src/slugkit/pool/refill.cpp
    src/slugkit/pool/storage.cpp
    src/slugkit/pool/memory_storage.cpp
    src/slugkit/pool/replenisher.cpp
)

add_library(slugkit_client OBJECT ${SLUGKIT_SRC})
target_link_libraries(slugkit_client PUBLIC userver::core)
target_include_directories(slugkit_client PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(slugkit_client PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# Kept out of the core target so a service that does not store its pool in
# PostgreSQL — or does not use the pool at all — never pulls in the driver.
if(SLUGKIT_BUILD_POOL_POSTGRES)
    add_library(slugkit_pool_postgres OBJECT
        include/slugkit/pool/postgres_storage.hpp
        src/slugkit/pool/postgres_storage.cpp
    )
    target_link_libraries(slugkit_pool_postgres PUBLIC slugkit_client userver::postgresql)
    target_include_directories(slugkit_pool_postgres PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_include_directories(slugkit_pool_postgres PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
endif()

if(SLUGKIT_BUILD_TESTS)
    add_executable(slugkit_unittest
        tests/policy_test.cpp
        tests/memory_pool_test.cpp
        tests/refill_test.cpp
        tests/secrets_test.cpp
        tests/metrics_test.cpp
    )
    target_link_libraries(slugkit_unittest PRIVATE slugkit_client userver::utest)
    target_include_directories(slugkit_unittest PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    add_google_tests(slugkit_unittest)
endif()
