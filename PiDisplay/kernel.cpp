//
// kernel.cpp — Kaypro Video Display (Circle bare metal, Pi Zero 2W)
//
// GPIO interrupts for instant encoder response.
// Direct framebuffer writes for fast rendering.
//
// Wiring:
//   GPIO15 (pin 10) UART0 RX → Capture Pico GP12 TX
//   GPIO23 (pin 16) CLK      → Encoder CLK
//   GPIO24 (pin 18) DT       → Encoder DT
//   GPIO25 (pin 22) SW       → Encoder push button
//   GND    (pin 20)          → Encoder GND + Capture Pico GND
//   3.3V   (pin 17)          → Encoder VCC
//

#include <circle/actled.h>
#include <circle/koptions.h>
#include <circle/devicenameservice.h>
#include <circle/screen.h>
#include <circle/bcmframebuffer.h>
#include <circle/exceptionhandler.h>
#include <circle/interrupt.h>
#include <circle/timer.h>
#include <circle/logger.h>
#include <circle/serial.h>
#include <circle/gpiopin.h>
#include <circle/gpiomanager.h>
#include <circle/string.h>
#include <circle/types.h>
#include <circle/util.h>
#include <fatfs/ff.h>

// ── Geometry ──────────────────────────────────────────────────────────────────
#define BYTES_PER_LINE      80
#define LINES_PER_FRAME     400
#define FRAME_BYTES         (BYTES_PER_LINE * LINES_PER_FRAME)
#define DISPLAY_W           640
#define DISPLAY_H           480

// ── Noise filter ──────────────────────────────────────────────────────────────
// A pixel bit is only shown as ON if it was ON in both current AND previous
// frame. This eliminates single-frame noise spikes at the cost of 1 frame lag.
// Set to 0 to disable (faster but noisier)
#define NOISE_FILTER        1

// Status bars
#define TOP_BAR_Y           0
#define TOP_BAR_H           16
#define BOT_BAR_H           16
#define BOT_BAR_Y           (DISPLAY_H - BOT_BAR_H)
#define VIDEO_Y             (TOP_BAR_H + (DISPLAY_H - TOP_BAR_H - BOT_BAR_H - LINES_PER_FRAME)/2)

// ── Encoder / button ──────────────────────────────────────────────────────────
#define PIN_ENC_CLK         23
#define PIN_ENC_DT          24
#define PIN_ENC_SW          25
#define SW_DEBOUNCE_US      50000u

// ── SD card ───────────────────────────────────────────────────────────────────
#define CFG_FILE            "SD:/kaypro.cfg"
#define PAL_FILE            "SD:/kaypro.pal"

// ── Color modes ───────────────────────────────────────────────────────────────
#define NUM_MODES 5
enum TColorMode { ColorGreen=0, ColorAmber, ColorWhite, ColorWhiteBlue, ColorRainbow };
static const char * const s_ModeNames[NUM_MODES] = {
    "Green","Amber","White","White/Blue","Rainbow"
};
struct TColorPair { u16 on; u16 off; };

// ── Frame protocol ────────────────────────────────────────────────────────────
static const u8 FRAME_MAGIC[4] = {0xAA,0x55,0xAA,0x55};
static u8 pixelbuf[2][FRAME_BYTES];
static u8 prevbuf[FRAME_BYTES];     // previous frame for noise filtering

enum TShutdownMode { ShutdownNone, ShutdownHalt, ShutdownReboot };

// ── Colour helpers ────────────────────────────────────────────────────────────
static u16 RGB(u8 r,u8 g,u8 b){return(u16)(((r>>3)<<11)|((g>>2)<<5)|(b>>3));}
static u16 RGB888(u32 c){return RGB((c>>16)&0xFF,(c>>8)&0xFF,c&0xFF);}
static u16 Dim(u16 c){return(u16)((((c>>11)>>1)<<11)|((((c>>5)&0x3F)>>1)<<5)|((c&0x1F)>>1));}
static u32 ParseHex6(const char *s){
    u32 v=0;for(int i=0;i<6&&s[i];i++){v<<=4;char c=s[i];
    if(c>='0'&&c<='9')v|=c-'0';else if(c>='a'&&c<='f')v|=c-'a'+10;
    else if(c>='A'&&c<='F')v|=c-'A'+10;}return v;}
