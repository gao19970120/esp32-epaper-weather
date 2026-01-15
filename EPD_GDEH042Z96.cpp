#include "EPD_GDEH042Z96.h"
#include <stdio.h>

#define Debug(__info,...) printf("Debug: " __info,##__VA_ARGS__)

/******************************************************************************
function :	Software reset
parameter:
******************************************************************************/
static void EPD_GDEH042Z96_Reset(void)
{
    DEV_Digital_Write(EPD_RST_PIN, 1);
    DEV_Delay_ms(200);
    DEV_Digital_Write(EPD_RST_PIN, 0);
    DEV_Delay_ms(2);
    DEV_Digital_Write(EPD_RST_PIN, 1);
    DEV_Delay_ms(200);
}

/******************************************************************************
function :	send command
parameter:
     Reg : Command register
******************************************************************************/
static void EPD_GDEH042Z96_SendCommand(UBYTE Reg)
{
    DEV_Digital_Write(EPD_DC_PIN, 0);
    DEV_Digital_Write(EPD_CS_PIN, 0);
    DEV_SPI_WriteByte(Reg);
    DEV_Digital_Write(EPD_CS_PIN, 1);
}

/******************************************************************************
function :	send data
parameter:
    Data : Write data
******************************************************************************/
static void EPD_GDEH042Z96_SendData(UBYTE Data)
{
    DEV_Digital_Write(EPD_DC_PIN, 1);
    DEV_Digital_Write(EPD_CS_PIN, 0);
    DEV_SPI_WriteByte(Data);
    DEV_Digital_Write(EPD_CS_PIN, 1);
}

/******************************************************************************
function :	Wait until the busy_pin goes LOW
parameter:
******************************************************************************/
static void EPD_GDEH042Z96_ReadBusy(void)
{
    Debug("e-Paper busy\r\n");
    while(DEV_Digital_Read(EPD_BUSY_PIN) == 1) {      //LOW: idle, HIGH: busy
        DEV_Delay_ms(10);
    }
    Debug("e-Paper busy release\r\n");
}

static void EPD_GDEH042Z96_SetWindows(UWORD Xstart, UWORD Ystart, UWORD Xend, UWORD Yend)
{
    EPD_GDEH042Z96_SendCommand(0x44); // SET_RAM_X_ADDRESS_START_END_POSITION
    EPD_GDEH042Z96_SendData((Xstart>>3) & 0xFF);
    EPD_GDEH042Z96_SendData((Xend>>3) & 0xFF);
	
    EPD_GDEH042Z96_SendCommand(0x45); // SET_RAM_Y_ADDRESS_START_END_POSITION
    EPD_GDEH042Z96_SendData(Ystart & 0xFF);
    EPD_GDEH042Z96_SendData((Ystart >> 8) & 0xFF);
    EPD_GDEH042Z96_SendData(Yend & 0xFF);
    EPD_GDEH042Z96_SendData((Yend >> 8) & 0xFF);
}

static void EPD_GDEH042Z96_SetCursor(UWORD Xstart, UWORD Ystart)
{
    EPD_GDEH042Z96_SendCommand(0x4E); // SET_RAM_X_ADDRESS_COUNTER
    EPD_GDEH042Z96_SendData((Xstart>>3) & 0xFF);

    EPD_GDEH042Z96_SendCommand(0x4F); // SET_RAM_Y_ADDRESS_COUNTER
    EPD_GDEH042Z96_SendData(Ystart & 0xFF);
    EPD_GDEH042Z96_SendData((Ystart >> 8) & 0xFF);
}

/******************************************************************************
function :	Initialize the e-Paper register
parameter:
******************************************************************************/
void EPD_GDEH042Z96_Init(void)
{
    EPD_GDEH042Z96_Reset();
    EPD_GDEH042Z96_ReadBusy();

    EPD_GDEH042Z96_SendCommand(0x12); // SWRESET
    EPD_GDEH042Z96_ReadBusy();

    // Waveshare V2 style initialization (Minimal)
    // Removed GDEH specific voltage/waveform settings to avoid red flash.
    
    EPD_GDEH042Z96_SendCommand(0x11); // Data entry mode
    EPD_GDEH042Z96_SendData(0x03);    // X+ Y+

    // Set Windows (0-399, 0-299)
    EPD_GDEH042Z96_SetWindows(0, 0, EPD_GDEH042Z96_WIDTH-1, EPD_GDEH042Z96_HEIGHT-1);
    EPD_GDEH042Z96_SetCursor(0, 0);

    EPD_GDEH042Z96_ReadBusy();
}

