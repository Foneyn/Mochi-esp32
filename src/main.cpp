#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <math.h>

// Mochi 1.1 — large, fluid eyes; visual inspiration: Plexynex
// Adapted for ESP32-WROOM-32 + SSD1306 128x64 OLED.
// I2C: SDA=21, SCL=18. MPU6050 address=0x68 (optional).

constexpr int PIN_SDA = 21, PIN_SCL = 18;
constexpr int PIN_BUTTON = 25; // momentary button to GND, INPUT_PULLUP
constexpr uint8_t OLED_ADDR = 0x3C, MPU_ADDR = 0x68;
constexpr int W = 128, H = 64;
Adafruit_SSD1306 display(W, H, &Wire, -1);

enum Emotion : uint8_t { NEUTRAL, HAPPY, DELIGHTED, AFFECTION, SURPRISED, ANGRY, SAD, SLEEPY, CURIOUS, SCARED };
Emotion requestedEmotion = NEUTRAL;
bool mpuPresent=false, waking=true, shakeRecovery=false, manualEmotion=false;
uint8_t recoveryStage=0; // 1 scared, 2 angry, 3 sad
uint32_t bootAt=0, lastFrame=0, lastMpu=0, emotionUntil=0, recoveryUntil=0, nextBlink=0, blinkEnd=0, nextIdle=0;
uint32_t nextLook=0, gazeUntil=0, lastMoodUpdate=0, lastInteraction=0, sideSince=0, sleepStageAt=0, nextHappyMouth=0;
float prevAx=0,prevAy=0,prevAz=0, gazeX=0,gazeY=0;
float filtAx=0, filtAy=0, filtAz=1;
bool tiltActive=false, sidePosition=false, sleeping=false, sleepSequence=false;
uint32_t shakeCooldown=0, lastShakePeak=0;
uint32_t buttonChangedAt=0;
bool buttonStableHigh=true, buttonLastReadHigh=true;
uint8_t shakePeaks=0;
float gazeTargetX=0, gazeTargetY=0;
int mood=60; // 0 = gloomy, 100 = delighted
uint8_t mouthStyle=0; // 0 auto/arc, 1 D, 2 P, 3 tiny 3, 4 O, 5 vertical line

// Animated face parameters. Every emotion changes targets, never hard-cuts.
struct Face {
  float eyeH, eyeW, eyeY, pupilX, pupilY, tilt, smile;
};
// smile: +1 = tiny smile, -1 = tiny frown, 0 = no mouth.
// Start with eyes already shut: no open-to-closed snap after the boot glitch.
Face face={7,46,31,0,0,0,0};
Face target={7,46,31,0,0,0,0};

