/*
 * A crappy utility to set the X root-window wallpaper using Imlib2.
 * Configuration is stored in "$HOME/.wp.toml".
 * Supported display modes: center, fill, max, scale, tile.
 * A solid background colour can be given in RGB or RRGGBB notation.
 * This does not have support for multiple monitors, and will never.
 *
 * Usage:
 *   wall <image> [-m mode] [-x N] [-y N] [-c RRGGBB]
 *   wall // restore saved settings
 */

#include <Imlib2.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <argp.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define AVIF_LOADER_IMPLEMENTATION
#include "avif.h"
#include "toml-c.h"

#define CONFIG_FILE "%s/.wp.toml"

static char doc[] = "Set X root-window wallpaper using Imlib2.\v"
                    "Run without arguments to restore saved settings.";

static char args_doc[] = "[IMAGE]";

// Wallpaper display modes.
typedef enum
{
    WM_Center,
    WM_Fill,
    WM_Max,
    WM_Scale,
    WM_Tile,
    WM_Count
} WallpaperMode;

// Lookup table for mode names.
static const struct
{
    const char *Name;
    WallpaperMode Mode;
} ModeLUT[] = {
    {"center", WM_Center}, {"fill", WM_Fill}, {"max", WM_Max}, {"scale", WM_Scale}, {"tile", WM_Tile},
};

// Serializable user configuration.
typedef struct
{
    char Path[PATH_MAX];
    WallpaperMode Mode;
    int OffsetX;
    int OffsetY;
    char BgColor[8];
} WallpaperConfig;

// Arguments passed through argp.
typedef struct
{
    char *Image;
    char *ModeStr;
    char *Color;
    int OffsetX;
    int OffsetY;
    int HasOffsetX;
    int HasOffsetY;
    int HasMode;
} Arguments;

// Utility helpers
static void die(const char *Message) __attribute__((noreturn));
static void die(const char *Message)
{
    perror(Message);
    exit(EXIT_FAILURE);
}

// Convert textual mode name to enum. Terminates on failure.
static WallpaperMode parseMode(const char *Str)
{
    for (size_t I = 0; I < sizeof ModeLUT / sizeof *ModeLUT; ++I)
        if (strcmp(Str, ModeLUT[I].Name) == 0)
            return ModeLUT[I].Mode;

    fprintf(stderr, "Invalid mode: %s\nAllowed: center fill max scale tile\n", Str);
    exit(EXIT_FAILURE);
}

// Hex digit to integer (0–15).
static inline int hexVal(int C)
{
    return (C <= '9') ? C - '0' : 10 + (C & 0x5F) - 'A';
}

static char *getConfigPath(char *Buffer, size_t Size)
{
    const char *Home = getenv("HOME");
    if (!Home)
        die("HOME not set");
    snprintf(Buffer, Size, CONFIG_FILE, Home);
    return Buffer;
}

// Persistent configuration I/O
static void saveConfig(const WallpaperConfig *Cfg)
{
    char Path[PATH_MAX];
    FILE *File = fopen(getConfigPath(Path, sizeof Path), "w");
    if (!File)
        die("open config");

    fprintf(File, "path = \"%s\"\n", Cfg->Path);
    fprintf(File, "mode = \"%s\"\n", ModeLUT[Cfg->Mode].Name);

    if ((Cfg->Mode == WM_Fill || Cfg->Mode == WM_Center) && (Cfg->OffsetX || Cfg->OffsetY))
    {
        fprintf(File, "offset = [%d, %d]\n", Cfg->OffsetX, Cfg->OffsetY);
    }

    fprintf(File, "background_color = \"%s\"\n", Cfg->BgColor);
    fclose(File);
}

