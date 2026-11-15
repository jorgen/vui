macro(Build3rdParty)
    # vio: the event loop, coroutines and I/O underneath every window. Built static with its src/
    # directory as a public include, since vui includes vio's public headers by path. cmake-dep adds
    # each package once per build, so a parent that already added vio wins.
    CmDepAddPackage(vio CONFIG PUBLIC_INCLUDE src
        OPTIONS VIO_BUILD_TESTS=OFF VIO_BUILD_EXAMPLES=OFF VIO_BUILD_SHARED=OFF VIO_INSTALL=OFF)

    # Text (DECISIONS.md G27, G39). Both end up as the targets vui::freetype and vui::harfbuzz
    # whichever way they arrive.
    CmDepAddPackage(freetype PACKAGE Freetype OPTIONS
        FT_DISABLE_HARFBUZZ=ON FT_DISABLE_ZLIB=ON FT_DISABLE_BZIP2=ON
        FT_DISABLE_PNG=ON FT_DISABLE_BROTLI=ON)
    if (TARGET freetype)
        add_library(vui::freetype ALIAS freetype)
    else ()
        add_library(vui::freetype ALIAS Freetype::Freetype)
    endif ()

    if (harfbuzz_USE_SYSTEM)
        find_package(harfbuzz CONFIG REQUIRED)
        add_library(vui::harfbuzz ALIAS harfbuzz::harfbuzz)
    else ()
        # The amalgamation: one translation unit, HarfBuzz's own OpenType font functions and Unicode
        # data, no FreeType, no threads library, nothing installed.
        add_library(vui_harfbuzz STATIC "${harfbuzz_SOURCE_DIR}/src/harfbuzz.cc")
        target_include_directories(vui_harfbuzz SYSTEM PUBLIC "${harfbuzz_SOURCE_DIR}/src")
        set_target_properties(vui_harfbuzz PROPERTIES CXX_STANDARD 17 POSITION_INDEPENDENT_CODE ON)
        if (MSVC)
            target_compile_options(vui_harfbuzz PRIVATE /bigobj /w)
        else ()
            target_compile_options(vui_harfbuzz PRIVATE -w)
        endif ()
        add_library(vui::harfbuzz ALIAS vui_harfbuzz)
    endif ()

    if (VUI_BUILD_TESTS)
        CmDepAddPackage(doctest OPTIONS DOCTEST_WITH_TESTS=OFF DOCTEST_NO_INSTALL=ON)
    endif ()
endmacro()
