option(BUILD_WARNINGS_AS_ERRORS "Enable building with -Wextra -Werror flags")

if(CMAKE_SYSTEM_NAME MATCHES "SunOS")
	set(CMD_MAKE gmake)
else()
	set(CMD_MAKE make)
endif()

if(NOT WIN32)

	set(LIBS_COMMON_FLAGS "-Wall -ggdb")
	set(LIBS_DEBUG_FLAGS "-D_DEBUG")
	set(LIBS_RELEASE_FLAGS "-O3 -fno-strict-aliasing -DNDEBUG")

	if(MINIMAL_BUILD)
	  set(LIBS_COMMON_FLAGS "${LIBS_COMMON_FLAGS} -DMINIMAL_BUILD")
	endif()

	if(MUSL_OPTIMIZED_BUILD)
		set(LIBS_COMMON_FLAGS "${LIBS_COMMON_FLAGS} -static -Os")
	endif()

	if(BUILD_WARNINGS_AS_ERRORS)
		set(CMAKE_SUPPRESSED_WARNINGS "-Wno-unused-parameter -Wno-missing-field-initializers -Wno-sign-compare -Wno-type-limits -Wno-implicit-fallthrough -Wno-format-truncation")
		set(LIBS_COMMON_FLAGS "${LIBS_COMMON_FLAGS} -Wextra -Werror ${CMAKE_SUPPRESSED_WARNINGS}")
	endif()

	set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${LIBS_COMMON_FLAGS}")
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${LIBS_COMMON_FLAGS} -std=c++0x")

	set(CMAKE_C_FLAGS_DEBUG "${LIBS_DEBUG_FLAGS}")
	set(CMAKE_CXX_FLAGS_DEBUG "${LIBS_DEBUG_FLAGS}")

	set(CMAKE_C_FLAGS_RELEASE "${LIBS_RELEASE_FLAGS}")
	set(CMAKE_CXX_FLAGS_RELEASE "${LIBS_RELEASE_FLAGS}")

	if(CMAKE_SYSTEM_NAME MATCHES "Linux")
		add_definitions(-DHAS_CAPTURE)
	endif()

else()
	set(MINIMAL_BUILD ON)

	set(LIBS_COMMON_FLAGS "-D_CRT_SECURE_NO_WARNINGS -DWIN32 -DMINIMAL_BUILD /EHsc /W3 /Zi")
	set(LIBS_DEBUG_FLAGS "/MTd /Od")
	set(LIBS_RELEASE_FLAGS "/MT")

	set(CMAKE_C_FLAGS "${LIBS_COMMON_FLAGS}")
	set(CMAKE_CXX_FLAGS "${LIBS_COMMON_FLAGS}")

	set(CMAKE_C_FLAGS_DEBUG "${LIBS_DEBUG_FLAGS}")
	set(CMAKE_CXX_FLAGS_DEBUG "${LIBS_DEBUG_FLAGS}")

	set(CMAKE_C_FLAGS_RELEASE "${LIBS_RELEASE_FLAGS}")
	set(CMAKE_CXX_FLAGS_RELEASE "${LIBS_RELEASE_FLAGS}")

	add_definitions(-DHAS_CAPTURE)

endif()

if(APPLE)
	set(CMAKE_EXE_LINKER_FLAGS "-pagezero_size 10000 -image_base 100000000")
endif()


