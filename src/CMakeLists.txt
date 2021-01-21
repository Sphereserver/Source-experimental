#	Main config	#

CMAKE_MINIMUM_REQUIRED (VERSION 3.1)


MESSAGE (STATUS "Scanning system for compilers...")
PROJECT (SphereServer)	# does a scan for C++ and C compilers


# If we have not specified a toolchain, let's detect which one we should use
IF (NOT TOOLCHAIN)

MESSAGE (STATUS "Toolchain not specified. Detecting the one to use.")
IF (MSVC)
	INCLUDE ("cmake/toolchains/Windows-MSVC.cmake")
ELSE (MSVC)
	MESSAGE (STATUS "Toolchain: defaulting to 64 bits compiler.")
	# ...or we can detect the architecture automatically
	# INCLUDE ("cmake/CMakeDetectArch.cmake")
	# target_architecture("ARCH_DETECT")

	IF (UNIX)
		#IF (ARCH_DETECT STREQUAL "x86")
		#	MESSAGE (STATUS "32 bits architecture detected.")
		#	INCLUDE ("cmake/toolchains/Linux-GNU-32.cmake")
		#ELSEIF (ARCH_DETECT STREQUAL "x86_64")
		#	MESSAGE (STATUS "64 bits architecture detected.")
		#	INCLUDE ("cmake/toolchains/Linux-GNU-64.cmake")
		#ELSE (ARCH_DETECT STREQUAL "x86")
		#	MESSAGE (STATUS "Unknown architecture, defaulting to 64 bits.")
		IF (APPLE)
			INCLUDE ("cmake/toolchains/OSX-64.cmake")
		ELSE (APPLE)
			INCLUDE ("cmake/toolchains/Linux-GNU-64.cmake")
		ENDIF (APPLE)
		#ENDIF (ARCH_DETECT STREQUAL "x86")
	ELSE (UNIX)
		#IF (ARCH_DETECT STREQUAL "x86")
		#	MESSAGE (STATUS "32 bits architecture detected.")
		#	INCLUDE ("cmake/toolchains/Windows-GNU-32.cmake")
		#ELSEIF (ARCH_DETECT STREQUAL "x86_64")
		#	MESSAGE (STATUS "64 bits architecture detected.")
		#	INCLUDE ("cmake/toolchains/Windows-GNU-64.cmake")
		#ELSE (ARCH_DETECT STREQUAL "x86")
		#	MESSAGE (STATUS "Unknown architecture, defaulting to 64 bits.")
			INCLUDE ("cmake/toolchains/Windows-GNU-64.cmake")
		#ENDIF (ARCH_DETECT STREQUAL "x86")
	ENDIF (UNIX)
ENDIF (MSVC)

ENDIF (NOT TOOLCHAIN)


toolchain_after_project()   # stuff that need to be executed before ADD_EXECUTABLE

# Need to clear shared library flags. If not, cmake sets -rdynamic and this
# add to the executable the full symbol table (included unused symbols).
# This is a problem because the binary is ~700 KB bigger.
SET (CMAKE_SHARED_LIBRARY_LINK_C_FLAGS)
SET (CMAKE_SHARED_LIBRARY_LINK_CXX_FLAGS)


STRING (FIND "${CMAKE_GENERATOR}" "Makefiles" MAKEFILE)
IF ( (MAKEFILE GREATER 0) AND (NOT MSVC) )
	SET (SINGLE_TARGET 1)
ELSEIF ( MSVC )
	#SET (SINGLE_TARGET)
ENDIF ( (MAKEFILE GREATER 0) AND (NOT MSVC) )

IF (SINGLE_TARGET)
	# If you want to manually specify the build type, call cmake with parameter: -DCMAKE_BUILD_TYPE=something

	MESSAGE (STATUS "Single-target build system (Makefile) detected: generating multiple projects!")
	IF (NOT ${CMAKE_BUILD_TYPE} STREQUAL "")
		IF ((NOT ${CMAKE_BUILD_TYPE} STREQUAL "Release") AND (NOT ${CMAKE_BUILD_TYPE} STREQUAL "Debug") AND (NOT ${CMAKE_BUILD_TYPE} STREQUAL "Nightly"))
			MESSAGE (WARNING "Invalid parameter -DCMAKE_BUILD_TYPE, defaulting to Release.")
			# -> needed only for MAKEFILE-STYLE generators, which can't switch between different configs
			set(CMAKE_BUILD_TYPE "Release" CACHE STRING "" FORCE)
		ELSE ((NOT ${CMAKE_BUILD_TYPE} STREQUAL "Release") AND (NOT ${CMAKE_BUILD_TYPE} STREQUAL "Debug") AND (NOT ${CMAKE_BUILD_TYPE} STREQUAL "Nightly"))
			MESSAGE (STATUS "Generating only specified project: ${CMAKE_BUILD_TYPE}.")
		ENDIF ((NOT ${CMAKE_BUILD_TYPE} STREQUAL "Release") AND (NOT ${CMAKE_BUILD_TYPE} STREQUAL "Debug") AND (NOT ${CMAKE_BUILD_TYPE} STREQUAL "Nightly"))
	ELSE (NOT ${CMAKE_BUILD_TYPE} STREQUAL "")
		MESSAGE (STATUS "No target specified: building all the projects (Release, Debug, Nightly).")
		# The only situation supported here is using MSVC, and /MP (multi-core) is set in its compiler flags.
	ENDIF (NOT ${CMAKE_BUILD_TYPE} STREQUAL "")

	# Setting parallel make
	include(ProcessorCount)
	ProcessorCount(MAKE_THREADS)
	IF (NOT MAKE_THREADS EQUAL 0)
		MATH(EXPR MAKE_THREADS "${MAKE_THREADS} + (${MAKE_THREADS}/2)")		# Suggested number of threads: cores * 1.5
		set(CMAKE_MAKE_PROGRAM "${CMAKE_MAKE_PROGRAM} -j${MAKE_THREADS}")
	ELSE (NOT MAKE_THREADS EQUAL 0)
		message(STATUS "Can't determine CPU cores number. Parallel compilation turned off.")
	ENDIF (NOT MAKE_THREADS EQUAL 0)

