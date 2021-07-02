SET (TOOLCHAIN 1)

function (toolchain_after_project)
	MESSAGE (STATUS "Toolchain: Linux-GNU-x86.cmake.")
	SET(CMAKE_SYSTEM_NAME	"Linux"		PARENT_SCOPE)
	SET(ARCH_BITS		32		PARENT_SCOPE)

	SET(CMAKE_RUNTIME_OUTPUT_DIRECTORY	"${CMAKE_BINARY_DIR}/bin"	PARENT_SCOPE)
endfunction()


function (toolchain_exe_stuff)
	SET (C_ARCH_OPTS	"-march=i686 -m32")
	SET (CXX_ARCH_OPTS	"-march=i686 -m32")

	SET (CMAKE_EXE_LINKER_FLAGS_EXTRA
		"-L/usr/lib/mysql -L/usr/lib/i386-linux-gnu/mysql\
		-Wl,-rpath=/usr/lib/mysql -Wl,-rpath=/usr/lib/i386-linux-gnu/mysql"
		PARENT_SCOPE)

	toolchain_exe_stuff_common()
endfunction()