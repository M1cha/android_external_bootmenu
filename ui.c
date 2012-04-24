/*
 * Copyright (C) 2007 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <linux/input.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <sys/time.h>
#include <stdint.h> 
#include <math.h>

#include "common.h"
#include "minui/minui.h"
#include "bootmenu_ui.h"
#include "extendedcommands.h"

#ifndef MAX_ROWS
#define MAX_COLS 96
#define MAX_ROWS 40
#endif


static int SQUARE_WIDTH=0;
static int SQUARE_TOP=100;
static int SQUARE_RIGHT=20;
static int SQUARE_BOTTOM=0;
static int SQUARE_LEFT=20;

#define ROW_HEIGHT 100
#define STATUSBAR_HEIGHT 40
#define TABCONTROL_HEIGHT 90
static char** tabitems;
static int activeTab = 0;

static int square_inner_top;
static int square_inner_right;
static int square_inner_bottom;
static int square_inner_left;

static pthread_mutex_t gUpdateMutex = PTHREAD_MUTEX_INITIALIZER;
static gr_surface gBackgroundIcon[NUM_BACKGROUND_ICONS];
static gr_surface gProgressBarEmpty;
static gr_surface gProgressBarFill;

#define PROGRESSBAR_INDETERMINATE_STATES 1
#define PROGRESSBAR_INDETERMINATE_FPS 15
static gr_surface gProgressBarIndeterminate[PROGRESSBAR_INDETERMINATE_STATES];

static const struct { gr_surface* surface; const char *name; } BITMAPS[] = {
    { &gBackgroundIcon[BACKGROUND_DEFAULT], "background" },
    { &gBackgroundIcon[BACKGROUND_ALT], "background" },
    { &gProgressBarIndeterminate[0],    "indeterminate1" },
    { &gProgressBarEmpty,               "progress_empty" },
    { &gProgressBarFill,                "progress_fill" },
    { NULL,                             NULL },
};

static gr_surface gCurrentIcon = NULL;

static enum ProgressBarType {
    PROGRESSBAR_TYPE_NONE,
    PROGRESSBAR_TYPE_INDETERMINATE,
    PROGRESSBAR_TYPE_NORMAL,
} gProgressBarType = PROGRESSBAR_TYPE_NONE;

// Progress bar scope of current operation
static float gProgressScopeStart = 0, gProgressScopeSize = 0, gProgress = 0;
static time_t gProgressScopeTime, gProgressScopeDuration;

// Set to 1 when both graphics pages are the same (except for the progress bar)
static int gPagesIdentical = 0;

// Log text overlay, displayed when a magic key is pressed
static char text[MAX_ROWS][MAX_COLS];
static int text_cols = 0, text_rows = 0;
static int text_col = 0, text_row = 0, text_top = 0;
static int show_text = 0;

// Progression % used for battery level
static bool show_percent = true;
static float percent = 0.0;

static struct UiMenuItem *menu;
static int show_menu = 0;
static int menu_items = 0, menu_sel = 0;
static int menu_show_start = 0;             // this is line which menu display is starting at
static char menu_headers[MAX_ROWS][MAX_COLS];
static int menu_header_lines = 0;

// Key event input queue
static pthread_mutex_t key_queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t key_queue_cond = PTHREAD_COND_INITIALIZER;
static struct ui_input_event key_queue[256];
static int key_queue_len = 0;
static volatile char key_pressed[KEY_MAX + 1];
static int evt_enabled = 0;

// touch-pointer
static int pointerx_start = -1;
static int pointery_start = -1;
static int pointerx = -1;
static int pointery = -1;
static int pointer_start_insidemenu=0;
static struct timeval tvTouchStart;

// scrolling
#define BOUNCEBACK_TIME 200
static int menutop_diff = 0;
static int enable_scrolling = 0;

// bounce
static int enable_bounceback = 0;
static int bounceback_start = 0;
static struct timeval bounceback_start_time;
static int menuToptmp = 0;
static int bounceback_targetpos = 0;


static int show_menu_selection=0;

// Clear the screen and draw the currently selected background icon (if any).
// Should only be called with gUpdateMutex locked.
static void draw_background_locked(gr_surface icon)
{
    gPagesIdentical = 0;
    gr_color(0, 0, 0, 255);
    gr_fill(0, 0, gr_fb_width(), gr_fb_height());

    if (icon) {
        int iconWidth = gr_get_width(icon);
        int iconHeight = gr_get_height(icon);
        int iconX = (gr_fb_width() - iconWidth) / 2;
        int iconY = (gr_fb_height() - iconHeight) / 2;
        gr_blit(icon, 0, 0, iconWidth, iconHeight, iconX, iconY);
    }
}

// Draw the progress bar (if any) on the screen.  Does not flip pages.
// Should only be called with gUpdateMutex locked.
static void draw_progress_locked()
{
    if (gProgressBarType == PROGRESSBAR_TYPE_NONE) return;

    int iconHeight = gr_get_height(gBackgroundIcon[BACKGROUND_ALT]);
    int width = gr_get_width(gProgressBarEmpty);
    int height = gr_get_height(gProgressBarEmpty);

    int dx = (gr_fb_width() - width)/2;
    int dy = (3*gr_fb_height() + iconHeight - 2*height)/4 - 5;

    // Erase behind the progress bar (in case this was a progress-only update)
    gr_color(0, 0, 0, 255);
    gr_fill(dx, dy, width, height);

    if (gProgressBarType == PROGRESSBAR_TYPE_NORMAL) {
        float progress = gProgressScopeStart + gProgress * gProgressScopeSize;
        int pos = (int) (progress * width);

        if (pos > 0) {
          gr_blit(gProgressBarFill, 0, 0, pos, height, dx, dy);
        }
        if (pos < width-1) {
          gr_blit(gProgressBarEmpty, pos, 0, width-pos, height, dx+pos, dy);
        }

        if (pos > 0 && show_percent && percent > 0.0) {
          char pct[8];
          sprintf(pct, "%3.0f %%", percent * 100);
          gr_color(255, 255, 255, 255);
          gr_text(dx + 8, dy - 4, pct);
        }
    }

    if (gProgressBarType == PROGRESSBAR_TYPE_INDETERMINATE) {
        static int frame = 0;
        gr_blit(gProgressBarIndeterminate[frame], 0, 0, width, height, dx, dy);
        frame = (frame + 1) % PROGRESSBAR_INDETERMINATE_STATES;
    }
}

static void draw_menuitem_selection(int top, int height) {
  gr_fill(square_inner_left, top, square_inner_right, top+height);
}

static int get_menuitem_height(int item) {
	switch(menu[item].type) {
		case MENUITEM_SMALL:
			return 80;
			break;
		case MENUITEM_MINUI_STANDARD:
			return 18;
			break;
		case MENUITEM_FULL:
			return 100;
			break;
	}
	return 0;
}

static int draw_menu_item(int top, int item) {
  int height=get_menuitem_height(item);
  struct UiColor color_text;
  struct UiColor color_background;
  switch(menu[item].type) {
    
    case MENUITEM_SMALL:
    	color_text = gr_make_uicolor(255, 255, 255, 255);
    	color_background = gr_make_uicolor(0, 0, 0, 255);

		if(show_menu_selection==1 && menu_sel==item) {
			color_text = gr_make_uicolor(0, 0, 0, 255);
			color_background = gr_make_uicolor(255, 255, 255, 255);
			draw_menuitem_selection(top,height);
		}

		if(ui_inside_menuitem(item, pointerx_start, pointery_start)==1 && enable_scrolling==0) {
			color_text = gr_make_uicolor(0, 0, 0, 255);
			if(ui_inside_menuitem(item, pointerx, pointery)==1) {
				color_background = gr_make_uicolor(255, 183, 0, 255);
			}
			else {
				color_background = gr_make_uicolor(255, 205, 86, 255);
			}
		}

		// draw background
		int bgtop = top;
		int bgheight = height;
		int cuttop = top+height;
		int bgbottom = top+height;
		int draw_bottom_line=1;

		// don't draw if item is outside of list's viewport
		if(bgtop+height>=square_inner_top && bgbottom<=square_inner_bottom+height) {

			// cut background at square's top side
			if(bgtop<square_inner_top) {
				bgheight-= square_inner_top-bgtop;
				bgtop = square_inner_top;


			}


			// cut background and text at sqaure's bottom side
			if(bgbottom>square_inner_bottom) {
				bgheight-=bgbottom-square_inner_bottom;
				bgbottom=square_inner_bottom;
				draw_bottom_line=0;
			}

			// draw background
			gr_set_uicolor(color_background);
			draw_menuitem_selection(bgtop,bgheight);

			// draw text
			gr_setfont(FONT_BIG);
			gr_set_uicolor(color_text);
			gr_text_cut(square_inner_left, top+height-height/2+gr_getfont_cheight()/2-gr_getfont_cheightfix(), menu[item].title, square_inner_left, square_inner_right, square_inner_top, bgbottom);

			// draw bottom_line
			if(draw_bottom_line==1) {
				gr_color(164,164,164,255);
				gr_drawLine(square_inner_left, top+height-1, square_inner_right, top+height-1, 1);
			}
		}

    break;
    
      
    case MENUITEM_NULL:
    default:
      break;
  }

  return top+height;
}

static void draw_log_line(int row, const char* t) {
  if (t[0] != '\0') {
    gr_text(square_inner_left-3, square_inner_top+(square_inner_bottom-square_inner_top)+(row+1)*gr_getfont_cheight()-1, t);
  }
}

static int ui_get_menu_top() {
	return STATUSBAR_HEIGHT+TABCONTROL_HEIGHT+menutop_diff;
}
static int ui_get_menu_height() {
	int i;
	int height = 0;

	for (i=0; i < menu_items; ++i) {
		height+=get_menuitem_height(i);
	}

	return height;
}

// Redraw everything on the screen.  Does not flip pages.
// Should only be called with gUpdateMutex locked.
static void draw_screen_locked(void)
{
    int i;
    int marginTop = ui_get_menu_top();
    struct timeval bouncediff, tvNow;
    gettimeofday(&tvNow, NULL);


    if(enable_bounceback==1) {
    	timeval_subtract(&bouncediff, &tvNow, &bounceback_start_time);
    	int t0 = BOUNCEBACK_TIME*1000;
    	int t1 = t0- (bouncediff.tv_sec*1000000+bouncediff.tv_usec);
    	int way = abs(bounceback_start-bounceback_targetpos);
    	int pway = round((double)way/t0*t1);

    	if(pway<=0) {
    		enable_bounceback=0;
    		menutop_diff=bounceback_targetpos;
    	}
    	else {
    		if(bounceback_start>bounceback_targetpos) {
    			menutop_diff=bounceback_targetpos+pway;
    		}
    		else {
    			menutop_diff=bounceback_targetpos-pway;
    		}
    	}

    }

    draw_background_locked(gCurrentIcon);
    draw_progress_locked();

    if (show_text) {
		i = 0;
		if (show_menu) {
			// draw menu
			gr_setfont(FONT_BIG);

			for (; i < menu_items; ++i) {
				if (i == menu_sel) {
					// draw item
					gr_color(0, 0, 0, 255);
					marginTop = draw_menu_item(marginTop, i);

				} else {
					gr_color(255, 255, 255, 255);
					marginTop = draw_menu_item(marginTop, i);
				}
			}
			++i;
		}

		// draw log
		gr_setfont(FONT_NORMAL);
		gr_color(255, 255, 0, 255);
        for (i=0; i < text_rows; ++i) {
            draw_log_line(i, text[(i+text_top) % text_rows]);
        }
        
        // draw statusbar
        int statusbar_right = 10;
		gr_color(0, 0, 0, 160);
		gr_fill(0, 0, gr_fb_width(), STATUSBAR_HEIGHT);

		// draw clock
		char time[50];
		ui_get_time(time);
		gr_color(0, 170, 255, 255);
		gr_text(gr_fb_width()-5*gr_getfont_cwidth()-statusbar_right,gr_getfont_cheight()/2+STATUSBAR_HEIGHT/2-gr_getfont_cheightfix(),time);
		statusbar_right+=5*gr_getfont_cwidth();
	
#ifdef BOARD_WITH_CPCAP
		// draw battery
		int level = battery_level();
		char level_s[5];
		sprintf(level_s, "%d%%", level);

		// count size of string */
		int level_s_size;
		for(level_s_size=0; level_s[level_s_size]; ++level_s_size) {}

		gr_text(gr_fb_width()-level_s_size*gr_getfont_cwidth()-statusbar_right,gr_getfont_cheight()/2+STATUSBAR_HEIGHT/2-gr_getfont_cheightfix(),level_s);
