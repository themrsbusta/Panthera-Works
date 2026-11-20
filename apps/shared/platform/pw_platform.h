#ifndef PANTHERA_SHARED_PLATFORM_H
#define PANTHERA_SHARED_PLATFORM_H

typedef unsigned char pw_platform_u8;

#define PW_PLATFORM_SCREEN_WIDTH 640
#define PW_PLATFORM_SCREEN_HEIGHT 480

enum {
    PW_PLATFORM_COLOR_LAB_TITLE = 254,
    PW_PLATFORM_COLOR_SHOWROOM_TITLE = 253
};

enum {
    PW_PLATFORM_VIDEO_OK = 0,
    PW_PLATFORM_VIDEO_VBE_QUERY = 1,
    PW_PLATFORM_VIDEO_MODE_INFO = 2,
    PW_PLATFORM_VIDEO_MODE_SET = 3,
    PW_PLATFORM_VIDEO_LFB_MAP = 4
};

enum {
    PW_PLATFORM_ICON_NEW = 0,
    PW_PLATFORM_ICON_OPEN,
    PW_PLATFORM_ICON_SAVE,
    PW_PLATFORM_ICON_UNDO,
    PW_PLATFORM_ICON_REDO,
    PW_PLATFORM_ICON_FIND,
    PW_PLATFORM_ICON_REPLACE,
    PW_PLATFORM_ICON_ADD_SLIDE,
    PW_PLATFORM_ICON_DELETE,
    PW_PLATFORM_ICON_PREVIOUS,
    PW_PLATFORM_ICON_NEXT,
    PW_PLATFORM_ICON_PRESENT,
    PW_PLATFORM_ICON_FUNCTION,
    PW_PLATFORM_ICON_TITLE,
    PW_PLATFORM_ICON_BODY
};

int pw_platform_video_start(void);
int pw_platform_video_set_palette_entry(unsigned index,
                                        unsigned red, unsigned green, unsigned blue);
void pw_platform_video_stop(void);
const char *pw_platform_video_error(void);
void pw_platform_video_present(const pw_platform_u8 *pixels);

void pw_platform_canvas_clear(pw_platform_u8 *pixels, pw_platform_u8 color);
void pw_platform_canvas_pixel(pw_platform_u8 *pixels, int x, int y, pw_platform_u8 color);
void pw_platform_canvas_fill_rect(pw_platform_u8 *pixels, int x, int y,
                                  int width, int height, pw_platform_u8 color);
void pw_platform_canvas_line(pw_platform_u8 *pixels, int x0, int y0, int x1, int y1,
                             pw_platform_u8 color);
void pw_platform_canvas_toolbar_icon(pw_platform_u8 *pixels, int icon, int x, int y,
                                     pw_platform_u8 ink, pw_platform_u8 accent);
void pw_platform_canvas_glyph(pw_platform_u8 *pixels,
                              const pw_platform_u8 font[256][16],
                              unsigned char glyph, int x, int y,
                              pw_platform_u8 foreground,
                              pw_platform_u8 background, int opaque);
void pw_platform_canvas_text(pw_platform_u8 *pixels,
                             const pw_platform_u8 font[256][16],
                             const char *text, int x, int y,
                             pw_platform_u8 color);

void pw_platform_font_init_fallback(pw_platform_u8 font[256][16]);
int pw_platform_font_load_bios(pw_platform_u8 font[256][16]);

int pw_platform_mouse_initialize(void);
void pw_platform_mouse_restore(void);
int pw_platform_mouse_read_state(int *x, int *y, unsigned *buttons);

#endif
