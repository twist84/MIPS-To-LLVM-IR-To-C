# raudio source provenance

Upstream: https://github.com/raysan5/raylib
Revision: c1ab645ca298a2801097931d1079b10ff7eb9df8 (raylib 5.5)

CMake FetchContent downloads raylib once into build/_deps/raylib-src, including headless configurations. Headless builds do not configure or link raylib graphics or GLFW. Window builds disable the library's own audio module and use the same separately compiled standalone audio target.

cmake/prepare_raudio.cmake generates an audio-only header from the pinned raylib.h and a patched raudio.c under build/_deps/raylib-build/ee_raudio. The checkout, upstream license and external/miniaudio.h remain intact. Compressed audio file-format flags are not enabled. The generated header retains the upstream license notice.

Raylib already synchronizes stream control and callback assignment and untracks buffers before converter destruction. Local patches reject the null backend, release the mixer mutex on device-start failure, join the device callback before destroying its mutex, terminate standalone logs with a newline, expose the negotiated device format and free standalone file allocations without requiring raylib's file utility implementation.