#endif
	
		// draw tabcontrol
		int tableft=0;
		gr_color(0, 0, 0, 255);
		gr_fill(0, STATUSBAR_HEIGHT, gr_fb_width(), STATUSBAR_HEIGHT+TABCONTROL_HEIGHT);
		if(tabitems!=NULL) {
		  for(i=0; tabitems[i]; ++i) {
			  int active=0;
			  if(i==activeTab)active=1;
			  tableft = drawTab(tableft, tabitems[i], active);
		  }
		}
	
		// draw divider-line
		gr_color(0, 170, 255, 255);
		gr_drawLine(0, STATUSBAR_HEIGHT+TABCONTROL_HEIGHT, gr_fb_width(), STATUSBAR_HEIGHT+TABCONTROL_HEIGHT, 4);

		// DEBUG: Pointer-location
		gr_color(255, 0, 0, 255);
		gr_fill(pointerx, pointery, pointerx+10, pointery+10);
    }
}

static int drawTab(int left, const char* s, int active)
{
  int s_size;
  int width;
  
  // count size of string */
  for(s_size=0; s[s_size]; ++s_size) {}
  
  width = s_size*gr_getfont_cwidth()+40;
  
  // tab-background
  gr_color(0, 0, 0, 255);
  gr_fill(left, STATUSBAR_HEIGHT, left+width, STATUSBAR_HEIGHT+TABCONTROL_HEIGHT);
  
  // text
  gr_color(255, 255, 255, 255);
  gr_text(left+20, STATUSBAR_HEIGHT+gr_getfont_cheight()/2+TABCONTROL_HEIGHT/2-gr_getfont_cheightfix(), s);
  
  // active-marker
  if(active==1) {
    gr_color(0, 170, 255, 255);
    gr_fill(left, STATUSBAR_HEIGHT+TABCONTROL_HEIGHT-10, left+width, STATUSBAR_HEIGHT+TABCONTROL_HEIGHT);
  }
  
  return left+width;
}

