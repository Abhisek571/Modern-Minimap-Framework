# x64-windows-static-md, except DirectXTex: that one is linked only into the Photo Baker app, which uses the static
# runtime (/MT) so it stays a single exe. Everything else (the SKSE plugin, the tests) uses the DLL runtime (/MD).
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
if(PORT STREQUAL "directxtex")
	set(VCPKG_CRT_LINKAGE static)
endif()