static u16 Rainbow(unsigned y){
    unsigned seg=(y*6)/LINES_PER_FRAME,t=((y*6)%LINES_PER_FRAME)*255/LINES_PER_FRAME;
    u8 r,g,b;
    switch(seg){case 0:r=255;g=t;b=0;break;case 1:r=255-t;g=255;b=0;break;
    case 2:r=0;g=255;b=t;break;case 3:r=0;g=255-t;b=255;break;
    case 4:r=t;g=0;b=255;break;default:r=255;g=0;b=255-t;break;}
    return RGB(r,g,b);}
static const char *Trim(const char *s){while(*s==' '||*s=='\t')s++;return s;}
static boolean StartsWith(const char *s,const char *p)
    {while(*p)if(*s++!=*p++)return FALSE;return TRUE;}

// ── 8x8 font (0x20..0x7E) ────────────────────────────────────────────────────
static const u8 s_Font[96][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // 20
    {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00}, // 21 !
    {0x36,0x36,0x00,0x00,0x00,0x00,0x00,0x00}, // 22 "
    {0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x00}, // 23 #
    {0x0C,0x3E,0x03,0x1E,0x30,0x1F,0x0C,0x00}, // 24 $
    {0x00,0x63,0x33,0x18,0x0C,0x66,0x63,0x00}, // 25 %
    {0x1C,0x36,0x1C,0x6E,0x3B,0x33,0x6E,0x00}, // 26 &
    {0x06,0x06,0x03,0x00,0x00,0x00,0x00,0x00}, // 27 '
    {0x18,0x0C,0x06,0x06,0x06,0x0C,0x18,0x00}, // 28 (
    {0x06,0x0C,0x18,0x18,0x18,0x0C,0x06,0x00}, // 29 )
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, // 2A *
    {0x00,0x0C,0x0C,0x3F,0x0C,0x0C,0x00,0x00}, // 2B +
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x06}, // 2C ,
    {0x00,0x00,0x00,0x3F,0x00,0x00,0x00,0x00}, // 2D -
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x00}, // 2E .
    {0x60,0x30,0x18,0x0C,0x06,0x03,0x01,0x00}, // 2F /
    {0x3E,0x63,0x73,0x7B,0x6F,0x67,0x3E,0x00}, // 30 0
    {0x0C,0x0E,0x0C,0x0C,0x0C,0x0C,0x3F,0x00}, // 31 1
    {0x1E,0x33,0x30,0x1C,0x06,0x33,0x3F,0x00}, // 32 2
    {0x1E,0x33,0x30,0x1C,0x30,0x33,0x1E,0x00}, // 33 3
    {0x38,0x3C,0x36,0x33,0x7F,0x30,0x78,0x00}, // 34 4
    {0x3F,0x03,0x1F,0x30,0x30,0x33,0x1E,0x00}, // 35 5
    {0x1C,0x06,0x03,0x1F,0x33,0x33,0x1E,0x00}, // 36 6
    {0x3F,0x33,0x30,0x18,0x0C,0x0C,0x0C,0x00}, // 37 7
    {0x1E,0x33,0x33,0x1E,0x33,0x33,0x1E,0x00}, // 38 8
    {0x1E,0x33,0x33,0x3E,0x30,0x18,0x0E,0x00}, // 39 9
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x00}, // 3A :
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x06}, // 3B ;
    {0x18,0x0C,0x06,0x03,0x06,0x0C,0x18,0x00}, // 3C <
    {0x00,0x00,0x3F,0x00,0x00,0x3F,0x00,0x00}, // 3D =
    {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00}, // 3E >
    {0x1E,0x33,0x30,0x18,0x0C,0x00,0x0C,0x00}, // 3F ?
    {0x3E,0x63,0x7B,0x7B,0x7B,0x03,0x1E,0x00}, // 40 @
    {0x0C,0x1E,0x33,0x33,0x3F,0x33,0x33,0x00}, // 41 A
    {0x3F,0x66,0x66,0x3E,0x66,0x66,0x3F,0x00}, // 42 B
    {0x3C,0x66,0x03,0x03,0x03,0x66,0x3C,0x00}, // 43 C
    {0x1F,0x36,0x66,0x66,0x66,0x36,0x1F,0x00}, // 44 D
    {0x7F,0x46,0x16,0x1E,0x16,0x46,0x7F,0x00}, // 45 E
    {0x7F,0x46,0x16,0x1E,0x16,0x06,0x0F,0x00}, // 46 F
    {0x3C,0x66,0x03,0x03,0x73,0x66,0x7C,0x00}, // 47 G
    {0x33,0x33,0x33,0x3F,0x33,0x33,0x33,0x00}, // 48 H
    {0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, // 49 I
    {0x78,0x30,0x30,0x30,0x33,0x33,0x1E,0x00}, // 4A J
    {0x67,0x66,0x36,0x1E,0x36,0x66,0x67,0x00}, // 4B K
    {0x0F,0x06,0x06,0x06,0x46,0x66,0x7F,0x00}, // 4C L
    {0x63,0x77,0x7F,0x7F,0x6B,0x63,0x63,0x00}, // 4D M
    {0x63,0x67,0x6F,0x7B,0x73,0x63,0x63,0x00}, // 4E N
    {0x1C,0x36,0x63,0x63,0x63,0x36,0x1C,0x00}, // 4F O
    {0x3F,0x66,0x66,0x3E,0x06,0x06,0x0F,0x00}, // 50 P
    {0x1E,0x33,0x33,0x33,0x3B,0x1E,0x38,0x00}, // 51 Q
    {0x3F,0x66,0x66,0x3E,0x36,0x66,0x67,0x00}, // 52 R
    {0x1E,0x33,0x07,0x0E,0x38,0x33,0x1E,0x00}, // 53 S
    {0x3F,0x2D,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, // 54 T
    {0x33,0x33,0x33,0x33,0x33,0x33,0x3F,0x00}, // 55 U
    {0x33,0x33,0x33,0x33,0x33,0x1E,0x0C,0x00}, // 56 V
    {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00}, // 57 W
    {0x63,0x63,0x36,0x1C,0x1C,0x36,0x63,0x00}, // 58 X
    {0x33,0x33,0x33,0x1E,0x0C,0x0C,0x1E,0x00}, // 59 Y
    {0x7F,0x63,0x31,0x18,0x4C,0x66,0x7F,0x00}, // 5A Z
    {0x1E,0x06,0x06,0x06,0x06,0x06,0x1E,0x00}, // 5B [
    {0x03,0x06,0x0C,0x18,0x30,0x60,0x40,0x00}, // 5C backslash
    {0x1E,0x18,0x18,0x18,0x18,0x18,0x1E,0x00}, // 5D ]
    {0x08,0x1C,0x36,0x63,0x00,0x00,0x00,0x00}, // 5E ^
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF}, // 5F _
};