static void recalcSquare()
{
    square_inner_top = ui_get_menu_top();
    square_inner_right = gr_fb_width()-SQUARE_RIGHT-SQUARE_WIDTH;
    square_inner_bottom = gr_fb_height()-SQUARE_BOTTOM-SQUARE_WIDTH;
    square_inner_left = SQUARE_LEFT+SQUARE_WIDTH;
}

// Redraw everything on the screen and flip the screen (make it visible).
// Should only be called with gUpdateMutex locked.
static void update_screen_locked(void)
{
    draw_screen_locked();
    gr_flip();
}

// Updates only the progress bar, if possible, otherwise redraws the screen.
// Should only be called with gUpdateMutex locked.
static void update_progress_locked(void)
{
    /*if (show_text || !gPagesIdentical) {
        draw_screen_locked();    // Must redraw the whole screen
        gPagesIdentical = 1;
    } else {
        draw_progress_locked();  // Draw only the progress bar
    }
    gr_flip();*/
}

// Keeps the progress bar updated, even when the process is otherwise busy.
static void *progress_thread(void *cookie)
{
    for (;;) {
        usleep(1000000 / PROGRESSBAR_INDETERMINATE_FPS);
        pthread_mutex_lock(&gUpdateMutex);

        // update the progress bar animation, if active
        // skip this if we have a text overlay (too expensive to update)
        if (gProgressBarType == PROGRESSBAR_TYPE_INDETERMINATE && !show_text) {
            update_progress_locked();
        }

        // move the progress bar forward on timed intervals, if configured
        int duration = gProgressScopeDuration;
        if (gProgressBarType == PROGRESSBAR_TYPE_NORMAL && duration > 0) {
            int elapsed = time(NULL) - gProgressScopeTime;
            float progress = 1.0 * elapsed / duration;
            if (progress > 1.0) progress = 1.0;
            if (progress > gProgress) {
                gProgress = progress;
                update_progress_locked();
            }
        }

        pthread_mutex_unlock(&gUpdateMutex);
    }
    return NULL;
}

