/**
* @file sphereversion.h
* @brief Versioning stuff.
*/

#ifndef _INC_SPHEREVERSION_H
#define _INC_SPHEREVERSION_H

#ifdef _GITVERSION
	#include "version/GitRevision.h"
	#define SPHERE_VER_BUILD			__GITREVISION__
#else
	#define SPHERE_VER_BUILD			0
#endif

#define SPHERE_VER_FILEVERSION		1,1,0,SPHERE_VER_BUILD		// version to be set on generated .exe file (Windows only)
#define SPHERE_VER_ID_STR           "110"                       // to be used for script and server VERSION property, which is numerical
#define SPHERE_VER_NAME				"X1"						// share version with all files

#define SPHERE_VERSION_PREFIX       "Version "
#if defined(_DEBUG)
	#define SPHERE_VERSION					SPHERE_VER_NAME "-Debug"
	#define SPHERE_VER_FILEFLAGS			0x1L	//VS_FF_DEBUG
#elif defined(_NIGHTLYBUILD)
	#define SPHERE_VERSION					SPHERE_VER_NAME "-Nightly"
	#define SPHERE_VER_FILEFLAGS			0x2L	//VS_FF_PRERELEASE
#elif defined(_PRIVATEBUILD)
	#define SPHERE_VERSION					SPHERE_VER_NAME "-Private"
	#define SPHERE_VER_FILEFLAGS			0x8L	//VS_FF_PRIVATEBUILD
#else
	#define SPHERE_VERSION					SPHERE_VER_NAME "-Release"
	#define SPHERE_VER_FILEFLAGS			0x0L
#endif

#if defined(_WIN32)
	#define SPHERE_VER_FILEOS				0x4L	// VOS__WINDOWS32
	#if defined(_64BITS)
		#define SPHERE_VER_FILEOS_STR		"[WIN64]"
	#else
		#define SPHERE_VER_FILEOS_STR		"[WIN32]"
	#endif
#elif defined(_BSD)
	#define SPHERE_VER_FILEOS				0x0L	// VOS_UNKNOWN
	#if defined(_64BITS)
		#define SPHERE_VER_FILEOS_STR		"[FreeBSD-64]"
	#else
		#define SPHERE_VER_FILEOS_STR		"[FreeBSD-32]"
	#endif
#elif __APPLE__
    #define SPHERE_VER_FILEOS                0x0L    // VOS_UNKNOWN
    #define SPHERE_VER_FILEOS_STR        "[OSX-64]"
#else
	#define SPHERE_VER_FILEOS				0x0L	// VOS_UNKNOWN
	#if defined(_64BITS)
		#define SPHERE_VER_FILEOS_STR		"[Linux-64]"
	#else
		#define SPHERE_VER_FILEOS_STR		"[Linux-32]"
	#endif
#endif


#endif // _INC_SPHEREVERSION_H