bool reached(uint32_t now,uint32_t t){return (int32_t)(now-t)>=0;}
float smooth(float current,float dest,float amount){return current+(dest-current)*amount;}
void setEmotion(Emotion e,uint32_t duration=0){
  requestedEmotion=e;
  emotionUntil=duration?millis()+duration:0;
}
void setTargets(){
  static Emotion lastMouthEmotion = (Emotion)255;
  bool enteringEmotion = (requestedEmotion != lastMouthEmotion);
  if(enteringEmotion) { mouthStyle=0; lastMouthEmotion=requestedEmotion; }
  target={35,46,14,gazeX,gazeY,0,0};
  switch(requestedEmotion){
    case ANGRY:
      target.eyeH=29; target.eyeY=17; target.tilt=4; target.smile=0;
      mouthStyle=6; break;
    case SAD:
      target.eyeH=29; target.eyeY=18; target.pupilY=gazeY+9; target.pupilX=gazeX-3;
      target.smile=0;
      if(enteringEmotion) mouthStyle=(random(0,3)==0)?0:((random(0,2)==0)?6:7);
      break;
    case SURPRISED:
      target.eyeH=47; target.eyeW=44; target.eyeY=6; target.smile=0; mouthStyle=4; break;
    case NEUTRAL:
      target.smile=0;
      if(enteringEmotion) mouthStyle=(random(0,5)==0)?5:0;
      break;
    case HAPPY:
      target.eyeH=43; target.eyeY=9; target.smile=0;
      if(enteringEmotion) mouthStyle=(random(0,2)==0)?0:1;
      break;
    case DELIGHTED:
      target.eyeH=53; target.eyeW=52; target.eyeY=1; target.smile=0;
      if(enteringEmotion) mouthStyle=random(1,4);
      break;
    case AFFECTION:
      target.eyeH=18; target.eyeW=46; target.eyeY=19; target.smile=0;
      if(enteringEmotion && mouthStyle!=2 && mouthStyle!=3) mouthStyle=2;
      break;
    case SCARED:
      target.eyeH=47; target.eyeW=44; target.eyeY=6; target.smile=0; mouthStyle=4; break;
    case SLEEPY:
      target.eyeH=10; target.eyeY=29; target.smile=0; mouthStyle=0; break;
    case CURIOUS:
      target.eyeH=49; target.eyeW=46; target.eyeY=5; target.tilt=0; break;
  }
  if(!shakeRecovery && requestedEmotion!=AFFECTION && requestedEmotion!=SCARED &&
     requestedEmotion!=SURPRISED && requestedEmotion!=SLEEPY){
    float low=constrain((50.0f-mood)/50.0f,0.0f,1.0f);
    float high=constrain((mood-50.0f)/50.0f,0.0f,1.0f);
    if(requestedEmotion==NEUTRAL){
      target.eyeY += low*5.0f-high*2.0f;
      target.eyeH += high*3.0f-low*4.0f;
      if(mood<20 && enteringEmotion){ mouthStyle=(random(0,3)==0)?0:6; }
      else if(mood>=80 && enteringEmotion && mouthStyle==0){ mouthStyle=random(1,4); }
    }
  }
}
void animateFace(){
  setTargets();
  // Exponential easing gives a soft, liquid-like transition.
  face.eyeH=smooth(face.eyeH,target.eyeH,0.14f);
  face.eyeW=smooth(face.eyeW,target.eyeW,0.14f);
  face.eyeY=smooth(face.eyeY,target.eyeY,0.14f);
  face.pupilX=smooth(face.pupilX,target.pupilX,0.12f);
  face.pupilY=smooth(face.pupilY,target.pupilY,0.12f);
  face.tilt=smooth(face.tilt,target.tilt,0.12f);
  face.smile=smooth(face.smile,target.smile,0.10f);
}
int clampi(int v,int lo,int hi){return v<lo?lo:(v>hi?hi:v);}

