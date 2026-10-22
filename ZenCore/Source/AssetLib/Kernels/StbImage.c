// stb_image decodes every imported PNG/JPEG/TGA image. Its implementation lives in
// the optimized asset-kernel library so unoptimized builds still decode at speed.
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
