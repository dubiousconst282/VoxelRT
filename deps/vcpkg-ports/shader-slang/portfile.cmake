vcpkg_check_linkage(ONLY_DYNAMIC_LIBRARY)

set(key NOTFOUND)
if(VCPKG_TARGET_IS_WINDOWS)
	set(key "windows-${VCPKG_TARGET_ARCHITECTURE}")
elseif(VCPKG_TARGET_IS_OSX)
	set(key "macosx-${VCPKG_TARGET_ARCHITECTURE}")
elseif(VCPKG_TARGET_IS_LINUX)
	set(key "linux-${VCPKG_TARGET_ARCHITECTURE}")
endif()
string(REPLACE "arm64" "aarch64" key "${key}")

set(ARCHIVE NOTFOUND)
# For convenient updates, use 
# vcpkg install shader-slang --cmake-args=-DVCPKG_SHADER_SLANG_UPDATE=1
if(key STREQUAL "windows-x86" OR VCPKG_SHADER_SLANG_UPDATE)
	vcpkg_download_distfile(
		ARCHIVE
		URLS "https://github.com/shader-slang/slang/releases/download/v${VERSION}/slang-${VERSION}-win32.zip"
		FILENAME "slang-${VERSION}-win32.zip"
		SHA512 70e80a8985e147f8c5198a729ba3d967ccb98e6850e1c8f4c6126d0240ecdb7d347cbdca080ca2925e8abc9aed552f31733d34e0d90131dc4d619e863abcf0f9
	)
endif()
if(key STREQUAL "windows-x64" OR VCPKG_SHADER_SLANG_UPDATE)
	vcpkg_download_distfile(
		ARCHIVE
		URLS "https://github.com/shader-slang/slang/releases/download/v${VERSION}/slang-${VERSION}-win64.zip"
		FILENAME "slang-${VERSION}-win64.zip"
		SHA512 0758f2d46a82fc1f044cc2627a03f6a5677aae36ba0e7f6b9293252a33e277eaec31e1bad93448b27da15534aadd9f2f2c5b21644e3d11f0c5064232caf8965b
	)
endif()
if(key STREQUAL "windows-aarch64" OR VCPKG_SHADER_SLANG_UPDATE)
	vcpkg_download_distfile(
		ARCHIVE
		URLS "https://github.com/shader-slang/slang/releases/download/v${VERSION}/slang-${VERSION}-win-arm64.zip"
		FILENAME "slang-${VERSION}-win-arm64.zip"
		SHA512 394ad46c547ef7d49383c4577350256ca7676840b0edece6cb7e04b3138217279eb342557a1ef35398c949d3b3856dbef9e517cc7a1d20a65154596bc8f16af1
	)
endif()
if(key STREQUAL "macosx-x64" OR VCPKG_SHADER_SLANG_UPDATE)
	vcpkg_download_distfile(
		ARCHIVE
		URLS "https://github.com/shader-slang/slang/releases/download/v${VERSION}/slang-${VERSION}-macos-x64.zip"
		FILENAME "slang-${VERSION}-macos-x64.zip"
		SHA512 35b3aec80f5b6b6aa0cb15fdac86ae153cbf23da8fb8a49114d655789912b41286912e21c9b202d32af07fd088f04ebc414ceb9617f3cf1ef4747ddbaa13b886
	)
endif()
if(key STREQUAL "macosx-aarch64" OR VCPKG_SHADER_SLANG_UPDATE)
	vcpkg_download_distfile(
		ARCHIVE
		URLS "https://github.com/shader-slang/slang/releases/download/v${VERSION}/slang-${VERSION}-macos-aarch64.zip"
		FILENAME "slang-${VERSION}-macos-aarch64.zip"
		SHA512 41bb8608604c510a4a4b21cc9646a26a48dc7d92923459cff6ba963731073c62b5b723ff88387d10bafcbf5ca7e31730bd35aa952219df2558b9d33cb436670a
	)
endif()
if(key STREQUAL "linux-x64" OR VCPKG_SHADER_SLANG_UPDATE)
	vcpkg_download_distfile(
		ARCHIVE
		URLS "https://github.com/shader-slang/slang/releases/download/v${VERSION}/slang-${VERSION}-linux-x86_64.zip"
		FILENAME "slang-${VERSION}-linux-x86_64.zip"
		SHA512 41eac4440039d56e7d71b6f69db2926381673b32f8b322559ce967b02d3624cabf360b3a3e71624eff6839e51c030746cc700674570956d5d2fafb30269c848f
	)
