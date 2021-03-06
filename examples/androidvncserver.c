/*
 * This example VNC server for Android is adopted from
 * http://code.google.com/p/android-vnc-server/ with some additional
 * fixes applied.
 *
 * To build, you'll need the Android Native Development Kit from
 * http://developer.android.com/sdk/ndk/.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2, or (at your option) any
 * later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * This project is an adaptation of the original fbvncserver for the iPAQ
 * and Zaurus.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

#include <sys/stat.h>
#include <sys/sysmacros.h>             /* For makedev() */

#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>

#include <assert.h>
#include <errno.h>

/* libvncserver */
#include "rfb/rfb.h"
#include "rfb/keysym.h"

/*****************************************************************************/
/* Key define from Android */
#define AKEYCODE_HOME        3
#define AKEYCODE_BACK        4
#define AKEYCODE_0           7
#define AKEYCODE_STAR        17
#define AKEYCODE_POUND       18
#define AKEYCODE_VOLUME_UP   24
#define AKEYCODE_VOLUME_DOWN 25
#define AKEYCODE_POWER       26
#define AKEYCODE_CAMERA      27
#define AKEYCODE_CLEAR       28
#define AKEYCODE_A           29
#define AKEYCODE_COMMA       55
#define AKEYCODE_PERIOD      56
#define AKEYCODE_SHIFT_LEFT  59
#define AKEYCODE_SHIFT_RIGHT 60
#define AKEYCODE_TAB         61
#define AKEYCODE_SPACE       62
#define AKEYCODE_ENTER       66
#define AKEYCODE_DEL         67
#define AKEYCODE_GRAVE       68
#define AKEYCODE_MINUS       69
#define AKEYCODE_EQUALS      70
#define AKEYCODE_LEFT_BRACKET  71
#define AKEYCODE_RIGHT_BRACKET 72
#define AKEYCODE_BACKSLASH   73
#define AKEYCODE_SEMICOLON   74
#define AKEYCODE_APOSTROPHE  75
#define AKEYCODE_SLASH       76
#define AKEYCODE_AT          77
#define AKEYCODE_MENU        82

/* Android does not use /dev/fb0. */
static char FB_DEVICE[256] = "/dev/graphics/fb0";
static char KBD_DEVICE[256] = "/dev/input/event9";
static char TOUCH_DEVICE[256] = "*";
static struct fb_var_screeninfo scrinfo;
static int fbfd = -1;
static int kbdfd = -1;
static int touchfd = -1;
static uint32_t touch_device_flags = 0;
static int global_tracking_id = 1;
static uint32_t touch_btn_state = 0;
struct timespec last_move_time;
int sample_interval_in_ms = 20;    // sample interval of touch press
enum {
	TOUCH_UP   = 0,
	TOUCH_DOWN =1,
	TOUCH_MOVE =2
};
static unsigned short int *fbmmap = MAP_FAILED;
static unsigned short int *vncbuf;
static unsigned short int *fbbuf;

/* Android already has 5900 bound natively. */
#define VNC_PORT 5901
static rfbScreenInfoPtr vncscr;

static int xmin, xmax;
static int ymin, ymax;

/* No idea, just copied from fbvncserver as part of the frame differerencing
 * algorithm.  I will probably be later rewriting all of this. */
static struct varblock_t
{
	int min_i;
	int min_j;
	int max_i;
	int max_j;
	int r_offset;
	int g_offset;
	int b_offset;
	int rfb_xres;
	int rfb_maxy;
} varblock;

/*****************************************************************************/

static void keyevent(rfbBool down, rfbKeySym key, rfbClientPtr cl);
static void ptrevent(int buttonMask, int x, int y, rfbClientPtr cl);

/*****************************************************************************/

