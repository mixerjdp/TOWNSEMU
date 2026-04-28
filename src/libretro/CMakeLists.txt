add_library(tsugaru_libretro MODULE
	libretro.cpp
	libretro.h
)

target_link_libraries(tsugaru_libretro
	PRIVATE
	towns
	townscommand
	townsparam
	townsrender
	outside_world
	discimg
	diskdrive
	yssimplesound_nownd
)

target_include_directories(tsugaru_libretro
	PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_definitions(tsugaru_libretro PRIVATE TSUGARU_LIBRETRO_CORE)

set_target_properties(tsugaru_libretro PROPERTIES
	PREFIX ""
	OUTPUT_NAME "tsugaru_libretro"
)

if(MSVC)
	target_compile_options(tsugaru_libretro PRIVATE /W4)
else()
	target_compile_options(tsugaru_libretro PRIVATE -Wall -Wextra)
endif()
