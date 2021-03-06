/*
 * OpenBOR - http://www.chronocrash.com
 * -----------------------------------------------------------------------
 * All rights reserved, see LICENSE in OpenBOR root for details.
 *
 * Copyright (c) 2004 - 2014 OpenBOR Team
 */

#include "sdlport.h"


#include "SDL_framerate.h"

#include<SDL.h>
#include<SDL_dreamcast.h>
#include <math.h>
#include "types.h"
#include "video.h"
#include "vga.h"
#include "screen.h"
#include "sdlport.h"
//#include "opengl.h"
#include "openbor.h"
#include "gfxtypes.h"
#include "gfx.h"
#include "types.h"
#include "video.h"

#include "screen.h"
#include "openbor.h"
#include "filecache.h"

extern int videoModes;
extern int videoMode;


#include "pngdec.h"





FPSmanager framerate_manager;

s_videomodes stored_videomodes;
static SDL_Surface *screen = NULL;
static SDL_Surface *bscreen = NULL;
static SDL_Surface *bscreen2 = NULL;
static SDL_Color colors[256];
static int bytes_per_pixel = 1;
int stretch = 0;

int nativeWidth, nativeHeight; // monitor resolution used in fullscreen mode
u8 pDeltaBuffer[240 * 2592];

static void * memcpyvideo (void *dest, const void *src, size_t len)
{
  if(!len)
  {
    return dest;
  }

  const uint8_t *s = (uint8_t *)src;
  uint8_t *d = (uint8_t *)dest;

  uint32_t diff = (uint32_t)d - (uint32_t)(s + 1); // extra offset because input gets incremented before output is calculated
  // Underflow would be like adding a negative offset

  // Can use 'd' as a scratch reg now
  asm volatile (
    "clrs\n" // Align for parallelism (CO) - SH4a use "stc SR, Rn" instead with a dummy Rn
  ".align 2\n"
  "0:\n\t"
    "dt %[size]\n\t" // (--len) ? 0 -> T : 1 -> T (EX 1)
    "mov.b @%[in]+, %[scratch]\n\t" // scratch = *(s++) (LS 1/2)
    "bf.s 0b\n\t" // while(s != nexts) aka while(!T) (BR 1/2)
    " mov.b %[scratch], @(%[offset], %[in])\n" // *(datatype_of_s*) ((char*)s + diff) = scratch, where src + diff = dest (LS 1)
    : [in] "+&r" ((uint32_t)s), [scratch] "=&r" ((uint32_t)d), [size] "+&r" (len) // outputs
    : [offset] "z" (diff) // inputs
    : "t", "memory" // clobbers
  );

  return dest;
}

inline void * memsetvideo (void *dest, const uint8_t val, size_t len)
{
  if(!len)
  {
    return dest;
  }

  uint8_t * d = (uint8_t*)dest;
  uint8_t * nextd = d + len;

  asm volatile (
    "clrs\n\t" // Align for parallelism (CO) - SH4a use "stc SR, Rn" instead with a dummy Rn
    "dt %[size]\n" // Decrement and test size here once to prevent extra jump (EX 1)
  ".align 2\n"
  "1:\n\t"
    // *--nextd = val
    "mov.b %[in], @-%[out]\n\t" // (LS 1/1)
    "bf.s 1b\n\t" // (BR 1/2)
    " dt %[size]\n" // (--len) ? 0 -> T : 1 -> T (EX 1)
    : [out] "+r" ((uint32_t)nextd), [size] "+&r" (len) // outputs
    : [in] "r" (val) // inputs
    : "t", "memory" // clobbers
  );

  return dest;
}


SDL_Surface* getSDLScreen()
{
	return screen;
}
// SDL 1.2
void initSDL()
{
	SDL_DC_ShowAskHz(SDL_FALSE);
    SDL_DC_Default60Hz(SDL_FALSE);
    SDL_DC_VerticalWait(SDL_FALSE);
  //  SDL_DC_SetVideoDriver(SDL_DC_DIRECT_VIDEO);
 //   SDL_DC_SetVideoDriver(SDL_DC_TEXTURED_VIDEO);
   SDL_DC_SetVideoDriver(SDL_DC_DMA_VIDEO);
	const SDL_VideoInfo* video_info;
	int init_flags = SDL_INIT_VIDEO;
	SDL_Surface *icon;


	if(SDL_Init(init_flags) < 0)
	{
		printf("SDL Failed to Init!!!! (%s)\n", SDL_GetError());
		borExit(0);
	}
	SDL_ShowCursor(SDL_DISABLE);
	atexit(SDL_Quit);



	// Store the monitor's current resolution before setting the video mode for the first time
	video_info = SDL_GetVideoInfo();


	SDL_initFramerate(&framerate_manager);
	SDL_setFramerate(&framerate_manager, 200);
}




