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
    ${ENGINE_TOP}/hexen2/r_webgl2.c
    ${ENGINE_TOP}/hexen2/r_part.c
    ${COMMONDIR}/draw_webgl2.c
    ${COMMONDIR}/model.c
    ${COMMONDIR}/screen.c
)

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
    ${COMMONDIR}/vid_webgl2.c
    ${COMMONDIR}/in_web.c
    ${COMMONDIR}/midi_nul.c
    ${COMMONDIR}/cd_null.c
)
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