// Rounded eye with top/bottom edge trimming. The trim itself is part of
// the eye silhouette (no separate eyebrows).
void drawOneEye(int cx,int cy,int ew,int eh,int slope,int px,int py,bool closed,
                bool cutInnerTop,bool cutOuterTop,bool starEye,bool scared){
  int x=cx-ew/2, y=cy-eh/2;
  if(closed || eh<5){
    display.drawFastHLine(x+5,cy,ew-10,SSD1306_WHITE);
    display.fillCircle(x+5,cy,1,SSD1306_WHITE);
    display.fillCircle(x+ew-5,cy,1,SSD1306_WHITE);
    return;
  }
  int radius=min(ew/3,eh/2);
  display.fillRoundRect(x,y,ew,eh,radius,SSD1306_WHITE);
  // Clean, straight diagonal corner cuts create a crisp furrowed-brow silhouette.
  // Use mirrored triangles so both eyes have the same geometry.
  int cutW=max(10,min(16,ew/3));
  int cutH=max(7,min(12,eh/3));
  if(cutInnerTop){
    if(cx<64) display.fillTriangle(x+ew-cutW,y,x+ew,y,x+ew,y+cutH,SSD1306_BLACK);
    else      display.fillTriangle(x,y,x+cutW,y,x,y+cutH,SSD1306_BLACK);
  }
  if(cutOuterTop){
    if(cx<64) display.fillTriangle(x,y,x+cutW,y,x,y+cutH,SSD1306_BLACK);
    else      display.fillTriangle(x+ew-cutW,y,x+ew,y,x+ew,y+cutH,SSD1306_BLACK);
  }
  int pupilCx=clampi(cx+px,x+8,x+ew-8);
  int pupilCy=clampi(cy+py,y+8,y+eh-8);
  if(scared){
    // Narrow vertical pupils, jittered by the caller to look frightened.
    display.fillRoundRect(pupilCx-2,pupilCy-7,4,14,2,SSD1306_BLACK);
  } else {
    int pr=(requestedEmotion==AFFECTION) ? max(5,min(9,eh/4)) : max(3,min(7,eh/5));
    display.fillCircle(pupilCx,pupilCy,pr,SSD1306_BLACK);
    display.fillCircle(pupilCx-2,pupilCy-2,1,SSD1306_WHITE);
    if(starEye){
      // White four-point sparkle drawn inside the dark pupil.
      display.drawLine(pupilCx,pupilCy-5,pupilCx,pupilCy+5,SSD1306_WHITE);
      display.drawLine(pupilCx-5,pupilCy,pupilCx+5,pupilCy,SSD1306_WHITE);
      display.drawPixel(pupilCx-3,pupilCy-3,SSD1306_WHITE);
      display.drawPixel(pupilCx+3,pupilCy+3,SSD1306_WHITE);
    }
  }
}
void drawMouth(float amount){
  const int cx=64, cy=55;
  switch(mouthStyle){
    case 0: // no mouth, unless a smooth neutral/smile curve is requested
      if(fabsf(amount)<0.08f) return;
      break;
    case 1: // D / smile
      display.drawLine(cx-7,cy-5,cx-7,cy+3,SSD1306_WHITE);
      display.drawLine(cx-7,cy+3,cx+2,cy+3,SSD1306_WHITE);
      display.drawLine(cx+2,cy+3,cx+7,cy,SSD1306_WHITE);
      display.drawLine(cx+7,cy,cx+2,cy-5,SSD1306_WHITE);
      display.drawLine(cx+2,cy-5,cx-7,cy-5,SSD1306_WHITE);
      return;
    case 2: // P
      display.drawLine(cx-5,cy+5,cx-5,cy-6,SSD1306_WHITE);
      display.drawLine(cx-5,cy-6,cx+3,cy-6,SSD1306_WHITE);
      display.drawLine(cx+3,cy-6,cx+6,cy-3,SSD1306_WHITE);
      display.drawLine(cx+6,cy-3,cx+3,cy,SSD1306_WHITE);
      display.drawLine(cx+3,cy,cx-5,cy,SSD1306_WHITE);
      return;
    case 3: // 3
      display.drawCircle(cx,cy-3,3,SSD1306_WHITE);
      display.drawCircle(cx,cy+3,3,SSD1306_WHITE);
      display.drawFastVLine(cx-4,cy-3,6,SSD1306_BLACK);
      return;
    case 4: // O
      display.drawCircle(cx,cy-1,5,SSD1306_WHITE);
      return;
    case 5: // |
      display.drawFastVLine(cx,cy-5,10,SSD1306_WHITE);
      return;
    case 6: // inverted D / frown
      display.drawLine(cx-8,cy-3,cx-4,cy+2,SSD1306_WHITE);
      display.drawLine(cx-4,cy+2,cx+4,cy+2,SSD1306_WHITE);
      display.drawLine(cx+4,cy+2,cx+8,cy-3,SSD1306_WHITE);
      return;
    case 7: // (
      display.drawLine(cx+3,cy-5,cx-2,cy-3,SSD1306_WHITE);
      display.drawLine(cx-2,cy-3,cx-4,cy+1,SSD1306_WHITE);
      display.drawLine(cx-4,cy+1,cx-2,cy+5,SSD1306_WHITE);
      return;
  }
  const int width=19, x0=cx-width/2;
  int bend=(int)(5.0f*amount), px=x0, py=cy;
  for(int i=1;i<=width;i++){
    float t=(float)i/width;
    int x=x0+i, y=cy+(int)(bend*4.0f*t*(1.0f-t));
    display.drawLine(px,py,x,y,SSD1306_WHITE); px=x; py=y;
  }
}
void renderFace(){
  display.clearDisplay();
  uint32_t now=millis();
  bool blink=!reached(now,blinkEnd);
  int ew=(int)face.eyeW, eh=(int)face.eyeH;
  int cy=(int)(face.eyeY+eh/2);
  int leftCx=34, rightCx=94;
  bool angry=(requestedEmotion==ANGRY);
  bool sad=(requestedEmotion==SAD);
  bool curious=(requestedEmotion==CURIOUS);
  bool scared=(requestedEmotion==SCARED);
  bool stars=(requestedEmotion==DELIGHTED);
  bool affection=(requestedEmotion==AFFECTION);
  bool sleepy=(requestedEmotion==SLEEPY || sleeping);
  int leftW=ew;
  int rightW=ew;
  int shakeX=scared?(int)(sinf(now*0.075f)*4.0f):0;
  int shakeY=scared?(int)(sinf(now*0.11f)*3.0f):0;
  if(affection && !blink){
    // Affection squint: expressive > < eyes.
    display.drawLine(21,cy-8,43,cy,SSD1306_WHITE);
    display.drawLine(43,cy,21,cy+8,SSD1306_WHITE);
    display.drawLine(107,cy-8,85,cy,SSD1306_WHITE);
    display.drawLine(85,cy,107,cy+8,SSD1306_WHITE);
  } else {
    drawOneEye(leftCx,cy,leftW,eh,(int)face.tilt,(int)face.pupilX+shakeX,(int)face.pupilY+shakeY,blink,
               angry,sad,stars,scared);
    drawOneEye(rightCx,cy,rightW,eh,-(int)face.tilt,(int)face.pupilX-shakeX,(int)face.pupilY-shakeY,blink,
               angry,sad,stars,scared);
  }
  if(requestedEmotion==AFFECTION){
    // Anime-style diagonal blush hatching on both cheeks.
    for(int side=0; side<2; ++side){
      int bx = side==0 ? 13 : 101;
      int by = 48;
      display.drawLine(bx,by+2,bx+3,by-3,SSD1306_WHITE);
      display.drawLine(bx+6,by+3,bx+9,by-3,SSD1306_WHITE);
      display.drawLine(bx+12,by+2,bx+15,by-3,SSD1306_WHITE);
    }
  }
  drawMouth(face.smile);
  display.display();
}
// Startup: keep the established one-second glitch, then wake-up sequence.
void glitchFrame(){
  display.clearDisplay();
  for(int i=0;i<random(7,17);i++){
    int y=random(0,H), x=random(0,W), len=random(5,55);
    display.drawFastHLine(x,y,min(len,W-x),SSD1306_WHITE);
  }
  if(random(0,3)==0) display.fillRect(random(0,112),random(0,58),random(4,16),random(2,6),SSD1306_WHITE);
  display.display();
}
void startup(){
  uint32_t t=millis()-bootAt;
  if(t<1000){glitchFrame();return;}
  if(t<1350){display.clearDisplay();display.display();return;}
  if(t<2200){
    // Eyes are closed from the very first visible wake-up frame; hold, don't close them.
    requestedEmotion=SLEEPY;
    face.eyeH=7;
    face.eyeY=31;
    renderFace();return;
  }
  if(t<3400){
    // Slowly open from the already-closed pose.
    requestedEmotion=NEUTRAL;
    animateFace();
    renderFace();return;
  }
  waking=false;
  setEmotion(NEUTRAL);
  nextBlink=millis()+random(2200,5000);
  nextIdle=millis()+5000;
}