static void init_fb(void)
{
	size_t pixels;
	size_t bytespp;

	if ((fbfd = open(FB_DEVICE, O_RDONLY)) == -1)
	{
		printf("cannot open fb device %s\n", FB_DEVICE);
		exit(EXIT_FAILURE);
	}

	if (ioctl(fbfd, FBIOGET_VSCREENINFO, &scrinfo) != 0)
	{
		printf("ioctl error\n");
		exit(EXIT_FAILURE);
	}

	pixels = scrinfo.xres * scrinfo.yres;
	bytespp = scrinfo.bits_per_pixel / 8;

	fprintf(stderr, "xres=%d, yres=%d, xresv=%d, yresv=%d, xoffs=%d, yoffs=%d, bpp=%d\n", 
	  (int)scrinfo.xres, (int)scrinfo.yres,
	  (int)scrinfo.xres_virtual, (int)scrinfo.yres_virtual,
	  (int)scrinfo.xoffset, (int)scrinfo.yoffset,
	  (int)scrinfo.bits_per_pixel);

	fbmmap = mmap(NULL, pixels * bytespp, PROT_READ, MAP_SHARED, fbfd, 0);

	if (fbmmap == MAP_FAILED)
	{
		printf("mmap failed\n");
		exit(EXIT_FAILURE);
	}
}

static void cleanup_fb(void)
{
	if(fbfd != -1)
	{
		close(fbfd);
	}
}

static void init_kbd()
{
	if((kbdfd = open(KBD_DEVICE, O_RDWR)) == -1)
	{
		printf("cannot open kbd device %s\n", KBD_DEVICE);
		exit(EXIT_FAILURE);
	}
}

static void cleanup_kbd()
{
	if(kbdfd != -1)
	{
		close(kbdfd);
	}
}

// code from https://github.com/wlach/orangutan orng.c
#define test_bit(bit, array)    (array[bit/8] & (1<<(bit%8)))
enum {
  /* The input device is a touchscreen or a touchpad (either single-touch or multi-touch). */
  INPUT_DEVICE_CLASS_TOUCH         = 0x00000004,

  /* The input device is a multi-touch touchscreen. */
  INPUT_DEVICE_CLASS_TOUCH_MT      = 0x00000010,

  /* The input device is a multi-touch touchscreen and needs MT_SYNC. */
  INPUT_DEVICE_CLASS_TOUCH_MT_SYNC = 0x00000200
};

static uint32_t figure_out_events_device_reports(int fd) {

  uint32_t device_classes = 0;

  uint8_t key_bitmask[KEY_CNT / 8 + !!(KEY_CNT % 8)];
  uint8_t abs_bitmask[ABS_CNT / 8 + !!(ABS_CNT % 8)];

  memset(key_bitmask, 0, sizeof(key_bitmask));
  memset(abs_bitmask, 0, sizeof(abs_bitmask));

  ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bitmask)), key_bitmask);
  ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs_bitmask)), abs_bitmask);

  // See if this is a touch pad.
  // Is this a new modern multi-touch driver?
  if (test_bit(ABS_MT_POSITION_X, abs_bitmask)
      && test_bit(ABS_MT_POSITION_Y, abs_bitmask)) {
    // Some joysticks such as the PS3 controller report axes that conflict
    // with the ABS_MT range.  Try to confirm that the device really is
    // a touch screen.
    // Mozilla Bug 741038 - support GB touchscreen drivers
    //if (test_bit(BTN_TOUCH, device->keyBitmask) || !haveGamepadButtons) {
    device_classes |= INPUT_DEVICE_CLASS_TOUCH | INPUT_DEVICE_CLASS_TOUCH_MT;
    char device_name[80];

    if(ioctl(fd, EVIOCGNAME(sizeof(device_name) - 1), &device_name) < 1) {
      //fprintf(stderr, "could not get device name for %s, %s\n", device, strerror(errno));
      device_name[0] = '\0';
    }

    // some touchscreen devices expect MT_SYN events to be sent after every
    // touch
    if(strcmp(device_name, "atmel-touchscreen") == 0 ||
       strcmp(device_name, "nvodm_touch") == 0 ||
       strcmp(device_name, "elan-touchscreen") == 0 ||
       strcmp(device_name, "ft5x06_ts") == 0) {
      device_classes |= INPUT_DEVICE_CLASS_TOUCH_MT_SYNC;
    }

  // Is this an old style single-touch driver?
  } else if ((test_bit(BTN_TOUCH, key_bitmask)
              && test_bit(ABS_X, abs_bitmask)
              && test_bit(ABS_Y, abs_bitmask))) {
    device_classes |= INPUT_DEVICE_CLASS_TOUCH;
  }

  return device_classes;
}