// Reads input events, handles special hot keys, and adds to the key queue.
static void *input_thread(void *cookie)
{
    int rel_sum = 0;
    int fake_key = 0;
    int drag = 0;

    for (;;) {
        // wait for the next key event
        struct input_event ev;
        struct ui_input_event uev;
        int state = 0;

        do {
            ev_get(&ev, 0);
            uev.time = ev.time;
            uev.type = ev.type;
            uev.code = ev.code;
            uev.value = ev.value;
            uev.utype = UINPUTEVENT_TYPE_KEY;
            uev.posx = -1;
            uev.posy = -1;

            if (ev.type == EV_SYN) {
                continue;
            } else if (ev.type == EV_REL) {
                if (ev.code == REL_Y) {
                    // accumulate the up or down motion reported by
                    // the trackball.  When it exceeds a threshold
                    // (positive or negative), fake an up/down
                    // key event.
                    rel_sum += ev.value;
                    if (rel_sum > 3) {
                        fake_key = 1;
                        ev.type = EV_KEY;
                        ev.code = KEY_DOWN;
                        ev.value = 1;
                        rel_sum = 0;
                    } else if (rel_sum < -3) {
                        fake_key = 1;
                        ev.type = EV_KEY;
                        ev.code = KEY_UP;
                        ev.value = 1;
                        rel_sum = 0;
                    }
                }
            } else if (ev.type == EV_ABS) {
            	int x, y;

				uev.posx = x = ev.value >> 16;
				uev.posy = y = ev.value & 0xFFFF;

				if (ev.code == 0)
				{
					if (state == 0)
					{
						uev.utype = UINPUTEVENT_TYPE_TOUCH_RELEASE;
					}
					state = 0;
					drag = 0;
				}
				else
				{
					if (!drag)
					{
						uev.utype = UINPUTEVENT_TYPE_TOUCH_START;
						drag=1;
					}
					else
					{
						if (state == 0)
						{
							uev.utype = UINPUTEVENT_TYPE_TOUCH_DRAG;
						}
					}
				}
            }
            else {
                rel_sum = 0;
            }
        } while ((ev.type != EV_KEY && ev.type != EV_ABS) || ev.code > KEY_MAX);

        pthread_mutex_lock(&key_queue_mutex);
        if (!fake_key) {
            // our "fake" keys only report a key-down event (no
            // key-up), so don't record them in the key_pressed
            // table.
            key_pressed[ev.code] = ev.value;
        }
        fake_key = 0;
        const int queue_max = sizeof(key_queue) / sizeof(key_queue[0]);
        if (ev.value > 0 && key_queue_len < queue_max) {
            key_queue[key_queue_len++] = uev;
            pthread_cond_signal(&key_queue_cond);
        }
        pthread_mutex_unlock(&key_queue_mutex);

        if (ev.type!= EV_ABS && ev.value > 0 && device_toggle_display(key_pressed, ev.code)) {
            pthread_mutex_lock(&gUpdateMutex);
            show_text = !show_text;
            //update_screen_locked();
            pthread_mutex_unlock(&gUpdateMutex);
        }

        if (ev.value > 0 && device_reboot_now(key_pressed, ev.code)) {
            reboot(RB_AUTOBOOT);
        }
    }
    return NULL;
}

