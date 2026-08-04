// DonkeyKong.c
// Donkey Kong clone for the TI MSPM0G3507 (ECE319K final project)
// Braden Feuer & Pranav Shivashankar
// Last Modified: April 8, 2026
//
// ---------------------------------------------------------------------------
// OVERVIEW
//   A playable Donkey Kong clone running bare-metal on an MSPM0G3507 at 80 MHz,
//   rendered to a 128x160 ST7735 TFT.
//
// FEATURES
//   - Five-level platform stage with ladders, gaps the hero falls through,
//     and one instant-death pit
//   - Up to 10 barrels rolling simultaneously, each following one of four
//     pre-computed paths chosen at random on spawn
//   - Table-driven jump arc so jump height/timing is tunable without touching
//     the physics code
//   - Collectible bananas that spawn at random locations on a timer
//   - 3 lives, running score, pause toggle, and an animated game-over screen
//   - Bilingual UI (English / Spanish) selected from the title screen via the
//     slide potentiometer
//   - Sound effects on jump, coin pickup, and death
//
// ARCHITECTURE
//   Two hardware timers drive the game independently of the render loop:
//     TIMG12 @ 30 Hz  - game engine tick: animation frames, score, banana
//                       timer, barrel spawning
//     TIMG0  @ 30 Hz  - barrel path stepping (moves every bcountMove ticks)
//   Both ISRs bail out immediately when pauseFlag is set. All ST7735 output
//   happens in main; the ISRs only mutate state, which keeps SPI traffic out
//   of interrupt context.
//
//   Rendering is dirty-rect based: the hero is only erased and redrawn when
//   its coordinates actually change, and barrels redraw only when the path
//   task raises bflag. This keeps the frame rate up over a slow SPI display.
//
// CONTROLS
//   Slide potentiometer (PB18, ADC1 ch5) - move left / right
//   PA26 - jump          PA27 - climb ladder up
//   PA24 - climb down    PA25 - pause
//
// DEPENDENCIES
//   ST7735.h, Clock.h, LaunchPad.h, Timer.h, ADC1.h, DAC5.h  (course drivers)
//   SmallFont.h, LED.h, Switch.h, Sound.h, images/images.h, paths.h
// ---------------------------------------------------------------------------

#include <stdio.h>
#include <stdint.h>
#include <ti/devices/msp/msp.h>
#include "../inc/ST7735.h"
#include "../inc/Clock.h"
#include "../inc/LaunchPad.h"
#include "../inc/Timer.h"
#include "../inc/ADC1.h"
#include "../inc/DAC5.h"
#include "SmallFont.h"
#include "LED.h"
#include "Switch.h"
#include "Sound.h"
#include "images/images.h"
#include "paths.h"

//============================================================ LOCALIZATION
// Non-ASCII glyphs use the ST7735 driver's extended character codes.
char *Engwords[] = {
  "Right for English", "Left for Espa\xA4ol", "Up", "Ladder", "Jump",
  "Move", "Down", "Pause", "Hit Jump to Start!"
};
char *Spanwords[] = {
  "__", "__", "Subir la", "Escalera", "Saltar",
  "Mover", "Bajar la", "Pausa", "\xADPresiona saltar", "para comenzar!"
};

//============================================================ LEVEL GEOMETRY
// Barrel paths (pre-computed pixel tracks) live in paths.h
path_t* allPaths[]  = {path0, path1, path2, path3};
int     pathLengths[] = {348, 299, 317, 292};

// Where a banana can appear
path_t bananaloc[5] = {
  {2, 106}, {110, 72}, {10, 76}, {120, 46}, {75, 132}
};

// Gaps in the floors and climbable ladders.
// type: 0 = hole (hero falls through), 1 = ladder
struct HL {
  int16_t type;
  int16_t startX;
  int16_t startY;
  int16_t width;
  int16_t depth;
};
typedef const struct HL Typ;