// probe code added by me
static void probe_touch(int find)
{
	struct input_absinfo info;
	char i = '0';
	uint32_t touch_type;

	strcpy(TOUCH_DEVICE, "/dev/input/event0");
	for (i='0'; i<='9'; i++) {
		TOUCH_DEVICE[16] = i;
    	if((touchfd = open(TOUCH_DEVICE, O_RDWR)) == -1)
			printf("Cannot open touch device %s\n", TOUCH_DEVICE);
		else {
			touch_type = figure_out_events_device_reports(touchfd);
			printf("Probe device %s: class=0x%x\n", TOUCH_DEVICE, touch_type);
			info.maximum = -1; //mark for invalid
    		if(ioctl(touchfd, EVIOCGABS(ABS_X), &info)) {
				printf("\tABS_X not supported! %s\n", strerror(errno));
				if(ioctl(touchfd, EVIOCGABS(ABS_MT_POSITION_X), &info)) {
					printf("\tABS_MT_POSTION_X not supported! %s\n", strerror(errno));
				}
			}
			if(info.maximum != -1)
				printf("\tx_min = %d, x_max = %d\n", info.minimum, info.maximum);

			info.maximum = -1; //mark for invalid
    		if(ioctl(touchfd, EVIOCGABS(ABS_Y), &info)) {
				printf("\tABS_Y not supported! %s\n", strerror(errno));
				if(ioctl(touchfd, EVIOCGABS(ABS_MT_POSITION_Y), &info)) {
					printf("\tABS_MT_POSTION_Y not supported! %s\n", strerror(errno));
				}
			}
			if(info.maximum != -1)
				printf("\ty_min = %d, y_max = %d\n", info.minimum, info.maximum);
			close(touchfd);
			if ((find==1) && (touch_type & INPUT_DEVICE_CLASS_TOUCH)) return; //stop searching
		}
	}
}

static void init_touch()
{
	struct input_absinfo info;
	if (TOUCH_DEVICE[0]=='*')
		probe_touch(1);  // auto detect
	if((touchfd = open(TOUCH_DEVICE, O_RDWR)) == -1)
	{
		printf("cannot open touch device %s\n", TOUCH_DEVICE);
		exit(EXIT_FAILURE);
	}
	touch_device_flags = figure_out_events_device_reports(touchfd);
	// Get the Range of X and Y
	if(ioctl(touchfd, EVIOCGABS(ABS_X), &info) && ioctl(touchfd, EVIOCGABS(ABS_MT_POSITION_X), &info)) {
		printf("cannot get ABS_X info, %s\n", strerror(errno));
		exit(EXIT_FAILURE);
	}
	xmin = info.minimum;
	xmax = info.maximum;
	if(ioctl(touchfd, EVIOCGABS(ABS_Y), &info) && ioctl(touchfd, EVIOCGABS(ABS_MT_POSITION_Y), &info)) {
		printf("cannot get ABS_Y, %s\n", strerror(errno));
		exit(EXIT_FAILURE);
	}
	ymin = info.minimum;
	ymax = info.maximum;
	clock_gettime(CLOCK_MONOTONIC, &last_move_time);
}

static void cleanup_touch()
{
	if(touchfd != -1)
	{
		close(touchfd);
	}
}

/*****************************************************************************/

