cmake_minimum_required(VERSION 3.30)
project(vui VERSION 0.0.1 DESCRIPTION "Windows, input and a declarative UI toolkit on the vio event loop" LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

option(VUI_BUILD_TESTS "Build the vui tests" ON)
option(VUI_BUILD_EXAMPLES "Build the vui examples" ON)
option(VUI_FETCH_SLANG "Fetch a prebuilt slangc for the vui_shaders target (DECISIONS.md G33)" OFF)

set(CMDEP_TAG 756bee3072d1bf67e179b62a9635cdcdef5931e7)
include(FetchContent)
FetchContent_Declare(cmake-dep
    GIT_REPOSITORY https://github.com/jorgen/cmake-dep.git
    GIT_TAG ${CMDEP_TAG}
    SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/cmake-dep-${CMDEP_TAG}"
)
FetchContent_MakeAvailable(cmake-dep)
list(APPEND CMAKE_MODULE_PATH "${cmake-dep_SOURCE_DIR}/cmake")
include(CmDepMain)

set(CMDEP_PACKAGES_FILE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/3rdPartyPackages.cmake")
CmDepFetch()

include(cmake/Build3rdParty.cmake)
Build3rdParty()

add_compile_options($<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

if(VUI_BUILD_TESTS)
    enable_testing()
endif()

set(VUI_SOURCES src/display.cpp src/context.cpp)
set(VUI_LIBRARIES vio)
set(VUI_DEFINITIONS)

if(WIN32)
    list(APPEND VUI_SOURCES src/backend_win32.cpp src/context_win32.cpp)
    list(APPEND VUI_LIBRARIES user32 gdi32 dwmapi)
elseif(APPLE)
    enable_language(OBJCXX)
    list(APPEND VUI_SOURCES src/backend_cocoa.mm)
    set_source_files_properties(src/backend_cocoa.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    list(APPEND VUI_LIBRARIES "-framework Cocoa" "-framework QuartzCore" "-framework OpenGL")
else()
    find_package(X11)
    if(X11_FOUND)
        list(APPEND VUI_SOURCES src/backend_x11.cpp)
        list(APPEND VUI_LIBRARIES X11::X11)
        list(APPEND VUI_DEFINITIONS VUI_HAVE_X11)
    endif()
    find_package(PkgConfig)
    if(PkgConfig_FOUND)
        pkg_check_modules(VUI_WAYLAND IMPORTED_TARGET wayland-client wayland-cursor xkbcommon wayland-protocols)
        pkg_get_variable(VUI_WAYLAND_PROTOCOLS wayland-protocols pkgdatadir)
        pkg_get_variable(VUI_WAYLAND_SCANNER wayland-scanner wayland_scanner)
    endif()
    if(VUI_WAYLAND_FOUND AND VUI_WAYLAND_SCANNER AND VUI_WAYLAND_PROTOCOLS)
        set(protocol_dir ${CMAKE_CURRENT_BINARY_DIR}/protocols)
        file(MAKE_DIRECTORY ${protocol_dir})
        foreach(protocol
                stable/xdg-shell/xdg-shell.xml
                unstable/xdg-decoration/xdg-decoration-unstable-v1.xml)
            get_filename_component(name ${protocol} NAME_WE)
            add_custom_command(
                OUTPUT ${protocol_dir}/${name}.h ${protocol_dir}/${name}.c
                COMMAND ${VUI_WAYLAND_SCANNER} client-header ${VUI_WAYLAND_PROTOCOLS}/${protocol} ${protocol_dir}/${name}.h
                COMMAND ${VUI_WAYLAND_SCANNER} private-code ${VUI_WAYLAND_PROTOCOLS}/${protocol} ${protocol_dir}/${name}.c
                DEPENDS ${VUI_WAYLAND_PROTOCOLS}/${protocol})
            list(APPEND VUI_SOURCES ${protocol_dir}/${name}.c)
        endforeach()
        list(APPEND VUI_SOURCES src/backend_wayland.cpp)
        list(APPEND VUI_LIBRARIES PkgConfig::VUI_WAYLAND)
        list(APPEND VUI_DEFINITIONS VUI_HAVE_WAYLAND)
    endif()
    list(APPEND VUI_SOURCES src/context_egl.cpp)
    list(APPEND VUI_LIBRARIES ${CMAKE_DL_LIBS})
endif()

add_library(vui STATIC ${VUI_SOURCES})
add_library(vui::vui ALIAS vui)
target_include_directories(vui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/protocols)
target_link_libraries(vui PUBLIC ${VUI_LIBRARIES})
target_compile_definitions(vui PRIVATE ${VUI_DEFINITIONS})

# The declarative toolkit, on top of the window layer and kept out of it (PLAN.md, "UI toolkit").
add_library(vui_ui STATIC src/ui.cpp src/ui_text.cpp src/ui_vulkan.cpp src/ui_widgets.cpp)
add_library(vui::ui ALIAS vui_ui)
# FreeType and HarfBuzz stay private: nothing in ui.h names them (G27).
target_link_libraries(vui_ui PUBLIC vui PRIVATE vui::freetype vui::harfbuzz)
# Vulkan declarations only: the loader is opened at run time (G6, G31), so nothing is linked.
if(vulkan_headers_USE_SYSTEM)
    find_package(VulkanHeaders CONFIG REQUIRED)
    target_link_libraries(vui_ui PRIVATE Vulkan::Headers)
else()
    target_include_directories(vui_ui SYSTEM PRIVATE ${vulkan_headers_SOURCE_DIR}/include)
endif()
if(NOT WIN32)
    target_link_libraries(vui_ui PRIVATE ${CMAKE_DL_LIBS})
endif()

# Regenerates src/shaders/ui_spirv.h from ui.slang (G25). Not part of any build: the header is
# committed, and only someone changing a shader needs slangc. -DVUI_FETCH_SLANG=ON uses the
# prebuilt one cmake-dep fetched for this host; otherwise slangc is looked for on PATH.
foreach(_slang slang_windows_x86_64 slang_windows_aarch64 slang_macos_x86_64 slang_macos_aarch64
        slang_linux_x86_64 slang_linux_aarch64)
    if(VUI_FETCH_SLANG AND ${_slang}_SOURCE_DIR)
        find_program(VUI_SLANGC slangc PATHS ${${_slang}_SOURCE_DIR}/bin NO_DEFAULT_PATH REQUIRED)
    endif()
endforeach()
find_program(VUI_SLANGC slangc)
if(VUI_SLANGC)
    set(shader_dir ${CMAKE_CURRENT_BINARY_DIR}/shaders)
    add_custom_target(vui_shaders
        COMMAND ${CMAKE_COMMAND} -E make_directory ${shader_dir}
        COMMAND ${VUI_SLANGC} ${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/ui.slang -target spirv -profile spirv_1_0
                -entry vertexMain -stage vertex -o ${shader_dir}/ui.vert.spv
        COMMAND ${VUI_SLANGC} ${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/ui.slang -target spirv -profile spirv_1_0
                -entry fragmentMain -stage fragment -o ${shader_dir}/ui.frag.spv
        COMMAND ${CMAKE_COMMAND} -DOUTPUT=${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/ui_spirv.h
                "-DCOMPILER=slangc from ui.slang"
                "-DNAMES=ui_vertex$<SEMICOLON>ui_fragment"
                "-DFILES=${shader_dir}/ui.vert.spv$<SEMICOLON>${shader_dir}/ui.frag.spv"
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_spirv.cmake
        COMMENT "Compiling the UI shaders with slangc"
        VERBATIM)
endif()

if(VUI_BUILD_TESTS)
    set(VUI_TEST_SOURCES test/test_main.cpp test/test_gui.cpp test/test_ui.cpp test/test_ui_render.cpp test/test_ui_text.cpp test/test_ui_layout.cpp test/test_ui_components.cpp test/test_ui_widgets.cpp test/test_ui_text_widgets.cpp)
    if(WIN32)
        list(APPEND VUI_TEST_SOURCES test/test_win32.cpp)
    elseif(APPLE)
        list(APPEND VUI_TEST_SOURCES test/test_cocoa.mm)
        set_source_files_properties(test/test_cocoa.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    else()
        list(APPEND VUI_TEST_SOURCES test/test_x11.cpp test/test_wayland.cpp test/test_context.cpp)
    endif()
    add_executable(vui_tests ${VUI_TEST_SOURCES})
    target_compile_definitions(vui_tests PRIVATE ${VUI_DEFINITIONS})
    target_link_libraries(vui_tests PRIVATE vui_ui)
    if(VUI_USE_SYSTEM_DOCTEST)
        target_link_libraries(vui_tests PRIVATE doctest::doctest)
    else()
        target_include_directories(vui_tests PRIVATE ${doctest_SOURCE_DIR})
    endif()
    # The text tests use fonts from HarfBuzz's own test suite, so they need its source tree.
    if(DEFINED harfbuzz_SOURCE_DIR AND NOT harfbuzz_USE_SYSTEM)
        target_compile_definitions(vui_tests PRIVATE "VUI_TEST_FONTS=\"${harfbuzz_SOURCE_DIR}\"")
    endif()
    add_test(NAME vui_tests COMMAND vui_tests)
endif()

if(VUI_BUILD_EXAMPLES)
    function(add_vui_example name source)
        add_executable(${name} examples/${source})
        target_link_libraries(${name} PRIVATE vui ${ARGN})
    endfunction()
    add_vui_example(vui_hello hello_window.cpp)
    add_vui_example(vui_input_echo input_echo.cpp)
    add_vui_example(vui_windows_and_network windows_and_network.cpp)
    add_vui_example(vui_ui_counter ui_counter.cpp vui_ui)
    add_vui_example(vui_ui_list ui_list.cpp vui_ui)
    add_vui_example(vui_ui_feed ui_feed.cpp vui_ui)
    add_vui_example(vui_ui_chips ui_chips.cpp vui_ui)
    add_vui_example(vui_ui_widgets ui_widgets.cpp vui_ui)
    add_vui_example(vui_ui_text ui_text.cpp vui_ui)
    add_executable(vui_task_manager examples/task_manager/main.cpp examples/task_manager/system_windows.cpp examples/task_manager/system_linux.cpp examples/task_manager/system_macos.cpp)
    target_link_libraries(vui_task_manager PRIVATE vui vui_ui)
    add_executable(vui_explorer examples/explorer/main.cpp examples/explorer/shell_windows.cpp examples/explorer/shell_linux.cpp examples/explorer/shell_macos.cpp)
    target_link_libraries(vui_explorer PRIVATE vui vui_ui)
    if(WIN32)
        target_link_libraries(vui_explorer PRIVATE shell32 ole32 shlwapi uuid)
    elseif(APPLE)
        target_link_libraries(vui_explorer PRIVATE "-framework CoreServices" "-framework QuickLook" "-framework CoreGraphics")
    else()
        target_include_directories(vui_explorer SYSTEM PRIVATE ${stb_SOURCE_DIR})
    endif()
    # The demos with the licence and a readme, zipped: what the dist-windows preset builds.
    set(VUI_DEMOS_DIR "${CMAKE_BINARY_DIR}/vui-demos")
    set(VUI_DEMOS_ZIP "${CMAKE_BINARY_DIR}/vui-demos-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}.zip")
    add_custom_target(vui_demos_package
        COMMAND ${CMAKE_COMMAND} -E rm -rf "${VUI_DEMOS_DIR}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${VUI_DEMOS_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:vui_task_manager> $<TARGET_FILE:vui_explorer> "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE" "${VUI_DEMOS_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/docs/demos.txt" "${VUI_DEMOS_DIR}/README.txt"
        COMMAND ${CMAKE_COMMAND} -E tar cf "${VUI_DEMOS_ZIP}" --format=zip vui-demos
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
        DEPENDS vui_task_manager vui_explorer
        COMMENT "Packaging the demos into ${VUI_DEMOS_ZIP}"
        VERBATIM)
    if(VUI_BUILD_TESTS)
        add_test(NAME vui_task_manager_probe COMMAND vui_task_manager --probe)
        add_test(NAME vui_explorer_probe COMMAND vui_explorer --probe)
    endif()
    find_package(Vulkan QUIET)
    if(Vulkan_FOUND AND NOT APPLE)
        add_vui_example(vui_vulkan_clear vulkan_clear.cpp Vulkan::Vulkan)
    endif()
    if(APPLE)
        add_vui_example(vui_metal_clear metal_clear.mm "-framework Metal" "-framework QuartzCore")
        set_source_files_properties(examples/metal_clear.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    endif()
    if(WIN32)
        add_vui_example(vui_d3d12_clear d3d12_clear.cpp d3d12 dxgi)
        if(VUI_BUILD_TESTS)
            add_test(NAME vui_hello COMMAND vui_hello 20)
            add_test(NAME vui_input_echo COMMAND vui_input_echo 300)
            add_test(NAME vui_windows_and_network COMMAND vui_windows_and_network 1300)
            add_test(NAME vui_ui_counter COMMAND vui_ui_counter 300)
            add_test(NAME vui_ui_list COMMAND vui_ui_list 300)
            add_test(NAME vui_ui_feed COMMAND vui_ui_feed 300)
            add_test(NAME vui_ui_text COMMAND vui_ui_text 600)
            add_test(NAME vui_task_manager COMMAND vui_task_manager 2500)
            add_test(NAME vui_explorer COMMAND vui_explorer 2500)
            add_test(NAME vui_d3d12_clear COMMAND vui_d3d12_clear 30)
        endif()
    endif()
endif()
