/*
 * fb_test.c
 *
 *  Edited based on https://stackoverflow.com/questions/22909849/writing-to-framebuffer-directly-on-android
 *
 */

#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <sys/mman.h>
#include <stdlib.h>
#include "yellow_face.zif"
int main()
{
  int fbfd = 0;
  struct fb_var_screeninfo vinfo;
  struct fb_fix_screeninfo finfo;
  struct fb_cmap cmapinfo;
  long int screensize = 0;
  char *fbp = 0;
  int x = 0, y = 0;
  long int location = 0;
  int b,g,r;
  int n;

  // Open the file for reading and writing
  fbfd = open("/dev/graphics/fb0", O_RDWR,0);           // 打开Frame Buffer设备
  if (fbfd < 0) {
    printf("Error: cannot open framebuffer device.%x\n",fbfd);
    exit(1);
  }
  printf("The framebuffer device was opened successfully.\n");

  // Get fixed screen information
  if (ioctl(fbfd, FBIOGET_FSCREENINFO, &finfo)) {       // 获取设备固有信息
    printf("Error reading fixed information.\n");
    exit(2);
  }
  printf("\ntype:0x%x\n", finfo.type );                 // FrameBuffer 类型,如0为象素
  printf("visual:%d\n", finfo.visual );                 // 视觉类型：如真彩2，伪彩3 FB_VISUAL_TRUECOLOR FB_VISUAL_DIRECTCOLOR
  printf("line_length:%d\n", finfo.line_length );       // 每行长度 in bytes. For 7027A: type:0x0, visual:2, line_length:960
  printf("\nsmem_start:0x%lx, smem_len:%u\n", finfo.smem_start, finfo.smem_len ); // 映象RAM的参数, Start of frame buffer mem (physical address), Length of frame buffer mem
  printf("mmio_start:0x%lx, mmio_len:%u\n", finfo.mmio_start, finfo.mmio_len );

  // Get variable screen information
  if (ioctl(fbfd, FBIOGET_VSCREENINFO, &vinfo)) {       // 获取设备可变信息
    printf("Error reading variable information.\n");
    exit(3);
  }
  printf("%dx%d, %dbpp, xres_virtual=%d, yres_virtual=%d, vinfo.xoffset=%d, vinfo.yoffset=%d\n", vinfo.xres, vinfo.yres, vinfo.bits_per_pixel,vinfo.xres_virtual,vinfo.yres_virtual,vinfo.xoffset,vinfo.yoffset);
  //7027A output: 480x800, 16bpp, xres_virtual=480, yres_virtual=2406, vinfo.xoffset=0, vinfo.yoffset=0
  screensize = finfo.line_length * vinfo.yres_virtual;  //MTP8960: 2432 * 3072 = 7471104
  // Map the device to memory 通过mmap系统调用将framebuffer内存映射到用户空间,并返回映射后的起始地址
  /* mmap the framebuffer into our address space */
  fbp = (char*) mmap(0, /* start */
    screensize, //same as finfo.smem_len? /* bytes */
    PROT_READ | PROT_WRITE, /* prot */
    MAP_SHARED, /* flags */
    fbfd, /* fd */
    0 /* offset */);
  if ((int)fbp == -1) {
    printf("Error: failed to map framebuffer device to memory.\n");
    exit(4);
  }
  printf("The framebuffer device was mapped to memory successfully.\n");

//For Android test, the system will refresh the FB. So has to use another n loop to repeat update the FB
/***************exampel 1**********************/
  b = 10;
  g = 100;
  r = 100;
  for (n=0; n<16; n++)
    for ( y = 40; y < 340; y++ )
      for ( x = 40; x < 420; x++ ) {
        location = x * (vinfo.bits_per_pixel/8) + y * finfo.line_length;
        if ( vinfo.bits_per_pixel == 32 ) {
          *(fbp + location) = b;          // Some blue
          *(fbp + location + 1) = g;      // A little green
          *(fbp + location + 2) = r;      // A lot of red
          *(fbp + location + 3) = 0;      // No transparency
        } else if ( vinfo.bits_per_pixel == 16 ) {
          *(fbp + location) = x/2;
          *(fbp + location + 1) = y/2;
        }
      }
/*****************exampel 1********************/
  sleep(1);
/*****************exampel 2********************/
 	unsigned char *pTemp = (unsigned char *)fbp;
	int i, j;
  // yellow face is 128*128 RGB565
	//起始坐标(x,y),终点坐标(right,bottom)
	x = 100;
	y = 100;
	int right = 400;//vinfo.xres;
	int bottom = 600;//vinfo.yres;

	for (n=0; n<16; n++)
    for(i=y; i< bottom; i++)
    {
      for(j=x; j<right; j++)
      {
        unsigned short data = yellow_face_data[(((i-y)  % 128) * 128) + ((j-x) %128)];
        if ( vinfo.bits_per_pixel == 32 ) {
          pTemp[i*finfo.line_length + (j*4) + 2] = (unsigned char)((data & 0xF800) >> 11 << 3);
          pTemp[i*finfo.line_length + (j*4) + 1] = (unsigned char)((data & 0x7E0) >> 5 << 2);
          pTemp[i*finfo.line_length + (j*4) + 0] = (unsigned char)((data & 0x1F) << 3);
        } else if ( vinfo.bits_per_pixel == 16 ) {
          pTemp[i*finfo.line_length + (j*2)] = (unsigned char)(data & 0xFF);
          pTemp[i*finfo.line_length + (j*2) + 1] = (unsigned char)(data >> 8);
        }
      }
    }
/*****************exampel 2********************/
//note：vinfo.xoffset =0 vinfo.yoffset =0 否则FBIOPAN_DISPLAY不成功
	if (ioctl(fbfd, FBIOPAN_DISPLAY, &vinfo)) {
		printf("Error FBIOPAN_DISPLAY information.\n");
		exit(5);
  }
	//memset(fbp, 128, FixedInfo.smem_len);
	sleep(1);
	munmap(fbp,finfo.smem_len);//finfo.smem_len == screensize == finfo.line_length * vinfo.yres_virtual
  close(fbfd);
  return 0;
}