static void init_fb_server(int argc, char **argv)
{
	printf("Initializing server...\n");

	/* Allocate the VNC server buffer to be managed (not manipulated) by 
	 * libvncserver. */
	vncbuf = calloc(scrinfo.xres * scrinfo.yres, scrinfo.bits_per_pixel / 8);
	assert(vncbuf != NULL);

	/* Allocate the comparison buffer for detecting drawing updates from frame
	 * to frame. */
	fbbuf = calloc(scrinfo.xres * scrinfo.yres, scrinfo.bits_per_pixel / 8);
	assert(fbbuf != NULL);

	/* TODO: This assumes scrinfo.bits_per_pixel is 16. */
	vncscr = rfbGetScreen(&argc, argv, scrinfo.xres, scrinfo.yres, 5, 2, (scrinfo.bits_per_pixel / 8));
	assert(vncscr != NULL);

	vncscr->desktopName = "Android";
	vncscr->frameBuffer = (char *)vncbuf;
	vncscr->alwaysShared = TRUE;
	vncscr->httpDir = NULL;
	vncscr->port = VNC_PORT;

	vncscr->kbdAddEvent = keyevent;
	vncscr->ptrAddEvent = ptrevent;

	rfbInitServer(vncscr);

	/* Mark as dirty since we haven't sent any updates at all yet. */
	rfbMarkRectAsModified(vncscr, 0, 0, scrinfo.xres, scrinfo.yres);

	/* No idea. */
	varblock.r_offset = scrinfo.red.offset + scrinfo.red.length - 5;
	varblock.g_offset = scrinfo.green.offset + scrinfo.green.length - 5;
	varblock.b_offset = scrinfo.blue.offset + scrinfo.blue.length - 5;
	varblock.rfb_xres = scrinfo.yres;
	varblock.rfb_maxy = scrinfo.xres - 1;
}

/*****************************************************************************/
void write_event(int fd, int type, int code, int value)
{
	struct input_event event;
	memset(&event, 0, sizeof(event));
	//gettimeofday(&event.time, 0);    // seems it is optional to set the time

	event.type = type;
	event.code = code;
	event.value = value;
	if(write(fd, &event, sizeof(event)) != sizeof(event))
	{
		printf("write event failed: %s\n", strerror(errno));
	}
}

void injectKeyEvent(uint16_t code, uint16_t value)
{
	/* inject KeyEvent via /dev/input/eventN does not work as expected. So use shell 'input keyevent' cmd to simulate key down
	write_event(kbdfd, EV_KEY, code, value);*/
	printf("injectKey (%d, %d)\n", code , value);
	if (value) {
		char buffer [32];
		sprintf(buffer, "input keyevent %d", code);
		system(buffer);
	}
}