int16_t mpuWord(uint8_t reg){
  Wire.beginTransmission(MPU_ADDR);Wire.write(reg);
  if(Wire.endTransmission(false)!=0)return 0;
  if(Wire.requestFrom(MPU_ADDR,(uint8_t)2)!=2)return 0;
  return (int16_t)((Wire.read()<<8)|Wire.read());
}
void initMPU(){
  Wire.beginTransmission(MPU_ADDR);Wire.write(0x6B);Wire.write(0);
  mpuPresent=(Wire.endTransmission()==0);
  if(mpuPresent){
    delay(25);
    prevAx=mpuWord(0x3B)/16384.0f;prevAy=mpuWord(0x3D)/16384.0f;prevAz=mpuWord(0x3F)/16384.0f;
    filtAx=prevAx;filtAy=prevAy;filtAz=prevAz;
  }
  Serial.println(mpuPresent?"MPU6050 detected":"MPU6050 not detected; OLED mode");
}
void triggerShakeReaction(){
  uint32_t now=millis();
  shakePeaks=0;
  manualEmotion=false;
  recoveryStage=1;
  setEmotion(SCARED);
  mood=max(0,mood-10);
  lastInteraction=now; sleeping=false; sleepSequence=false;
  shakeRecovery=true;
  recoveryUntil=now+3000;
  shakeCooldown=now+2800;
  // Brief startled gaze; normal tilt tracking resumes after the reaction.
  gazeTargetX=0; gazeTargetY=-3;
  Serial.println("Mochi: shake detected! startled -> recovery");
}