Typ holeLadder[9] = {
  {0,  70,  22, 57, 30},   // top hole
  {0,   0,  52, 30, 30},   // 2nd row, left hole
  {0, 100,  52, 15, 60},   // 2nd row, right hole
  {0,  95,  82, 32, 30},   // 3rd row, right hole
  {0,  15, 112, 15, 30},   // 4th row hole
  {0,  50, 142, 15, 28},   // 5th row hole  <-- instant-death pit
  {1,  83,  82, 12, 30},   // top ladder
  {1,  51, 112, 12, 30},   // middle ladder
  {1, 106, 142, 12, 30}    // bottom ladder
};

//============================================================ SPRITES & STATE
struct sprite {
  int16_t x;
  int16_t y;
  int16_t prevx;
  int16_t prevy;
  int16_t sizex;
  int16_t sizey;
  int     life;
  int16_t vy;
  int16_t onGround;
};
typedef struct sprite sprite_t;

sprite_t hero;
sprite_t bananas;

#define MAX_BARRELS 10
sprite_t barrels[MAX_BARRELS];      // barrel sprites
int      barrelPathIdx[MAX_BARRELS];  // current step along its path
int      barrelPathType[MAX_BARRELS]; // which of the 4 paths it follows
uint32_t spawnTimer = 0;

// Animation / timing counters
int16_t monkflag = 0;   // Donkey Kong + hero walk-cycle frame
int16_t overflag = 0;   // game-over animation frame
int16_t startcyc = 0;   // title-screen animation frame
int     count    = 0;
int     bcount   = 0;
int     bcountMove = 35;  // TIMG0 ticks between barrel path steps
int     bflag    = 0;     // barrels moved, needs redraw

// Hero movement state
uint8_t Holeflag  = 0;  // currently falling through a hole
uint8_t Holetyp   = 0;  // pixels left to fall
uint8_t Deathhole = 0;  // the hole being fallen into is the death pit
uint8_t Ladmoveflag = 0;
uint8_t LadDelay    = 0;

// Game state
uint8_t  death = 1;     // 1 while the current life is still in play
uint32_t score = 0;
uint8_t  bananaflag = 0;
uint8_t  randomban  = 0;
uint8_t  bantimer   = 0;
uint8_t  pauseFlag  = 0;
uint32_t adc;

// Title-screen / language selection
uint8_t  engflag  = 0;
uint8_t  spanflag = 0;
uint8_t  permSpan = 0;   // Spanish chosen; used for in-game strings
uint32_t startcoun = 0;

//============================================================ JUMP TABLE
// Vertical offset from the launch height, one entry per jump frame.
const int8_t jumpArc[] = {
   0, -1, -2, -3, -4, -5, -6, -7, -8,
  -9,-10,-11,-12,
 -12,-12,-12,-12,-12,-12,-12,-12,-12,
 -11,-10, -9, -8, -7, -6, -5,
  -4, -3, -2, -1,
   0,  0
};
#define JUMP_ARC_LEN 35

int16_t heroGroundY;   // y the hero launched from
int16_t jumpActive;
int16_t jumpIndex;
int16_t jumpDelay;

//============================================================ INIT / RANDOM
void Initialize(void){
  hero.x = 5;
  hero.y = 142;
  hero.sizex = 10;
  hero.sizey = 10;
  hero.vy = 0;
  hero.onGround = 1;
  heroGroundY = 142;

  bananas.sizex = 6;
  bananas.sizey = 6;

  jumpActive = 0;
  jumpIndex  = 0;
  jumpDelay  = 0;

  Holeflag    = 0;
  Holetyp     = 0;
  Deathhole   = 0;
  Ladmoveflag = 0;
  LadDelay    = 0;
}

// Note: the datasheet says the ADC is unspecified at 80 MHz, but it runs
// reliably on our boards. Swap to Clock_Init40MHz() if yours doesn't.
void PLL_Init(void){
  Clock_Init80MHz(0);
}

// Linear congruential PRNG (no stdlib rand on this target)
uint32_t M = 1;
uint32_t Random32(void){
  M = 1664525*M + 1013904223;
  return M;
}
uint32_t Random(uint32_t n){
  return (Random32()>>16) % n;
}