class CKernel
{
public:
    CKernel (void)
    :   m_Screen (DISPLAY_W, DISPLAY_H),
        m_Timer (&m_Interrupt),
        m_Logger (LogDebug, &m_Timer),
        m_Serial (&m_Interrupt, TRUE),
        m_GPIOManager (&m_Interrupt),
        m_CLK (PIN_ENC_CLK, GPIOModeInputPullUp, &m_GPIOManager),
        m_DT  (PIN_ENC_DT,  GPIOModeInputPullUp, &m_GPIOManager),
        m_SW  (PIN_ENC_SW,  GPIOModeInputPullUp, &m_GPIOManager)
    {
        s_pThis = this;
        m_Pal[ColorGreen]     = { RGB888(0x00FF46), RGB888(0x000000) };
        m_Pal[ColorAmber]     = { RGB888(0xFFB000), RGB888(0x000000) };
        m_Pal[ColorWhite]     = { RGB888(0xFFFFFF), RGB888(0x000000) };
        m_Pal[ColorWhiteBlue] = { RGB888(0xFFFFFF), RGB888(0x0000AA) };
        m_Pal[ColorRainbow]   = { 0,                RGB888(0x000000) };
    }

    boolean Initialize (void)
    {
        if (!m_Interrupt.Initialize ())        return FALSE;
        if (!m_Screen.Initialize ())           return FALSE;
        if (!m_Timer.Initialize ())            return FALSE;
        if (!m_Logger.Initialize (&m_Screen))  return FALSE;
        if (!m_Serial.Initialize (3000000))    return FALSE;

        // Get direct framebuffer for fast pixel writes
        CBcmFrameBuffer *pFB = m_Screen.GetFrameBuffer ();
        if (pFB) {
            m_pFB   = (u16 *)(uintptr_t)pFB->GetBuffer ();
            m_Pitch = pFB->GetPitch () / sizeof (u16);
        }

        // Clear screen to black
        ClearScreen ();

        // SD card
        if (f_mount (&m_FS, "SD:", 1) == FR_OK) {
            m_SDMounted = TRUE;
            LoadPalette ();
            LoadConfig ();
        }

        // GPIO interrupts — fire instantly, set volatile flags for main loop
        m_GPIOManager.Initialize ();
        m_CLK.ConnectInterrupt (OnCLK, this);
        m_CLK.EnableInterrupt (GPIOInterruptOnFallingEdge);
        m_SW.ConnectInterrupt (OnSW, this);
        m_SW.EnableInterrupt (GPIOInterruptOnFallingEdge);

        return TRUE;
    }