static int loadConfig(WallpaperConfig *Cfg)
{
    char Path[PATH_MAX];
    char errbuf[256];

    FILE *File = fopen(getConfigPath(Path, sizeof Path), "r");
    if (!File)
        return 0;

    toml_table_t *root = toml_parse_file(File, errbuf, sizeof(errbuf));
    fclose(File);

    if (!root)
    {
        fprintf(stderr, "TOML parse error: %s\n", errbuf);
        return 0;
    }

    // Initialize defaults
    Cfg->OffsetX = Cfg->OffsetY = 0;
    strcpy(Cfg->BgColor, "000000");

    // Get path
    toml_value_t path_val = toml_table_string(root, "path");
    if (!path_val.ok)
    {
        fprintf(stderr, "Config missing 'path' key\n");
        toml_free(root);
        return 0;
    }
    strncpy(Cfg->Path, path_val.u.s, sizeof(Cfg->Path) - 1);
    Cfg->Path[sizeof(Cfg->Path) - 1] = '\0';
    free(path_val.u.s);

    // Get mode
    toml_value_t mode_val = toml_table_string(root, "mode");
    if (!mode_val.ok)
    {
        fprintf(stderr, "Config missing 'mode' key\n");
        toml_free(root);
        return 0;
    }
    Cfg->Mode = parseMode(mode_val.u.s);
    free(mode_val.u.s);

    // Get offset array (optional)
    toml_array_t *offset_arr = toml_table_array(root, "offset");
    if (offset_arr && toml_array_len(offset_arr) >= 2)
    {
        toml_value_t x_val = toml_array_int(offset_arr, 0);
        toml_value_t y_val = toml_array_int(offset_arr, 1);
        if (x_val.ok)
            Cfg->OffsetX = (int)x_val.u.i;
        if (y_val.ok)
            Cfg->OffsetY = (int)y_val.u.i;
    }

    // Get background_color (optional)
    toml_value_t color_val = toml_table_string(root, "background_color");
    if (color_val.ok)
    {
        strncpy(Cfg->BgColor, color_val.u.s, sizeof(Cfg->BgColor) - 1);
        Cfg->BgColor[sizeof(Cfg->BgColor) - 1] = '\0';
        free(color_val.u.s);
    }

    toml_free(root);
    return 1;
}

// X11 helpers

