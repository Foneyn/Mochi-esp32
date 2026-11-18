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
uint32_t nextLook=0, gazeUntil=0, lastMoodUpdate=0;
float prevAx=0,prevAy=0,prevAz=0, gazeX=0,gazeY=0;
float filtAx=0, filtAy=0, filtAz=1;
bool tiltActive=false;
uint32_t shakeCooldown=0, lastShakePeak=0;
uint32_t buttonChangedAt=0;
bool buttonStableHigh=true, buttonLastReadHigh=true;
uint8_t shakePeaks=0;
float gazeTargetX=0, gazeTargetY=0;
int mood=60; // 0 = gloomy, 100 = delighted

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
  target={35,46,14,gazeX,gazeY,0,0};
  switch(requestedEmotion){
    case ANGRY:
      // A strong inward brow: the inner upper corners are cut deeply and the eyes slope down inward.
      target.eyeH=27; target.eyeY=19; target.tilt=5; target.smile=-1.0f;
      break;
    case SAD:
      target.eyeH=29; target.eyeY=18; target.pupilY=gazeY+7; target.smile=-0.85f;
      break;
    case SURPRISED: target.eyeH=46; target.eyeW=43; target.eyeY=7; break;
    case NEUTRAL: break;
    case HAPPY: target.eyeH=39; target.eyeY=12; target.smile=1; break;
    case DELIGHTED: target.eyeH=42; target.eyeY=10; target.smile=1; break;\n    case AFFECTION: target.eyeH=45; target.eyeW=48; target.eyeY=8; target.smile=1.0f; break;
    case SCARED: target.eyeH=43; target.eyeW=43; target.eyeY=9; break;
    case SLEEPY: target.eyeH=15; target.eyeY=28; break;
    case CURIOUS: target.eyeH=38; target.eyeY=12; target.tilt=2; break;
  }
  // Mood affects the face continuously, not only the occasional idle emotion.
  // Keep manually selected emotions recognizable while changing their intensity.
  if(!shakeRecovery){
    float low=constrain((50.0f-mood)/50.0f,0.0f,1.0f);
    float high=constrain((mood-50.0f)/50.0f,0.0f,1.0f);
    target.eyeY += low*5.0f - high*2.0f;
    target.eyeH += high*3.0f - low*4.0f;
    target.smile += high*0.85f - low*0.45f;
    target.smile=constrain(target.smile,-1.0f,1.0f);
    if(requestedEmotion==NEUTRAL && low>0.45f) target.smile=-0.45f;
    if(requestedEmotion==NEUTRAL && high>0.45f) target.smile=0.45f;
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
  if(fabsf(amount)<0.08f)return;
  // Small smooth arc. Positive is a smile; negative is an inverted smile/frown.
  int width=19;
  int x0=64-width/2, x1=64+width/2;
  int baseY=55;
  int bend=(int)(5.0f*amount);
  int prevX=x0, prevY=baseY;
  for(int i=1;i<=width;i++){
    float t=(float)i/width;
    int x=x0+i;
    int y=baseY+(int)(bend*4.0f*t*(1.0f-t));
    display.drawLine(prevX,prevY,x,y,SSD1306_WHITE);
    prevX=x; prevY=y;
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
  int leftW=curious?min(56,ew+9):ew;
  int rightW=curious?max(29,ew-12):ew;
  int shakeX=scared?(int)(sinf(now*0.075f)*4.0f):0;
  int shakeY=scared?(int)(sinf(now*0.11f)*3.0f):0;
  drawOneEye(leftCx,cy,leftW,eh,(int)face.tilt,(int)face.pupilX+shakeX,(int)face.pupilY+shakeY,blink,
             angry,sad,stars,scared);
  drawOneEye(rightCx,cy,rightW,eh,-(int)face.tilt,(int)face.pupilX-shakeX,(int)face.pupilY-shakeY,blink,
             angry,sad,stars,scared);
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
  mood=max(0,mood-8);
  shakeRecovery=true;
  recoveryUntil=now+1100;
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
  tiltActive=(fabsf(tx)>0.12f || fabsf(ty)>0.14f);
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
    // A deliberate emotional arc: startled -> angry -> sad -> calm.
    // Each face morphs through the normal animation interpolation.
    if(recoveryStage==1){
      recoveryStage=2;
      setEmotion(ANGRY);
      recoveryUntil=now+1250;
    } else if(recoveryStage==2){
      recoveryStage=3;
      setEmotion(SAD);
      recoveryUntil=now+1900;
    } else {
      recoveryStage=0;
      shakeRecovery=false;
      setEmotion(NEUTRAL);
    }
  }
  if(emotionUntil&&reached(now,emotionUntil)){emotionUntil=0;setEmotion(NEUTRAL);}

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
  if(!manualEmotion&&!shakeRecovery&&!emotionUntil&&reached(now,nextIdle)){
    int action=random(0,100);
    if(mood<30){
      if(action<45)setEmotion(SAD,1800);
      else if(action<75)setEmotion(SLEEPY,1800);
      else setEmotion(CURIOUS,1300);
    } else if(mood>75){
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
      // A press triggers the dedicated affectionate blush expression.
      mood = min(100, mood + 9);
      manualEmotion = false;
      shakeRecovery = false;
      recoveryStage = 0;
      emotionUntil = 0;
      setEmotion(AFFECTION, 2400);
      nextIdle = now + 2200;
      Serial.printf("Mochi: affection button! mood=%d/100\\n", mood);
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
  initMPU();bootAt=millis();lastMoodUpdate=bootAt;
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