ELSE (SINGLE_TARGET)

    MESSAGE (STATUS "Multi-target build system (Non-Makefile) detected: generating single project with multiple targets!")
    set(CMAKE_CONFIGURATION_TYPES "Debug;Release;Nightly" CACHE STRING "" FORCE)

ENDIF (SINGLE_TARGET)


# Include the list of our source files
INCLUDE ("cmake/CMakeSources.cmake")

SET(ALL_SRCS 
	${game_SRCS}
	${items_SRCS}
	${chars_SRCS}
	${clients_SRCS}
	${components_SRCS}
	${uofiles_SRCS}
	${common_SRCS}
	${resource_SRCS}
	${resourcesections_SRCS}
	${network_SRCS}
	${crypto_SRCS}
	${sphere_SRCS}
	${crashdump_SRCS}
	${libev_SRCS}
	${spherelibrary_SRCS}
	${sqlite_SRCS}
	${zlib_SRCS}
	${tables_SRCS}
)

IF (SINGLE_TARGET)

	IF (("${CMAKE_BUILD_TYPE}" STREQUAL "") OR (${CMAKE_BUILD_TYPE} MATCHES "(R|r?)elease"))
		SET(TARGETS ${TARGETS} spheresvr_release)
		ADD_EXECUTABLE (spheresvr_release
					${ALL_SRCS}
		#			${docs_TEXT}
			)
        	SET_TARGET_PROPERTIES(spheresvr_release PROPERTIES OUTPUT_NAME SphereSvrX${ARCH_BITS}_release)
	ENDIF ()
	IF (("${CMAKE_BUILD_TYPE}" STREQUAL "") OR (${CMAKE_BUILD_TYPE} MATCHES "(N|n?)ightly"))
		SET(TARGETS ${TARGETS} spheresvr_nightly)
		ADD_EXECUTABLE (spheresvr_nightly
					${ALL_SRCS}
		#			${docs_TEXT}
			)
        	SET_TARGET_PROPERTIES(spheresvr_nightly PROPERTIES OUTPUT_NAME SphereSvrX${ARCH_BITS}_nightly)
	ENDIF ()
	IF (("${CMAKE_BUILD_TYPE}" STREQUAL "") OR (${CMAKE_BUILD_TYPE} MATCHES "(D|d?)ebug"))
		SET(TARGETS ${TARGETS} spheresvr_debug)
		ADD_EXECUTABLE (spheresvr_debug
					${ALL_SRCS}
		#			${docs_TEXT}
			)
        	SET_TARGET_PROPERTIES(spheresvr_debug PROPERTIES OUTPUT_NAME SphereSvrX${ARCH_BITS}_debug)
	ENDIF ()

ELSE (SINGLE_TARGET)

	SET(TARGETS ${TARGETS} spheresvr)
    	ADD_EXECUTABLE (spheresvr
            ${ALL_SRCS}
            ${docs_TEXT}
		)
	SET_TARGET_PROPERTIES(spheresvr PROPERTIES OUTPUT_NAME_RELEASE SphereSvrX${ARCH_BITS}_release)
	SET_TARGET_PROPERTIES(spheresvr PROPERTIES OUTPUT_NAME_NIGHTLY SphereSvrX${ARCH_BITS}_nightly)
	SET_TARGET_PROPERTIES(spheresvr PROPERTIES OUTPUT_NAME_DEBUG SphereSvrX${ARCH_BITS}_debug)

ENDIF (SINGLE_TARGET)


toolchain_exe_stuff()   # stuff to be executed after ADD_EXECUTABLE


# Get the Git revision number
INCLUDE ("cmake/CMakeGitStatus.cmake")

# Generate config.h file for libev (if we are using it)
SET(USE_EV 0)
FOREACH (TARG ${TARGETS})
	GET_TARGET_PROPERTY(DEFS ${TARG} COMPILE_DEFINITIONS)
	FOREACH (DEF ${DEFS})
		IF ("${DEF}" STREQUAL "_LIBEV")
			SET(USE_EV 1)
		ENDIF ("${DEF}" STREQUAL "_LIBEV")
	ENDFOREACH (DEF ${DEFS})
ENDFOREACH (TARG ${TARGETS})
IF (USE_EV)
	INCLUDE ("${CMAKE_CURRENT_SOURCE_DIR}/common/libev/cmake/configure.cmake")
	CONFIGURE_FILE ( "${CMAKE_CURRENT_SOURCE_DIR}/common/libev/cmake/config.h.cmake" "${CMAKE_CURRENT_SOURCE_DIR}/common/libev/config.h" )
ENDIF (USE_EV)
