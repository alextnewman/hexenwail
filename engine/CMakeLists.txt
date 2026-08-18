cmake_minimum_required(VERSION 3.16)
project(uhexen2_web VERSION 1.5.10 LANGUAGES C)

if(NOT EMSCRIPTEN)
    message(FATAL_ERROR "This fork only supports the Emscripten iOS-PWA target")
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

set(ENGINE_TOP ${CMAKE_CURRENT_SOURCE_DIR})
set(COMMONDIR ${ENGINE_TOP}/h2shared)
set(UHEXEN2_TOP ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(UHEXEN2_SHARED ${UHEXEN2_TOP}/common)

option(USE_DEBUG "Debug build" OFF)
option(USE_SOUND "Enable the browser audio backend" ON)
set(GAME_DATA_DIR "" CACHE PATH "Optional game data directory to preload")

# Renderer selection. "software" is the shipping configuration: the classic
# 8bpp software rasterizer presented through an accelerated canvas. "webgl2"
# keeps the experimental GPU renderer buildable.
# See docs/web/SOFTWARE_RENDERER.md and docs/web/ARCHITECTURE.md.
set(WEB_RENDERER "software" CACHE STRING "Web renderer: software or webgl2")
set_property(CACHE WEB_RENDERER PROPERTY STRINGS software webgl2)
if(NOT WEB_RENDERER STREQUAL "software" AND NOT WEB_RENDERER STREQUAL "webgl2")
    message(FATAL_ERROR "WEB_RENDERER must be 'software' or 'webgl2' (got '${WEB_RENDERER}')")
endif()
message(STATUS "Web renderer: ${WEB_RENDERER}")

add_compile_options(
    -Wall
    -Wextra
    -Wno-unused-parameter
    -Wno-unused-result
    -Wno-missing-field-initializers
)

if(USE_DEBUG)
    add_compile_options(-O0 -g)
    add_compile_definitions(DEBUG=1 DEBUG_BUILD=1)
else()
    add_compile_options(-O2 -DNDEBUG=1 -ffast-math)
endif()

add_compile_definitions(
    WEBQUAKE
    PLATFORM_WEB
    NO_SPLASHES
    __EMSCRIPTEN__
)

include_directories(
    ${ENGINE_TOP}/hexen2
    ${COMMONDIR}
    ${UHEXEN2_SHARED}
)

set(RENDERER_SOURCES
    ${ENGINE_TOP}/hexen2/r_part.c
    ${COMMONDIR}/model.c
    ${COMMONDIR}/screen.c
)

if(WEB_RENDERER STREQUAL "webgl2")
    add_compile_definitions(WEBGL2QUAKE)
    list(APPEND RENDERER_SOURCES
        ${ENGINE_TOP}/hexen2/r_webgl2.c
        ${COMMONDIR}/draw_webgl2.c
    )
else()
    add_compile_definitions(WEBSOFT)
    list(APPEND RENDERER_SOURCES
        ${ENGINE_TOP}/hexen2/r_alias.c
        ${ENGINE_TOP}/hexen2/r_main.c
        ${ENGINE_TOP}/hexen2/r_misc.c
        ${ENGINE_TOP}/hexen2/r_soft_web.c
        ${COMMONDIR}/d_edge.c
        ${COMMONDIR}/d_fill.c
        ${COMMONDIR}/d_init.c
        ${COMMONDIR}/d_modech.c
        ${COMMONDIR}/d_part.c
        ${COMMONDIR}/d_polyse.c
        ${COMMONDIR}/d_scan.c
        ${COMMONDIR}/d_sky.c
        ${COMMONDIR}/d_sprite.c
        ${COMMONDIR}/d_surf.c
        ${COMMONDIR}/d_vars.c
        ${COMMONDIR}/d_zpoint.c
        ${COMMONDIR}/r_aclip.c
        ${COMMONDIR}/r_bsp.c
        ${COMMONDIR}/r_draw.c
        ${COMMONDIR}/r_edge.c
        ${COMMONDIR}/r_efrag.c
        ${COMMONDIR}/r_light.c
        ${COMMONDIR}/r_sky.c
        ${COMMONDIR}/r_sprite.c
        ${COMMONDIR}/r_surf.c
        ${COMMONDIR}/r_vars.c
        ${COMMONDIR}/draw.c
        ${COMMONDIR}/draw_soft_web.c
    )
endif()

set(CLIENT_SOURCES
    ${ENGINE_TOP}/hexen2/cl_demo.c
    ${ENGINE_TOP}/hexen2/cl_effect.c
    ${ENGINE_TOP}/hexen2/cl_inlude.c
    ${ENGINE_TOP}/hexen2/cl_input.c
    ${ENGINE_TOP}/hexen2/cl_main.c
    ${ENGINE_TOP}/hexen2/cl_parse.c
    ${ENGINE_TOP}/hexen2/cl_string.c
    ${ENGINE_TOP}/hexen2/cl_tent.c
    ${ENGINE_TOP}/hexen2/cl_cmd.c
    ${ENGINE_TOP}/hexen2/console.c
    ${ENGINE_TOP}/hexen2/keys.c
    ${ENGINE_TOP}/hexen2/menu.c
    ${ENGINE_TOP}/hexen2/sbar.c
    ${ENGINE_TOP}/hexen2/view.c
    ${ENGINE_TOP}/hexen2/chase.c
    ${ENGINE_TOP}/hexen2/cl_csqc.c
    ${COMMONDIR}/pr_csqc.c
)

set(COMMON_SOURCES
    ${ENGINE_TOP}/hexen2/host.c
    ${ENGINE_TOP}/hexen2/host_cmd.c
    ${ENGINE_TOP}/hexen2/host_async.c
    ${ENGINE_TOP}/hexen2/sv_main.c
    ${ENGINE_TOP}/hexen2/sv_phys.c
    ${ENGINE_TOP}/hexen2/sv_user.c
    ${ENGINE_TOP}/hexen2/sv_effect.c
    ${ENGINE_TOP}/hexen2/sv_move.c
    ${ENGINE_TOP}/hexen2/world.c
    ${ENGINE_TOP}/hexen2/net_main.c
    ${ENGINE_TOP}/hexen2/net_loop.c
    ${ENGINE_TOP}/hexen2/net_web.c
    ${COMMONDIR}/cmd.c
    ${COMMONDIR}/cvar.c
    ${COMMONDIR}/pr_cmds.c
    ${COMMONDIR}/pr_edict.c
    ${COMMONDIR}/pr_exec.c
    ${COMMONDIR}/mathlib.c
    ${COMMONDIR}/sizebuf.c
    ${COMMONDIR}/link_ops.c
    ${COMMONDIR}/hashindex.c
    ${COMMONDIR}/zone.c
    ${COMMONDIR}/quakefs.c
    ${COMMONDIR}/debuglog.c
    ${COMMONDIR}/host_string.c
    ${COMMONDIR}/cfgfile.c
    ${COMMONDIR}/common.c
    ${COMMONDIR}/wad.c
    ${COMMONDIR}/msg_io.c
    ${UHEXEN2_SHARED}/qsnprint.c
    ${UHEXEN2_SHARED}/strlcat.c
    ${UHEXEN2_SHARED}/strlcpy.c
    ${UHEXEN2_SHARED}/q_endian.c
    ${UHEXEN2_SHARED}/crc.c
)

if(USE_SOUND)
    set(SOUND_SOURCES
        ${COMMONDIR}/snd_sys.c
        ${COMMONDIR}/snd_dma.c
        ${COMMONDIR}/snd_mix.c
        ${COMMONDIR}/snd_mem.c
        ${COMMONDIR}/snd_wave.c
        ${COMMONDIR}/snd_mp3tag.c
        ${COMMONDIR}/snd_codec.c
        ${COMMONDIR}/snd_web.c
        ${COMMONDIR}/bgmusic.c
    )
    add_compile_definitions(HAVE_WEB_SOUND=1)
else()
    set(SOUND_SOURCES ${COMMONDIR}/snd_null.c ${COMMONDIR}/bgmnull.c)
    add_compile_definitions(_NO_SOUND)
endif()

set(PLATFORM_SOURCES
    ${ENGINE_TOP}/hexen2/sys_web.c
    ${COMMONDIR}/in_web.c
    ${COMMONDIR}/midi_nul.c
    ${COMMONDIR}/cd_null.c
)
if(WEB_RENDERER STREQUAL "webgl2")
    list(APPEND PLATFORM_SOURCES ${COMMONDIR}/vid_webgl2.c)
else()
    list(APPEND PLATFORM_SOURCES
        ${COMMONDIR}/vid_soft_web.c
        ${COMMONDIR}/web_canvas_gl2.c
    )
endif()
add_compile_definitions(_NO_MIDIDRV)

add_executable(uhexen2_web
    ${RENDERER_SOURCES}
    ${CLIENT_SOURCES}
    ${COMMON_SOURCES}
    ${SOUND_SOURCES}
    ${PLATFORM_SOURCES}
)

target_link_options(uhexen2_web PRIVATE
    -sMIN_WEBGL_VERSION=2
    -sMAX_WEBGL_VERSION=2
    -sALLOW_MEMORY_GROWTH=1
    -sINITIAL_MEMORY=134217728
    -sSTACK_SIZE=2097152
    -sENVIRONMENT=web
    -sFORCE_FILESYSTEM=1
    -sINVOKE_RUN=0
    -sNO_EXIT_RUNTIME=1
    -sEXPORTED_FUNCTIONS=['_main','_Web_TouchKey','_Web_TouchLook','_Web_ResizeCanvas']
    -sEXPORTED_RUNTIME_METHODS=['FS','callMain','ccall']
    --shell-file=${CMAKE_CURRENT_SOURCE_DIR}/web/shell.html
)

if(GAME_DATA_DIR)
    target_link_options(uhexen2_web PRIVATE --preload-file=${GAME_DATA_DIR}@/data1)
endif()

target_link_libraries(uhexen2_web PRIVATE m)
set_target_properties(uhexen2_web PROPERTIES
    OUTPUT_NAME hexenwail
    SUFFIX ".html"
)
