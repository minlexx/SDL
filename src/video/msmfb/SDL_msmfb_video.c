/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2019 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/
#include "../../SDL_internal.h"

#if SDL_VIDEO_DRIVER_MSMFB

/* Based on dummy SDL video driver implementation */

#include "SDL_log.h"
#include "SDL_video.h"
#include "SDL_mouse.h"
#include "SDL_msmfb_video.h"

#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>

#define MSMFB_DRIVER_NAME "msmfb"


struct MSMFB_VideoDriverData
{
    int fb_fd;           /* Framebuffer device file descriptor */
    int	fb_mem_offset;   /* offset from memory start address (usually 0) */
    struct fb_fix_screeninfo fb_fix;  /* Fixed FB info from linux/fb.h (name, address) */
    struct fb_var_screeninfo fb_var;  /* Var FB info (current display mode, color space) */
    struct fb_var_screeninfo fb_var_orig;  /* Original Var FB info (to restore) */
    unsigned char *fb_mem;   /* Address of framebuffer memory mapped to user space */
};


/* Taken from mdss_fb / mdp kernel driver sources */
struct mdp_rect {
    unsigned x;
    unsigned y;
    unsigned w;
    unsigned h;
};

struct mdp_display_commit {
    unsigned flags;
    unsigned wait_for_finish;
    struct fb_var_screeninfo var;
    struct mdp_rect roi;
};


void msmfb_display_commit(int fd)
{
#define MSMFB_IOCTL_MAGIC 'm'
#define MSMFB_DISPLAY_COMMIT _IOW(MSMFB_IOCTL_MAGIC, 164, struct mdp_display_commit)
#define MDP_DISPLAY_COMMIT_OVERLAY	1

    struct mdp_display_commit info;
    memset(&info, 0, sizeof(struct mdp_display_commit));
    info.flags = MDP_DISPLAY_COMMIT_OVERLAY;

    if (ioctl(fd, MSMFB_DISPLAY_COMMIT, &info) == -1) {
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO, "Failed to call ioctl MSMFB_DISPLAY_COMMIT");
    }
}



static int  MSMFB_VideoInit(_THIS);
static int  MSMFB_SetDisplayMode(_THIS, SDL_VideoDisplay * display, SDL_DisplayMode * mode);
static void MSMFB_VideoQuit(_THIS);
static void MSMFB_PumpEvents(_THIS);
static int  MSMFB_CreateWindowFramebuffer(_THIS, SDL_Window * window, Uint32 * format, void ** pixels, int *pitch);
static int  MSMFB_UpdateWindowFramebuffer(_THIS, SDL_Window * window, const SDL_Rect * rects, int numrects);
static void MSMFB_DestroyWindowFramebuffer(_THIS, SDL_Window * window);


static int
MSMFB_Available(void)
{
    const char *envr = SDL_getenv("SDL_VIDEODRIVER");
    if (envr && (SDL_strcmp(envr, MSMFB_DRIVER_NAME) == 0)) {
        SDL_LogSetPriority(SDL_LOG_CATEGORY_VIDEO, SDL_LOG_PRIORITY_VERBOSE);
        SDL_LogDebug(SDL_LOG_CATEGORY_VIDEO, "MSMFB SDL videodriver enabled");
        return 1;
    }

    SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO, "MSMFB: not enabled, use SDL_VIDEODRIVER=" MSMFB_DRIVER_NAME " to enable");

    /* Not available */
    return 0;
}

static void
MSMFB_DeleteDevice(SDL_VideoDevice * device)
{
    SDL_free(device->driverdata);
    SDL_free(device);
}

