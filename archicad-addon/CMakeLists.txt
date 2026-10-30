cmake_minimum_required (VERSION 3.17)

include (Tools/CMakeCommon.cmake)

set_property (GLOBAL PROPERTY USE_FOLDERS ON)

set (CMAKE_CONFIGURATION_TYPES Debug;Release;RelWithDebInfo)
set (AC_API_DEVKIT_DIR ${AC_API_DEVKIT_DIR} CACHE PATH "API DevKit directory.")
set (AC_VERSION ${API_AC_VERSION} CACHE STRING "Archicad version.")

SetGlobalCompilerDefinitions (${AC_VERSION})
# Renzo's build: no Tapir update check and no "Check for Updates..." menu item, so Tapir's updater
# never replaces this add-on with the official one.
add_definitions (-DRENZO_ADDON)

if (WIN32)
	set (NamePostFix Win)
else ()
	set (NamePostFix Mac)
endif ()
set (AddOnName "RenzoArchicad_AC${AC_VERSION}_${NamePostFix}")

project (${AddOnName})

set (AddOnSourcesFolder Sources)
set (AddOnResourcesFolder Sources)
GenerateAddOnProject (AddOn ${AC_VERSION} ${AC_API_DEVKIT_DIR} ${AddOnName} ${AddOnSourcesFolder} ${AddOnResourcesFolder} INT)