//============================================================ INPUT
void ControlSwitches_Init(void){
  IOMUX->SECCFG.PINCM[PA24INDEX] = 0x00040081;
  IOMUX->SECCFG.PINCM[PA25INDEX] = 0x00040081;
  IOMUX->SECCFG.PINCM[PA26INDEX] = 0x00040081;
  IOMUX->SECCFG.PINCM[PA27INDEX] = 0x00040081;

  // configure as inputs
  GPIOA->DOE31_0 &= ~((1<<24) | (1<<25) | (1<<26) | (1<<27));
}

uint32_t Jump_In(void)      { return (GPIOA->DIN31_0 & (1<<26)) ? 1 : 0; }
uint32_t LadderUp_In(void)  { return (GPIOA->DIN31_0 & (1<<27)) ? 1 : 0; }
uint32_t LadderDown_In(void){ return (GPIOA->DIN31_0 & (1<<24)) ? 1 : 0; }
uint32_t Pause_In(void)     { return (GPIOA->DIN31_0 & (1<<25)) ? 1 : 0; }

//============================================================ INTERRUPTS
// Barrel movement: steps every active barrel one node along its path.
void TIMG0_IRQHandler(void){
  if((TIMG0->CPU_INT.IIDX) == 1){   // acknowledge
    if(pauseFlag){
      return;
    }
    if(bcount == bcountMove){
      bcount = 0;
      for(int i = 0; i < MAX_BARRELS; i++){
        if(barrels[i].life == 2){          // freshly spawned: place at start
          barrels[i].life = 1;
          int type = barrelPathType[i];
          barrels[i].x = allPaths[type][6].x;
          barrels[i].y = allPaths[type][6].y;
          barrels[i].prevx = allPaths[type][0].x;
          barrels[i].prevy = allPaths[type][0].y;
        }
        else if(barrels[i].life == 1){     // rolling: advance one node
          int type = barrelPathType[i];
          barrels[i].prevx = barrels[i].x;
          barrels[i].prevy = barrels[i].y;
          barrelPathIdx[i]++;
          barrels[i].x = allPaths[type][barrelPathIdx[i]+6].x;
          barrels[i].y = allPaths[type][barrelPathIdx[i]+6].y;
          if(barrelPathIdx[i] >= pathLengths[type] - 1){
            barrels[i].life = 0;           // reached the end, free the slot
          }
        }
      }
      bflag = 1;
    }
    bcount++;
  }
}

// Game engine, 30 Hz: animation frames, score, banana spawn, barrel spawn.
void TIMG12_IRQHandler(void){
  if((TIMG12->CPU_INT.IIDX) == 1){   // acknowledge
    if(pauseFlag){
      return;
    }

    GPIOB->DOUTTGL31_0 = GREEN;   // profiling pulse on PB27 (scope)
    GPIOB->DOUTTGL31_0 = GREEN;

    // banana appears 2 s after the last one was collected
    if(bananaflag == 0){
      bantimer++;
      if(bantimer >= 60){
        randomban = Random(5);
        bananas.x = bananaloc[randomban].x;
        bananas.y = bananaloc[randomban].y;
        bananaflag = 1;
        bantimer = 0;
      }
    }

    // animation frames + score tick, 3 Hz
    count++;
    if(count == 10){
      count = 0;
      startcyc = (startcyc + 1) % 2;
      monkflag = (monkflag + 1) % 2;
      overflag = (overflag + 1) % 3;
      if(hero.life > 0){
        score++;
      }
    }

    // spawn a barrel every 3 s into the first free slot
    spawnTimer++;
    if(spawnTimer >= 90){
      spawnTimer = 0;
      for(int i = 0; i < MAX_BARRELS; i++){
        if(barrels[i].life == 0){
          barrelPathIdx[i]  = 0;
          barrelPathType[i] = Random(4);
          barrels[i].life   = 2;   // queued; TIMG0 places it
          break;
        }
      }
    }

    GPIOB->DOUTTGL31_0 = GREEN;   // end profiling pulse
  }
}

