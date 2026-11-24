# 3rdparty package definitions. Included by CmDepFetch (and by `cmdep` in script mode).
# CmDepFetchPackage/CmDepFetchGit auto-declare, per dep: VUI_<DEP>_* (CACHE overrides, -D wins) and
# VUI_USE_SYSTEM_<DEP> (option; ON => consume a pre-built copy instead of fetching). The VUI_ prefix
# is derived from the project() name. Build against a local vio with -DCMDEP_SOURCE_DIR_VIO=<dir>.

CmDepFetchGit(vio e090d73
    https://github.com/jorgen/vio.git
    e090d7367091879be37cea468124e2f8706338cf
    SHA256=f13c8dc5a2012ad41da13843c38e7eb1fca3a8573dcfa4a384b6c34d896eaf38)

# Vulkan-Headers: the declarations only. The renderer loads the Vulkan loader at run time
# (DECISIONS.md G6, G31), so nothing is linked and no Vulkan SDK is needed to build.
CmDepFetchPackage(vulkan_headers 1.4.350.1
    https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/vulkan-sdk-1.4.350.1.tar.gz
    SHA256=6d1bb65e49520344cc0a48af3dc02e993781efff14c7ebdcb8ae9fa23ddf7e83)

# Text (G27, G39): HarfBuzz shapes, FreeType rasterizes. FreeType is built without HarfBuzz, zlib,
# bzip2, PNG or Brotli, so the two never see each other. HarfBuzz is compiled from its single-file
# amalgamation, and its test fonts are what the text tests use.
CmDepFetchPackage(freetype 2.15.0
    https://github.com/freetype/freetype/archive/refs/tags/VER-2-15-0.tar.gz
    SHA256=87c7f1f8699b91e89f773f7dcfa505b044e1dae67f62a55d4e29c3afdcac4fb8)
CmDepFetchPackage(harfbuzz 15.1.0
    https://github.com/harfbuzz/harfbuzz/releases/download/15.1.0/harfbuzz-15.1.0.tar.xz
    SHA256=0fc7c565443a5f4be7da65ac5ee5bd737d9b158dc908708b7689a0a5796b0c8d)

# stb_image, PNG only, for the explorer example on Linux, where a thumbnail comes as a PNG from the
# freedesktop cache or a thumbnailer (G57). Declared with the examples on every platform, so the lock
# holds it wherever it is written; compiled only on Linux.
if(VUI_BUILD_EXAMPLES)
    CmDepFetchPackage(stb 31c1ad3
        https://github.com/nothings/stb/archive/31c1ad37456438565541f4919958214b6e762fb4.tar.gz
        SHA256=e4e3bba9c572a4a4148373a914d88ea0f0d11de8cc2c66739926e7eca0223319)
endif()

if(VUI_BUILD_TESTS)
    CmDepFetchPackage(doctest v2.4.11
        https://github.com/doctest/doctest/archive/refs/tags/v2.4.12.tar.gz
        SHA256=73381c7aa4dee704bd935609668cf41880ea7f19fa0504a200e13b74999c2d70)
endif()

# slangc, prebuilt, for the vui_shaders target only (G33). Opt-in: the SPIR-V it produces is
# committed, so only someone changing a shader needs it. A tool runs on the host, so the archive is
# picked by the host's system and processor, never the target's.
if(VUI_FETCH_SLANG)
    string(TOLOWER "${CMAKE_HOST_SYSTEM_PROCESSOR}" _vui_host_cpu)
    if(_vui_host_cpu MATCHES "^(arm64|aarch64)$")
        set(_vui_host_cpu aarch64)
    else()
        set(_vui_host_cpu x86_64)
    endif()
    set(_vui_slang_base https://github.com/shader-slang/slang/releases/download/v2026.21/slang-2026.21)
    if(CMAKE_HOST_WIN32 AND _vui_host_cpu STREQUAL aarch64)
        CmDepFetchPackage(slang_windows_aarch64 2026.21 ${_vui_slang_base}-windows-aarch64.tar.gz
            SHA256=b286783ed92d4e6ab22fc17e37ce39d7cac060f4e356fa10f1e2d67486fbb9c4)
    elseif(CMAKE_HOST_WIN32)
        CmDepFetchPackage(slang_windows_x86_64 2026.21 ${_vui_slang_base}-windows-x86_64.tar.gz
            SHA256=d2f4a1547f38d050aa87168bfd7379ad71aaa0a063c317ab6f727e9d8d901628)
    elseif(CMAKE_HOST_APPLE AND _vui_host_cpu STREQUAL aarch64)
        CmDepFetchPackage(slang_macos_aarch64 2026.21 ${_vui_slang_base}-macos-aarch64.tar.gz
            SHA256=c3d3c959ba3e0c8fdb6c2a5cf6a471ed097ed19c41820af9fb21625779794fb2)
    elseif(CMAKE_HOST_APPLE)
        CmDepFetchPackage(slang_macos_x86_64 2026.21 ${_vui_slang_base}-macos-x86_64.tar.gz
            SHA256=a6bc9ba38c5d3a092ae868595ba9c838663682b3e16de40d5786bd38964970ba)
    elseif(_vui_host_cpu STREQUAL aarch64)
        CmDepFetchPackage(slang_linux_aarch64 2026.21 ${_vui_slang_base}-linux-aarch64-glibc-2.28.tar.gz
            SHA256=c48f2c9a12f1a382a84a39f2b314b1cf3159c93136fbe8c4f377c87db4ef4dc8)
    else()
        CmDepFetchPackage(slang_linux_x86_64 2026.21 ${_vui_slang_base}-linux-x86_64-glibc-2.27.tar.gz
            SHA256=1b75315dabc97bab349449502c2e64faaa849360c17d9fdf709c0628ad5365ac)
    endif()
endif()