static unsigned masks[4][4] = {{0,0,0,0},{0x1F,0x07E0,0xF800,0},{0xFF,0xFF00,0xFF0000,0},{0xFF,0xFF00,0xFF0000,0}};

// Function to set the video mode on any SDL version (1.2 or 2.0)
SDL_Surface* SetVideoMode(int w, int h, int bpp, bool gl)
{

	return SDL_SetVideoMode(w, h, bpp, savedata.fullscreen?(SDL_FULLSCREEN|SDL_DOUBLEBUF|SDL_HWSURFACE):(SDL_FULLSCREEN|SDL_DOUBLEBUF|SDL_HWSURFACE));

}

int video_set_mode(s_videomodes videomodes)
{
	stored_videomodes = videomodes;

	if(screen) { SDL_FreeSurface(screen); screen=NULL; }
	if(bscreen) { SDL_FreeSurface(bscreen); bscreen=NULL; }
	if(bscreen2) { SDL_FreeSurface(bscreen2); bscreen2=NULL; }


	// FIXME: OpenGL surfaces aren't freed when switching from OpenGL to SDL

	bytes_per_pixel = videomodes.pixel;
	if(videomodes.hRes==0 && videomodes.vRes==0)
	{
		//Term_Gfx();
		return 0;
	}

	if(savedata.screen[videoMode][0])
	{
		screen = SetVideoMode(videomodes.hRes*savedata.screen[videoMode][0], videomodes.vRes*savedata.screen[videoMode][0], 16, false);
		SDL_ShowCursor(SDL_DISABLE);
		bscreen = SDL_CreateRGBSurface(SDL_HWSURFACE, videomodes.hRes, videomodes.vRes, 8*bytes_per_pixel, masks[bytes_per_pixel-1][0], masks[bytes_per_pixel-1][1], masks[bytes_per_pixel-1][2], masks[bytes_per_pixel-1][3]); // 24bit mask
		bscreen2 = SDL_CreateRGBSurface(SDL_HWSURFACE, videomodes.hRes+4, videomodes.vRes+8, 16, masks[1][2], masks[1][1], masks[1][0], masks[1][3]);
		//Init_Gfx(565, 16);
		memsetvideo(pDeltaBuffer, 0x00, 1244160);
		if(bscreen==NULL || bscreen2==NULL) return 0;
	}
	else
	{
		if(bytes_per_pixel>1)
		{
			bscreen = SDL_CreateRGBSurface(SDL_HWSURFACE, videomodes.hRes, videomodes.vRes, 8*bytes_per_pixel, masks[bytes_per_pixel-1][0], masks[bytes_per_pixel-1][1], masks[bytes_per_pixel-1][2], masks[bytes_per_pixel-1][3]); // 24bit mask
			if(!bscreen) return 0;
		}
		screen = SetVideoMode(videomodes.hRes, videomodes.vRes, 8*bytes_per_pixel, false);
		SDL_ShowCursor(SDL_DISABLE);
	}

	if(bytes_per_pixel==1)
	{

		SDL_SetColors(screen,colors,0,256);
		if(bscreen) SDL_SetColors(bscreen,colors,0,256);

	}

	if(screen==NULL) return 0;

	//printf("debug: screen->w:%d screen->h:%d   fullscreen:%d   depth:%d\n", screen->w, screen->h, savedata.fullscreen, screen->format->BitsPerPixel);
	video_clearscreen();
	return 1;
}

void video_fullscreen_flip()
{
	savedata.fullscreen ^= 1;

	video_set_mode(stored_videomodes);
}

//16bit, scale 2x 4x 8x ...
void _stretchblit(SDL_Surface *src, SDL_Rect *srcrect, SDL_Surface *dst, SDL_Rect *dstrect)
{
	SDL_Rect rect;
	int dst_x, dst_y, dst_w, dst_h, dst_row, src_row;
	int i;
	Uint16* psrc, *pdst;

	if(!srcrect)
	{
		rect.x = rect.y = 0;
		rect.w = src->w;
		rect.h = src->h;
		srcrect = &rect;
	}
	//dst_w = savedata.screen[s_videomodes][0] * srcrect->w;
	//dst_h = savedata.screen[s_videomodes][0] * srcrect->h;
	if(!dstrect)
	{
		dst_x = dst_y = 0;
		if(dst_w>dst->w) dst_w = dst->w;
		if(dst_h>dst->h) dst_h = dst->h;
	}
	else
	{
		dst_x = dstrect->x;
		dst_y = dstrect->y;
		if(dst_w>dstrect->w) dst_w = dstrect->w;
		if(dst_h>dstrect->h) dst_h = dstrect->h;
	}
	psrc = (Uint16*)src->pixels + srcrect->x + srcrect->y * src->pitch/2;
	pdst = (Uint16*)dst->pixels + dst_x + dst_y*dst->pitch/2;
	dst_row = dst->pitch/2;
	src_row = src->pitch/2;
	while(dst_h>0)
	{
		for(i=0; i<dst_w; i++)
		{
			*(pdst + i) = *(psrc+(i/savedata.screen[videoModes][0]));
		}

		for(i=1, pdst += dst_row; i<savedata.screen[videoModes][0] && dst_h; i++, dst_h--, pdst += dst_row)
		{
			memcpyvideo(pdst, pdst-dst_row, dst_w<<1);
		}
		dst_h--;
		psrc += src_row;
	}
}