static int keysym2scancode(rfbBool down, rfbKeySym key, rfbClientPtr cl)
{
	int scancode = 0;

	int code = (int)key;
	if (code>='0' && code<='9') {
		/*scancode = (code & 0xF) - 1;
		if (scancode<0) scancode += 10;
		scancode += KEY_1;*/
		scancode = (code & 0xF) + AKEYCODE_0;
	} else if (code>=0xFF50 && code<=0xFF58) {
		static const uint16_t map[] =
			{	KEY_HOME, KEY_LEFT, KEY_UP, KEY_RIGHT, KEY_DOWN,
				KEY_END, 0 };
		scancode = map[code & 0xF];
	} else if (code>=0xFFE1 && code<=0xFFEE) {
		static const uint16_t map[] =
			{	AKEYCODE_SHIFT_LEFT, AKEYCODE_SHIFT_LEFT,
				KEY_COMPOSE, KEY_COMPOSE,
				AKEYCODE_SHIFT_LEFT, AKEYCODE_SHIFT_LEFT,
				0,0,
				KEY_LEFTALT, KEY_RIGHTALT,
				0, 0, 0, 0 };
		scancode = map[code & 0xF];
	} else if ((code>='A' && code<='Z') || (code>='a' && code<='z')) {
		/*static const uint16_t map[] = {
				KEY_A, KEY_B, KEY_C, KEY_D, KEY_E,
				KEY_F, KEY_G, KEY_H, KEY_I, KEY_J,
				KEY_K, KEY_L, KEY_M, KEY_N, KEY_O,
				KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T,
				KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z };
		scancode = map[(code & 0x5F) - 'A'];*/
		if (code>='a') scancode = code - 'a' + AKEYCODE_A;  //cannot distingush lower case and capital case yet
		else  scancode = code - 'A' + AKEYCODE_A;
	} else {
		switch (code) {
			case 0x0020:    scancode = AKEYCODE_SPACE;  break;
			case 0x002C:    scancode = AKEYCODE_COMMA;  break;
			case 0x003C:    scancode = AKEYCODE_COMMA;  break;
			case 0x002E:    scancode = AKEYCODE_PERIOD; break;
			case 0x003E:    scancode = AKEYCODE_PERIOD; break;
			case 0x002F:    scancode = AKEYCODE_SLASH;  break;
			case 0x003F:    scancode = AKEYCODE_SLASH;  break;
			case 0x0032:    scancode = KEY_EMAIL;       break;
			case 0x0040:    scancode = KEY_EMAIL;       break;
			case 0xFF08:    scancode = AKEYCODE_DEL;    break;
			case 0xFF1B:    scancode = AKEYCODE_DEL;    break;
			case 0xFF09:    scancode = AKEYCODE_TAB;    break;
			case 0xFF0D:    scancode = AKEYCODE_ENTER;  break;
			case 0xFFBE:    scancode = KEY_F1;          break; // F1
			case 0xFFBF:    scancode = KEY_F2;          break; // F2
			case 0xFFC0:    scancode = KEY_F3;          break; // F3
			case 0xFFC1:    scancode = KEY_F4;          break; // F4
			case 0xFFC2:    scancode = AKEYCODE_HOME;   break; // F5 => HOME
			case 0xFFC3:    scancode = AKEYCODE_BACK;   break; // F6 => BACK
			case 0xFFC4:    scancode = AKEYCODE_MENU;   break; // F7 => MENU/UNLOCK
			case 0xFFC5:    scancode = AKEYCODE_ENTER;  break; // F8 => ENTER
			case 0xFFC6:    scancode = AKEYCODE_POWER;  break; // F9 => POWER
			case 0xFFC8:    rfbShutdownServer(cl->screen,TRUE);       break; // F11            
		}
	}

	return scancode;
}

static void keyevent(rfbBool down, rfbKeySym key, rfbClientPtr cl)
{
	int scancode;

	printf("Got keysym: %04x (down=%d)\n", (unsigned int)key, (int)down);

	if ((scancode = keysym2scancode(down, key, cl)))
	{
		injectKeyEvent(scancode, down);
	}
}

