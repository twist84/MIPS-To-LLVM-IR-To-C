# Generate a locally patched translation unit without dirtying the pinned checkout.
# The source changes are intentionally kept small and fail closed if upstream drifts.
function(ee_prepare_raudio source_dir binary_dir out_var)
    set(input "${source_dir}/src/raudio.c")
    set(output "${binary_dir}/raudio.c")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${input}")
    file(READ "${input}" source)
    string(REPLACE "\r\n" "\n" source "${source}")
    set(before [====[*     3. This notice may not be removed or altered from any source distribution.
*
**********************************************************************************************/

#if defined(RAUDIO_STANDALONE)
    #include "raudio.h"
#else
    #include "raylib.h"         // Declares module functions]====])
    set(after [====[*     3. This notice may not be removed or altered from any source distribution.
*
**********************************************************************************************/

/* Local lifecycle fixes for the PS2 runner: see UPSTREAM.md. */
#if defined(RAUDIO_STANDALONE)
    #include "raudio.h"
#else
    #include "raylib.h"         // Declares module functions]====])
    string(FIND "${source}" "${before}" match)
    if(match EQUAL -1)
        message(FATAL_ERROR "Pinned raudio source does not match lifecycle patch")
    endif()
    string(REPLACE "${before}" "${after}" source "${source}")

    set(before [====[#include <string.h>                     // Required for: strcmp() [Used in IsFileExtension(), LoadWaveFromMemory(), LoadMusicStreamFromMemory()]

#if defined(RAUDIO_STANDALONE)
    #ifndef TRACELOG
        #define TRACELOG(level, ...)    printf(__VA_ARGS__)
    #endif

    // Allow custom memory allocators
    #ifndef RL_MALLOC]====])
    set(after [====[#include <string.h>                     // Required for: strcmp() [Used in IsFileExtension(), LoadWaveFromMemory(), LoadMusicStreamFromMemory()]

#if defined(RAUDIO_STANDALONE)
    #ifndef TRACELOG
        #define TRACELOG(level, ...)    do { printf(__VA_ARGS__); printf("\n"); } while (0)
    #endif

    // Allow custom memory allocators
    #ifndef RL_MALLOC]====])
    string(FIND "${source}" "${before}" match)
    if(match EQUAL -1)
        message(FATAL_ERROR "Pinned raudio source does not match lifecycle patch")
    endif()
    string(REPLACE "${before}" "${after}" source "${source}")

    set(before [====[        TRACELOG(LOG_WARNING, "AUDIO: Failed to initialize context");
        return;
    }

    // Init audio device
    // NOTE: Using the default device. Format is floating point because it simplifies mixing.
    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.pDeviceID = NULL;  // NULL for the default playback AUDIO.System.device.]====])
    set(after [====[        TRACELOG(LOG_WARNING, "AUDIO: Failed to initialize context");
        return;
    }

    // Do not silently treat the null backend as an audible device.
    if (AUDIO.System.context.backend == ma_backend_null)
    {
        TRACELOG(LOG_WARNING, "AUDIO: No physical playback backend");
        ma_context_uninit(&AUDIO.System.context);
        return;
    }

    // Init audio device
    // NOTE: Using the default device. Format is floating point because it simplifies mixing.
    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.pDeviceID = NULL;  // NULL for the default playback AUDIO.System.device.]====])
    string(FIND "${source}" "${before}" match)
    if(match EQUAL -1)
        message(FATAL_ERROR "Pinned raudio source does not match lifecycle patch")
    endif()
    string(REPLACE "${before}" "${after}" source "${source}")

    set(before [====[    if (result != MA_SUCCESS)
    {
        TRACELOG(LOG_WARNING, "AUDIO: Failed to start playback device");
        ma_device_uninit(&AUDIO.System.device);
        ma_context_uninit(&AUDIO.System.context);
        return;
    }
]====])
    set(after [====[    if (result != MA_SUCCESS)
    {
        TRACELOG(LOG_WARNING, "AUDIO: Failed to start playback device");
        ma_device_uninit(&AUDIO.System.device);
        ma_mutex_uninit(&AUDIO.System.lock);
        ma_context_uninit(&AUDIO.System.context);
        return;
    }
]====])
    string(FIND "${source}" "${before}" match)
    if(match EQUAL -1)
        message(FATAL_ERROR "Pinned raudio source does not match lifecycle patch")
    endif()
    string(REPLACE "${before}" "${after}" source "${source}")

    set(before [====[void CloseAudioDevice(void)
{
    if (AUDIO.System.isReady)
    {
        ma_mutex_uninit(&AUDIO.System.lock);
        ma_device_uninit(&AUDIO.System.device);
        ma_context_uninit(&AUDIO.System.context);

        AUDIO.System.isReady = false;
        RL_FREE(AUDIO.System.pcmBuffer);]====])
    set(after [====[void CloseAudioDevice(void)
{
    if (AUDIO.System.isReady)
    {
        // Stop and join the device callback before destroying its mutex.
        ma_device_uninit(&AUDIO.System.device);
        ma_mutex_uninit(&AUDIO.System.lock);
        ma_context_uninit(&AUDIO.System.context);

        AUDIO.System.isReady = false;
        RL_FREE(AUDIO.System.pcmBuffer);]====])
    string(FIND "${source}" "${before}" match)
    if(match EQUAL -1)
        message(FATAL_ERROR "Pinned raudio source does not match lifecycle patch")
    endif()
    string(REPLACE "${before}" "${after}" source "${source}")

    set(before [====[{
    return AUDIO.System.isReady;
}

// Set master volume (listener)
void SetMasterVolume(float volume)
{
    ma_device_set_master_volume(&AUDIO.System.device, volume);]====])
    set(after [====[{
    return AUDIO.System.isReady;
}

// Local diagnostic accessors: physical/backend format after device negotiation.
unsigned int raudio_device_sample_rate(void)
{
    return AUDIO.System.isReady ? AUDIO.System.device.playback.internalSampleRate : 0u;
}

unsigned int raudio_device_channels(void)
{
    return AUDIO.System.isReady ? AUDIO.System.device.playback.internalChannels : 0u;
}

// Set master volume (listener)
void SetMasterVolume(float volume)
{
    ma_device_set_master_volume(&AUDIO.System.device, volume);]====])
    string(FIND "${source}" "${before}" match)
    if(match EQUAL -1)
        message(FATAL_ERROR "Pinned raudio source does not match lifecycle patch")
    endif()
    string(REPLACE "${before}" "${after}" source "${source}")

    set(before [====[void UnloadAudioBuffer(AudioBuffer *buffer)
{
    if (buffer != NULL)
    {
        ma_data_converter_uninit(&buffer->converter, NULL);
        UntrackAudioBuffer(buffer);
        RL_FREE(buffer->data);
        RL_FREE(buffer);
    }
}]====])
    set(after [====[void UnloadAudioBuffer(AudioBuffer *buffer)
{
    if (buffer != NULL)
    {
        // Untracking synchronizes with the mixer before converter teardown.
        UntrackAudioBuffer(buffer);
        ma_data_converter_uninit(&buffer->converter, NULL);
        RL_FREE(buffer->data);
        RL_FREE(buffer);
    }
}]====])
    string(FIND "${source}" "${before}" match)
    if(match EQUAL -1)
        message(FATAL_ERROR "Pinned raudio source does not match lifecycle patch")
    endif()
    string(REPLACE "${before}" "${after}" source "${source}")

    set(before [====[
// Play audio stream
void PlayAudioStream(AudioStream stream)
{
    PlayAudioBuffer(stream.buffer);
}

// Play audio stream
void PauseAudioStream(AudioStream stream)
{
    PauseAudioBuffer(stream.buffer);
}

// Resume audio stream playing
void ResumeAudioStream(AudioStream stream)
{
    ResumeAudioBuffer(stream.buffer);
}

// Check if audio stream is playing.
bool IsAudioStreamPlaying(AudioStream stream)]====])
    set(after [====[
// Play audio stream
void PlayAudioStream(AudioStream stream)
{
    ma_mutex_lock(&AUDIO.System.lock);
    PlayAudioBuffer(stream.buffer);
    ma_mutex_unlock(&AUDIO.System.lock);
}

// Play audio stream
void PauseAudioStream(AudioStream stream)
{
    ma_mutex_lock(&AUDIO.System.lock);
    PauseAudioBuffer(stream.buffer);
    ma_mutex_unlock(&AUDIO.System.lock);
}

// Resume audio stream playing
void ResumeAudioStream(AudioStream stream)
{
    ma_mutex_lock(&AUDIO.System.lock);
    ResumeAudioBuffer(stream.buffer);
    ma_mutex_unlock(&AUDIO.System.lock);
}

// Check if audio stream is playing.
bool IsAudioStreamPlaying(AudioStream stream)]====])
    string(FIND "${source}" "${before}" match)
    if(match EQUAL -1)
        message(FATAL_ERROR "Pinned raudio source does not match lifecycle patch")
    endif()
    string(REPLACE "${before}" "${after}" source "${source}")

    set(before [====[
// Audio thread callback to request new data
void SetAudioStreamCallback(AudioStream stream, AudioCallback callback)
{
    if (stream.buffer != NULL) stream.buffer->callback = callback;
}

// Add processor to audio stream. Contrary to buffers, the order of processors is important.
// The new processor must be added at the end. As there aren't supposed to be a lot of processors attached to]====])
    set(after [====[
// Audio thread callback to request new data
void SetAudioStreamCallback(AudioStream stream, AudioCallback callback)
{
    ma_mutex_lock(&AUDIO.System.lock);
    if (stream.buffer != NULL) stream.buffer->callback = callback;
    ma_mutex_unlock(&AUDIO.System.lock);
}

// Add processor to audio stream. Contrary to buffers, the order of processors is important.
// The new processor must be added at the end. As there aren't supposed to be a lot of processors attached to]====])
    string(FIND "${source}" "${before}" match)
    if(match EQUAL -1)
        message(FATAL_ERROR "Pinned raudio source does not match lifecycle patch")
    endif()
    string(REPLACE "${before}" "${after}" source "${source}")

    file(MAKE_DIRECTORY "${binary_dir}")
    # COPYONLY preserves the destination timestamp when contents are unchanged.
    file(WRITE "${output}.tmp" "${source}")
    configure_file("${output}.tmp" "${output}" COPYONLY)
    file(REMOVE "${output}.tmp")
    set(${out_var} "${output}" PARENT_SCOPE)
endfunction()
