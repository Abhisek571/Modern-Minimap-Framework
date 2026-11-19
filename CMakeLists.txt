cmake_minimum_required(VERSION 3.21)

project(ModernMinimap VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

# Keep build-machine paths out of the binary (__FILE__ / source_location).
file(TO_NATIVE_PATH "${CMAKE_SOURCE_DIR}/" MM_SOURCE_ROOT)
add_compile_options("/d1trimfile:${MM_SOURCE_ROOT}")

find_package(directxtk CONFIG REQUIRED)

set(MM_WARNINGS /W4 /permissive- /external:anglebrackets /external:W0)
set(MM_DEFINES WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)

# Source folders, one per feature:
#   src/Minimap    drawing the minimap on the HUD (renderer, shaders, tile cache, map maths, draw hook)
#   src/Terrain    reading the game's terrain LOD meshes
#   src/Interiors  the room outline for interiors, built from navmeshes
#   src/Icons      map markers, doors, quest targets and actors
#   src/PhotoBake  the in-game town photo bake and its image processing
#   src/Settings   the ini file and the SKSE Menu Framework page
set(MM_SOURCE_DIRS src src/Minimap src/Terrain src/Interiors src/Icons src/PhotoBake src/Settings)

# The parts with no game types (maths, mesh reading, drawing, image processing): shared by the plugin and the test tool.
add_library(MinimapCore STATIC
	src/Minimap/MapMath.cpp
	src/Minimap/TileCache.cpp
	src/Minimap/MapRenderer.cpp
	src/Terrain/TerrainMesh.cpp
	src/Interiors/InteriorMap.cpp
	src/Icons/Icons.cpp
	src/PhotoBake/PhotoProcessing.cpp
)
target_include_directories(MinimapCore PUBLIC ${MM_SOURCE_DIRS})
target_compile_definitions(MinimapCore PUBLIC ${MM_DEFINES})
target_compile_options(MinimapCore PRIVATE ${MM_WARNINGS} /utf-8)
target_link_libraries(MinimapCore PUBLIC Microsoft::DirectXTK d3d11 dxgi d3dcompiler)

set(ENABLE_SKYRIM_SE ON CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_AE ON CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_VR OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(lib/commonlibsse-ng CommonLibSSE EXCLUDE_FROM_ALL)
get_target_property(COMMONLIB_SRC_DIR CommonLibSSE SOURCE_DIR)
include(${COMMONLIB_SRC_DIR}/cmake/CommonLibSSE.cmake)

add_commonlibsse_plugin(${PROJECT_NAME}
	AUTHOR "itsPigeon2000"
	SOURCES
		src/main.cpp
		src/Minimap/Minimap.cpp
		src/Minimap/Hooks.cpp
		src/Icons/IconData.cpp
		src/PhotoBake/PhotoBake.cpp
		src/Settings/Settings.cpp
		src/Settings/SettingsMenu.cpp
)
target_compile_options(${PROJECT_NAME} PRIVATE /Zc:preprocessor /utf-8 ${MM_WARNINGS})
target_precompile_headers(${PROJECT_NAME} PRIVATE src/PCH.h)
target_include_directories(${PROJECT_NAME} PRIVATE ${MM_SOURCE_DIRS})
# Third-party header (SKSE Menu Framework API): angle-bracket include, so its warnings stay external.
target_include_directories(${PROJECT_NAME} SYSTEM PRIVATE include)
target_link_libraries(${PROJECT_NAME} PRIVATE MinimapCore)

# Offline test tool: maths, btr parsing against real tiles, and a render of the tile pass to PNG.
add_executable(ModernMinimapTests tests/TestMain.cpp)
target_compile_options(ModernMinimapTests PRIVATE ${MM_WARNINGS} /utf-8)
target_link_libraries(ModernMinimapTests PRIVATE MinimapCore windowscodecs ole32)
