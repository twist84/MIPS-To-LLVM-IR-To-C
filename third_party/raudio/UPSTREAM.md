# raudio source provenance

Upstream: https://github.com/raysan5/raudio
Revision: 811c150fe97a2abb8ec67857dc52e022bfaa4e43

The upstream LICENSE, src/raudio.h and src/external/miniaudio.h are verbatim.
Only the raw PCM audio device/stream implementation is built; compressed
file-format loading flags are not enabled. Additional upstream decoders
are therefore not required. miniaudio's license is retained in its header.

src/raudio.c is modified locally: stream play/pause/resume and callback
assignment synchronize with the mixer; untracking precedes converter
destruction; device callback shutdown precedes mutex destruction; start
failure releases its mutex; the null backend is rejected; standalone logs
terminate with a newline; device format accessors expose the negotiated
sample rate and channel count. The upstream license notice is retained.