void injectTouchEvent(int action, int x, int y)   //action: 0-release, 1-down, 2-move
{
	// Calculate the final x and y
	/* Fake touch screen always reports zero */
	if (xmin != 0 && xmax != 0 && ymin != 0 && ymax != 0)
	{
		x = xmin + (x * (xmax - xmin)) / (scrinfo.xres);
		y = ymin + (y * (ymax - ymin)) / (scrinfo.yres);
	}

	if (touch_device_flags & INPUT_DEVICE_CLASS_TOUCH_MT) {
		if(action == TOUCH_UP)
			write_event(touchfd, EV_ABS, ABS_MT_TRACKING_ID, -1);
		else {
			if (action == TOUCH_DOWN)
				write_event(touchfd, EV_ABS, ABS_MT_TRACKING_ID, global_tracking_id++);
			write_event(touchfd, EV_ABS, ABS_MT_POSITION_X, x);
			write_event(touchfd, EV_ABS, ABS_MT_POSITION_Y, y);
			write_event(touchfd, EV_ABS, ABS_MT_PRESSURE, 127);
			write_event(touchfd, EV_ABS, ABS_MT_TOUCH_MAJOR, 127);
			write_event(touchfd, EV_ABS, ABS_MT_WIDTH_MAJOR, 4);  // optional?
		}
		if (touch_device_flags & INPUT_DEVICE_CLASS_TOUCH_MT_SYNC) 
			write_event(touchfd, EV_SYN, SYN_MT_REPORT, 0);
		if (action != TOUCH_MOVE)
			write_event(touchfd, EV_KEY, BTN_TOUCH, action);
		write_event(touchfd, EV_SYN, SYN_REPORT, 0);
	} else if (touch_device_flags & INPUT_DEVICE_CLASS_TOUCH) {
		if (action != TOUCH_MOVE)
			write_event(touchfd, EV_KEY, BTN_TOUCH, action);
		if(action != TOUCH_UP){
			write_event(touchfd, EV_ABS, ABS_X, x);
			write_event(touchfd, EV_ABS, ABS_Y, y);
		}
		write_event(touchfd, EV_SYN, SYN_REPORT, 0);
	}
	printf("injectTouchEvent (x=%d, y=%d, action=%d)\n", x , y, action);
}

static void ptrevent(int buttonMask, int x, int y, rfbClientPtr cl)
{
	/* Indicates either pointer movement or a pointer button press or release. The pointer is
	now at (x-position, y-position), and the current state of buttons 1 to 8 are represented
	by bits 0 to 7 of button-mask respectively, 0 meaning up, 1 meaning down (pressed).
	On a conventional mouse, buttons 1, 2 and 3 correspond to the left, middle and right
	buttons on the mouse. On a wheel mouse, each step of the wheel upwards is represented
	by a press and release of button 4, and each step downwards is represented by
	a press and release of button 5. 
	From (no longer available): http://www.vislab.usyd.edu.au/blogs/index.php/2009/05/22/an-headerless-indexed-protocol-for-input-1?blog=61 */
	
	//printf("Got ptrevent: %04x (x=%d, y=%d)\n", buttonMask, x, y);
	if (touch_btn_state != buttonMask) {
		injectTouchEvent(buttonMask & 1, x, y); // Simulate left mouse event as touch event
	} else if(buttonMask & 1) {  //only track motion if button is down; sample with interval to avoid sending flooding touch event
		struct timespec current_time;
		clock_gettime(CLOCK_MONOTONIC, &current_time);
		if((((current_time.tv_sec - last_move_time.tv_sec)*1000.0) + 1e-6 * (current_time.tv_nsec - last_move_time.tv_nsec)) > sample_interval_in_ms) {
			injectTouchEvent(TOUCH_MOVE, x, y);
			last_move_time = current_time;
		}
	}
	touch_btn_state = buttonMask;
}

#define PIXEL_FB_TO_RFB(p,r,g,b) ((p>>r)&0x1f001f)|(((p>>g)&0x1f001f)<<5)|(((p>>b)&0x1f001f)<<10)

