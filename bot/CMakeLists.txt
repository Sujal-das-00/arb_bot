cmake_minimum_required(VERSION 3.16)
project(arb_bot CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(Boost REQUIRED COMPONENTS system)
find_package(OpenSSL REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(Threads REQUIRED)

# Headers live in include/ (flat layout).
include_directories(${CMAKE_SOURCE_DIR}/include)

# ---- shared core: config + feed + parser ----
add_library(arb_core STATIC
    src/Config.cpp
    src/FeedHandler.cpp
    src/MessageParser.cpp
    src/TriangleCalculator.cpp
    src/Dispatch.cpp
    src/Logger.cpp
)
target_include_directories(arb_core PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(arb_core PUBLIC
    Boost::system
    OpenSSL::SSL
    OpenSSL::Crypto
    nlohmann_json::nlohmann_json
    Threads::Threads
)

# ---- the live bot ----
add_executable(arb_bot src/main.cpp)
target_link_libraries(arb_bot PRIVATE arb_core)

# ---- tests ----
option(BUILD_TESTING "Build unit tests" ON)
if(BUILD_TESTING)
    find_package(GTest REQUIRED)
    enable_testing()
    add_executable(arb_tests tests/test_message_parser.cpp)
    target_link_libraries(arb_tests PRIVATE arb_core GTest::gtest GTest::gtest_main)
    add_test(NAME arb_tests COMMAND arb_tests)
    add_executable(test_triangle_calculator tests/test_triangle_calculator.cpp)
    target_link_libraries(test_triangle_calculator PRIVATE arb_core GTest::gtest_main)
    add_test(NAME test_triangle_calculator COMMAND test_triangle_calculator)
    add_executable(test_dispatch tests/test_dispatch.cpp)
    target_link_libraries(test_dispatch PRIVATE arb_core GTest::gtest_main)
    add_test(NAME test_dispatch COMMAND test_dispatch)
endif()
