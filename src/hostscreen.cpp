/*
	Hostscreen, base class
	Software renderer

	(C) 2007 ARAnyM developer team

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program; if not, write to the Free Software
	Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <SDL.h>

#include "dirty_rects.h"
#include "host_surface.h"
#include "logo.h"
#include "hostscreen.h"
#include "parameters.h"	/* bx_options */
#include "main.h"	/* QuitEmulator */

#ifdef NFVDI_SUPPORT
# include "nf_objs.h"
# include "nfvdi.h"
#endif

#ifdef SDL_GUI
# include "gui-sdl/sdlgui.h"
#endif

#define DEBUG 1
#include "debug.h"

HostScreen::HostScreen(void)
	: DirtyRects(), logo(NULL), logo_present(true), clear_screen(true),
	refreshCounter(0), screen(NULL), new_width(0), new_height(0),
	snapCounter(0)
{
}

HostScreen::~HostScreen(void)
{
	if (logo) {
		delete logo;
	}
}

void HostScreen::reset(void)
{
	lastVidelWidth = lastVidelHeight = lastVidelBpp = -1;
	setVidelRendering(true);
	DisableOpenGLVdi();

	setVideoMode(MIN_WIDTH,MIN_HEIGHT,8);

	/* Set window caption */
	char buf[sizeof(VERSION_STRING)+128];
#ifdef SDL_GUI
	char key[80];
	displayKeysym(bx_options.hotkeys.setup, key);
	snprintf(buf, sizeof(buf), "%s (press %s key for SETUP)", VERSION_STRING, key);
#else
	snprintf(buf, sizeof(buf), "%s", VERSION_STRING);
#endif /* SDL_GUI */
	SDL_WM_SetCaption(buf, "ARAnyM");
}

int HostScreen::getWidth(void)
{
	return screen->w;
}

int HostScreen::getHeight(void)
{
	return screen->h;
}

int HostScreen::getBpp(void)
{
	return screen->format->BitsPerPixel;
}

void HostScreen::makeSnapshot(void)
{
	char filename[15];
	sprintf( filename, "snap%03d.bmp", snapCounter++ );

	SDL_SaveBMP(screen, filename);
}

void HostScreen::toggleFullScreen(void)
{
	bx_options.video.fullscreen = !bx_options.video.fullscreen;

	setVideoMode(getWidth(), getHeight(), getBpp());
}

void HostScreen::setVideoMode(int width, int height, int bpp)
{
	if (bx_options.autozoom.fixedsize) {
		width = bx_options.autozoom.width;
		height = bx_options.autozoom.height;
	}
	if (width<MIN_WIDTH) {
		width=MIN_WIDTH;
	}
	if (height<MIN_HEIGHT) {
		height=MIN_HEIGHT;
	}

	int screenFlags = SDL_HWSURFACE|SDL_HWPALETTE|SDL_RESIZABLE;
	if (bx_options.video.fullscreen) {
		screenFlags |= SDL_FULLSCREEN;
	}

	screen = SDL_SetVideoMode(width, height, bpp, screenFlags);
	if (screen==NULL) {
		/* Try with default bpp */
		screen = SDL_SetVideoMode(width, height, 0, screenFlags);
	}
	if (screen==NULL) {
		/* Try with default resolution */
		screen = SDL_SetVideoMode(0, 0, 0, screenFlags);
	}
	if (screen==NULL) {
		panicbug(("Can not set video mode\n"));
		QuitEmulator();
		return;
	}

	SDL_SetClipRect(screen, NULL);

	bx_options.video.fullscreen = ((screen->flags & SDL_FULLSCREEN) == SDL_FULLSCREEN);

	new_width = screen->w;
	new_height = screen->h;
	resizeDirty(screen->w, screen->h);
	forceRefreshScreen();
}

void HostScreen::resizeWindow(int new_width, int new_height)
{
	this->new_width = new_width;
	this->new_height = new_height;
}

void HostScreen::EnableOpenGLVdi(void)
{
	OpenGLVdi = SDL_TRUE;
}

void HostScreen::DisableOpenGLVdi(void)
{
	OpenGLVdi = SDL_FALSE;
}

/*
 * this is called in VBL, i.e. 50 times per second
 */
void HostScreen::refresh(void)
{
	if (++refreshCounter < bx_options.video.refresh) {
		return;
	}

	refreshCounter = 0;

	initScreen();
	if (clear_screen || bx_options.opengl.enabled) {
		clearScreen();
		clear_screen = false;
	}

	/* Render videl surface ? */
	if (renderVidelSurface) {
		refreshVidel();
	} else {
		refreshNfvdi();
	}

#ifdef SDL_GUI
	if (!SDLGui_isClosed()) {
		refreshGui();
	}
#endif

	refreshScreen();

	if ((new_width!=screen->w) || (new_height!=screen->h)) {
		setVideoMode(new_width, new_height, getBpp());
	}
}

void HostScreen::setVidelRendering(bool videlRender)
{
	renderVidelSurface = videlRender;
}

void HostScreen::initScreen(void)
{
}

void HostScreen::clearScreen(void)
{
	SDL_FillRect(screen, NULL, 0);
}

