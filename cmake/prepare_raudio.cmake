# Extract only the audio API from pinned raylib.h and compile raudio.c
# in standalone mode. No graphics dependency or upstream checkout mutation.
function(ee_raudio_extract source start end out)
    string(FIND "${source}" "${start}" first)
    if(first LESS 0)
        message(FATAL_ERROR "Pinned raylib audio header start not found: ${start}")
    endif()
    string(SUBSTRING "${source}" ${first} -1 tail)
    string(FIND "${tail}" "${end}" length)
    if(length LESS 0)
        message(FATAL_ERROR "Pinned raylib audio header end not found: ${end}")
    endif()
    string(SUBSTRING "${tail}" 0 ${length} part)
    set(${out} "${part}" PARENT_SCOPE)
endfunction()

function(ee_raudio_patch source_var before after)
    string(FIND "${${source_var}}" "${before}" match)
    if(match LESS 0)
        message(FATAL_ERROR "Pinned raylib raudio source does not match patch: ${before}")
    endif()
    string(REPLACE "${before}" "${after}" source "${${source_var}}")
    set(${source_var} "${source}" PARENT_SCOPE)
endfunction()

function(ee_raudio_write path content)
    file(WRITE "${path}.tmp" "${content}")
    configure_file("${path}.tmp" "${path}" COPYONLY)
    file(REMOVE "${path}.tmp")
endfunction()

function(ee_prepare_raudio source_dir binary_dir out_var)
    set(input "${source_dir}/src/raudio.c")
    set(header "${source_dir}/src/raylib.h")
    set(output "${binary_dir}/raudio.c")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${input}" "${header}")
    file(READ "${input}" source)
    file(READ "${header}" declarations)
    string(REPLACE "\r\n" "\n" source "${source}")
    string(REPLACE "\r\n" "\n" declarations "${declarations}")
    ee_raudio_extract("${declarations}" "/******" "#ifndef RAYLIB_H" license)
    ee_raudio_extract("${declarations}" "// Wave, audio wave data" "// VrDeviceInfo" types)
    ee_raudio_extract("${declarations}" "typedef void (*AudioCallback)" ";" callback)
    ee_raudio_extract("${declarations}" "// Audio device management functions" "#if defined(__cplusplus)\n}" api)
    string(REPLACE "RLAPI " "" api "${api}")
    set(audio_header "${license}\n/* Generated audio-only subset of raylib.h. */\n#pragma once\n#include <stdbool.h>\n#include <stdlib.h>\n#define RL_MALLOC(sz) malloc(sz)\n#define RL_CALLOC(n,sz) calloc(n,sz)\n#define RL_REALLOC(p,sz) realloc(p,sz)\n#define RL_FREE(p) free(p)\n\n${types}\n${callback};\n#ifdef __cplusplus\nextern \"C\" {\n#endif\n${api}\nunsigned int raudio_device_sample_rate(void);\nunsigned int raudio_device_channels(void);\n#ifdef __cplusplus\n}\n#endif\n")
    ee_raudio_patch(source [====[#if defined(RAUDIO_STANDALONE)
    #include "raudio.h"]====] [====[/* Local PS2 runner lifecycle fixes: see docs/RAUDIO_UPSTREAM.md. */
#if defined(RAUDIO_STANDALONE)
    #include "raudio.h"]====])

    ee_raudio_patch(source [====[#define TRACELOG(level, ...)    printf(__VA_ARGS__)]====] [====[#define TRACELOG(level, ...)    do { printf(__VA_ARGS__); printf("\n"); } while (0)]====])

    ee_raudio_patch(source [====[    // Init audio device
    // NOTE: Using the default device.]====] [====[    if (AUDIO.System.context.backend == ma_backend_null)
    {
        TRACELOG(LOG_WARNING, "AUDIO: No physical playback backend");
        ma_context_uninit(&AUDIO.System.context);
        return;
    }

    // Init audio device
    // NOTE: Using the default device.]====])

    ee_raudio_patch(source [====[        TRACELOG(LOG_WARNING, "AUDIO: Failed to start playback device");
        ma_device_uninit(&AUDIO.System.device);
        ma_context_uninit(&AUDIO.System.context);]====] [====[        TRACELOG(LOG_WARNING, "AUDIO: Failed to start playback device");
        ma_device_uninit(&AUDIO.System.device);
        ma_mutex_uninit(&AUDIO.System.lock);
        ma_context_uninit(&AUDIO.System.context);]====])

    ee_raudio_patch(source [====[        ma_mutex_uninit(&AUDIO.System.lock);
        ma_device_uninit(&AUDIO.System.device);]====] [====[        // Join the playback callback before destroying its mutex.
        ma_device_uninit(&AUDIO.System.device);
        ma_mutex_uninit(&AUDIO.System.lock);]====])

    ee_raudio_patch(source [====[// Set master volume (listener)
void SetMasterVolume(float volume)]====] [====[// Local physical/backend format diagnostics.
unsigned int raudio_device_sample_rate(void)
{
    return AUDIO.System.isReady ? AUDIO.System.device.playback.internalSampleRate : 0u;
}

unsigned int raudio_device_channels(void)
{
    return AUDIO.System.isReady ? AUDIO.System.device.playback.internalChannels : 0u;
}

// Set master volume (listener)
void SetMasterVolume(float volume)]====])

    ee_raudio_patch(source [====[    UnloadFileData(fileData);]====] [====[    // The standalone module owns its file allocation.
    RL_FREE(fileData);]====])

    file(MAKE_DIRECTORY "${binary_dir}")
    ee_raudio_write("${binary_dir}/raudio.h" "${audio_header}")
    ee_raudio_write("${output}" "${source}")
    set(${out_var} "${output}" PARENT_SCOPE)
endfunction()
