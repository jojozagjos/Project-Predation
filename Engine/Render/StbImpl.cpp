// Single translation unit that owns the stb implementations used by the engine.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

// Reading, for textures that arrive inside a downloaded model. glTF embeds them as PNG or JPEG in
// the binary chunk, so the decoder has to work from memory rather than from a path.
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