static SDL_VideoDevice *
MSMFB_CreateDevice(int devindex)
{
    SDL_VideoDevice *device;
    (void)devindex;

    /* Initialize all variables that we clean on shutdown */
    device = (SDL_VideoDevice *) SDL_calloc(1, sizeof(SDL_VideoDevice));
    if (!device) {
        SDL_OutOfMemory();
        SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "MSMFB_CreateDevice: out of memory.");
        return 0;
    }
    
    device->is_dummy = SDL_FALSE;
    device->name = MSMFB_DRIVER_NAME;

    /* Set the function pointers */
    device->VideoInit = MSMFB_VideoInit;
    device->VideoQuit = MSMFB_VideoQuit;
    device->free = MSMFB_DeleteDevice;
    device->SetDisplayMode = MSMFB_SetDisplayMode; // possible to set this to NULL to indicate that we don't support changing modes
    device->PumpEvents = MSMFB_PumpEvents;

    /* Create- and Update- functions have to be implemented together */
    device->CreateWindowFramebuffer = MSMFB_CreateWindowFramebuffer;
    device->UpdateWindowFramebuffer = MSMFB_UpdateWindowFramebuffer;
    device->DestroyWindowFramebuffer = MSMFB_DestroyWindowFramebuffer;

    /* Allocate our SDL-driver private data section */
    device->driverdata = SDL_malloc(sizeof(struct MSMFB_VideoDriverData));
    if (!device->driverdata) {
        SDL_free(device);
        SDL_OutOfMemory();
        SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "MSMFB_CreateDevice: out of memory.");
        return 0;
    }

    SDL_memset(device->driverdata, 0, sizeof(struct MSMFB_VideoDriverData));

    SDL_LogInfo(SDL_LOG_CATEGORY_VIDEO, "MSMFB_CreateDevice: OK");
    return device;
}

VideoBootStrap MSMFB_bootstrap = {
    MSMFB_DRIVER_NAME, "MSM Framebuffer video driver",
    MSMFB_Available, MSMFB_CreateDevice
};


static int
MSMFB_VideoInit(_THIS)
{
    SDL_DisplayMode mode;
    const char *devfb0 = "/dev/fb0";
    struct MSMFB_VideoDriverData *driverdata = (struct MSMFB_VideoDriverData *)_this->driverdata;
    unsigned int pixel_format = 0;
    
    /* open framebuffer device, query for display modes and format */
    const char *env_devfb0 = SDL_getenv("SDL_MSMFB_FBDEVICE");
    if (env_devfb0) {
        devfb0 = env_devfb0;
    }

    SDL_LogDebug(SDL_LOG_CATEGORY_VIDEO, "MSMFB_VideoInit: trying fbdev: %s ...", devfb0);

    /* open framebuffe device */
    if (-1 == (driverdata->fb_fd = open(devfb0, O_RDWR /* O_WRONLY */))) {
        SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "MSMFB_VideoInit: error opening %s: %s", devfb0, strerror(errno));
        return -1;
    }

    /* get fixed info */
    if (-1 == ioctl(driverdata->fb_fd, FBIOGET_FSCREENINFO, &driverdata->fb_fix)) {
        SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "MSMFB_VideoInit: ioctl FBIOGET_FSCREENINFO: %s", strerror(errno));
        return -1;
    }

    /* get var info */
    if (-1 == ioctl(driverdata->fb_fd, FBIOGET_VSCREENINFO, &driverdata->fb_var)) {
        SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "MSMFB_VideoInit: ioctl FBIOGET_VSCREENINFO: %s", strerror(errno));
        return -1;
    }
    /* save original video mode data to restore it on close */
    SDL_memcpy(&driverdata->fb_var_orig, &driverdata->fb_var, sizeof(struct fb_var_screeninfo));

    SDL_LogDebug(SDL_LOG_CATEGORY_VIDEO, "MSMFB_VideoInit: Opened framebuffer name: %s", driverdata->fb_fix.id);

    if (driverdata->fb_fix.type != FB_TYPE_PACKED_PIXELS) {
        SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "MSMFB_VideoInit: can handle only packed pixel frame buffers!");
        return -1;
    }
    if (driverdata->fb_fix.visual != FB_VISUAL_TRUECOLOR) {
        SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "MSMFB_VideoInit: can handle only true color format!");
        return -1;
    }

    /* SDL2 ARGB, ABGR naming rules: (see SDL_pixels.h:75) */
    /** Packed component order, high bit -> low bit. */
    /* TODO: we can try to autodetect it from offsets in fb_var structure */
    pixel_format = SDL_PIXELFORMAT_ABGR8888;

    mode.format = pixel_format;
    mode.w = (int)driverdata->fb_var.xres;
    mode.h = (int)driverdata->fb_var.yres;
    mode.refresh_rate = 60;
    mode.driverdata = NULL;
    if (SDL_AddBasicVideoDisplay(&mode) < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "MSMFB_VideoInit: SDL_AddBasicVideoDisplay() failed!");
        return -1;
    }

    SDL_zero(mode); /* why??? */
    mode.format = pixel_format;
    mode.w = (int)driverdata->fb_var.xres;
    mode.h = (int)driverdata->fb_var.yres;
    mode.refresh_rate = 60;
    mode.driverdata = NULL;
    SDL_AddDisplayMode(&_this->displays[0], &mode);

    /* We're done! */
    SDL_LogDebug(SDL_LOG_CATEGORY_VIDEO, "MSMFB_VideoInit done.");
    return 0;
}

