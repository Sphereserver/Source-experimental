SET (TOOLCHAIN 1)

function (toolchain_after_project)
	MESSAGE (STATUS "Toolchain: OSX-64.cmake.")
	SET(CMAKE_SYSTEM_NAME	"OSX"		PARENT_SCOPE)
	SET(ARCH_BITS		64		PARENT_SCOPE)

	SET(CMAKE_RUNTIME_OUTPUT_DIRECTORY	"${CMAKE_BINARY_DIR}/bin64"	PARENT_SCOPE)
endfunction()


function (toolchain_exe_stuff)
	#-- Setting compiler flags common to all builds.

	SET (C_WARNING_OPTS
		"-w") # this line is for warnings issued by 3rd party C code
	SET (CXX_WARNING_OPTS
		"-w")
	SET (C_ARCH_OPTS	"-march=x86-64 -m64")
	SET (CXX_ARCH_OPTS	"-march=x86-64 -m64")
	SET (C_OPTS		"-std=c11   -pthread -fexceptions -fnon-call-exceptions")
	SET (CXX_OPTS		"-std=c++17 -pthread -fexceptions -fnon-call-exceptions")
	SET (C_SPECIAL		"-pipe -fno-expensive-optimizations")
	SET (CXX_SPECIAL	"-pipe -ffast-math")

	SET (CMAKE_C_FLAGS	"${C_WARNING_OPTS} ${C_ARCH_OPTS} ${C_OPTS} ${C_SPECIAL} ${C_FLAGS_EXTRA}"		PARENT_SCOPE)
	SET (CMAKE_CXX_FLAGS	"${CXX_WARNING_OPTS} ${CXX_ARCH_OPTS} ${CXX_OPTS} ${CXX_SPECIAL} ${CXX_FLAGS_EXTRA}"	PARENT_SCOPE)


	#-- Setting common linker flags

	 # -s and -g need to be added/removed also to/from linker flags!
	SET (CMAKE_EXE_LINKER_FLAGS	"-pthread -dynamic\
					-I/usr/local/opt/mysql-client/include\
					-L/usr/local/opt/mysql-client/lib -lmysqlclient"
					PARENT_SCOPE)


	#-- Adding compiler flags per build.

	 # (note: since cmake 3.3 the generator $<COMPILE_LANGUAGE> exists).
	 # do not use " " to delimitate these flags!
	 # -s: strips debug info (remove it when debugging); -g: adds debug informations;
	 # -fno-omit-frame-pointer disables a good optimization which may corrupt the debugger stack trace.
	IF (TARGET spheresvr_release)
		TARGET_COMPILE_OPTIONS ( spheresvr_release	PUBLIC -s -O3 	)
	ENDIF (TARGET spheresvr_release)
	IF (TARGET spheresvr_nightly)
		TARGET_COMPILE_OPTIONS ( spheresvr_nightly	PUBLIC -O3    )
	ENDIF (TARGET spheresvr_nightly)
	IF (TARGET spheresvr_debug)
		IF (${ENABLE_SANITIZERS})
			SET (SANITIZERS "-fsanitize=address,undefined")
		ENDIF (${ENABLE_SANITIZERS})
		TARGET_COMPILE_OPTIONS ( spheresvr_debug	PUBLIC -ggdb3 -Og -fno-omit-frame-pointer ${SANITIZERS} )
	ENDIF (TARGET spheresvr_debug)


	#-- Setting per-build linker flags.

	 # Linking Unix libs.
	 # same here, do not use " " to delimitate these flags!
	IF (TARGET spheresvr_release)
		TARGET_LINK_LIBRARIES ( spheresvr_release	mysqlclient rt dl )
	ENDIF (TARGET spheresvr_release)
	IF (TARGET spheresvr_nightly)
		TARGET_LINK_LIBRARIES ( spheresvr_nightly	mysqlclient rt dl )
	ENDIF (TARGET spheresvr_nightly)
	IF (TARGET spheresvr_debug)
		TARGET_LINK_LIBRARIES ( spheresvr_debug		mysqlclient rt dl )
	ENDIF (TARGET spheresvr_debug)

	#-- Set common define macros.
	
	add_definitions(-D_64BITS)
	add_definitions(-DZ_PREFIX)
	add_definitions(-D_EXCEPTIONS_DEBUG)
	# _64BITS: 64 bits architecture.
	# Z_PREFIX: Use the "z_" prefix for the zlib functions
	# _EXCEPTIONS_DEBUG: Enable advanced exceptions catching. Consumes some more resources, but is very useful for debug
	#   on a running environment. Also it makes sphere more stable since exceptions are local.

	#-- Set per-build define macros.

	IF (TARGET spheresvr_release)
		TARGET_COMPILE_DEFINITIONS ( spheresvr_release	PUBLIC NDEBUG )
	ENDIF (TARGET spheresvr_release)
	IF (TARGET spheresvr_nightly)
		TARGET_COMPILE_DEFINITIONS ( spheresvr_nightly	PUBLIC NDEBUG THREAD_TRACK_CALLSTACK _NIGHTLYBUILD )
	ENDIF (TARGET spheresvr_nightly)
	IF (TARGET spheresvr_debug)
		TARGET_COMPILE_DEFINITIONS ( spheresvr_debug	PUBLIC _DEBUG THREAD_TRACK_CALLSTACK _PACKETDUMP )
	ENDIF (TARGET spheresvr_debug)
endfunction()
