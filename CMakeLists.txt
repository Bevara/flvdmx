cmake_minimum_required(VERSION 3.7)
project(Filters)

include(filters.cmake)

add_definitions(-fpic)

SET(FLVDMX_LIB
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/libflv.a
)

SET(FLVDMX_SRC
        ${CMAKE_CURRENT_SOURCE_DIR}/dmx_flv.c
)

SET(FLVDMX_INC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/lib
)

add_filter(flvdmx
        "${FLVDMX_SRC}"
        "${FLVDMX_LIB}"
        []
        ""
        "${FLVDMX_INC}"
        ""
        "1")