//============================================================ SCREEN DRAWING
// Draws the static stage: floors, ladder cut-outs, and ladders.
void DrawStage(void){
  // floors (row bitmap tiled, with gaps left where holes are)
  ST7735_DrawBitmap(  0,  30, row, 70, 8);
  ST7735_DrawBitmap( 30,  60, row, 57, 8);
  ST7735_DrawBitmap( 95,  60, row,  5, 8);
  ST7735_DrawBitmap(115,  60, row, 12, 8);
  ST7735_DrawBitmap(  0,  90, row, 55, 8);
  ST7735_DrawBitmap( 63,  90, row, 32, 8);
  ST7735_DrawBitmap(  0, 120, row, 15, 8);
  ST7735_DrawBitmap( 30, 120, row, 80, 8);
  ST7735_DrawBitmap(118, 120, row,  9, 8);
  ST7735_DrawBitmap(  0, 150, row, 50, 8);
  ST7735_DrawBitmap( 65, 150, row, 62, 8);

  // floor sections with a ladder opening
  ST7735_DrawBitmap(100, 120, rowladder, 24, 8);
  ST7735_DrawBitmap( 45,  90, rowladder, 24, 8);
  ST7735_DrawBitmap( 77,  60, rowladder, 24, 8);

  // ladders
  ST7735_DrawBitmap(106, 142, ladder, 12, 22);
  ST7735_DrawBitmap( 51, 112, ladder, 12, 22);
  ST7735_DrawBitmap( 83,  82, ladder, 12, 22);
}

// Redraws ladders and Donkey Kong, which barrels and the hero draw over.
void DrawForeground(void){
  ST7735_DrawBitmap(25, 22, (monkflag == 0) ? monkey1 : monkey2, 20, 15);
  ST7735_DrawBitmap(106, 142, ladder, 12, 22);
  ST7735_DrawBitmap( 51, 112, ladder, 12, 22);
  ST7735_DrawBitmap( 83,  82, ladder, 12, 22);
  ST7735_DrawBitmap(100, 120, rowladder, 24, 8);
  ST7735_DrawBitmap( 45,  90, rowladder, 24, 8);
  ST7735_DrawBitmap( 77,  60, rowladder, 24, 8);
}

void DrawLives(void){
  if(hero.life == 3){
    ST7735_DrawBitmap(108, 16, hearts3, 18, 6);
  }
  if(hero.life == 2){
    ST7735_FillRect(108, 11, 18, 6, ST7735_BLACK);
    ST7735_DrawBitmap(108, 16, hearts2, 12, 6);
  }
  if(hero.life == 1){
    ST7735_FillRect(108, 11, 12, 6, ST7735_BLACK);
    ST7735_DrawBitmap(108, 16, hearts1, 6, 6);
  }
}

//============================================================ TITLE SCREENS
// Animated title; slide pot right = English, left = Spanish.
void TitleScreen(void){
  ST7735_SetCursor(3, 10);
  ST7735_OutString(Engwords[0]);
  ST7735_SetCursor(3, 12);
  ST7735_OutString(Engwords[1]);

  while((engflag == 0) && (spanflag == 0)){
    adc = ADCin();
    if(startcoun >= 100){          // ignore input briefly so the pot settles
      if(adc > 2400){
        engflag = 1;
      }
      else if(adc < 1700){
        spanflag = 1;
      }
    }
    if(startcyc == 0){
      ST7735_DrawBitmap(18, 45, Name2, 92, 40);
      ST7735_DrawBitmap(34, 90, monkS1, 55, 40);
    }
    if(startcyc == 1){
      ST7735_DrawBitmap(19, 45, Name1, 90, 40);
      ST7735_DrawBitmap(34, 90, monkS2, 55, 40);
    }
    startcoun++;
  }
}

