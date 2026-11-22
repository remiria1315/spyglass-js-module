cmake_minimum_required(VERSION 3.28)
project(spyglass VERSION 0.5.6 LANGUAGES CXX)

if (NOT (WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8))
    message(FATAL_ERROR "spyglass targets 64-bit Windows")
endif ()

add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(MINECRAFT_CLIENTS "1.26.50;1.26.60;1.26.70.21" CACHE STRING
        "Minecraft client builds to target, e.g. 1.26.50;1.26.60.21")

add_subdirectory(third_party)

set(sources
        src/spyglass/camera.cpp
        src/spyglass/core/output.cpp
        src/spyglass/error.cpp
        src/spyglass/filename.cpp
        src/spyglass/hook.cpp
        src/spyglass/install.cpp
        src/spyglass/main.cpp
        src/spyglass/network.cpp
        src/spyglass/overlay/bytes.cpp
        src/spyglass/overlay/capture.cpp
        src/spyglass/overlay/export.cpp
        src/spyglass/overlay/filter.cpp
        src/spyglass/overlay/i18n.cpp
        src/spyglass/overlay/navigate.cpp
        src/spyglass/overlay/pane/expert_window.cpp
        src/spyglass/overlay/pane/export_dialog.cpp
        src/spyglass/overlay/pane/filter_window.cpp
        src/spyglass/overlay/pane/menu_bar.cpp
        src/spyglass/overlay/pane/modules_window.cpp
        src/spyglass/overlay/pane/packet_bytes.cpp
        src/spyglass/overlay/pane/packet_details.cpp
        src/spyglass/overlay/pane/packet_list.cpp
        src/spyglass/overlay/pane/settings_window.cpp
        src/spyglass/overlay/pane/statistics_window.cpp
        src/spyglass/overlay/pane/status_bar.cpp
        src/spyglass/overlay/pane/toolbar.cpp
        src/spyglass/overlay/report.cpp
        src/spyglass/overlay/settings.cpp
        src/spyglass/overlay/store.cpp
        src/spyglass/overlay/theme.cpp
        src/spyglass/overlay/view.cpp
        src/spyglass/overlay/windows/d3d11_backend.cpp
        src/spyglass/overlay/windows/d3d12_backend.cpp
        src/spyglass/overlay/windows/input.cpp
        src/spyglass/overlay/windows/mouse.cpp
        src/spyglass/overlay/windows/overlay.cpp
        src/spyglass/pattern.cpp
        src/spyglass/reflect.cpp
        src/spyglass/script/chat.cpp
        src/spyglass/script/engine.cpp
        src/spyglass/script/system.cpp
        src/spyglass/signature.cpp)

foreach (client IN LISTS MINECRAFT_CLIENTS)
    if (NOT "${client}" MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)(\\.([0-9]+))?$")
        message(FATAL_ERROR
                "a MINECRAFT_CLIENTS entry reads 1.26.50 for a release or 1.26.60.21 for a preview, not '${client}'")
    endif ()
    set(version "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${CMAKE_MATCH_3}")
    set(tag "${version}")
    set(name "Windows ${tag}")
    if (CMAKE_MATCH_4)
        set(preview 1)
        set(build ${CMAKE_MATCH_5})
        string(APPEND version ".${build}")
        string(APPEND tag ".preview")
        string(APPEND name " preview")
    else ()
        set(preview 0)
        set(build 0)
    endif ()
    entt_for_client(${version} entt)
    message(STATUS "Minecraft client: ${name} (${version}), ${entt}")

    set(target spyglass-${tag})
    add_library(${target} SHARED ${sources})
    target_include_directories(${target} PRIVATE src)
    target_compile_definitions(${target} PRIVATE
            SPYGLASS_VERSION="${PROJECT_VERSION}"
            MINECRAFT_CLIENT="${name}"
            MINECRAFT_VERSION_MAJOR=${CMAKE_MATCH_1}
            MINECRAFT_VERSION_MINOR=${CMAKE_MATCH_2}
            MINECRAFT_VERSION_PATCH=${CMAKE_MATCH_3}
            MINECRAFT_VERSION_BUILD=${build}
            MINECRAFT_PREVIEW=${preview})
    set_target_properties(${target} PROPERTIES OUTPUT_NAME spyglass-${PROJECT_VERSION}-${tag})
    target_link_libraries(${target} PRIVATE
            ${entt}
            nonstd::expected-lite
            funchook-static
            imgui
            libhat::libhat
            nlohmann_json::nlohmann_json
            qjs
            shell32 version winhttp)

    install(TARGETS ${target} RUNTIME DESTINATION .)
    install(FILES $<TARGET_PDB_FILE:${target}> DESTINATION . OPTIONAL)
endforeach ()

install(FILES LICENSE README.md DESTINATION .)

add_executable(spyglass_injector src/injector/main.cpp)
target_compile_definitions(spyglass_injector PRIVATE SPYGLASS_VERSION="${PROJECT_VERSION}")
target_link_libraries(spyglass_injector PRIVATE advapi32 shell32 version)
target_link_options(spyglass_injector PRIVATE /DEBUG:NONE)
set_target_properties(spyglass_injector PROPERTIES OUTPUT_NAME spyglass)

install(TARGETS spyglass_injector RUNTIME DESTINATION .)
