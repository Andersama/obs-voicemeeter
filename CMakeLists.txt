cmake_minimum_required(VERSION 3.5)

project(obs-voicemeeter)

##########################################
# set architecture                       #
##########################################
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
	set(ARCH 64)
else()
	set(ARCH 32)
endif()

if(ARCH EQUAL 64)
	set(OBS_ARCH_NAME "64bit")
	set(OBS_BUILDDIR_ARCH "build64")
else()
	set(OBS_ARCH_NAME "32bit")
	set(OBS_BUILDDIR_ARCH "build32")
endif()

########################################
# find libobs                          #
########################################
#include(external/ObsPluginHelpers.cmake)
#include(external/FindLibObs.cmake)
#if(NOT DEFINED OBS_FRONTEND_LIB)
#	set(OBS_FRONTEND_LIB "OBS_FRONTEND_LIB-NOTFOUND" CACHE FILEPATH "OBS frontend library")
#	message(FATAL_ERROR "Could not find OBS Frontend API\'s library !")
#else()
#	message(STATUS "${OBS_FRONTEND_LIB}")
#endif()

##########################################
# QT support                             #
##########################################
set(CMAKE_PREFIX_PATH "${QTDIR}")
set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)

include(external/FindLibObs.cmake)

find_package(LibObs REQUIRED)
find_package(Qt5Core REQUIRED)
find_package(Qt5Widgets REQUIRED)

include_directories(SYSTEM "obs-frontend-api")
include_directories(
#	SYSTEM "${LIBOBS_INCLUDE_DIR}"
#	"${LibObs_INCLUDE_DIR}/../UI/obs-frontend-api"
	"${LIBOBS_INCLUDE_DIR}/../UI/obs-frontend-api"
	/
#	"${LibObs_INCLUDE_DIR}/../${OBS_BUILDDIR_ARCH}/UI"
	${Qt5Core_INCLUDES}
	${Qt5Widgets_INCLUDES}
)

message(STATUS "${LibObs_INCLUDE_DIR}")

set(obs-voicemeeter_HEADERS
	VoicemeeterRemote.h
	circle-buffer.h
)

set(obs-voicemeeter_SOURCES
	obs-voicemeeter.cpp
)

add_library(obs-voicemeeter MODULE
	${obs-voicemeeter_HEADERS}
	${obs-voicemeeter_SOURCES}
)

target_link_libraries(obs-voicemeeter
	libobs
       "${OBS_FRONTEND_LIB}"
	obs-frontend-api
	Qt5::Core
	Qt5::Widgets
)

install_obs_plugin_with_data(obs-voicemeeter data)
#install_external_plugin_with_data(obs-voicemeeter data)
