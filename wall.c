/*
 * A crappy utility to set the X root-window wallpaper using Imlib2.
 * Configuration is stored in "$HOME/.wprc".
 * Supported display modes: center, fill, max, scale, tile.
 * A solid background colour can be given in RGB or RRGGBB notation.
 * This does not have support for multiple monitors, and will never.
 *
 * Usage:
 *   wall <image> mode [-c RRGGBB]
 *   wall <image> fill [x=N,y=N] [-c RRGGBB]
 *   wall // restore saved settings
 */

#include <Imlib2.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "avif.h"

#define CONFIG_FILE "%s/.wprc"

// Wallpaper display modes.
typedef enum {
  WM_Center,
  WM_Fill,
  WM_Max,
  WM_Scale,
  WM_Tile,
  WM_Count
} WallpaperMode;

// Lookup table for mode names.
static const struct {
  const char *Name;
  WallpaperMode Mode;
} ModeLUT[] = {
  {"center", WM_Center}, {"fill", WM_Fill}, {"max", WM_Max},
  {"scale", WM_Scale},   {"tile", WM_Tile},
};

// Serializable user configuration.
typedef struct {
  char Path[PATH_MAX];
  WallpaperMode Mode;
  int OffsetX;
  int OffsetY;
  char BgColor[8];
} WallpaperConfig;

// Utility helpers
static void die(const char *Message) __attribute__((noreturn));
static void die(const char *Message) {
  perror(Message);
  exit(EXIT_FAILURE);
}

// Convert textual mode name to enum. Terminates on failure.
static WallpaperMode parseMode(const char *Str) {
  for (size_t I = 0; I < sizeof ModeLUT / sizeof *ModeLUT; ++I)
    if (strcmp(Str, ModeLUT[I].Name) == 0)
      return ModeLUT[I].Mode;

  fprintf(stderr, "Invalid mode: %s\nAllowed: center fill max scale tile\n",
          Str);
  exit(EXIT_FAILURE);
}

// Parse "x=N,y=M" offset specification. Returns 1 on success, 0 otherwise.
static int parseOffset(const char *Str, int *OffX, int *OffY) {
  int Parsed = 0;
  return sscanf(Str, "x=%d,y=%d%n", OffX, OffY, &Parsed) == 2 &&
         Str[Parsed] == '\0';
}

// Hex digit to integer (0–15).
static inline int hexVal(int C) {
  return (C <= '9') ? C - '0' : 10 + (C & 0x5F) - 'A';
}

static char *getConfigPath(char *Buffer, size_t Size) {
  const char *Home = getenv("HOME");
  if (!Home)
    die("HOME not set");
  snprintf(Buffer, Size, CONFIG_FILE, Home);
  return Buffer;
}

// Persistent configuration I/O
static void saveConfig(const WallpaperConfig *Cfg) {
  char Path[PATH_MAX];
  FILE *File = fopen(getConfigPath(Path, sizeof Path), "w");
  if (!File)
    die("open config");

  fprintf(File, "%s\n%s\n", Cfg->Path, ModeLUT[Cfg->Mode].Name);

  if ((Cfg->Mode == WM_Fill || Cfg->Mode == WM_Center) &&
      (Cfg->OffsetX || Cfg->OffsetY))
    fprintf(File, "x=%d,y=%d\n", Cfg->OffsetX, Cfg->OffsetY);

  fprintf(File, "%s\n", Cfg->BgColor);
  fclose(File);
}

static int loadConfig(WallpaperConfig *Cfg) {
  char Path[PATH_MAX], ModeStr[32], Line[64];

  FILE *File = fopen(getConfigPath(Path, sizeof Path), "r");
  if (!File)
    return 0;

  Cfg->OffsetX = Cfg->OffsetY = 0;
  strcpy(Cfg->BgColor, "000000");

  if (!fgets(Cfg->Path, PATH_MAX, File) ||
      !fgets(ModeStr, sizeof ModeStr, File)) {
    fclose(File);
    return 0;
  }

  Cfg->Path[strcspn(Cfg->Path, "\n")] = '\0';
  ModeStr[strcspn(ModeStr, "\n")] = '\0';
  Cfg->Mode = parseMode(ModeStr);

  if (!fgets(Line, sizeof Line, File)) {
    fclose(File);
    return 1; // no optional section present
  }

  Line[strcspn(Line, "\n")] = '\0';

  if (Line[0] == 'x') { // offset first
    if (!parseOffset(Line, &Cfg->OffsetX, &Cfg->OffsetY))
      fprintf(stderr, "Invalid offset in rc file\n");

    if (fgets(Line, sizeof Line, File)) {
      Line[strcspn(Line, "\n")] = '\0';
      if (isxdigit((unsigned char)Line[0]))
        strncpy(Cfg->BgColor, Line, sizeof Cfg->BgColor - 1);
    }
  } else if (isxdigit((unsigned char)Line[0])) { // just colour
    strncpy(Cfg->BgColor, Line, sizeof Cfg->BgColor - 1);
  }

  fclose(File);
  return 1;
}