// Controls diagram; press jump to start.
void ControlsScreen(void){
  ST7735_FillScreen(ST7735_BLACK);

  while(engflag == 1){
    ST7735_DrawBitmap(4, 40, controls, 120, 30);
    if(Jump_In() == 1){
      engflag = 0;
    }
    ST7735_SetCursor(2, 7);   ST7735_OutString(Engwords[2]);  // up
    ST7735_SetCursor(1, 8);   ST7735_OutString(Engwords[3]);  // ladder
    ST7735_SetCursor(4, 5);   ST7735_OutString(Engwords[4]);  // jump
    ST7735_SetCursor(8, 6);   ST7735_OutString(Engwords[5]);  // move
    ST7735_SetCursor(13, 7);  ST7735_OutString(Engwords[6]);  // down
    ST7735_SetCursor(12, 8);  ST7735_OutString(Engwords[3]);  // ladder
    ST7735_SetCursor(15, 5);  ST7735_OutString(Engwords[7]);  // pause
    ST7735_SetCursor(2, 12);  ST7735_OutString(Engwords[8]);  // hit jump to start
  }

  while(spanflag == 1){
    permSpan = 1;
    ST7735_DrawBitmap(4, 40, controls, 120, 30);
    if(Jump_In() == 1){
      spanflag = 0;
    }
    ST7735_SetCursor(1, 7);   ST7735_OutString(Spanwords[2]);
    ST7735_SetCursor(1, 8);   ST7735_OutString(Spanwords[3]);
    ST7735_SetCursor(4, 5);   ST7735_OutString(Spanwords[4]);
    ST7735_SetCursor(8, 6);   ST7735_OutString(Spanwords[5]);
    ST7735_SetCursor(12, 7);  ST7735_OutString(Spanwords[6]);
    ST7735_SetCursor(12, 8);  ST7735_OutString(Spanwords[3]);
    ST7735_SetCursor(15, 5);  ST7735_OutString(Spanwords[7]);
    ST7735_SetCursor(2, 12);  ST7735_OutString(Spanwords[8]);
    ST7735_SetCursor(2, 13);  ST7735_OutString(Spanwords[9]);
  }
}