void updateMotion(){
  uint32_t now=millis();
  if(!mpuPresent||now-lastMpu<25)return;
  lastMpu=now;
  float ax=mpuWord(0x3B)/16384.0f;
  float ay=mpuWord(0x3D)/16384.0f;
  float az=mpuWord(0x3F)/16384.0f;

  // Low-pass gravity estimate: tilt is measured from orientation, not sudden
  // acceleration. This prevents a static tilt from being mistaken for sadness.
  filtAx=0.82f*filtAx+0.18f*ax;
  filtAy=0.82f*filtAy+0.18f*ay;
  filtAz=0.82f*filtAz+0.18f*az;

  // Shake detection uses rapid acceleration changes (high-pass/jerk),
  // not the gravity estimate used for tilt. Require repeated peaks so a
  // single tilt or putting the robot down does not count as shaking.
  float delta=fabsf(ax-prevAx)+fabsf(ay-prevAy)+fabsf(az-prevAz);
  float accelMagnitude=sqrtf(ax*ax+ay*ay+az*az);
  prevAx=ax; prevAy=ay; prevAz=az;
  bool shakePeak = (delta > 1.00f) &&
                   (fabsf(accelMagnitude-1.0f) > 0.30f || delta > 1.35f);
  if(shakePeak && reached(now,shakeCooldown) && !shakeRecovery){
    if(now-lastShakePeak > 500) shakePeaks=0;
    lastShakePeak=now;
    if(shakePeaks < 4) shakePeaks++;
    if(shakePeaks >= 3){
      triggerShakeReaction();
    }
  }

  // Tilting only moves the pupils/eyes. It never changes the emotion.
  // Axis mapping preserves the previous X-axis convention, with stronger
  // travel and a small dead zone to avoid jitter around the centre.
  float tx=filtAx;
  float ty=filtAy;
  if(fabsf(tx)<0.12f)tx=0;
  if(fabsf(ty)<0.14f)ty=0;
  float gx=constrain(tx*25.0f,-17.0f,17.0f);
  float gy=constrain(-ty*21.0f,-12.0f,12.0f);
  tiltActive=(fabsf(tx)>0.07f || fabsf(ty)>0.07f);
   // Detect laying on either side: gravity is mostly horizontal on X/Y.
   bool nowSide = fabsf(filtAx)>0.72f || fabsf(filtAy)>0.72f;
   if(nowSide){ if(!sidePosition) sideSince=now; sidePosition=true; }
   else { sidePosition=false; sideSince=0; }
  // Only let the IMU take control while the device is actually tilted.
  // Otherwise autonomous glances and serial gaze tests remain in control.
  if(tiltActive){
    gazeTargetX=gx;
    gazeTargetY=gy;
  }
}

