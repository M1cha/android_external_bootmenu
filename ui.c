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

#include "common.h"
#include "minui/minui.h"
#include "bootmenu_ui.h"
#include "extendedcommands.h"

#ifndef MAX_ROWS
#define MAX_COLS 96
#define MAX_ROWS 40
#endif


static int SQUARE_WIDTH=5;
static int SQUARE_TOP=100;
static int SQUARE_RIGHT=20;
static int SQUARE_BOTTOM=20;
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
static int key_queue[256], key_queue_len = 0;
static volatile char key_pressed[KEY_MAX + 1];
static int evt_enabled = 0;

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

static int draw_menu_item(int top, int item) {
  int height=0;
  fprintf(stdout, "type[%d]=%d;\n", item, menu[item].type);fflush(stdout);
  switch(menu[item].type) {
    
    case MENUITEM_SMALL:
      height=80;
      
      // draw selection 
      if(menu_sel==item) {
		gr_color(255, 255, 255, 255);
		draw_menuitem_selection(top,height);
		gr_color(0, 0, 0, 255);
      }
      else {
    	gr_color(255, 255, 255, 255);
      }
      gr_setfont(FONT_BIG);
      gr_text_cut(square_inner_left, top+height-height/2+gr_getfont_cheight()/2-gr_getfont_cheightfix(), menu[item].title, square_inner_right,top+height);
      gr_color(164,164,164,255);
      gr_drawLine(square_inner_left, top+height, square_inner_right, top+height, 1);
    break;
    
    case MENUITEM_MINUI_STANDARD:
      height=18;
      
      // draw selection 
      if(menu_sel==item) {
	gr_color(64, 96, 255, 255);
	draw_menuitem_selection(top,height);
	gr_color(0, 0, 0, 255);
      }
      else {
	gr_color(64, 96, 255, 255);
      }
      gr_setfont(FONT_NORMAL);
      gr_text_cut(square_inner_left, top+height-height/2+gr_getfont_cheight()/2-gr_getfont_cheightfix(), menu[item].title, square_inner_right,top+height);
    break;
    
    case MENUITEM_FULL:
      height=100;
      
      // draw selection 
      if(menu_sel==item) {
	gr_color(255, 255, 255, 255);
	draw_menuitem_selection(top,height);
	gr_color(0, 0, 0, 255);
      }
      else {
	gr_color(255, 255, 255, 255);
      }
      gr_setfont(FONT_NORMAL);
      
      int comboheight = gr_getfont_cheight()*2;
      int combomiddle = top+height-height/2+gr_getfont_cheight()/2-gr_getfont_cheightfix();
      int combotop = top+(combomiddle-top);
      
      gr_text_cut(square_inner_left, combotop+height, menu[item].title, -1,-1);
      
      /*if(menu[item].description!=NULL) {
	comboheight*=2;
	//gr_setfont(FONT_NORMAL);
	//gr_text_cut(square_inner_left, top+height-height/2+gr_getfont_cheight()/2-gr_getfont_cheightfix(), menu[item].description, square_inner_right,top+height);
      }*/
      
      
      
      gr_color(164,164,164,255);
      gr_drawLine(square_inner_left, top+height, square_inner_right, top+height, 1);
    break;
      
    case MENUITEM_NULL:
    default:
      break;
  }

  return top+height;
}

/*static void draw_header_line(int row, const char* t) {
  if (t[0] != '\0') {
    gr_text(square_inner_left-3, 20+(row+1)*gr_getfont_cheight()-1, t);
  }
}*/

static void draw_log_line(int row, const char* t) {
  if (t[0] != '\0') {
    gr_text(square_inner_left-3, square_inner_top+(square_inner_bottom-square_inner_top)+(row+1)*gr_getfont_cheight()-1, t);
  }
}

// Redraw everything on the screen.  Does not flip pages.
// Should only be called with gUpdateMutex locked.
static void draw_screen_locked(void)
{
    int i;
    draw_background_locked(gCurrentIcon);
    draw_progress_locked();
    int marginTop = STATUSBAR_HEIGHT+TABCONTROL_HEIGHT;
    
    // draw transparent overlay
    /*if(show_text) {
      gr_color(0, 0, 0, 160);
      gr_fill(0, 0, gr_fb_width(), gr_fb_height());
    }*/
    
    // draw headers
    gr_setfont(FONT_NORMAL);
    /*gr_color(255, 255, 255, 255);
    for (i=0; i < menu_header_lines; ++i) {
      draw_header_line(i, menu_headers[i]);
    }*/
   
    if (show_text) {
        i = 0;
        if (show_menu) {
	    

	    // draw menu
	    gr_setfont(FONT_BIG);
	    fprintf(stdout, "\n\n", marginTop);fflush(stdout);
            for (; i < menu_items; ++i) {
            	//fprintf(stdout, "marginTop=%d\n", marginTop);fflush(stdout);
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
    square_inner_top = SQUARE_TOP+SQUARE_WIDTH-2;
    square_inner_right = gr_fb_width()-SQUARE_RIGHT-SQUARE_WIDTH;
    square_inner_bottom = SQUARE_BOTTOM;
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
    if (show_text || !gPagesIdentical) {
        draw_screen_locked();    // Must redraw the whole screen
        gPagesIdentical = 1;
    } else {
        draw_progress_locked();  // Draw only the progress bar
    }
    gr_flip();
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
    for (;;) {
        // wait for the next key event
        struct input_event ev;
        do {
            ev_get(&ev, 0);

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
            } else {
                rel_sum = 0;
            }
        } while (ev.type != EV_KEY || ev.code > KEY_MAX);

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
            key_queue[key_queue_len++] = ev.code;
            pthread_cond_signal(&key_queue_cond);
        }
        pthread_mutex_unlock(&key_queue_mutex);

        if (ev.value > 0 && device_toggle_display(key_pressed, ev.code)) {
            pthread_mutex_lock(&gUpdateMutex);
            show_text = !show_text;
            update_screen_locked();
            pthread_mutex_unlock(&gUpdateMutex);
        }

        if (ev.value > 0 && device_reboot_now(key_pressed, ev.code)) {
            reboot(RB_AUTOBOOT);
        }
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
    update_screen_locked();
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
    update_screen_locked();
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
        update_screen_locked();
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
        update_screen_locked();
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
        if (menu_sel != old_sel) update_screen_locked();
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
        update_screen_locked();
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
    update_screen_locked();
    pthread_mutex_unlock(&gUpdateMutex);
}

int ui_wait_key()
{
    int key;

    pthread_mutex_lock(&key_queue_mutex);
    while (key_queue_len == 0) {
        pthread_cond_wait(&key_queue_cond, &key_queue_mutex);
    }

    key = key_queue[0];
    memcpy(&key_queue[0], &key_queue[1], sizeof(int) * --key_queue_len);
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
  
  // update screen
  pthread_mutex_lock(&gUpdateMutex);
  update_screen_locked();
  pthread_mutex_unlock(&gUpdateMutex);
  
  return activeTab;
}

struct UiMenuItem buildMenuItem(int type, char *title, char *description) {
  struct UiMenuItem item = {type, title, description};
  return item;
}