//============================================================ MAIN
// All ST7735 output happens here; the ISRs only touch state.
int main(void){
  __disable_irq();
  PLL_Init();
  LaunchPad_Init();
  ControlSwitches_Init();
  TimerG12_IntArm(80000000/30, 2);      // game engine, 30 Hz
  TimerG0_IntArm(80000000/30, 1, 2);    // barrel movement, 30 Hz
  ST7735_InitPrintf(INITR_REDTAB);      // INITR_BLACKTAB for HiLetGo displays
  ADCinit();                            // PB18 = ADC1 ch5, slide pot
  Sound_Init();
  __enable_irq();
  ST7735_SetRotation(2);

  TitleScreen();
  ControlsScreen();

  hero.life = 3;

  //---------------------------------------------------- one pass per life
  while(hero.life > 0){
    for(int i = 0; i < MAX_BARRELS; i++){
      barrels[i].life  = 0;
      barrels[i].x     = 0;
      barrels[i].y     = 0;
      barrels[i].prevx = 0;
      barrels[i].prevy = 0;
    }

    ST7735_FillScreen(ST7735_BLACK);
    Initialize();
    death = 1;
    pauseFlag = 0;

    DrawLives();
    DrawStage();
    ST7735_DrawBitmap(hero.x, hero.y, hero1, hero.sizex, hero.sizey);
    ST7735_DrawBitmap(25, 22, monkey1, 20, 15);
    ST7735_DrawBitmap(5, 22, fourbarrels, 15, 15);

    // seed the first barrel so the level isn't empty on spawn
    barrels[0].life = 1;
    barrels[0].x = 60;
    barrels[0].y = 22;
    barrelPathIdx[0]  = 0;
    barrelPathType[0] = Random(4);
    ST7735_DrawBitmap(barrels[0].x, barrels[0].y, barrelz, 6, 6);

    //-------------------------------------------------- game loop
    while(death){
      static int      moveCount = 0;
      static uint32_t lastJump  = 0;
      static uint32_t lastPause = 0;
      uint32_t nowJump, nowPause;
      uint32_t up = 0, down = 0;
      uint8_t  ground = 0, top = 0;
      uint8_t  Ladderflag = 0;

      int heroLeft   = hero.x;
      int heroRight  = hero.x + (hero.sizex - 1);
      int heroTop    = hero.y - (hero.sizey + 1);
      int heroBottom = hero.y;

      int bananaLeft   = bananas.x;
      int bananaRight  = bananas.x + (bananas.sizex - 1);
      int bananaTop    = bananas.y - (bananas.sizey + 1);
      int bananaBottom = bananas.y;

      hero.prevx = hero.x;
      hero.prevy = hero.y;

      //---- pause toggle (rising edge) ----
      nowPause = Pause_In();
      if((lastPause == 0) && (nowPause == 1)){
        pauseFlag ^= 1;
        if(pauseFlag){
          ST7735_SetCursor(0, 0);
          if(permSpan == 0){
            ST7735_OutString("    PAUSED        ");
          }
          else{
            ST7735_OutString("    PAUSADO       ");
          }
        }
        else{
          ST7735_SetCursor(0, 0);
          ST7735_OutString("                  ");
          SmallFont_OutVertical(score, 104, 6);
        }
      }
      lastPause = nowPause;

      if(pauseFlag){
        continue;
      }

      //---- banana draw + pickup ----
      if(bananaflag == 1){
        ST7735_DrawBitmap(bananas.x, bananas.y, banana, bananas.sizex, bananas.sizey);
      }

      if((bananaflag == 1) &&
         (heroRight >= bananaLeft) && (heroLeft <= bananaRight) &&
         (heroBottom >= bananaTop) && (heroTop <= bananaBottom)){
        score += 200;
        ST7735_FillRect(bananas.x, bananas.y - bananas.sizey + 1,
                        bananas.sizex, bananas.sizey, ST7735_BLACK);
        bananaflag = 0;
        bantimer = 0;
        playCoin();
      }

      SmallFont_OutVertical(score, 104, 6);   // score, top right

      //---- barrel collision ----
      for(int i = 0; i < MAX_BARRELS; i++){
        if(barrels[i].life == 1){
          if(((hero.x + 2) < (barrels[i].x + 5)) && ((hero.x + 8) > (barrels[i].x + 1)) &&
             ((hero.y + 2) < (barrels[i].y + 5)) && ((hero.y + 8) > (barrels[i].y + 1))){
            playDeath();
            hero.life--;
            death = 0;
          }
        }
      }

      adc = ADCin();

      //---- hole detection: start falling ----
      if(Holeflag == 0){
        for(int i = 0; i < 9; i++){
          if((hero.x > holeLadder[i].startX) &&
             (hero.x < (holeLadder[i].startX + (holeLadder[i].width - 10))) &&
             (hero.y == holeLadder[i].startY)){
            if(holeLadder[i].type == 0){
              Holeflag = 1;
              Holetyp  = holeLadder[i].depth;
              jumpActive = 0;
              jumpIndex  = 0;
              jumpDelay  = 0;
              hero.onGround = 0;
              if(holeLadder[i].startX == 50){
                Deathhole = 1;   // the pit on the bottom row
              }
            }
          }
        }
      }

      //---- falling ----
      if(Holeflag == 1){
        if(Holetyp != 0){
          hero.y++;
          Holetyp--;
        }
        else{
          Holeflag = 0;
          hero.onGround = 1;
          if(Deathhole == 1){
            playDeath();
            hero.life -= 2;   // the pit costs two lives
            death = 0;
            Deathhole = 0;
          }
        }
      }

      //---- ladder detection ----
      if(Holeflag == 0){
        for(int i = 0; i < 9; i++){
          if((hero.x > (holeLadder[i].startX - 6)) &&
             (hero.x < (holeLadder[i].startX + (holeLadder[i].width - 4))) &&
             (hero.y <= holeLadder[i].startY) &&
             (hero.y >= (holeLadder[i].startY - 30))){
            if(holeLadder[i].type == 1){
              Ladderflag = 1;
              if(hero.y == holeLadder[i].startY){        // standing at the base
                ground = 1;
                Ladmoveflag = 0;
                hero.onGround = 1;
              }
              if(hero.y == (holeLadder[i].startY - 30)){ // standing at the top
                top = 1;
                Ladmoveflag = 0;
                hero.onGround = 1;
              }
            }
          }
        }
      }

      //---- climbing ----
      if(Ladderflag == 1){
        up = LadderUp_In();
        if((up == 1) && (top != 1) && (LadDelay >= 3)){
          hero.y--;
          Ladmoveflag = 1;
          LadDelay = 0;
          jumpActive = 0;
          jumpIndex  = 0;
          jumpDelay  = 0;
          hero.onGround = 0;
        }
        else{
          down = LadderDown_In();
          if((down == 1) && (ground != 1) && (LadDelay >= 3)){
            hero.y++;
            Ladmoveflag = 1;
            LadDelay = 0;
            jumpActive = 0;
            jumpIndex  = 0;
            jumpDelay  = 0;
            hero.onGround = 0;
          }
        }
        LadDelay++;
      }

      //---- walking + jumping (locked out while falling or climbing) ----
      if((Holeflag == 0) && (Ladmoveflag == 0)){
        moveCount++;
        if(moveCount >= 5){       // slow the pot down to a walking pace
          moveCount = 0;
          if(adc > 2400){
            if(hero.x < (128 - hero.sizex)){
              hero.x += 1;
            }
          }
          else if(adc < 1700){
            if(hero.x > 0){
              hero.x -= 1;
            }
          }
        }

        // jump on rising edge only, and only from the ground
        nowJump = Jump_In();
        if((lastJump == 0) && (nowJump == 1) && hero.onGround){
          playJump();
          jumpActive = 1;
          jumpIndex  = 0;
          hero.onGround = 0;
          heroGroundY = hero.y;
        }
        lastJump = nowJump;

        // step through the jump arc table
        if(jumpActive){
          jumpDelay++;
          if(jumpDelay >= 4){
            jumpDelay = 0;
            hero.y = heroGroundY + jumpArc[jumpIndex];
            jumpIndex++;
            if(jumpIndex >= JUMP_ARC_LEN){
              jumpActive = 0;
              hero.y = heroGroundY;
              hero.onGround = 1;
            }
          }
        }
      }

      //---- redraw hero only if it moved ----
      if(hero.x != hero.prevx || hero.y != hero.prevy){
        ST7735_FillRect(hero.prevx, hero.prevy - hero.sizey + 1,
                        hero.sizex, hero.sizey, ST7735_BLACK);
        if(hero.onGround){
          ST7735_DrawBitmap(hero.x, hero.y, (monkflag == 0) ? hero1 : hero2,
                            hero.sizex, hero.sizey);
        }
        else{
          ST7735_DrawBitmap(hero.x, hero.y, hero1, hero.sizex, hero.sizey);
        }
      }

      DrawForeground();

      //---- redraw barrels only when the path task moved them ----
      if(bflag == 1){
        for(int i = 0; i < MAX_BARRELS; i++){
          if(barrels[i].life == 1){
            ST7735_DrawBitmap(barrels[i].prevx, barrels[i].prevy, barrelprev, 6, 6);
            ST7735_DrawBitmap(barrels[i].x, barrels[i].y, barrelz, 6, 6);
          }
        }
        bflag = 0;
      }
    }
  }

  //---------------------------------------------------- game over
  ST7735_FillScreen(ST7735_BLACK);
  while(1){
    SmallFont_OutVertical(score, 45, 120);
    if(overflag == 0){
      ST7735_DrawBitmap(34, 100, gameover1, 60, 60);
    }
    else if(overflag == 1){
      ST7735_DrawBitmap(34, 100, gameover2, 60, 60);
    }
    else{
      ST7735_DrawBitmap(34, 100, gameover3, 60, 60);
    }
  }
}
