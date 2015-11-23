INCLUDE (${CMAKE_ROOT}/Modules/CheckFunctionExists.cmake)

if (NOT NOPOLL_LIB_NAME)
    set(NOPOLL_LIB_NAME nopoll)
endif (NOT NOPOLL_LIB_NAME)

if(CMAKE_HOST_UNIX)
    add_definitions("-DNOPOLL_OS_UNIX=(1)")
elseif (CMAKE_HOST_WIN32)
    add_definitions("-DNOPOLL_OS_WIN32=(1)")
endif (CMAKE_HOST_UNIX)

CHECK_FUNCTION_EXISTS (vasprintf         HAVE_VASPRINTF)
if (HAVE_VASPRINTF)
    add_definitions("-DNOPOLL_HAVE_VASPRINTF=(1)")
endif(HAVE_VASPRINTF)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    add_definitions("-DNOPOLL_64BIT_PLATFORM=(1)")
endif()

add_definitions("-DSHOW_DEBUG_LOG")
