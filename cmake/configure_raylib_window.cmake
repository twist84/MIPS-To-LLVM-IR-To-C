# The GS renderer and performance overlay use WebGPU, not raylib.
# raylib supplies the Win32 window, GLFW input and host clock only;
# the standalone ee_raudio target retains audio independently.
function(ee_configure_raylib_window source binary)
    set(BUILD_EXAMPLES OFF CACHE BOOL "Build raylib examples" FORCE)
    set(CUSTOMIZE_BUILD ON CACHE BOOL "Customize raylib modules" FORCE)
    set(USE_AUDIO OFF CACHE BOOL "Disable duplicate raylib audio" FORCE)
    set(disabled_features
        SUPPORT_MODULE_RAUDIO
        SUPPORT_MODULE_RSHAPES
        SUPPORT_MODULE_RTEXTURES
        SUPPORT_MODULE_RTEXT
        SUPPORT_MODULE_RMODELS
        SUPPORT_CAMERA_SYSTEM
        SUPPORT_GESTURES_SYSTEM
        SUPPORT_MOUSE_GESTURES
        SUPPORT_SSH_KEYBOARD_RPI
        SUPPORT_RPRAND_GENERATOR
        SUPPORT_SCREEN_CAPTURE
        SUPPORT_GIF_RECORDING
        SUPPORT_COMPRESSION_API
        SUPPORT_AUTOMATION_EVENTS
        SUPPORT_CLIPBOARD_IMAGE
        RL_SUPPORT_MESH_GPU_SKINNING
        SUPPORT_FILEFORMAT_PNG
        SUPPORT_FILEFORMAT_BMP
        SUPPORT_FILEFORMAT_TGA
        SUPPORT_FILEFORMAT_JPG
        SUPPORT_FILEFORMAT_GIF
        SUPPORT_FILEFORMAT_QOI
        SUPPORT_FILEFORMAT_PSD
        SUPPORT_FILEFORMAT_DDS
        SUPPORT_FILEFORMAT_HDR
        SUPPORT_FILEFORMAT_PIC
        SUPPORT_FILEFORMAT_KTX
        SUPPORT_FILEFORMAT_ASTC
        SUPPORT_FILEFORMAT_PKM
        SUPPORT_FILEFORMAT_PVR
        SUPPORT_IMAGE_EXPORT
        SUPPORT_IMAGE_GENERATION
        SUPPORT_IMAGE_MANIPULATION
        SUPPORT_DEFAULT_FONT
        SUPPORT_FILEFORMAT_TTF
        SUPPORT_FILEFORMAT_FNT
        SUPPORT_FILEFORMAT_BDF
        SUPPORT_TEXT_MANIPULATION
        SUPPORT_FONT_ATLAS_WHITE_REC
        SUPPORT_FILEFORMAT_OBJ
        SUPPORT_FILEFORMAT_MTL
        SUPPORT_FILEFORMAT_IQM
        SUPPORT_FILEFORMAT_GLTF
        SUPPORT_FILEFORMAT_VOX
        SUPPORT_FILEFORMAT_M3D
        SUPPORT_MESH_GENERATION
        SUPPORT_FILEFORMAT_WAV
        SUPPORT_FILEFORMAT_OGG
        SUPPORT_FILEFORMAT_MP3
        SUPPORT_FILEFORMAT_QOA
        SUPPORT_FILEFORMAT_FLAC
        SUPPORT_FILEFORMAT_XM
        SUPPORT_FILEFORMAT_MOD)
    foreach(feature IN LISTS disabled_features)
        # FORCE also migrates existing build/CMakeCache.txt configurations.
        set(${feature} OFF CACHE BOOL "Unused by the native WebGPU window" FORCE)
    endforeach()

    # Keep input, the existing host timer behaviour, diagnostics, standard
    # file IO and the rlgl context needed by InitWindow(). Do not suppress
    # compiler diagnostics globally or change the pinned dependency.
    # raylib's old cmake_minimum_required emits a deprecation warning with
    # CMake 4.5. Scope the exception to this dependency only.
    set(CMAKE_WARN_DEPRECATED OFF)
    add_subdirectory("${source}" "${binary}" SYSTEM)

    # The pinned raylib ParseConfigHeader.cmake is not a C preprocessor.
    # It collects BOTH arms of the mesh-skinning conditional in config.h.
    # Replace the conflicting definitions with the actual non-skinning
    # value, rather than hiding -Wmacro-redefined.
    get_target_property(definitions raylib COMPILE_DEFINITIONS)
    if(NOT definitions)
        message(FATAL_ERROR "raylib did not expose its configuration definitions")
    endif()
    list(FILTER definitions EXCLUDE REGEX "^MAX_MESH_VERTEX_BUFFERS(=|$)")
    list(APPEND definitions "MAX_MESH_VERTEX_BUFFERS=7")
    list(REMOVE_DUPLICATES definitions)
    set_property(TARGET raylib PROPERTY COMPILE_DEFINITIONS "${definitions}")

    # Check the actual target rather than trusting cached option values.
    foreach(definition IN LISTS definitions)
        string(REGEX REPLACE "=.*$" "" name "${definition}")
        if(name IN_LIST disabled_features)
            message(FATAL_ERROR "Unused raylib feature is enabled: ${definition}")
        endif()
    endforeach()
    message(STATUS "Native raylib window: core/GLFW input and timing; rendering via WebGPU")
endfunction()