static void *redraw_thread(void *cookie)
{
    for (;;) {
    	usleep(1000000 / 60);
    	update_screen_locked();
    }
    return NULL;
}

int ui_create_bitmaps()
{
    int i, result=0;

    for (i = 0; BITMAPS[i].name != NULL; ++i) {
        result = res_create_surface(BITMAPS[i].name, BITMAPS[i].surface);
        if (result < 0) {
            if (result == -2) {
                LOGI("Bitmap %s missing header\n", BITMAPS[i].name);
            } else {
                LOGE("Missing bitmap %s\n(Code %d)\n", BITMAPS[i].name, result);
            }
            *BITMAPS[i].surface = NULL;
        }
    }
    return result;
}

void ui_init(void)
{
    gr_init();
    ev_init();
    recalcSquare();

    text_col = text_row = 0;
    text_rows = gr_fb_height() / ROW_HEIGHT;
    if (text_rows > MAX_ROWS) text_rows = MAX_ROWS;
    text_top = 1;

    text_cols = gr_fb_width() / gr_getfont_cwidth();
    if (text_cols > MAX_COLS - 1) text_cols = MAX_COLS - 1;

    ui_create_bitmaps();

    pthread_t t;
    pthread_create(&t, NULL, progress_thread, NULL);

    pthread_create(&t, NULL, input_thread, NULL);
    evt_enabled = 1;

    pthread_create(&t, NULL, redraw_thread, NULL);
}

