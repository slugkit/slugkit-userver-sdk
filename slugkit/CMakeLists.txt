cmake_minimum_required(VERSION 3.12...3.31)
project(slugkit_client CXX)

set(SLUGKIT_SRC
    include/slugkit/types.hpp
    include/slugkit/exceptions.hpp

    include/slugkit/dto/mint.hpp
    include/slugkit/dto/forge.hpp
    include/slugkit/dto/slice.hpp
    include/slugkit/dto/reset.hpp
    include/slugkit/dto/pattern_info.hpp

    include/slugkit/components/client.hpp

    src/slugkit/components/client.cpp
)

add_library(slugkit_client OBJECT ${SLUGKIT_SRC})
target_link_libraries(slugkit_client PUBLIC userver::core)
target_include_directories(slugkit_client PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(slugkit_client PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
