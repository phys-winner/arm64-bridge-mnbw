# Texture2DDecoder fallback subset

This directory vendors the ASTC and ETC decoders from
`texture2ddecoder` 1.0.6. Only the decoder sources needed by the bridge's
uncompressed texture-upload fallback are included.

- Upstream: https://github.com/K0lb3/texture2ddecoder
- Version: 1.0.6
- License: MIT (`LICENSE`)
- Imported files: `astc.cpp`, `astc.h`, `etc.cpp`, `etc.h`, `color.h`,
  `endianness.h`, `fp16.h`, and `fp16/*`

The decoders produce BGRA8 words. The bridge swaps red and blue before a
portable `GL_RGBA` / `GL_UNSIGNED_BYTE` upload.