void updateCharacter(){
  uint32_t now=millis();
  if(shakeRecovery && reached(now,recoveryUntil)){
    if(recoveryStage==1){
      recoveryStage=2;
      setEmotion(SURPRISED);
      recoveryUntil=now+2000;
    } else {
      recoveryStage=0;
      shakeRecovery=false;
      setEmotion(NEUTRAL);
    }
  }
  if(!shakeRecovery && emotionUntil&&reached(now,emotionUntil)){emotionUntil=0;setEmotion(NEUTRAL);}

  // Sleep after 2 minutes of inactivity, or after lying on a side for 12 seconds.
  if(!sleeping && !sleepSequence && !shakeRecovery && !emotionUntil){
    if((sidePosition && sideSince && now-sideSince>=12000) ||
       (lastInteraction && now-lastInteraction>=120000)){
      sleepSequence=true;
      sleepStageAt=now;
      setEmotion(SLEEPY);
    }
  }
  if(sleepSequence){
    uint32_t elapsed=now-sleepStageAt;
    if(elapsed<10000){
      if((elapsed%2600)<850) blinkEnd=now+90;
    } else if(elapsed<20000){
      uint32_t phase=elapsed-10000;
      if(phase<3500) face.eyeH=smooth(face.eyeH,18.0f,0.025f);
      else if(phase<5200) face.eyeH=smooth(face.eyeH,31.0f,0.018f);
      else face.eyeH=smooth(face.eyeH,3.0f,0.025f);
    } else {
      sleeping=true; sleepSequence=false; setEmotion(SLEEPY); face.eyeH=3;
    }
  }

  // Mood slowly drifts toward neutral, but interaction changes it immediately.
  if(now-lastMoodUpdate>12000){
    lastMoodUpdate=now;
    if(mood>58)mood--; else if(mood<42)mood++;
  }

  // Natural eye movement: glance left/right/up, pause, then return to center.
  if(reached(now,nextLook)&&!shakeRecovery&&!tiltActive&&reached(now,gazeUntil)){
    int glance=random(0,6);
    if(glance==0){gazeTargetX=-11;gazeTargetY=0;}
    else if(glance==1){gazeTargetX=11;gazeTargetY=0;}
    else if(glance==2){gazeTargetX=0;gazeTargetY=-3;}
    else if(glance==3){gazeTargetX=0;gazeTargetY=2;}
    else {gazeTargetX=0;gazeTargetY=0;}
    gazeUntil=now+random(500,1100);
    nextLook=now+random(1700,3600);
  }
  if(reached(now,gazeUntil)){gazeTargetX=0;gazeTargetY=0;}
  gazeX=smooth(gazeX,gazeTargetX,0.32f);
  gazeY=smooth(gazeY,gazeTargetY,0.32f);

  if(reached(now,nextBlink)){
    blinkEnd=now+110;
    nextBlink=now+random(2500,6500);
  }
  if(mood>=80 && (requestedEmotion==HAPPY || requestedEmotion==DELIGHTED) && reached(now,nextHappyMouth)){
    mouthStyle=random(1,4); nextHappyMouth=now+random(5000,12000);
  }
  if(!manualEmotion&&!shakeRecovery&&!emotionUntil&&reached(now,nextIdle)){
    int action=random(0,100);
    if(mood<20){
      if(action<45)setEmotion(SAD,1800);
      else if(action<75)setEmotion(SLEEPY,1800);
      else setEmotion(CURIOUS,1300);
    } else if(mood>=80){
      if(action<40){setEmotion(HAPPY,1700);mood=min(100,mood+2);}
      else if(action<65){setEmotion(DELIGHTED,1600);mood=min(100,mood+3);}
      else setEmotion(CURIOUS,1300);
    } else {
      if(action<22)setEmotion(CURIOUS,1400);
      else if(action<38){setEmotion(HAPPY,1500);mood=min(100,mood+1);}
      else if(action<52)setEmotion(SLEEPY,1600);
      else setEmotion(NEUTRAL,1200);
    }
    nextIdle=now+random(6500,12500);
  }
}

void triggerAffection(){
  uint32_t now=millis();
  mood=min(100,mood+20);
  lastInteraction=now;
  sleeping=false; sleepSequence=false;
  manualEmotion=false; shakeRecovery=false; recoveryStage=0; emotionUntil=0;
  setEmotion(AFFECTION,3200);
  mouthStyle=random(0,2)?2:3;
  nextIdle=now+2200;
  Serial.printf("Mochi: AFFECTION! mood=%d/100\n",mood);
}

