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
if(key STREQUAL "windows-x64" OR VCPKG_SHADER_SLANG_UPDATE)
	vcpkg_download_distfile(
		ARCHIVE
		URLS "https://github.com/shader-slang/slang/releases/download/v${VERSION}/slang-${VERSION}-windows-x86_64.zip"
		FILENAME "slang-${VERSION}-windows-x86_64.zip"
		SHA512 6f19e8a59462a70d6615bb27768090df4da837c79e67ed130d15fb684ceb4341e3fe31411b814acc1b1540305fdfad22004ad4c3b697e8236e77cacba27816f5
	)
endif()
if(key STREQUAL "windows-aarch64" OR VCPKG_SHADER_SLANG_UPDATE)
	vcpkg_download_distfile(
		ARCHIVE
		URLS "https://github.com/shader-slang/slang/releases/download/v${VERSION}/slang-${VERSION}-windows-aarch64.zip"
		FILENAME "slang-${VERSION}-windows-aarch64.zip"
		SHA512 07f48beb5ec676de71bb0d8774fae6984e720ce4fcc47cbb684c87bc9763a8e4a6440175e801425a1a17aa2fd9292aa6dc18da6a085b0524cfe2e80d701389df
	)
endif()
# if(key STREQUAL "macosx-x64" OR VCPKG_SHADER_SLANG_UPDATE)
# 	vcpkg_download_distfile(
# 		ARCHIVE
# 		URLS "https://github.com/shader-slang/slang/releases/download/v${VERSION}/slang-${VERSION}-macos-x86_64.zip"
# 		FILENAME "slang-${VERSION}-macos-x86_64.zip"
# 		SHA512 9acda6c43c5b56d8cc33be7f150c7b25e00373d036f0629eae28fa134621a4c0db1be4bd319d243723308e1455456268b037b9fb4d661ef53213ce0ef1ec326d
# 	)
# endif()
if(key STREQUAL "macosx-aarch64" OR VCPKG_SHADER_SLANG_UPDATE)
	vcpkg_download_distfile(
		ARCHIVE
		URLS "https://github.com/shader-slang/slang/releases/download/v${VERSION}/slang-${VERSION}-macos-aarch64.zip"
		FILENAME "slang-${VERSION}-macos-aarch64.zip"
		SHA512 166950d26df51818eb2206a1d3659d7061b1853b562dce43fa436971ff8fe946cd4d7a96afa7aa20016b3ae2a2617759220eb424b7caa2494d10ff53222da057
	)
endif()
if(key STREQUAL "linux-x64" OR VCPKG_SHADER_SLANG_UPDATE)
	vcpkg_download_distfile(
		ARCHIVE
		URLS "https://github.com/shader-slang/slang/releases/download/v${VERSION}/slang-${VERSION}-linux-x86_64.zip"
		FILENAME "slang-${VERSION}-linux-x86_64.zip"
		SHA512 2679732c9b27b97e347f6609414ad6953ffe1e165544d16aca48cffaaee6bfb63a2e4726fbe5ef533716788e98a1d47b0e20ecdd23158f9311f92161fcc25631
	)
endif()
if(key STREQUAL "linux-aarch64" OR VCPKG_SHADER_SLANG_UPDATE)
	vcpkg_download_distfile(
		ARCHIVE
		URLS "https://github.com/shader-slang/slang/releases/download/v${VERSION}/slang-${VERSION}-linux-aarch64.zip"
		FILENAME "slang-${VERSION}-linux-aarch64.zip"
		SHA512 3ce15de5a7a770460108bee42706a3fa83ed2cfce24297b65cfd7e61b3568e37aaf4f38b8e4b2ea7352abfb932bdc43065b361858900fc02aca136730f22d4bb
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

if(VCPKG_SHADER_SLANG_UPDATE)
	message(STATUS "All downloads are up-to-date.")
	message(FATAL_ERROR "Stopping due to VCPKG_SHADER_SLANG_UPDATE being enabled.")
endif()

file(GLOB libs
	"${BINDIST_PATH}/lib/*.lib"
	"${BINDIST_PATH}/lib/*.dylib"
	"${BINDIST_PATH}/lib/*.so"
)
file(INSTALL ${libs} DESTINATION "${CURRENT_PACKAGES_DIR}/lib")

if(VCPKG_TARGET_IS_WINDOWS)
	file(GLOB dlls "${BINDIST_PATH}/bin/*.dll")
	file(INSTALL ${dlls} DESTINATION "${CURRENT_PACKAGES_DIR}/bin")
endif()

if(NOT VCPKG_BUILD_TYPE)
	file(INSTALL "${CURRENT_PACKAGES_DIR}/lib" DESTINATION "${CURRENT_PACKAGES_DIR}/debug/lib")
	if(VCPKG_TARGET_IS_WINDOWS)
		file(INSTALL "${CURRENT_PACKAGES_DIR}/bin" DESTINATION "${CURRENT_PACKAGES_DIR}/debug/bin")
	endif()
endif()

vcpkg_copy_tools(TOOL_NAMES slangc slangd SEARCH_DIR "${BINDIST_PATH}/bin")

file(GLOB headers "${BINDIST_PATH}/include/*.h")
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