void ui_free_bitmaps(void)
{
    int i;

    //free bitmaps
    for (i = 0; BITMAPS[i].name != NULL; ++i) {
        if (BITMAPS[i].surface != NULL) {

            ui_print("free bitmap %d @ %x\n", i, (unsigned) BITMAPS[i].surface);

            res_free_surface(BITMAPS[i].surface);
        }
    }
}


void evt_init(void)
{
    ev_init();

    if (!evt_enabled) {
       pthread_t t;
       pthread_create(&t, NULL, input_thread, NULL);
       evt_enabled = 1;
    }
}

void evt_exit(void)
{
    if (evt_enabled) {
      ev_exit();
    }
    evt_enabled = 0;
}

void ui_final(void)
{
    evt_exit();

    ui_show_text(0);
    gr_exit();

    //ui_free_bitmaps();
}

void ui_set_background(int icon)
{
    pthread_mutex_lock(&gUpdateMutex);
    gCurrentIcon = gBackgroundIcon[icon];
    pthread_mutex_unlock(&gUpdateMutex);
}

void ui_show_indeterminate_progress()
{
    pthread_mutex_lock(&gUpdateMutex);
    if (gProgressBarType != PROGRESSBAR_TYPE_INDETERMINATE) {
        gProgressBarType = PROGRESSBAR_TYPE_INDETERMINATE;
        update_progress_locked();
    }
    pthread_mutex_unlock(&gUpdateMutex);
}

void ui_show_progress(float portion, int seconds)
{
    pthread_mutex_lock(&gUpdateMutex);
    gProgressBarType = PROGRESSBAR_TYPE_NORMAL;
    gProgressScopeStart += gProgressScopeSize;
    gProgressScopeSize = portion;
    gProgressScopeTime = time(NULL);
    gProgressScopeDuration = seconds;
    gProgress = 0;
    percent = gProgressScopeStart;
    update_progress_locked();
    pthread_mutex_unlock(&gUpdateMutex);
}

void ui_set_progress(float fraction)
{
    pthread_mutex_lock(&gUpdateMutex);
    if (fraction < 0.0) fraction = 0.0;
    if (fraction > 1.0) fraction = 1.0;
    percent = gProgressScopeStart + (fraction * gProgressScopeSize);
    if (gProgressBarType == PROGRESSBAR_TYPE_NORMAL && fraction > gProgress) {
        // Skip updates that aren't visibly different.
        int width = gr_get_width(gProgressBarIndeterminate[0]);
        float scale = width * gProgressScopeSize;
        if ((int) (gProgress * scale) != (int) (fraction * scale)) {
            gProgress = fraction;
            update_progress_locked();
        }
    }
    pthread_mutex_unlock(&gUpdateMutex);
}

void ui_reset_progress()
{
    pthread_mutex_lock(&gUpdateMutex);
    gProgressBarType = PROGRESSBAR_TYPE_NONE;
    gProgressScopeStart = gProgressScopeSize = 0;
    gProgressScopeTime = gProgressScopeDuration = 0;
    gProgress = 0;
    percent = 0.0;
    pthread_mutex_unlock(&gUpdateMutex);
}

void ui_print_str(char *str) {
    char buf[256];

    strncpy(buf, str, 255);
    fputs(buf, stdout);

    // This can get called before ui_init(), so be careful.
    pthread_mutex_lock(&gUpdateMutex);
    if (text_rows > 0 && text_cols > 0) {
        char *ptr;
        for (ptr = buf; *ptr != '\0'; ++ptr) {
            if (*ptr == '\n' || text_col >= text_cols) {
                text[text_row][text_col] = '\0';
                text_col = 0;
                text_row = (text_row + 1) % text_rows;
                if (text_row == text_top) text_top = (text_top + 1) % text_rows;
            }
            if (*ptr != '\n') text[text_row][text_col++] = *ptr;
        }
        text[text_row][text_col] = '\0';
    }
    pthread_mutex_unlock(&gUpdateMutex);

}