// X11 helpers

// Create a 24-bit pixmap and paint it with a RGB colour string.
static Pixmap getOrCreateRootPixmap(Display *Dpy, Window Root, int Width,
                                    int Height, const char *Hex, int *created) {
  Atom AtomRootPixmap = XInternAtom(Dpy, "_XROOTPMAP_ID", False);
  Pixmap Pix = None;
  Atom ActualType;
  int ActualFormat;
  unsigned long NItems, BytesAfter;
  unsigned char *Data = NULL;
  *created = 0;

  if (XGetWindowProperty(Dpy, Root, AtomRootPixmap, 0, 1, False, XA_PIXMAP,
                         &ActualType, &ActualFormat, &NItems, &BytesAfter,
                         &Data) == Success &&
      ActualType == XA_PIXMAP && ActualFormat == 32 && NItems == 1) {
    Pix = *(Pixmap *)Data;
  }
  if (Data)
    XFree(Data);

  if (Pix != None) {
    Window RootRet;
    int X, Y;
    unsigned int WidthRet, HeightRet, BorderRet, DepthRet;
    if (!XGetGeometry(Dpy, Pix, &RootRet, &X, &Y, &WidthRet, &HeightRet,
                      &BorderRet, &DepthRet) ||
        WidthRet != (unsigned int)Width || HeightRet != (unsigned int)Height) {
      Pix = None;
    }
  }

  if (Pix == None) {
    Pix = XCreatePixmap(Dpy, Root, Width, Height,
                        DefaultDepth(Dpy, DefaultScreen(Dpy)));
    *created = 1;
  }

  // Always repaint the background colour
  int R = 0, G = 0, B = 0;
  size_t Len = strlen(Hex);
  if (Len == 3) {
    R = hexVal(Hex[0]) * 17;
    G = hexVal(Hex[1]) * 17;
    B = hexVal(Hex[2]) * 17;
  } else if (Len == 6) {
    R = (hexVal(Hex[0]) << 4) | hexVal(Hex[1]);
    G = (hexVal(Hex[2]) << 4) | hexVal(Hex[3]);
    B = (hexVal(Hex[4]) << 4) | hexVal(Hex[5]);
  } else {
    fprintf(stderr, "Invalid colour: %s\n", Hex);
    exit(EXIT_FAILURE);
  }

  GC GCtx = XCreateGC(Dpy, Pix, 0, NULL);
  XColor Col = {.red = (unsigned short)(R * 257),
                .green = (unsigned short)(G * 257),
                .blue = (unsigned short)(B * 257),
                .flags = DoRed | DoGreen | DoBlue};

  XAllocColor(Dpy, DefaultColormap(Dpy, DefaultScreen(Dpy)), &Col);
  XSetForeground(Dpy, GCtx, Col.pixel);
  XFillRectangle(Dpy, Pix, GCtx, 0, 0, Width, Height);
  XFreeGC(Dpy, GCtx);

  return Pix;
}