    TShutdownMode Run (void)
    {
        m_ActLED.On ();
        DrawBars ();

        int recv=0, disp=1;
        unsigned frames=0;
        boolean connected=FALSE;
        unsigned lastTick=0;

        while (1)
        {
            // Handle encoder/button events set by ISR
            if (m_EncEvent) {
                m_EncEvent = FALSE;
                SaveConfig ();
                DrawBars ();
                RenderFrame (pixelbuf[disp]);
            }
            if (m_SwEvent) {
                m_SwEvent = FALSE;
                // Debounce: ignore if SW fired again too soon
                unsigned now = m_Timer.GetTicks ();
                if (now - m_SwTick > SW_DEBOUNCE_US) {
                    m_SwTick = now;
                    m_Scanlines = !m_Scanlines;
                    SaveConfig ();
                    DrawBars ();
                    RenderFrame (pixelbuf[disp]);
                }
            }

            // Non-blocking UART
            u8 b;
            if (m_Serial.Read (&b, 1) > 0) {
                if (!connected) {
                    connected = TRUE;
                    DrawTopBar (TRUE, 0);
                }
                lastTick = m_Timer.GetTicks ();
                if (FeedByte (b, pixelbuf[recv])) {
                    int t=disp; disp=recv; recv=t;
                    frames++;
                    if      (frames%120==0) m_ActLED.On ();
                    else if (frames%60 ==0) m_ActLED.Off ();
                    RenderFrame (pixelbuf[disp]);
                    if (frames%30==0) DrawTopBar (TRUE, frames);
                }
            } else if (connected && m_Timer.GetTicks()-lastTick > 3000000u) {
                connected = FALSE;
                DrawTopBar (FALSE, frames);
            }
        }
        return ShutdownHalt;
    }

private:
    // ── ISR handlers ─────────────────────────────────────────────────────────
    static void OnCLK (void *p)
    {
        CKernel *k = (CKernel *)p;
        boolean dt = (boolean)k->m_DT.Read ();
        if (dt)
            k->m_Mode=(TColorMode)((k->m_Mode+1)%NUM_MODES);
        else
            k->m_Mode=(TColorMode)((k->m_Mode+NUM_MODES-1)%NUM_MODES);
        k->m_EncEvent = TRUE;
    }
    static void OnSW (void *p)
    {
        CKernel *k = (CKernel *)p;
        // Just set the flag — main loop handles debounce
        k->m_SwEvent = TRUE;
    }