static void update_screen(void)
{
	unsigned int *f, *c, *r;
	int x, y;

	varblock.min_i = varblock.min_j = 9999;
	varblock.max_i = varblock.max_j = -1;

	f = (unsigned int *)fbmmap;        /* -> framebuffer         */
	c = (unsigned int *)fbbuf;         /* -> compare framebuffer */
	r = (unsigned int *)vncbuf;        /* -> remote framebuffer  */

	for (y = 0; y < scrinfo.yres; y++)
	{
		/* Compare every 2 pixels at a time, assuming that changes are likely
		 * in pairs. */
		for (x = 0; x < scrinfo.xres; x += 2)
		{
			unsigned int pixel = *f;

			if (pixel != *c)
			{
				*c = pixel;

				/* XXX: Undo the checkered pattern to test the efficiency
				 * gain using hextile encoding. */
				if (pixel == 0x18e320e4 || pixel == 0x20e418e3)
					pixel = 0x18e318e3;

				*r = PIXEL_FB_TO_RFB(pixel,
				  varblock.r_offset, varblock.g_offset, varblock.b_offset);

				if (x < varblock.min_i)
					varblock.min_i = x;
				else
				{
					if (x > varblock.max_i)
						varblock.max_i = x;

					if (y > varblock.max_j)
						varblock.max_j = y;
					else if (y < varblock.min_j)
						varblock.min_j = y;
				}
			}

			f++, c++;
			r++;
		}
	}

	if (varblock.min_i < 9999)
	{
		if (varblock.max_i < 0)
			varblock.max_i = varblock.min_i;

		if (varblock.max_j < 0)
			varblock.max_j = varblock.min_j;

		fprintf(stderr, "Dirty page: %dx%d+%d+%d...\n",
			(varblock.max_i+2) - varblock.min_i, (varblock.max_j+1) - varblock.min_j,
			varblock.min_i, varblock.min_j);

		rfbMarkRectAsModified(vncscr, varblock.min_i, varblock.min_j,
			varblock.max_i + 2, varblock.max_j + 1);

		rfbProcessEvents(vncscr, 10000);
	}
}

/*****************************************************************************/

void print_usage(char **argv)
{
	printf("%s [-f device] [-k device] [-t device] [-p] [-P code] [-h]\n"
		"-f device: framebuffer device node, default is /dev/graphics/fb0\n"
		"-k device: keyboard device node, default is /dev/input/event9\n"
		"-t device: touch device node, default is /dev/input/event3\n"
		"-p : probe touch device with /dev/input/eventN, where N=0..9\n"
		"-P : send code to key device\n"
		"-h : print this help\n", argv[0]);
}

int main(int argc, char **argv)
{
	if(argc > 1)
	{
		int i=1;
		while(i < argc)
		{
			if(*argv[i] == '-')
			{
				switch(*(argv[i] + 1))
				{
					case 'h':
						print_usage(argv);
						exit(0);
						break;
					case 'f':
						i++;
						strcpy(FB_DEVICE, argv[i]);
						break;
					case 'k':
						i++;
						strcpy(KBD_DEVICE, argv[i]);
						break;
					case 't':
						i++;
						strcpy(TOUCH_DEVICE, argv[i]);
						break;
					case 'p':
						probe_touch(0);
						exit(0);
					case 'P':
					{
						i++;
						uint32_t code = atoi(argv[i]);
						printf("Sending code=%d to device %s ...\n", code, KBD_DEVICE);
						init_kbd();
						injectKeyEvent(code, 1);
						sleep(1);
						injectKeyEvent(code, 0);
						exit(0);
					}
				}
			}
			i++;
		}
	}

	printf("Initializing framebuffer device %s ...\n", FB_DEVICE);
	init_fb();
	/*printf("Initializing keyboard device %s ...\n", KBD_DEVICE);
	init_kbd();*/
	printf("Initializing touch device %s ...\n", TOUCH_DEVICE);
	init_touch();

	printf("Initializing VNC server:\n");
	printf("	width:  %d\n", (int)scrinfo.xres);
	printf("	height: %d\n", (int)scrinfo.yres);
	printf("	bpp:    %d\n", (int)scrinfo.bits_per_pixel);
	printf("	port:   %d\n", (int)VNC_PORT);
	init_fb_server(argc, argv);

	/* Implement our own event loop to detect changes in the framebuffer. */
	while (1)
	{
		while (vncscr->clientHead == NULL)
			rfbProcessEvents(vncscr, 100000);

		rfbProcessEvents(vncscr, 100000);
		update_screen();
	}

	printf("Cleaning up...\n");
	cleanup_fb();
	cleanup_kbd();
	cleanup_touch();
}