// Core wallpaper routine
static void setWallpaper(const WallpaperConfig *Cfg) {
  Display *Dpy = XOpenDisplay(NULL);
  if (!Dpy)
    die("XOpenDisplay");

  const int Scr = DefaultScreen(Dpy);
  const Window Root = RootWindow(Dpy, Scr);
  const int ScrW = DisplayWidth(Dpy, Scr);
  const int ScrH = DisplayHeight(Dpy, Scr);

  int created = 0;
  Pixmap Pix =
    getOrCreateRootPixmap(Dpy, Root, ScrW, ScrH, Cfg->BgColor, &created);

  // Imlib2 context
  imlib_context_set_display(Dpy);
  imlib_context_set_visual(DefaultVisual(Dpy, Scr));
  imlib_context_set_colormap(DefaultColormap(Dpy, Scr));

  const char *ext = strrchr(Cfg->Path, '.');
  Imlib_Image Img = (ext && strcasecmp(ext, ".avif") == 0)
                      ? loadAvif(Cfg->Path)
                      : imlib_load_image(Cfg->Path);
  if (!Img) {
    fprintf(stderr, "Cannot load: %s\n", Cfg->Path);
    XCloseDisplay(Dpy);
    exit(EXIT_FAILURE);
  }

  imlib_context_set_image(Img);
  const int ImgW = imlib_image_get_width();
  const int ImgH = imlib_image_get_height();

  imlib_context_set_drawable(Pix);

  int Dx = 0, Dy = 0, NewW, NewH;
  double Scale = 1.0, SX = 1.0, SY = 1.0;

  switch (Cfg->Mode) {
  case WM_Center:
    Dx = (ScrW - ImgW) / 2 + Cfg->OffsetX;
    Dy = (ScrH - ImgH) / 2 + Cfg->OffsetY;
    imlib_render_image_on_drawable_at_size(Dx, Dy, ImgW, ImgH);
    break;

  case WM_Fill:
    SX = (double)ScrW / ImgW;
    SY = (double)ScrH / ImgH;
    Scale = (SX > SY) ? SX : SY;
    NewW = (int)(ImgW * Scale);
    NewH = (int)(ImgH * Scale);
    Dx = (ScrW - NewW) / 2 + Cfg->OffsetX;
    Dy = (ScrH - NewH) / 2 + Cfg->OffsetY;
    imlib_render_image_on_drawable_at_size(Dx, Dy, NewW, NewH);
    break;

  case WM_Max:
    SX = (double)ScrW / ImgW;
    SY = (double)ScrH / ImgH;
    Scale = (SX < SY) ? SX : SY;
    NewW = (int)(ImgW * Scale);
    NewH = (int)(ImgH * Scale);
    Dx = (ScrW - NewW) / 2;
    Dy = (ScrH - NewH) / 2;
    imlib_render_image_on_drawable_at_size(Dx, Dy, NewW, NewH);
    break;

  case WM_Scale:
    imlib_render_image_on_drawable_at_size(0, 0, ScrW, ScrH);
    break;

  case WM_Tile: {
    Pixmap tile = XCreatePixmap(Dpy, Pix, ImgW, ImgH, DefaultDepth(Dpy, Scr));

    imlib_context_set_drawable(tile);
    imlib_render_image_on_drawable(0, 0);

    imlib_context_set_drawable(Pix);

    GC gc = XCreateGC(Dpy, Pix, 0, NULL);
    XSetTile(Dpy, gc, tile);
    XSetFillStyle(Dpy, gc, FillTiled);
    XSetTSOrigin(Dpy, gc, 0, 0);

    XFillRectangle(Dpy, Pix, gc, 0, 0, ScrW, ScrH);

    XFreeGC(Dpy, gc);
    XFreePixmap(Dpy, tile);
    break;
  } break;

  default:
    fprintf(stderr, "unhandled mode\n");
    abort();
  }

  static Atom AtomRoot = None;
  static Atom AtomSetroot = None;

  if (AtomRoot == None) {
    AtomRoot = XInternAtom(Dpy, "_XROOTPMAP_ID", False);
    AtomSetroot = XInternAtom(Dpy, "_XSETROOT_ID", False);
  }

  XChangeProperty(Dpy, Root, AtomRoot, XA_PIXMAP, 32, PropModeReplace,
                  (unsigned char *)&Pix, 1);
  XChangeProperty(Dpy, Root, AtomSetroot, XA_PIXMAP, 32, PropModeReplace,
                  (unsigned char *)&Pix, 1);

  XSetWindowBackgroundPixmap(Dpy, Root, Pix);
  XClearWindow(Dpy, Root);
  XFlush(Dpy);

  if (created)
    XSetCloseDownMode(Dpy, RetainPermanent);

  imlib_context_set_image(Img);
  imlib_free_image();

  XCloseDisplay(Dpy);
}

// Main entry point
int main(int Argc, char *Argv[]) {
  WallpaperConfig Cfg = {.Mode = WM_Fill};
  strcpy(Cfg.BgColor, "000000");

  int Positional = 0;
  char *Image = NULL;
  char *ModeStr = NULL;
  char *OffsetStr = NULL;

  // Parse command line
  for (int I = 1; I < Argc; ++I) {
    if (strcmp(Argv[I], "-c") == 0) {
      if (++I >= Argc) {
        fprintf(stderr, "-c needs a colour\n");
        return EXIT_FAILURE;
      }
      char *Colour = Argv[I];

      const size_t L = strlen(Colour);
      if (!(L == 3 || L == 6) ||
          strspn(Colour, "0123456789aAbBcCdDeEfF") != L) {
        fprintf(stderr, "Colour must be RGB or RRGGBB\n");
        return EXIT_FAILURE;
      }

      strncpy(Cfg.BgColor, Colour, sizeof Cfg.BgColor - 1);
      continue;
    }

    switch (Positional++) {
    case 0:
      Image = Argv[I];
      break;
    case 1:
      ModeStr = Argv[I];
      break;
    case 2:
      OffsetStr = Argv[I];
      break;
    default:
      fprintf(stderr, "Too many arguments!\n");
      return EXIT_FAILURE;
    }
  }

  if (Positional) {
    if (!realpath(Image, Cfg.Path))
      die("realpath");

    Cfg.Mode = ModeStr ? parseMode(ModeStr) : WM_Fill;

    if ((Cfg.Mode == WM_Fill || Cfg.Mode == WM_Center) && OffsetStr &&
        !parseOffset(OffsetStr, &Cfg.OffsetX, &Cfg.OffsetY)) {
      fprintf(stderr, "Offset must be x=N,y=M\n");
      return EXIT_FAILURE;
    }
  } else if (!loadConfig(&Cfg)) {
    fprintf(stderr,
            "No stored configuration\n"
            "Usage: %s [image] [mode] [x=N,y=M] [-c colour]"
            " (offset only for fill and center modes)\n",
            Argv[0]);
    return EXIT_FAILURE;
  }

  setWallpaper(&Cfg);
  saveConfig(&Cfg);
  return EXIT_SUCCESS;
}
