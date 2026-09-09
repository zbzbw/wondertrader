
#1. 确定CMake的最低版本需求
cmake_minimum_required(VERSION 3.0.0)

#2. 确定工程名
project(WtCore LANGUAGES CXX)
SET(CMAKE_CXX_STANDARD 17)

IF(MSVC)
ELSE(GNUCC)
	SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fPIC")
ENDIF()

SET(LIBRARY_OUTPUT_PATH  ${CMAKE_BINARY_DIR}/build_${PLATFORM}/${CMAKE_BUILD_TYPE}/libs)

#7. 添加源码
file(GLOB SRCS *.cpp)
file(GLOB HDRS *.h)

INCLUDE_DIRECTORIES(${INCS})
LINK_DIRECTORIES(${LNKS})

ADD_LIBRARY(WtCore STATIC ${SRCS} ${HDRS})

ADD_EXECUTABLE(TraderAdapterLiveTest EXCLUDE_FROM_ALL tests/live_gate.cpp)
ADD_EXECUTABLE(CtaLiveStateTest EXCLUDE_FROM_ALL tests/cta_state.cpp)
ADD_EXECUTABLE(CtaLiveClockTest EXCLUDE_FROM_ALL tests/cta_clock.cpp)
TARGET_LINK_LIBRARIES(CtaLiveClockTest WtCore WTSTools WTSUtils)
TARGET_LINK_LIBRARIES(CtaLiveStateTest WtCore WTSTools WTSUtils)
TARGET_LINK_LIBRARIES(TraderAdapterLiveTest WtCore WTSTools WTSUtils)
IF(MSVC)
    TARGET_LINK_LIBRARIES(TraderAdapterLiveTest ws2_32)
    TARGET_LINK_LIBRARIES(CtaLiveStateTest ws2_32)
    TARGET_LINK_LIBRARIES(CtaLiveClockTest ws2_32)
ELSE()
    TARGET_LINK_LIBRARIES(TraderAdapterLiveTest dl pthread stdc++fs boost_filesystem boost_thread)
    TARGET_LINK_LIBRARIES(CtaLiveStateTest dl pthread stdc++fs boost_filesystem boost_thread)
    TARGET_LINK_LIBRARIES(CtaLiveClockTest dl pthread stdc++fs boost_filesystem boost_thread)
ENDIF()