void HostScreen::refreshVidel(void)
{
	HostSurface *videl_hsurf = getVIDEL()->getSurface();
	if (!videl_hsurf) {
		return;
	}

	SDL_Surface *videl_surf = videl_hsurf->getSdlSurface();

	/* Display logo if videl not ready */
	bool displayLogo = true;
	if (videl_surf) {
		if ((videl_surf->w > 64) && (videl_surf->h > 64)) {
			displayLogo = false;
		}
	}
	if (displayLogo) {
		refreshLogo();
		return;
	}

	if (!videl_surf) {
		return;
	}

	int w = (videl_surf->w < 320) ? 320 : videl_surf->w;
	int h = (videl_surf->h < 200) ? 200 : videl_surf->h;
	int bpp = videl_surf->format->BitsPerPixel;
	if ((w!=lastVidelWidth) || (h!=lastVidelHeight) || (bpp!=lastVidelBpp)) {
		setVideoMode(w, h, bpp);
		lastVidelWidth = w;
		lastVidelHeight = h;
		lastVidelBpp = bpp;
	}

	/* Set palette from videl surface if needed */
	if (!bx_options.opengl.enabled && (bpp==8) && (getBpp() == 8)) {
		SDL_Color palette[256];
		for (int i=0; i<256; i++) {
			palette[i].r = videl_surf->format->palette->colors[i].r;
			palette[i].g = videl_surf->format->palette->colors[i].g;
			palette[i].b = videl_surf->format->palette->colors[i].b;
		}
		SDL_SetPalette(screen, SDL_LOGPAL|SDL_PHYSPAL, palette, 0,256);
	}

	int flags = DRAW_CROPPED;
	if (bx_options.opengl.enabled && bx_options.autozoom.enabled) {
		flags = DRAW_RESCALED;
	}
	drawSurfaceToScreen(videl_hsurf, NULL, NULL, flags);
}

void HostScreen::refreshLogo(void)
{
	if (!logo_present) {
		return;
	}
	if (!logo) {
		logo = new Logo(bx_options.logo_path);
		if (!logo) {
			return;
		}
	}

	HostSurface *logo_hsurf = logo->getSurface();
	if (!logo_hsurf) {
		logo->load(bx_options.logo_path);
		logo_hsurf = logo->getSurface();
		if (!logo_hsurf) {
			fprintf(stderr, "Can not load logo from %s file\n",
				bx_options.logo_path); 
			logo_present = false;
			return;
		}
	}

	SDL_Surface *logo_surf = logo_hsurf->getSdlSurface();
	if (!logo_surf) {
		return;
	}

	int logo_width = logo_hsurf->getWidth();
	int logo_height = logo_hsurf->getHeight();

	int w = (logo_width < 320) ? 320 : logo_width;
	int h = (logo_height < 200) ? 200 : logo_height;
	int bpp = logo_hsurf->getBpp();
	if ((w!=lastVidelWidth) || (h!=lastVidelHeight) || (bpp!=lastVidelBpp)) {
		setVideoMode(w, h, bpp);
		lastVidelWidth = w;
		lastVidelHeight = h;
		lastVidelBpp = bpp;
	}

	/* Set palette from surface */
	if (!bx_options.opengl.enabled && (bpp==8) && (getBpp() == 8)) {
		SDL_Color palette[256];
		for (int i=0; i<256; i++) {
			palette[i].r = logo_surf->format->palette->colors[i].r;
			palette[i].g = logo_surf->format->palette->colors[i].g;
			palette[i].b = logo_surf->format->palette->colors[i].b;
		}
		SDL_SetPalette(screen, SDL_LOGPAL|SDL_PHYSPAL, palette, 0,256);
	}

	drawSurfaceToScreen(logo_hsurf);
}

void HostScreen::forceRefreshNfvdi(void)
{
#ifdef NFVDI_SUPPORT
	/* Force nfvdi surface refresh */
	NF_Base* fvdi = NFGetDriver("fVDI");
	if (!fvdi) {
		return;
	}

	HostSurface *nfvdi_hsurf = ((VdiDriver *) fvdi)->getSurface();
	if (!nfvdi_hsurf) {
		return;
	}

	nfvdi_hsurf->setDirtyRect(0,0,
		nfvdi_hsurf->getWidth(), nfvdi_hsurf->getHeight());
#endif
}