endif()
if(key STREQUAL "linux-aarch64" OR VCPKG_SHADER_SLANG_UPDATE)
	vcpkg_download_distfile(
		ARCHIVE
		URLS "https://github.com/shader-slang/slang/releases/download/v${VERSION}/slang-${VERSION}-linux-aarch64.zip"
		FILENAME "slang-${VERSION}-linux-aarch64.zip"
		SHA512 c40947e0137371486e5b6625d3c20546454b464cb0a4b1092f7a6a0e5ef171b78ab159f57ef3e4b957a67144fb2f6d91fd63bce408ace285166971f508b2c4f5
	)
endif()
if(NOT ARCHIVE)
	message(FATAL_ERROR "Unsupported platform. Please implement me!")
endif()

vcpkg_extract_source_archive(
	BINDIST_PATH
	ARCHIVE "${ARCHIVE}"
	NO_REMOVE_ONE_LEVEL
)

# https://github.com/shader-slang/slang/issues/4117
if(NOT EXISTS "${BINDIST_PATH}/LICENSE" OR VCPKG_SHADER_SLANG_UPDATE)
	vcpkg_download_distfile(
		LICENSE_ARCHIVE
		URLS "https://github.com/shader-slang/slang/releases/download/v${VERSION}/slang-${VERSION}-source.zip"
		FILENAME "slang-${VERSION}-source.zip"
		SHA512 ce1939eb8a9dc5ca86cc0a0ac99fda2e2fd3743e13b0d015dc73ce9e165266fc3f8651666a865458f2d379d0aac75c8cef28c8e5e6d05fbfb0d78b92366d771a
	)
	vcpkg_extract_source_archive(
		SOURCE_PATH
		ARCHIVE "${LICENSE_ARCHIVE}"
		NO_REMOVE_ONE_LEVEL
	)
	file(COPY "${SOURCE_PATH}/LICENSE" DESTINATION "${BINDIST_PATH}")
endif()

if(VCPKG_SHADER_SLANG_UPDATE)
	message(STATUS "All downloads are up-to-date.")
	message(FATAL_ERROR "Stopping due to VCPKG_SHADER_SLANG_UPDATE being enabled.")
endif()

set(SLANG_BIN_PATH "bin/${key}/release")
file(GLOB libs
	"${BINDIST_PATH}/${SLANG_BIN_PATH}/*.lib"
	"${BINDIST_PATH}/${SLANG_BIN_PATH}/*.dylib"
	"${BINDIST_PATH}/${SLANG_BIN_PATH}/*.so"
)
file(INSTALL ${libs} DESTINATION "${CURRENT_PACKAGES_DIR}/lib")

if(VCPKG_TARGET_IS_WINDOWS)
	file(GLOB dlls "${BINDIST_PATH}/${SLANG_BIN_PATH}/*.dll")
	file(INSTALL ${dlls} DESTINATION "${CURRENT_PACKAGES_DIR}/bin")
endif()

if(NOT VCPKG_BUILD_TYPE)
	file(INSTALL "${CURRENT_PACKAGES_DIR}/lib" DESTINATION "${CURRENT_PACKAGES_DIR}/debug/lib")
	if(VCPKG_TARGET_IS_WINDOWS)
		file(INSTALL "${CURRENT_PACKAGES_DIR}/bin" DESTINATION "${CURRENT_PACKAGES_DIR}/debug/bin")
	endif()
endif()

vcpkg_copy_tools(TOOL_NAMES slangc slangd SEARCH_DIR "${BINDIST_PATH}/${SLANG_BIN_PATH}")

file(GLOB headers "${BINDIST_PATH}/*.h" "${BINDIST_PATH}/prelude")
file(INSTALL ${headers} DESTINATION "${CURRENT_PACKAGES_DIR}/include")

vcpkg_install_copyright(
	FILE_LIST "${BINDIST_PATH}/LICENSE"
	COMMENT #[[ from README ]] [[
The Slang code itself is under the MIT license.

Builds of the core Slang tools depend on the following projects, either automatically or optionally, which may have their own licenses:

* [`glslang`](https://github.com/KhronosGroup/glslang) (BSD)
* [`lz4`](https://github.com/lz4/lz4) (BSD)
* [`miniz`](https://github.com/richgel999/miniz) (MIT)
* [`spirv-headers`](https://github.com/KhronosGroup/SPIRV-Headers) (Modified MIT)
* [`spirv-tools`](https://github.com/KhronosGroup/SPIRV-Tools) (Apache 2.0)
* [`ankerl::unordered_dense::{map, set}`](https://github.com/martinus/unordered_dense) (MIT)

Slang releases may include [slang-llvm](https://github.com/shader-slang/slang-llvm) which includes [LLVM](https://github.com/llvm/llvm-project) under the license:

* [`llvm`](https://llvm.org/docs/DeveloperPolicy.html#new-llvm-project-license-framework) (Apache 2.0 License with LLVM exceptions)
]])