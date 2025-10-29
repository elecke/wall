// Provides a helper to decode AVIF images faster.
// Feeds libavif + dav1d decoded images into Imlib2.
#include "avif.h"
#include <Imlib2.h>
#include <avif/avif.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

// Falls back to 1 if core count can't be determined;
// used for parallel decoding.
static int getCpuCount(void) {
#ifdef _SC_NPROCESSORS_ONLN
  long n = sysconf(_SC_NPROCESSORS_ONLN);
  return (n > 0) ? (int)n : 1;
#else
  return 1;
#endif
}

// Loads an AVIF image from disk and decodes to BGRA via libavif;
// returns an Imlib2 image.
Imlib_Image loadAvif(const char *path) {
  avifDecoder *dec = NULL;
  avifRGBImage rgb;
  Imlib_Image im = NULL;
  int pixelsAlloc = 0;

  dec = avifDecoderCreate();
  if (!dec) {
    fprintf(stderr, "avifDecoderCreate failed\n");
    goto cleanup;
  }

  // Ensure the dav1d decoder is present. Aborts if unavailable.
  dec->maxThreads = getCpuCount();
  if (!avifCodecName(AVIF_CODEC_CHOICE_DAV1D, AVIF_CODEC_FLAG_CAN_DECODE)) {
    fprintf(stderr, "dav1d not available at runtime\n");
    goto cleanup;
  }
  dec->codecChoice = AVIF_CODEC_CHOICE_DAV1D;

  avifResult r = avifDecoderSetIOFile(dec, path);
  if (r != AVIF_RESULT_OK) {
    fprintf(stderr, "AVIF I/O error: %s\n", avifResultToString(r));
    goto cleanup;
  }

  if ((r = avifDecoderParse(dec)) != AVIF_RESULT_OK) {
    fprintf(stderr, "AVIF parse error: %s\n", avifResultToString(r));
    goto cleanup;
  }

  if ((r = avifDecoderNextImage(dec)) != AVIF_RESULT_OK) {
    fprintf(stderr, "AVIF decode error: %s\n", avifResultToString(r));
    goto cleanup;
  }

  avifImage *y = dec->image;
  avifRGBImageSetDefaults(&rgb, y);
  rgb.format = AVIF_RGB_FORMAT_BGRA;
  rgb.depth = 8;

  if ((r = avifRGBImageAllocatePixels(&rgb)) != AVIF_RESULT_OK) {
    fprintf(stderr, "AVIF pixel alloc error: %s\n", avifResultToString(r));
    goto cleanup;
  }
  pixelsAlloc = 1;

  if ((r = avifImageYUVToRGB(y, &rgb)) != AVIF_RESULT_OK) {
    fprintf(stderr, "AVIF to RGB error: %s\n", avifResultToString(r));
    goto cleanup;
  }

  // Check for integer overflow in image dimensions.
  size_t totalPixels = (size_t)rgb.width * (size_t)rgb.height;
  if (rgb.width != 0 && totalPixels / rgb.width != (size_t)rgb.height) {
    fprintf(stderr, "Image dimensions overflow\n");
    goto cleanup;
  }
  size_t totalBytes = totalPixels * 4;
  if (totalBytes > SIZE_MAX) {
    fprintf(stderr, "Image size overflow\n");
    goto cleanup;
  }

  // Create Imlib2 image using decoded BGRA pixel data.
  im =
    imlib_create_image_using_data(rgb.width, rgb.height, (DATA32 *)rgb.pixels);
  // fuck you enlightenment
  if (!im) {
    fprintf(stderr, "Imlib image alloc failed\n");
    goto cleanup;
  }

  pixelsAlloc = 0;

cleanup:
  if (pixelsAlloc)
    avifRGBImageFreePixels(&rgb);
  if (dec)
    avifDecoderDestroy(dec);
  return im;
}