void ui_print(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, 256, fmt, ap);
    va_end(ap);

    ui_print_str(buf);
}

void ui_start_menu(char** headers, char** tabs, struct UiMenuItem* items, int initial_selection) {
    int i;
    pthread_mutex_lock(&gUpdateMutex);
    
    if (text_rows > 0 && text_cols > 0) {
      
		tabitems=tabs;
		menu=items;
	
        for (i = 0; i < MAX_ROWS; ++i) {
            if (headers[i] == NULL) break;
            strncpy(menu_headers[i], headers[i], text_cols-1);
            menu_headers[i][text_cols-1] = '\0';
        }
        menu_header_lines = i;

		// count menuitems
        for (i = 0; i < MAX_ROWS; ++i) {
        	if (items[i].type == MENUITEM_NULL) break;
        }

        menu_items = i;
        show_menu = 1;
        menu_sel = initial_selection;
        menutop_diff=0;
    }
    pthread_mutex_unlock(&gUpdateMutex);
}

int ui_menu_select(int sel) {
    int old_sel;
    pthread_mutex_lock(&gUpdateMutex);
    if (show_menu > 0) {
        old_sel = menu_sel;
        menu_sel = sel;
        if (menu_sel < 0) menu_sel = menu_items + menu_sel;
        if (menu_sel >= menu_items) menu_sel = menu_sel - menu_items;

        if (menu_sel < menu_show_start && menu_show_start > 0) {
            menu_show_start = menu_sel;
        }

        if (menu_sel - menu_show_start >= text_rows) {
            menu_show_start = menu_sel - text_rows + 1;
        }

        sel = menu_sel;
    }
    pthread_mutex_unlock(&gUpdateMutex);
    fprintf(stdout, "selection: %d\n", sel);fflush(stdout);
    return sel;
}

void ui_end_menu() {
    int i;
    pthread_mutex_lock(&gUpdateMutex);
    if (show_menu > 0 && text_rows > 0 && text_cols > 0) {
        show_menu = 0;
    }
    pthread_mutex_unlock(&gUpdateMutex);
}

int ui_text_visible()
{
    pthread_mutex_lock(&gUpdateMutex);
    int visible = show_text;
    pthread_mutex_unlock(&gUpdateMutex);
    return visible;
}

void ui_show_text(int visible)
{
    pthread_mutex_lock(&gUpdateMutex);
    show_text = visible;
    pthread_mutex_unlock(&gUpdateMutex);
}

int ui_wait_key()
{
    int key;
    key = ui_wait_input().code;

    return key;
}

struct ui_input_event ui_wait_input()
{
	struct ui_input_event key;

    pthread_mutex_lock(&key_queue_mutex);
    while (key_queue_len == 0) {
        pthread_cond_wait(&key_queue_cond, &key_queue_mutex);
    }

    key = key_queue[0];
    memcpy(&key_queue[0], &key_queue[1], sizeof(struct ui_input_event) * --key_queue_len);
    pthread_mutex_unlock(&key_queue_mutex);

    return key;
}

int ui_key_pressed(int key)
{
    // This is a volatile static array, don't bother locking
    return key_pressed[key];
}

void ui_clear_key_queue() {
    pthread_mutex_lock(&key_queue_mutex);
    key_queue_len = 0;
    pthread_mutex_unlock(&key_queue_mutex);
}

void ui_get_time(char* result)
{
  time_t rawtime;
  struct tm * timeinfo;
  //char buffer [80];

  time ( &rawtime );
  timeinfo = localtime ( &rawtime );

  strftime (result,80,"%I:%M",timeinfo);
}

void ui_set_activeTab(int i)
{
  activeTab=i;
}

int ui_get_activeTab()
{
  return activeTab;
}