static int
MSMFB_SetDisplayMode(_THIS, SDL_VideoDisplay *display, SDL_DisplayMode *mode)
{
    (void)_this;
    (void)display;
    (void)mode;
    return 0;
}

static void
MSMFB_VideoQuit(_THIS)
{
    struct MSMFB_VideoDriverData *driverdata = (struct MSMFB_VideoDriverData *)_this->driverdata;

    /* restore original framebuffer video mode */
    if (-1 == ioctl(driverdata->fb_fd, FBIOPUT_VSCREENINFO, &driverdata->fb_var_orig)) {
        SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "MSMFB_VideoQuit: ioctl FBIOPUT_VSCREENINFO: %s", strerror(errno));
    }
    if (driverdata->fb_fd != 0) {
        close(driverdata->fb_fd);
    }
    driverdata->fb_fd = 0;
}

static void
MSMFB_PumpEvents(_THIS)
{
    /* do nothing. */
    (void)_this;
}

#define MSMFB_SURFACE   "_SDL_MSMFB_Surface"

static int
MSMFB_CreateWindowFramebuffer(_THIS, SDL_Window *window, Uint32 *format, void **pixels, int *pitch)
{
    struct MSMFB_VideoDriverData *driverdata = (struct MSMFB_VideoDriverData *)_this->driverdata;
    const Uint32 surface_format = SDL_PIXELFORMAT_ABGR8888; /* TODO: hardcoded pixel format */
    int page_mask = 0;
    (void)window; /* unused */

    page_mask = getpagesize() - 1;

    SDL_LogDebug(SDL_LOG_CATEGORY_VIDEO, "MSMFB_CreateWindowFramebuffer: page_mask = %08X", (unsigned int)page_mask);

    driverdata->fb_mem_offset = (int)driverdata->fb_fix.smem_start & page_mask;
    SDL_LogDebug(SDL_LOG_CATEGORY_VIDEO, "MSMFB_CreateWindowFramebuffer: fb_mem_offset = %08X", (unsigned int)driverdata->fb_mem_offset);

    if ((driverdata->fb_fix.smem_len < 1) || (driverdata->fb_fix.smem_start < 1)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO, "MSMFB_CreateWindowFramebuffer: Probably, "
                         "it does not make sense to map invalid memory! The errors will follow.");
    }

    driverdata->fb_mem = mmap(
                NULL,
                (size_t)((int)driverdata->fb_fix.smem_len + driverdata->fb_mem_offset),
                PROT_READ | PROT_WRITE,
                MAP_SHARED,
                driverdata->fb_fd,
                0);
    if (-1L == (long)driverdata->fb_mem) {
        SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "MSMFB_CreateWindowFramebuffer: Could not mmap framebufer to userspace: %s", strerror(errno));
        return -1;
    }

    SDL_LogDebug(SDL_LOG_CATEGORY_VIDEO, "MSMFB_CreateWindowFramebuffer: Mapped framebuffer mem to %0zX", (size_t)driverdata->fb_mem);

    /* move viewport to upper left corner */
    if (driverdata->fb_var.xoffset != 0 || driverdata->fb_var.yoffset != 0) {
        driverdata->fb_var.xoffset = 0;
        driverdata->fb_var.yoffset = 0;
        if (-1 == ioctl(driverdata->fb_fd, FBIOPAN_DISPLAY, &driverdata->fb_var)) {
            SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "MSMFB_CreateWindowFramebuffer: ioctl FBIOPAN_DISPLAY: %s",
                         strerror(errno));
            return -1;
        }
    }

    driverdata->fb_var.activate = FB_ACTIVATE_NOW | FB_ACTIVATE_ALL | FB_ACTIVATE_FORCE;
    if (ioctl(driverdata->fb_fd, FBIOPUT_VSCREENINFO, &driverdata->fb_var) < 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO, "MSMFB_CreateWindowFramebuffer: ioctl FBIOPUT_VSCREENINFO failed");
    } else {
        SDL_LogInfo(SDL_LOG_CATEGORY_VIDEO, "MSMFB_CreateWindowFramebuffer: Framebuffer created OK.");
    }

    /* return results */
    *format = surface_format;
    *pixels = driverdata->fb_mem;
    *pitch = (int)driverdata->fb_fix.line_length;

    return 0;
}