/******************************************************************************
function :	Clear screen
parameter:
******************************************************************************/
void EPD_GDEH042Z96_Clear(void)
{
    UWORD Width = (EPD_GDEH042Z96_WIDTH % 8 == 0)? (EPD_GDEH042Z96_WIDTH / 8 ): (EPD_GDEH042Z96_WIDTH / 8 + 1);
    UWORD Height = EPD_GDEH042Z96_HEIGHT;

    // Black/White RAM (0x24) -> 0xFF (White)
    EPD_GDEH042Z96_SendCommand(0x24);
    for (UWORD j = 0; j < Height; j++) {
        for (UWORD i = 0; i < Width; i++) {
            EPD_GDEH042Z96_SendData(0xFF);
        }
    }

    // Red RAM (0x26) -> 0x00 (No Red)
    EPD_GDEH042Z96_SendCommand(0x26);
    for (UWORD j = 0; j < Height; j++) {
        for (UWORD i = 0; i < Width; i++) {
            EPD_GDEH042Z96_SendData(0x00);
        }
    }

    EPD_GDEH042Z96_SendCommand(0x22);
    EPD_GDEH042Z96_SendData(0xF7); // Use 0xF7 (Waveshare style) instead of 0xC7 to try to match refresh effect
    EPD_GDEH042Z96_SendCommand(0x20);
    EPD_GDEH042Z96_ReadBusy();
}

/******************************************************************************
function :	Sends the image buffer in RAM to e-Paper and displays
parameter:
******************************************************************************/
void EPD_GDEH042Z96_Display(const UBYTE *blackimage, const UBYTE *redimage)
{
    UWORD Width = (EPD_GDEH042Z96_WIDTH % 8 == 0)? (EPD_GDEH042Z96_WIDTH / 8 ): (EPD_GDEH042Z96_WIDTH / 8 + 1);
    UWORD Height = EPD_GDEH042Z96_HEIGHT;

    // Write Black Image
    EPD_GDEH042Z96_SendCommand(0x24);
    for (UWORD j = 0; j < Height; j++) {
        for (UWORD i = 0; i < Width; i++) {
            if (blackimage != NULL) {
                EPD_GDEH042Z96_SendData(blackimage[i + j * Width]);
            } else {
                EPD_GDEH042Z96_SendData(0xFF); // Default White
            }
        }
    }

    // Write Red Image
    EPD_GDEH042Z96_SendCommand(0x26);
    for (UWORD j = 0; j < Height; j++) {
        for (UWORD i = 0; i < Width; i++) {
            if (redimage != NULL) {
                EPD_GDEH042Z96_SendData(~redimage[i + j * Width]);
            } else {
                EPD_GDEH042Z96_SendData(0x00); // Default No Red
            }
        }
    }

    EPD_GDEH042Z96_SendCommand(0x22);
    EPD_GDEH042Z96_SendData(0xF7);
    EPD_GDEH042Z96_SendCommand(0x20);
    EPD_GDEH042Z96_ReadBusy();
}

void EPD_GDEH042Z96_Display_Partial(const UBYTE *blackimage)
{
    // Try to replicate Waveshare V2 Partial Display sequence EXACTLY
    
    // 1. Set Border to 0x80 (Waveshare does this for partial)
    EPD_GDEH042Z96_SendCommand(0x3C); 
    EPD_GDEH042Z96_SendData(0x80);

    // 2. Display Update Control 1 (0x21) -> 0x00, 0x00
    // Waveshare partial logic sets this.
    EPD_GDEH042Z96_SendCommand(0x21); 
    EPD_GDEH042Z96_SendData(0x00);
    EPD_GDEH042Z96_SendData(0x00);

    // 3. Set Windows & Cursor (Full Screen for now)
    EPD_GDEH042Z96_SetWindows(0, 0, EPD_GDEH042Z96_WIDTH-1, EPD_GDEH042Z96_HEIGHT-1);
    EPD_GDEH042Z96_SetCursor(0, 0);

    // 4. Write Black Image (0x24)
    UWORD Width = (EPD_GDEH042Z96_WIDTH % 8 == 0)? (EPD_GDEH042Z96_WIDTH / 8 ): (EPD_GDEH042Z96_WIDTH / 8 + 1);
    UWORD Height = EPD_GDEH042Z96_HEIGHT;

    EPD_GDEH042Z96_SendCommand(0x24);
    for (UWORD j = 0; j < Height; j++) {
        for (UWORD i = 0; i < Width; i++) {
            if (blackimage != NULL) {
                EPD_GDEH042Z96_SendData(blackimage[i + j * Width]);
            } else {
                EPD_GDEH042Z96_SendData(0xFF);
            }
        }
    }

    // 5. Update using 0xFF (Waveshare style)
    EPD_GDEH042Z96_SendCommand(0x22);
    EPD_GDEH042Z96_SendData(0xFF); 
    EPD_GDEH042Z96_SendCommand(0x20);
    EPD_GDEH042Z96_ReadBusy();
}

/******************************************************************************
function :	Enter sleep mode
parameter:
******************************************************************************/
void EPD_GDEH042Z96_Sleep(void)
{
    EPD_GDEH042Z96_SendCommand(0x10);
    EPD_GDEH042Z96_SendData(0x01);
}