int ui_setTab_next() {
  int i;
  
  // count tabs
  for(i=0; tabitems[i]; i++){}
  
  // set next tab as active tab
  if(activeTab>=i-1) {
    activeTab=0;  
  }
  else {
    activeTab++;
  }
  
  return activeTab;
}

struct UiMenuItem buildMenuItem(int type, char *title, char *description) {
  struct UiMenuItem item = {type, title, description};
  return item;
}

int ui_inside_menuitem(int item, int x, int y) {
	int i;
	int top = ui_get_menu_top();

	// get top-position
	for(i=0; i<item; ++i) {
		top+=get_menuitem_height(i);
	}

	// the check itself
	if(x>=square_inner_left && x<=square_inner_right && y>=top && y<top+get_menuitem_height(item)) {
		return 1;
	}
	return 0;
}

/* Return 1 if the difference is negative, otherwise 0.  */
int timeval_subtract(struct timeval *result, struct timeval *t2, struct timeval *t1)
{
    long int diff = (t2->tv_usec + 1000000 * t2->tv_sec) - (t1->tv_usec + 1000000 * t1->tv_sec);
    result->tv_sec = diff / 1000000;
    result->tv_usec = diff % 1000000;

    return (diff<0);
}

struct ui_touchresult ui_handle_touch(struct ui_input_event uev) {
	int i;
	int clickedItem=-1;
	struct ui_touchresult ret = {TOUCHRESULT_TYPE_EMPTY,-1};
	struct timeval tvNow, tvDiff;

	switch(uev.utype) {
		case UINPUTEVENT_TYPE_TOUCH_START:

			if(enable_scrolling==1) break;

			// save start-time
			gettimeofday(&tvTouchStart, NULL);

			// check if touch was inside list
			pointer_start_insidemenu=0;
			if(uev.posx>=square_inner_left && uev.posx<=square_inner_right && uev.posy>=square_inner_top && uev.posy<=square_inner_bottom) {
				pointer_start_insidemenu = 1;
			}

			pointerx_start = pointerx = uev.posx;
			pointery_start = pointery = uev.posy;
			menuToptmp = menutop_diff;
			enable_scrolling=0;
		break;

		case UINPUTEVENT_TYPE_TOUCH_DRAG:

			// calculate difference to start-time
			gettimeofday(&tvNow, NULL);
			timeval_subtract(&tvDiff, &tvNow, &tvTouchStart);

			// enable scrolling on different conditions
			if(pointer_start_insidemenu==1 && enable_scrolling!=1 && tvDiff.tv_sec==0 && tvDiff.tv_usec<=300000 && abs(uev.posy-pointery_start)>=10) {
				enable_scrolling=1;
			}

			// scroll!! :D
			if(enable_scrolling==1) {
				menutop_diff=menuToptmp+uev.posy-pointery_start;
			}

			pointerx = uev.posx;
			pointery = uev.posy;
		break;

		case UINPUTEVENT_TYPE_TOUCH_RELEASE:
			// check onclick for listitem
			for(i=0; i<menu_items; ++i) {
				if(ui_inside_menuitem(i, pointerx_start, pointery_start)==1 && ui_inside_menuitem(i, uev.posx, uev.posy)==1 && enable_scrolling==0) {
					ret.type = TOUCHRESULT_TYPE_ONCLICK_LIST;
					ret.item = i;
					break;
				}
			}

			// enable bouncing if scrolling was enabled
			if(enable_scrolling==1){
				gettimeofday(&bounceback_start_time, NULL);
				bounceback_start=menutop_diff;

				if(menutop_diff>0) {
					bounceback_targetpos=0;
					enable_bounceback=1;
				}
				else if(menutop_diff<0 && square_inner_top+ui_get_menu_height()<square_inner_bottom) {
					bounceback_targetpos=0;
					enable_bounceback=1;
				}
				else if(menutop_diff<0 && square_inner_top+menutop_diff+ui_get_menu_height()<square_inner_bottom) {
					bounceback_targetpos=-(square_inner_top+ui_get_menu_height()-square_inner_bottom);
					enable_bounceback=1;
				}
			}

			pointerx_start = pointerx = -1;
			pointery_start = pointery = -1;
			enable_scrolling=0;
		break;
	}

	return ret;
}

void enableMenuSelection(int i) {
	show_menu_selection=i;
}