// Create a 24-bit pixmap and paint it with a RGB colour string.
static Pixmap getOrCreateRootPixmap(Display *Dpy, Window Root, int Width, int Height, const char *Hex, int *created)
{
    Atom AtomRootPixmap = XInternAtom(Dpy, "_XROOTPMAP_ID", False);
    Pixmap Pix = None;
    Atom ActualType;
    int ActualFormat;
    unsigned long NItems, BytesAfter;
    unsigned char *Data = NULL;
    *created = 0;

    if (XGetWindowProperty(Dpy, Root, AtomRootPixmap, 0, 1, False, XA_PIXMAP, &ActualType, &ActualFormat, &NItems,
                           &BytesAfter, &Data) == Success &&
        ActualType == XA_PIXMAP && ActualFormat == 32 && NItems == 1)
    {
        Pix = *(Pixmap *)Data;
    }
    if (Data)
        XFree(Data);

    if (Pix != None)
    {
        Window RootRet;
        int X, Y;
        unsigned int WidthRet, HeightRet, BorderRet, DepthRet;
        if (!XGetGeometry(Dpy, Pix, &RootRet, &X, &Y, &WidthRet, &HeightRet, &BorderRet, &DepthRet) ||
            WidthRet != (unsigned int)Width || HeightRet != (unsigned int)Height)
        {
            Pix = None;
        }
    }

    if (Pix == None)
    {
        Pix = XCreatePixmap(Dpy, Root, Width, Height, DefaultDepth(Dpy, DefaultScreen(Dpy)));
        *created = 1;
    }

    // Always repaint the background colour
    int R = 0, G = 0, B = 0;
    size_t Len = strlen(Hex);
    if (Len == 3)
    {
        R = hexVal(Hex[0]) * 17;
        G = hexVal(Hex[1]) * 17;
        B = hexVal(Hex[2]) * 17;
    }
    else if (Len == 6)
    {
        R = (hexVal(Hex[0]) << 4) | hexVal(Hex[1]);
        G = (hexVal(Hex[2]) << 4) | hexVal(Hex[3]);
        B = (hexVal(Hex[4]) << 4) | hexVal(Hex[5]);
    }
    else
    {
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
static void setWallpaper(const WallpaperConfig *Cfg)
{
    Display *Dpy = XOpenDisplay(NULL);
    if (!Dpy)
        die("XOpenDisplay");

    const int Scr = DefaultScreen(Dpy);
    const Window Root = RootWindow(Dpy, Scr);
    const int ScrW = DisplayWidth(Dpy, Scr);
    const int ScrH = DisplayHeight(Dpy, Scr);

    int created = 0;
    Pixmap Pix = getOrCreateRootPixmap(Dpy, Root, ScrW, ScrH, Cfg->BgColor, &created);

    // Imlib2 context
    imlib_context_set_display(Dpy);
    imlib_context_set_visual(DefaultVisual(Dpy, Scr));
    imlib_context_set_colormap(DefaultColormap(Dpy, Scr));

    const char *ext = strrchr(Cfg->Path, '.');
    Imlib_Image Img = (ext && strcasecmp(ext, ".avif") == 0) ? loadAvif(Cfg->Path) : imlib_load_image(Cfg->Path);
    if (!Img)
    {
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

    switch (Cfg->Mode)
    {
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
    }

    default:
        fprintf(stderr, "unhandled mode\n");
        abort();
    }

    static Atom AtomRoot = None;
    static Atom AtomSetroot = None;

    if (AtomRoot == None)
    {
        AtomRoot = XInternAtom(Dpy, "_XROOTPMAP_ID", False);
        AtomSetroot = XInternAtom(Dpy, "_XSETROOT_ID", False);
    }

    XChangeProperty(Dpy, Root, AtomRoot, XA_PIXMAP, 32, PropModeReplace, (unsigned char *)&Pix, 1);
    XChangeProperty(Dpy, Root, AtomSetroot, XA_PIXMAP, 32, PropModeReplace, (unsigned char *)&Pix, 1);

    XSetWindowBackgroundPixmap(Dpy, Root, Pix);
    XClearWindow(Dpy, Root);
    XFlush(Dpy);

    if (created)
        XSetCloseDownMode(Dpy, RetainPermanent);

    imlib_context_set_image(Img);
    imlib_free_image();

    XCloseDisplay(Dpy);
}

// argp option definitions
static struct argp_option options[] = {{"mode", 'm', "MODE", 0, "Display mode (center/fill/max/scale/tile)", 0},
                                       {"color", 'c', "HEX", 0, "Background colour (RGB or RRGGBB)", 0},
                                       {"offset-x", 'x', "N", 0, "Horizontal offset (fill/center only)", 0},
                                       {"offset-y", 'y', "N", 0, "Vertical offset (fill/center only)", 0},
                                       {0}};

static error_t parse_opt(int Key, char *Arg, struct argp_state *State)
{
    Arguments *Args = State->input;
    char *End;

    switch (Key)
    {
    case 'm':
        Args->ModeStr = Arg;
        Args->HasMode = 1;
        break;

    case 'c': {
        size_t Len = strlen(Arg);
        if (!(Len == 3 || Len == 6) || strspn(Arg, "0123456789aAbBcCdDeEfF") != Len)
        {
            argp_error(State, "Colour must be RGB or RRGGBB");
        }
        Args->Color = Arg;
        break;
    }

    case 'x':
        Args->OffsetX = (int)strtol(Arg, &End, 10);
        if (*End != '\0')
            argp_error(State, "Invalid X offset: %s", Arg);
        Args->HasOffsetX = 1;
        break;

    case 'y':
        Args->OffsetY = (int)strtol(Arg, &End, 10);
        if (*End != '\0')
            argp_error(State, "Invalid Y offset: %s", Arg);
        Args->HasOffsetY = 1;
        break;

    case ARGP_KEY_ARG:
        if (Args->Image)
            argp_error(State, "Too many arguments");
        Args->Image = Arg;
        break;

    default:
        return ARGP_ERR_UNKNOWN;
    }

    return 0;
}

static struct argp argp = {options, parse_opt, args_doc, doc, NULL, NULL, NULL};

// Main entry point
int main(int Argc, char *Argv[])
{
    WallpaperConfig Cfg = {.Mode = WM_Fill};
    strcpy(Cfg.BgColor, "000000");

    Arguments Args = {0};

    argp_parse(&argp, Argc, Argv, 0, NULL, &Args);

    if (Args.Color)
        strncpy(Cfg.BgColor, Args.Color, sizeof Cfg.BgColor - 1);

    if (Args.Image)
    {
        if (!realpath(Args.Image, Cfg.Path))
            die("realpath");

        Cfg.Mode = Args.HasMode ? parseMode(Args.ModeStr) : WM_Fill;

        if (Args.HasOffsetX || Args.HasOffsetY)
        {
            if (Cfg.Mode != WM_Fill && Cfg.Mode != WM_Center)
            {
                fprintf(stderr, "Offset only valid for fill/center modes\n");
                return EXIT_FAILURE;
            }
            Cfg.OffsetX = Args.OffsetX;
            Cfg.OffsetY = Args.OffsetY;
        }
    }
    else if (!loadConfig(&Cfg))
    {
        fprintf(stderr, "No stored configuration\n");
        argp_help(&argp, stderr, ARGP_HELP_STD_USAGE, Argv[0]);
        return EXIT_FAILURE;
    }

    setWallpaper(&Cfg);
    saveConfig(&Cfg);
    return EXIT_SUCCESS;
}
