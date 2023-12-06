cmake_minimum_required(VERSION 3.10)
project(kvsd_stream_master)

set(CMAKE_BUILD_TYPE "Debug")

add_executable(${PROJECT_NAME} main.c)

# Config for cJSON
option(ENABLE_CJSON_TEST "Enable testing of cJSON" OFF)
option(ENABLE_CJSON_UTILS "Enable cJSON utilities" OFF)
option(ENABLE_VALGRIND "Enable cJSON testing with valgrind" OFF)
option(BUILD_SHARED_LIBS "Build shared library" OFF)
# \Config for cJSON

if(DEFINED ENV{PLATFORM_X86})
	message("PLATFORM_X86")
	find_package(PkgConfig REQUIRED)
	pkg_search_module(gstreamer REQUIRED IMPORTED_TARGET gstreamer-1.0>=1.4)
	pkg_search_module(gstreamer-sdp REQUIRED IMPORTED_TARGET gstreamer-sdp-1.0>=1.4)
	pkg_search_module(gstreamer-app REQUIRED IMPORTED_TARGET gstreamer-app-1.0>=1.4)
	pkg_search_module(gstreamer-video REQUIRED IMPORTED_TARGET gstreamer-video-1.0>=1.4)

	add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/cJSON-1.7.16 cJSON)

	target_link_libraries(${PROJECT_NAME}
			PkgConfig::gstreamer
			PkgConfig::gstreamer-sdp
			PkgConfig::gstreamer-app
			PkgConfig::gstreamer-video
			pthread
			cjson
	)

	target_include_directories(${PROJECT_NAME} PUBLIC
		${CMAKE_CURRENT_SOURCE_DIR}/cJSON-1.7.16
	)
else()
	message("Embedded platform")
	set(CMAKE_POSITION_INDEPENDENT_CODE ON)

	add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/cJSON-1.7.16 cJSON)

	target_link_libraries(${PROJECT_NAME} PRIVATE
			z
			ffi
			gmodule-2.0
			gobject-2.0
			gio-2.0
			glib-2.0
			gstreamer-1.0
			gstbase-1.0
			gstapp-1.0
			gstvideo-1.0
			pthread
			cjson
	)

	target_include_directories(${PROJECT_NAME} PUBLIC
			${CMAKE_CURRENT_SOURCE_DIR}/cJSON-1.7.16
	)

	target_link_directories(${PROJECT_NAME} PUBLIC
	${CMAKE_LIBRARY_PATH}
	)
endif()




