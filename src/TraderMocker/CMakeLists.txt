
#1. 确定CMake的最低版本需求
CMAKE_MINIMUM_REQUIRED(VERSION 3.0.0)

#2. 确定工程名
PROJECT(TraderMocker LANGUAGES CXX)
SET(CMAKE_CXX_STANDARD 17)

SET(SRC  
	${PROJECT_SOURCE_DIR}/TraderMocker.cpp
	${PROJECT_SOURCE_DIR}/TraderMocker.h
	${PROJECT_SOURCE_DIR}/PaperAccount.cpp
	${PROJECT_SOURCE_DIR}/PaperAccount.h
	${PROJECT_SOURCE_DIR}/ControlledState.cpp
)

SET(LIBRARY_OUTPUT_PATH ${CMAKE_BINARY_DIR}/build_${PLATFORM}/${CMAKE_BUILD_TYPE}/bin)

INCLUDE_DIRECTORIES(${INCS})
LINK_DIRECTORIES(${LNKS})
ADD_LIBRARY(TraderMocker SHARED ${SRC})

IF(MSVC)
	SET(LIBS
		ws2_32
		)
ELSE(GNUCC)
	IF(WIN32)
		SET(LIBS
			boost_thread
			stdc++fs
			ws2_32
		)
	ELSE(UNIX)
		SET(LIBS
			boost_thread
			stdc++fs
		)
	ENDIF()
ENDIF()
TARGET_LINK_LIBRARIES(TraderMocker ${LIBS})

# Explicit opt-in build target; excluded from normal runtime builds.
ADD_EXECUTABLE(TraderMockerMatchingTest EXCLUDE_FROM_ALL tests/matching.cpp TraderMocker.cpp PaperAccount.cpp ControlledState.cpp)
TARGET_LINK_LIBRARIES(TraderMockerMatchingTest ${LIBS})
ADD_EXECUTABLE(TraderMockerAccountTest EXCLUDE_FROM_ALL tests/account.cpp PaperAccount.cpp)

IF (MSVC)
ELSE (GNUCC)
	SET_TARGET_PROPERTIES(TraderMocker PROPERTIES
		CXX_VISIBILITY_PRESET hidden
		C_VISIBILITY_PRESET hidden
		VISIBILITY_INLINES_HIDDEN 1
        LINK_FLAGS_RELEASE -s)
ENDIF ()