    // ── Frame state machine ───────────────────────────────────────────────────
    boolean FeedByte (u8 b, u8 *buf)
    {
        switch (m_RxState) {
        case RX_MAGIC:
            if (b==FRAME_MAGIC[m_RxPos]){if(++m_RxPos==4){m_RxState=RX_NUM;m_RxPos=0;}}
            else{m_RxPos=(b==FRAME_MAGIC[0])?1:0;}
            break;
        case RX_NUM:
            if(++m_RxPos==2){m_RxState=RX_LEN;m_RxPos=0;m_RxLen=0;}
            break;
        case RX_LEN:
            m_RxLen|=(u16)b<<(m_RxPos*8);
            if(++m_RxPos==2){m_RxState=(m_RxLen==FRAME_BYTES)?RX_DATA:RX_MAGIC;m_RxPos=0;}
            break;
        case RX_DATA:
            buf[m_RxPos++]=b;
            if(m_RxPos>=(int)FRAME_BYTES){m_RxState=RX_MAGIC;m_RxPos=0;return TRUE;}
            break;
        }
        return FALSE;
    }

    // ── Pixel write — direct FB or SetPixel fallback ──────────────────────────
    inline void PutPixel (unsigned x, unsigned y, u16 c)
    {
        if (m_pFB) m_pFB[y*m_Pitch+x] = c;
        else       m_Screen.SetPixel (x, y, (TScreenColor)c);
    }

    void ClearScreen (void)
    {
        if (m_pFB)
            for (unsigned i=0;i<DISPLAY_H*m_Pitch;i++) m_pFB[i]=0;
        else
            for (unsigned y=0;y<DISPLAY_H;y++)
                for (unsigned x=0;x<DISPLAY_W;x++)
                    m_Screen.SetPixel(x,y,0);
    }

    void FillRect (unsigned x0,unsigned y0,unsigned w,unsigned h,u16 c)
    {
        if (m_pFB) {
            for(unsigned y=y0;y<y0+h;y++){
                u16 *row=m_pFB+y*m_Pitch;
                for(unsigned x=x0;x<x0+w;x++) row[x]=c;
            }
        } else {
            for(unsigned y=y0;y<y0+h;y++)
                for(unsigned x=x0;x<x0+w;x++)
                    m_Screen.SetPixel(x,y,(TScreenColor)c);
        }
    }

    void RenderFrame (const u8 *src)
    {
        TColorPair &p=m_Pal[m_Mode];
        boolean rain=(m_Mode==ColorRainbow);
        for(unsigned y=0;y<LINES_PER_FRAME;y++){
            const u8 *line=src+y*BYTES_PER_LINE;
            const u8 *prev=prevbuf+y*BYTES_PER_LINE;
            unsigned sy=y+VIDEO_Y;
            u16 on=rain?Rainbow(y):p.on, off=p.off;
            if(m_Scanlines&&(y&1)){on=Dim(on);off=Dim(off);}
            if(m_pFB){
                u16 *row=m_pFB+sy*m_Pitch;
                for(unsigned xb=0;xb<BYTES_PER_LINE;xb++){
                    // Temporal filter: pixel ON only if set in both current
                    // and previous frame — eliminates single-frame noise spikes
#if NOISE_FILTER
                    u8 byt = line[xb] & prev[xb];
#else
                    u8 byt = line[xb];
#endif
                    u16 *px=row+xb*8;
                    px[0]=(byt&0x80)?on:off; px[1]=(byt&0x40)?on:off;
                    px[2]=(byt&0x20)?on:off; px[3]=(byt&0x10)?on:off;
                    px[4]=(byt&0x08)?on:off; px[5]=(byt&0x04)?on:off;
                    px[6]=(byt&0x02)?on:off; px[7]=(byt&0x01)?on:off;
                }
            } else {
                for(unsigned xb=0;xb<BYTES_PER_LINE;xb++){
#if NOISE_FILTER
                    u8 byt = line[xb] & prev[xb];
#else
                    u8 byt = line[xb];
#endif
                    unsigned x=xb*8;
                    m_Screen.SetPixel(x+0,sy,(byt&0x80)?(TScreenColor)on:(TScreenColor)off);
                    m_Screen.SetPixel(x+1,sy,(byt&0x40)?(TScreenColor)on:(TScreenColor)off);
                    m_Screen.SetPixel(x+2,sy,(byt&0x20)?(TScreenColor)on:(TScreenColor)off);
                    m_Screen.SetPixel(x+3,sy,(byt&0x10)?(TScreenColor)on:(TScreenColor)off);
                    m_Screen.SetPixel(x+4,sy,(byt&0x08)?(TScreenColor)on:(TScreenColor)off);
                    m_Screen.SetPixel(x+5,sy,(byt&0x04)?(TScreenColor)on:(TScreenColor)off);
                    m_Screen.SetPixel(x+6,sy,(byt&0x02)?(TScreenColor)on:(TScreenColor)off);
                    m_Screen.SetPixel(x+7,sy,(byt&0x01)?(TScreenColor)on:(TScreenColor)off);
                }
            }
        }
        // Save current frame as previous for next render
        memcpy(prevbuf, src, FRAME_BYTES);
    }

