# flvdmx
This filter demultiplexes FLV (Flash Video) files into one pid per stream, with the codec named and the decoder configuration attached.

## What it emits, and what decodes it

FLV names its codec in the first byte of every audio and video tag. The
demuxer passes each stream on with that name and leaves decoding to whatever
chain link claims it:

| In the FLV | Pid codec | Decoded by |
|---|---|---|
| Sorenson H.263 (codec 2) | `FLV1` | [ffmpeg-h26x](https://github.com/Bevara/ffmpeg-h26x) |
| AVC (codec 7) | `GF_CODECID_AVC`, avcC as decoder config | [h264bsd](https://github.com/Bevara/h264bsd) |
| MP3 (formats 2 and 14) | `GF_CODECID_MPEG_AUDIO` | [libmad](https://github.com/Bevara/libmad), [libmpg123](https://github.com/Bevara/libmpg123), or stored as is by mp4mx |
| AAC (format 10) | `GF_CODECID_AAC_MPEG4`, AudioSpecificConfig as decoder config | [libfaad](https://github.com/Bevara/libfaad), or stored as is |
| G.711 A-law / mu-law (7, 8) | `GF_CODECID_ALAW` / `GF_CODECID_MULAW` | [libg711](https://github.com/Bevara/libg711) |
| Speex (11) | `GF_CODECID_SPEEX` | [libspeex](https://github.com/Bevara/libspeex) |

VP6, Screen Video and Nellymoser have no decoder in this player; the demuxer
logs one warning and skips the stream.

Output pids are created when the first tag of each stream is seen, with the
picture size read from the stream itself - the Sorenson picture header, or the
sequence parameter set inside the avcC - and, for MP3, the sampling rate read
from the frame header: the FLV tag's own 2-bit rate field cannot say 48 kHz,
which is what most MP3 in FLV is. The `onMetaData` script tag is read for the
frame rate and the duration.

## Why it no longer uses libflv

The first version wrapped libflv, an RTMP library, and inherited its idea of
what FLV holds: AVC and AAC, which is what RTMP carries today. The FLV files
this player is for - and every test signal - hold Sorenson H.263 and MP3, the
codecs Flash shipped with, on which that version emitted nothing. FLV is a
tag stream with an eleven-byte header per tag; parsing it is shorter than the
glue was.

## Requirements

[CMake](https://cmake.org/) is used as a build system. To install it, follow
[Debian build instructions](developing_in_debian.md).

[Emscripten SDK](https://emscripten.org/) is required for building
WebAssembly artifacts. To install it, follow the
[Download and Install](https://emscripten.org/docs/getting_started/downloads.html)
guide:

```bash
cd $OPT

# Get the emsdk repo.
git clone https://github.com/emscripten-core/emsdk.git

# Enter that directory.
cd emsdk

# Download and install the latest SDK tools.
./emsdk install latest

# Make the "latest" SDK "active" for the current user. (writes ~/.emscripten file)
./emsdk activate latest
```

## Building the accessor

```bash
# Setup EMSDK and other environment variables. In practice EMSDK is set to be
# $OPT/emsdk.
source $OPT/emsdk/emsdk_env.sh

# Assuming you are in the root level of the cloned repo :
emcmake cmake .
emmake make
```

Once built, you can use and distribute flvdmx_1.wasm with your universal tags.

## Documentation

For more details, please visit our documentation at https://bevara.com/documentation/develop/.