void HostScreen::refreshNfvdi(void)
{
#ifdef NFVDI_SUPPORT
	NF_Base* fvdi = NFGetDriver("fVDI");
	if (!fvdi) {
		return;
	}

	HostSurface *nfvdi_hsurf = ((VdiDriver *) fvdi)->getSurface();
	if (!nfvdi_hsurf) {
		return;
	}
	SDL_Surface *nfvdi_surf = nfvdi_hsurf->getSdlSurface();
	if (!nfvdi_surf) {
		return;
	}

	int vdi_width = nfvdi_hsurf->getWidth();
	int vdi_height = nfvdi_hsurf->getHeight();

	int w = (vdi_width < 320) ? 320 : vdi_width;
	int h = (vdi_height < 200) ? 200 : vdi_height;
	int bpp = nfvdi_hsurf->getBpp();
	if ((w!=lastVidelWidth) || (h!=lastVidelHeight) || (bpp!=lastVidelBpp)) {
		setVideoMode(w, h, bpp);
		lastVidelWidth = w;
		lastVidelHeight = h;
		lastVidelBpp = bpp;
	}

	/* Set palette from videl surface if needed */
	if (!bx_options.opengl.enabled && (bpp==8) && (getBpp() == 8)) {
		SDL_Color palette[256];
		for (int i=0; i<256; i++) {
			palette[i].r = nfvdi_surf->format->palette->colors[i].r;
			palette[i].g = nfvdi_surf->format->palette->colors[i].g;
			palette[i].b = nfvdi_surf->format->palette->colors[i].b;
		}
		SDL_SetPalette(screen, SDL_LOGPAL|SDL_PHYSPAL, palette, 0,256);
	}

	drawSurfaceToScreen(nfvdi_hsurf);
#endif
}

void HostScreen::refreshGui(void)
{
#ifdef SDL_GUI
	int gui_x, gui_y;

	drawSurfaceToScreen(SDLGui_getSurface(), &gui_x, &gui_y);

	SDLGui_setGuiPos(gui_x, gui_y);
#endif /* SDL_GUI */
}

void HostScreen::drawSurfaceToScreen(HostSurface *hsurf, int *dst_x, int *dst_y, int /*flags*/)
{
	if (!hsurf) {
		return;
	}
	hsurf->update();

	SDL_Surface *sdl_surf = hsurf->getSdlSurface();
	if (!sdl_surf) {
		return;
	}

	int width = hsurf->getWidth();
	int height = hsurf->getHeight();

	SDL_Rect src_rect = {0,0, width, height};
	SDL_Rect dst_rect = {0,0, screen->w, screen->h};
	if (screen->w > width) {
		dst_rect.x = (screen->w - width) >> 1;
		dst_rect.w = width;
	} else {
		src_rect.w = screen->w;
	}
	if (screen->h > height) {
		dst_rect.y = (screen->h - height) >> 1;
		dst_rect.h = height;
	} else {
		src_rect.h = screen->h;
	}

	Uint8 *dirtyRects = hsurf->getDirtyRects();
	if (!dirtyRects) {
		SDL_BlitSurface(sdl_surf, &src_rect, screen, &dst_rect);

		setDirtyRect(dst_rect.x,dst_rect.y,dst_rect.w,dst_rect.h);
	} else {
		int dirty_w = hsurf->getDirtyWidth();
		int dirty_h = hsurf->getDirtyHeight();
		for (int y=0; y<dirty_h; y++) {
			for (int x=0; x<dirty_w; x++) {
				if (dirtyRects[y * dirty_w + x]) {
					SDL_Rect src, dst;

					src.x = src_rect.x + (x<<4);
					src.y = src_rect.y + (y<<4);
					src.w = (1<<4);
					src.h = (1<<4);

					dst.x = dst_rect.x + (x<<4);
					dst.y = dst_rect.y + (y<<4);
					dst.w = (1<<4);
					dst.h = (1<<4);

					SDL_BlitSurface(sdl_surf, &src, screen, &dst);

					setDirtyRect(dst.x,dst.y,dst.w,dst.h);
				}
			}
		}

		hsurf->clearDirtyRects();
	}

	/* GUI need to know where it is */
	if (dst_x) {
		*dst_x = dst_rect.x;
	}
	if (dst_y) {
		*dst_y = dst_rect.y;
	}
}

void HostScreen::refreshScreen(void)
{
	if ((screen->flags & SDL_DOUBLEBUF)==SDL_DOUBLEBUF) {
		SDL_Flip(screen);
		return;
	}

	if (!dirtyMarker) {
		return;
	}

	/* Only update dirtied rects */
	SDL_Rect update_rects[dirtyW*dirtyH];
	int i = 0;
	for (int y=0; y<dirtyH; y++) {
		for (int x=0; x<dirtyW; x++) {
			if (dirtyMarker[y * dirtyW + x]) {
				int maxw = 1<<4, maxh = 1<<4;
				if (screen->w - (x<<4) < (1<<4)) {
					maxw = screen->w - (x<<4);
				}
				if (screen->h - (y<<4) < (1<<4)) {
					maxh = screen->h - (y<<4);
				}

				update_rects[i].x = x<<4;
				update_rects[i].y = y<<4;
				update_rects[i].w = maxw;
				update_rects[i].h = maxh;

				i++;
			}
		}
	}

	SDL_UpdateRects(screen,i,update_rects);

	clearDirtyRects();
}

void HostScreen::forceRefreshScreen(void)
{
	clear_screen = true;
	forceRefreshNfvdi();
	if (screen) {
		setDirtyRect(0,0, screen->w, screen->h);
	}
}

HostSurface *HostScreen::createSurface(int width, int height, int bpp)
{
	return new HostSurface(width, height, bpp);
}

HostSurface *HostScreen::createSurface(SDL_Surface *sdl_surf)
{
	return new HostSurface(sdl_surf);
}

/*
vim:ts=4:sw=4:
*/