void updateButton(){
  bool readingHigh = (digitalRead(PIN_BUTTON) != LOW);
  uint32_t now = millis();
  if(readingHigh != buttonLastReadHigh){
    buttonLastReadHigh = readingHigh;
    buttonChangedAt = now;
  }
  if(now - buttonChangedAt > 35 && readingHigh != buttonStableHigh){
    buttonStableHigh = readingHigh;
    if(!buttonStableHigh){
      triggerAffection();
    }
  }
}

void printHelp(){
  Serial.println("MOCHI TEST COMMANDS:");
  Serial.println("1 angry | 2 sad | 3 surprised | 4 neutral | 5 happy | 6 star-eyes");
  Serial.println("7 sleepy | 8 curious | 9 scared | a auto mode | + mood up | - mood down");
  Serial.println("l/r/u/d/c gaze left/right/up/down/center | b blink | m mood value | ? help");
  Serial.println("s simulate shake reaction | GPIO25 button to GND = affection");
}
void serialCommands(){
  while(Serial.available()){
    char c=Serial.read();
    switch(c){
      case '1': manualEmotion=true;setEmotion(ANGRY);break;
      case '2': manualEmotion=true;setEmotion(SAD);break;
      case '3': manualEmotion=true;setEmotion(SURPRISED);break;
      case '4': manualEmotion=true;setEmotion(NEUTRAL);break;
      case '5': manualEmotion=true;setEmotion(HAPPY);break;
      case '6': manualEmotion=true;setEmotion(DELIGHTED);break;
      case '7': manualEmotion=true;setEmotion(SLEEPY);break;
      case '8': manualEmotion=true;setEmotion(CURIOUS);break;
      case '9': manualEmotion=true;setEmotion(SCARED);break;
      case 'a': manualEmotion=false;emotionUntil=0;setEmotion(NEUTRAL);nextIdle=millis()+1500;break;
      case '+': mood=min(100,mood+10);Serial.printf("mood=%d\n",mood);break;
      case '-': mood=max(0,mood-10);Serial.printf("mood=%d\n",mood);break;
      case 'm': Serial.printf("mood=%d/100\n",mood);break;
      case 'b': blinkEnd=millis()+180;break;
      case 's': triggerShakeReaction();break;
      case 'p': triggerAffection();break;
      case 'l': gazeTargetX=-13;gazeTargetY=0;tiltActive=false;gazeUntil=millis()+1200;break;
      case 'r': gazeTargetX=13;gazeTargetY=0;tiltActive=false;gazeUntil=millis()+1200;break;
      case 'u': gazeTargetX=0;gazeTargetY=-9;tiltActive=false;gazeUntil=millis()+1200;break;
      case 'd': gazeTargetX=0;gazeTargetY=9;tiltActive=false;gazeUntil=millis()+1200;break;
      case 'c': gazeTargetX=0;gazeTargetY=0;tiltActive=false;gazeUntil=0;break;
      case '?': printHelp();break;
    }
  }
}

void setup(){
  Serial.begin(115200);randomSeed(esp_random());
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  Wire.begin(PIN_SDA,PIN_SCL);Wire.setClock(100000);
  if(!display.begin(SSD1306_SWITCHCAPVCC,OLED_ADDR)){
    Serial.println("OLED init failed; check SSD1306/address");while(true)delay(100);
  }
  display.clearDisplay();display.display();
  initMPU();bootAt=millis();lastMoodUpdate=bootAt;lastInteraction=bootAt;
  nextLook=millis()+1800;
  printHelp();
  Serial.println("Tilt controls gaze only; it does not select sad/happy emotions.");
}
void loop(){
  uint32_t now=millis();
  if(now-lastFrame>=33){
    lastFrame=now;
    if(waking)startup();
    else {updateMotion();updateButton();updateCharacter();animateFace();renderFace();}
  }
  serialCommands();
}