int video_copy_screen(s_screen* src)
{
	unsigned char *sp;
	char *dp;
	int width, height, linew, slinew;
	int h;
	SDL_Surface* ds = NULL;
	SDL_Rect rectdes, rectsrc;


	width = screen->w;
	if(width > src->width) width = src->width;
	height = screen->h;
	if(height > src->height) height = src->height;
	if(!width || !height) return 0;
	h = height;

	if(bscreen)
	{
		rectdes.x = rectdes.y = 0;
		rectdes.w = width*savedata.screen[videoMode][0]; rectdes.h = height*savedata.screen[videoMode][0];
		if(bscreen2) {rectsrc.x = 2; rectsrc.y = 4;}
		else         {rectsrc.x = 0; rectsrc.y = 0;}
		rectsrc.w = width; rectsrc.h = height;
		if(SDL_MUSTLOCK(bscreen)) SDL_LockSurface(bscreen);
	}

	// Copy to linear video ram
	if(SDL_MUSTLOCK(screen)) SDL_LockSurface(screen);

	sp = (unsigned char*)src->data;
	ds = (bscreen?bscreen:screen);
	dp = ds->pixels;

	linew = width*bytes_per_pixel;
	slinew = src->width*bytes_per_pixel;

	do{
		memcpyvideo(dp, sp, linew);
		sp += slinew;
		dp += ds->pitch;
	}while(--h);

	if(SDL_MUSTLOCK(screen)) SDL_UnlockSurface(screen);

	if(bscreen)
	{
		//printf(" bscreen ");
		if(SDL_MUSTLOCK(bscreen)) SDL_UnlockSurface(bscreen);
		if(bscreen2)
		{
			SDL_BlitSurface(bscreen, NULL, bscreen2, &rectsrc);
			if(SDL_MUSTLOCK(bscreen2)) SDL_LockSurface(bscreen2);
			if(SDL_MUSTLOCK(screen)) SDL_LockSurface(screen);



			if(SDL_MUSTLOCK(bscreen2)) SDL_UnlockSurface(bscreen2);
			if(SDL_MUSTLOCK(screen)) SDL_UnlockSurface(screen);
		}
		else
		{
			rectdes.x=(screen->w-bscreen->w)/2;
			rectdes.y=(screen->h-bscreen->h)/2;
			SDL_BlitSurface(bscreen, NULL, screen, &rectdes);
		}
	}


	SDL_Flip(screen);



	return 1;
}

void video_clearscreen()
{


	if(SDL_MUSTLOCK(screen)) SDL_LockSurface(screen);
	sq_cpy(screen->pixels, 0, screen->pitch*screen->h);
	if(SDL_MUSTLOCK(screen)) SDL_UnlockSurface(screen);
	if(bscreen)
	{
		if(SDL_MUSTLOCK(bscreen)) SDL_LockSurface(bscreen);
		sq_cpy(bscreen->pixels, 0, bscreen->pitch*bscreen->h);
		if(SDL_MUSTLOCK(bscreen)) SDL_UnlockSurface(bscreen);
	}
}

void video_stretch(int enable)
{
	//if(screen || opengl) video_clearscreen();
	stretch = enable;
}

void vga_vwait(void)
{

	static int prevtick = 0;
	int now = SDL_GetTicks();
	int wait = 1000/60 - (now - prevtick);
	if (wait>0)
	{
		SDL_Delay(wait);
	}
	else SDL_Delay(1);
	prevtick = now;

}

void vga_setpalette(unsigned char* palette)
{
	int i;

	for(i=0;i<256;i++){
		colors[i].r=palette[0];
		colors[i].g=palette[1];
		colors[i].b=palette[2];
		palette+=3;
	}


		SDL_SetColors(screen,colors,0,256);
		if(bscreen) SDL_SetColors(bscreen,colors,0,256);

	{

	}


// TODO: give this function a boolean (int) return type denoting success/failure
void vga_set_color_correction(int gm, int br)
{

}
}

