#ifndef __EPD_GDEH042Z96_H_
#define __EPD_GDEH042Z96_H_

#include "DEV_Config.h"

// Display resolution
#define EPD_GDEH042Z96_WIDTH       400
#define EPD_GDEH042Z96_HEIGHT      300

void EPD_GDEH042Z96_Init(void);
void EPD_GDEH042Z96_Clear(void);
void EPD_GDEH042Z96_Display(const UBYTE *blackimage, const UBYTE *redimage);
void EPD_GDEH042Z96_Display_Partial(const UBYTE *blackimage); // New Partial Refresh
void EPD_GDEH042Z96_Sleep(void);

#endif
