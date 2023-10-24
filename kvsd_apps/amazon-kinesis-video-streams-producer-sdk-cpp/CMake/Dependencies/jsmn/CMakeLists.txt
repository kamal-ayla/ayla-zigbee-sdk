cmake_minimum_required(VERSION 3.6.3)
project(jsmn C)


if("${CMAKE_C_COMPILER_ID}" MATCHES "GNU|Clang")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fPIC")
endif()

add_library(jsmn STATIC jsmn.h jsmn.c)
install(TARGETS jsmn DESTINATION lib)
install(FILES jsmn.h DESTINATION include)