    // ── Font / status bars ────────────────────────────────────────────────────
    void Glyph (unsigned x,unsigned y,char c,u16 fg,u16 bg)
    {
        if(c<0x20||c>0x5F)c=0x20;
        const u8 *g=s_Font[c-0x20];
        for(unsigned row=0;row<8;row++){
            u8 bits=g[row];
            if(m_pFB){
                u16 *p=m_pFB+(y+row)*m_Pitch+x;
                // bit0=leftmost pixel (reversed from MSB-first font data)
                p[7]=(bits&0x80)?fg:bg; p[6]=(bits&0x40)?fg:bg;
                p[5]=(bits&0x20)?fg:bg; p[4]=(bits&0x10)?fg:bg;
                p[3]=(bits&0x08)?fg:bg; p[2]=(bits&0x04)?fg:bg;
                p[1]=(bits&0x02)?fg:bg; p[0]=(bits&0x01)?fg:bg;
            } else {
                for(unsigned col=0;col<8;col++)
                    m_Screen.SetPixel(x+(7-col),y+row,
                        (TScreenColor)((bits&(0x80>>col))?fg:bg));
            }
        }
    }
    void Text(unsigned x,unsigned y,const char *s,u16 fg,u16 bg)
        {while(*s){Glyph(x,y,*s++,fg,bg);x+=8;}}

    void DrawTopBar(boolean connected,unsigned frames)
    {
        u16 bg=connected?RGB888(0x003300):RGB888(0x330000);
        u16 fg=RGB888(0xFFFFFF);
        FillRect(0,TOP_BAR_Y,DISPLAY_W,TOP_BAR_H,bg);
        Text(2,TOP_BAR_Y+4,connected?"CONNECTED":"NO SIGNAL",fg,bg);
        if(connected){
            char buf[20]; unsigned len=0;
            buf[len++]='F';buf[len++]='r';buf[len++]='m';buf[len++]=':';
            char tmp[10];int ti=0;unsigned f=frames;
            if(!f)tmp[ti++]='0';else while(f){tmp[ti++]='0'+(f%10);f/=10;}
            for(int k=ti-1;k>=0;k--)buf[len++]=tmp[k];buf[len]=0;
            Text(DISPLAY_W-len*8-2,TOP_BAR_Y+4,buf,fg,bg);
        }
    }
    void DrawBotBar(void)
    {
        u16 bg=RGB888(0x000033),fg=RGB888(0xFFFFFF),hl=RGB888(0x00FF46);
        FillRect(0,BOT_BAR_Y,DISPLAY_W,BOT_BAR_H,bg);
        Text(2,BOT_BAR_Y+4,"Color:",fg,bg);
        Text(50,BOT_BAR_Y+4,s_ModeNames[m_Mode],hl,bg);
        Text(DISPLAY_W-104,BOT_BAR_Y+4,
             m_Scanlines?"Scanlines:ON ":"Scanlines:OFF",fg,bg);
    }
    void DrawBars(){DrawTopBar(FALSE,0);DrawBotBar();}