static int
MSMFB_UpdateWindowFramebuffer(_THIS, SDL_Window *window, const SDL_Rect *rects, int numrects)
{
    struct MSMFB_VideoDriverData *driverdata = (struct MSMFB_VideoDriverData *)_this->driverdata;
    int i;
    int x, y, w ,h;

    SDL_LogDebug(SDL_LOG_CATEGORY_VIDEO, "MSMFB_UpdateWindowFramebuffer: called");

    for (i = 0; i < numrects; ++i) {
        x = rects[i].x;
        y = rects[i].y;
        w = rects[i].w;
        h = rects[i].h;

        if (w <= 0 || h <= 0 || (x + w) <= 0 || (y + h) <= 0) {
            /* Clipped? */
            continue;
        }
        if (x < 0) {
            x += w;
            w += rects[i].x;
        }
        if (y < 0) {
            y += h;
            h += rects[i].y;
        }
        if (x + w > window->w) {
            w = window->w - x;
        }
        if (y + h > window->h) {
            h = window->h - y;
        }

        /* TODO: implement appropriate copy bits operation for framebuffer */
        /* this code copies ximage to xwindow */
        /* looks like we don't have to copy anything, just kick framebuffer to update itself */
        /* X11_XShmPutImage(display, data->xwindow, data->gc, data->ximage, x, y, x, y, w, h, False); */
    }

    /* Don't forget to commit changes to update actual display after pixels copying */
    msmfb_display_commit(driverdata->fb_fd);
    return 0;
}

static void
MSMFB_DestroyWindowFramebuffer(_THIS, SDL_Window * window)
{
    struct MSMFB_VideoDriverData *driverdata = (struct MSMFB_VideoDriverData *)_this->driverdata;
    (void)window; /* unused */

    if (driverdata->fb_mem != 0) {
        munmap(driverdata->fb_mem, (size_t)((int)driverdata->fb_fix.smem_len + driverdata->fb_mem_offset));
        driverdata->fb_mem = NULL;
        SDL_LogDebug(SDL_LOG_CATEGORY_VIDEO, "unmapped framebuffer mem\n");
    }
}

#endif /* SDL_VIDEO_DRIVER_DUMMY */

/* vi: set ts=4 sw=4 expandtab: */
