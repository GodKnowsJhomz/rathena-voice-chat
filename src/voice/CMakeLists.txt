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

if( NOT WITH_MYSQL )
	message( FATAL_ERROR "Stopping target voice-server (requires MYSQL)" )
endif()

set( VOICE_INCLUDE_DIRS
	${GLOBAL_INCLUDE_DIRS}
	${RA_INCLUDE_DIRS}
	${JSON_INCLUDE_DIRS}
	${MYSQL_INCLUDE_DIRS}
)

set( VOICE_SOURCES
	${VOICE_SOURCE_DIR}/config.cpp
	${VOICE_SOURCE_DIR}/main.cpp
	${VOICE_SOURCE_DIR}/voice_transport.cpp
	${VOICE_SOURCE_DIR}/room_manager.cpp
	${VOICE_SOURCE_DIR}/server.cpp
	${VOICE_SOURCE_DIR}/whisper_manager.cpp
)

set( VOICE_HEADERS
	${VOICE_SOURCE_DIR}/config.hpp
	${VOICE_SOURCE_DIR}/player.hpp
	${VOICE_SOURCE_DIR}/voice_transport.hpp
	${VOICE_SOURCE_DIR}/room_manager.hpp
	${VOICE_SOURCE_DIR}/whisper_manager.hpp
)

set( LIBRARIES ${GLOBAL_LIBRARIES} ${MYSQL_LIBRARIES} )
set( DEFINITIONS "${GLOBAL_DEFINITIONS}" )

if( WIN32 )
	set( DEFINITIONS "${DEFINITIONS} -DWIN32_LEAN_AND_MEAN -DNOMINMAX -DFD_SETSIZE=4096" )
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