    // ── SD card ───────────────────────────────────────────────────────────────
    void SaveConfig(void){
        if(!m_SDMounted)return;
        FIL f;if(f_open(&f,CFG_FILE,FA_WRITE|FA_CREATE_ALWAYS)!=FR_OK)return;
        char buf[4];buf[0]=(char)('0'+(int)m_Mode);
        buf[1]=' ';buf[2]=m_Scanlines?'1':'0';buf[3]='\n';
        UINT bw;f_write(&f,buf,4,&bw);f_close(&f);}
    void LoadConfig(void){
        FIL f;if(f_open(&f,CFG_FILE,FA_READ)!=FR_OK)return;
        char buf[16]={0};UINT br;f_read(&f,buf,sizeof(buf)-1,&br);f_close(&f);
        int m=buf[0]-'0';if(m>=0&&m<NUM_MODES)m_Mode=(TColorMode)m;
        if(br>=3)m_Scanlines=(buf[2]=='1');}
    void LoadPalette(void){
        FIL f;if(f_open(&f,PAL_FILE,FA_READ)!=FR_OK)return;
        char line[64];
        while(f_gets(line,sizeof(line),&f)){
            const char *p=Trim(line);
            if(*p=='#'||!*p||*p=='\r'||*p=='\n')continue;
            char nm[16]={0},fg[8]={0},bg[8]={0};const char *t=p;
            for(int fi=0;fi<3&&*t;fi++){
                while(*t==' '||*t=='\t')t++;if(!*t)break;
                char *dst=(fi==0)?nm:(fi==1)?fg:bg;int mx=(fi==0)?15:7,i=0;
                while(*t&&*t!=' '&&*t!='\t'&&*t!='\r'&&*t!='\n'&&i<mx)dst[i++]=*t++;
                dst[i]=0;}
            int mode=-1;
            if     (StartsWith(nm,"green"))     mode=ColorGreen;
            else if(StartsWith(nm,"amber"))     mode=ColorAmber;
            else if(StartsWith(nm,"white_blue"))mode=ColorWhiteBlue;
            else if(StartsWith(nm,"white"))     mode=ColorWhite;
            else if(StartsWith(nm,"rainbow"))   mode=ColorRainbow;
            if(mode<0)continue;
            m_Pal[mode].on=RGB888(ParseHex6(fg));m_Pal[mode].off=RGB888(ParseHex6(bg));}
        f_close(&f);}

private:
    // ── Circle member order — do not change ───────────────────────────────────
    CActLED             m_ActLED;
    CKernelOptions      m_Options;
    CDeviceNameService  m_DeviceNameService;
    CScreenDevice       m_Screen;
    CExceptionHandler   m_ExceptionHandler;
    CInterruptSystem    m_Interrupt;
    CTimer              m_Timer;
    CLogger             m_Logger;
    CSerialDevice       m_Serial;
    CGPIOManager        m_GPIOManager;
    CGPIOPin            m_CLK;
    CGPIOPin            m_DT;
    CGPIOPin            m_SW;

    // Framebuffer (null = fall back to SetPixel)
    u16                *m_pFB   = nullptr;
    unsigned            m_Pitch = 0;

    // Volatile state — modified by ISR
    volatile TColorMode m_Mode     = ColorGreen;
    volatile boolean    m_Scanlines= FALSE;
    volatile boolean    m_EncEvent = FALSE;
    volatile boolean    m_SwEvent  = FALSE;
    unsigned            m_SwTick   = 0;   // debounce timestamp (main loop only)

    TColorPair          m_Pal[NUM_MODES];
    FATFS               m_FS;
    boolean             m_SDMounted = FALSE;

    enum TRxState { RX_MAGIC, RX_NUM, RX_LEN, RX_DATA };
    TRxState  m_RxState = RX_MAGIC;
    int       m_RxPos   = 0;
    u16       m_RxLen   = 0;

    static CKernel *s_pThis;
};

CKernel *CKernel::s_pThis = nullptr;


int main (void)
{
    CKernel kernel;
    if (!kernel.Initialize ()) {
        CActLED led;
        while (1){led.On();CTimer::SimpleMsDelay(100);led.Off();CTimer::SimpleMsDelay(100);}
    }
    kernel.Run ();
    return 0;
}
