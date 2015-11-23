# cmake project to build nopoll library
#
# Copyright (c) 2015 Wind River Systems, Inc.
#
# The right to copy, distribute, modify or otherwise make use
# of this software may be licensed only pursuant to the terms
# of an applicable Wind River license agreement.

INCLUDE (nopoll.cmake)
cmake_minimum_required(VERSION 2.6)
INCLUDE (${CMAKE_ROOT}/Modules/FindOpenSSL.cmake)

if (NOT OPENSSL_FOUND)
    message(FATAL_ERROR "ERROR: Could not find OpenSSL!!!")
endif(NOT OPENSSL_FOUND)

include_directories(${OPENSSL_INCLUDE_DIR})

file(GLOB_RECURSE NOPOLL_SRC_LIST *.c)
if (NOT NOPOLL_LIB_TYPE)
    set(NOPOLL_LIB_TYPE STATIC)
endif (NOT NOPOLL_LIB_TYPE)
file(GLOB_RECURSE NOPOLL_HDR_LIST *.h)
source_group("Header Files" FILES ${NOPOLL_HDR_LIST})
include_directories(src)
add_library(${NOPOLL_LIB_NAME} ${NOPOLL_LIB_TYPE} ${NOPOLL_SRC_LIST} ${NOPOLL_HDR_LIST})
target_link_libraries(${NOPOLL_LIB_NAME} ${OPENSSL_LIBRARIES})

add_definitions("-DVERSION=\"0.2.7.b164\"")
if(MSVC)
    add_definitions(/D _CRT_NONSTDC_NO_DEPRECATE)
    add_definitions(/D _CRT_SECURE_NO_WARNINGS)
endif(MSVC)
if(APPLE)
    add_definitions(-Wno-deprecated-declarations)
endif(APPLE)
