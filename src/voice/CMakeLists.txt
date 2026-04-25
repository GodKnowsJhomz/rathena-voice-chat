#
# setup
#
set( VOICE_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR} CACHE INTERNAL "" )


#
# voice server
#
if( NOT ENABLE_VOICE_SERVER )
	return()
endif()

if( NOT BUILD_SERVERS )
	return()
endif()

message( STATUS "Creating target voice-server" )

set( UWS_DIR "${CMAKE_SOURCE_DIR}/3rdparty/uWebSockets" )
set( USOCKS_DIR "${CMAKE_SOURCE_DIR}/3rdparty/uSockets" )
set( LIBUV_LOCAL_DIR "${CMAKE_SOURCE_DIR}/3rdparty/libuv" )

if( NOT EXISTS "${UWS_DIR}/src/App.h" )
	message( FATAL_ERROR
		"\n[voice-server] uWebSockets not found!\n"
		"Please populate 3rdparty/uWebSockets before building.\n"
	)
endif()

if( NOT EXISTS "${USOCKS_DIR}/src/libusockets.h" )
	message( FATAL_ERROR
		"\n[voice-server] uSockets not found!\n"
		"Please populate 3rdparty/uSockets before building.\n"
	)
endif()

if( WIN32 )
	if( CMAKE_SIZEOF_VOID_P EQUAL 8 )
		set( LIBUV_PLATFORM_LIBDIR "x64" )
	else()
		set( LIBUV_PLATFORM_LIBDIR "Win32" )
	endif()

	set( LIBUV_INCLUDE_DIRS "${LIBUV_LOCAL_DIR}/include" )
	set( LIBUV_LIBRARIES "${LIBUV_LOCAL_DIR}/lib/${LIBUV_PLATFORM_LIBDIR}/libuv.lib" )
else()
	find_path( LIBUV_INCLUDE_DIRS "uv.h"
		PATHS
			"/usr/include"
			"/usr/local/include"
			"/opt/homebrew/include"
			"${LIBUV_LOCAL_DIR}/include"
	)
	find_library( LIBUV_LIBRARIES
		NAMES uv libuv
		PATHS
			"/usr/lib"
			"/usr/local/lib"
			"/opt/homebrew/lib"
	)
endif()

if( NOT LIBUV_INCLUDE_DIRS OR NOT LIBUV_LIBRARIES )
	message( FATAL_ERROR
		"\n[voice-server] libuv not found!\n"
		"Install libuv development files or provide them in 3rdparty/libuv.\n"
	)
endif()

if( NOT WITH_MYSQL )
	message( FATAL_ERROR "Stopping target voice-server (requires MYSQL)" )
endif()

if( NOT WITH_ZLIB )
	message( FATAL_ERROR "Stopping target voice-server (requires ZLIB)" )
endif()

set( VOICE_INCLUDE_DIRS
	${GLOBAL_INCLUDE_DIRS}
	${RA_INCLUDE_DIRS}
	${UWS_DIR}/src
	${USOCKS_DIR}/src
	${JSON_INCLUDE_DIRS}
	${MYSQL_INCLUDE_DIRS}
	${ZLIB_INCLUDE_DIRS}
	${LIBUV_INCLUDE_DIRS}
)

set( VOICE_SOURCES
	${VOICE_SOURCE_DIR}/config.cpp
	${VOICE_SOURCE_DIR}/main.cpp
	${VOICE_SOURCE_DIR}/room_manager.cpp
	${VOICE_SOURCE_DIR}/server.cpp
	${VOICE_SOURCE_DIR}/whisper_manager.cpp
	${USOCKS_DIR}/src/bsd.c
	${USOCKS_DIR}/src/context.c
	${USOCKS_DIR}/src/loop.c
	${USOCKS_DIR}/src/socket.c
	${USOCKS_DIR}/src/udp.c
	${USOCKS_DIR}/src/eventing/libuv.c
)

set( VOICE_HEADERS
	${VOICE_SOURCE_DIR}/config.hpp
	${VOICE_SOURCE_DIR}/player.hpp
	${VOICE_SOURCE_DIR}/room_manager.hpp
	${VOICE_SOURCE_DIR}/whisper_manager.hpp
)

set( LIBRARIES ${GLOBAL_LIBRARIES} ${MYSQL_LIBRARIES} ${ZLIB_LIBRARIES} ${LIBUV_LIBRARIES} )
set( DEFINITIONS "${GLOBAL_DEFINITIONS} -DLIBUS_NO_SSL -DLIBUS_USE_LIBUV" )

if( WIN32 )
	set( DEFINITIONS "${DEFINITIONS} -DWIN32_LEAN_AND_MEAN -DFD_SETSIZE=4096" )
	list( APPEND LIBRARIES ws2_32 iphlpapi userenv psapi dbghelp )
endif()

source_group( voice FILES ${VOICE_HEADERS} ${VOICE_SOURCES} )
include_directories( ${VOICE_INCLUDE_DIRS} )

add_executable( voice-server ${VOICE_HEADERS} ${VOICE_SOURCES} )
target_link_libraries( voice-server ${LIBRARIES} )
set_target_properties( voice-server PROPERTIES COMPILE_FLAGS "${DEFINITIONS}" )

if( INSTALL_COMPONENT_RUNTIME )
	cpack_add_component( Runtime_voiceserver
		DESCRIPTION "voice-server"
		DISPLAY_NAME "voice-server"
		GROUP Runtime )
	install( TARGETS voice-server
		DESTINATION "."
		COMPONENT Runtime_voiceserver )
endif()

set( TARGET_LIST ${TARGET_LIST} voice-server CACHE INTERNAL "" )
message( STATUS "Creating target voice-server - done" )
