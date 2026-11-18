#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <math.h>

// Mochi 1.1 — large, fluid eyes; visual inspiration: Plexynex
// Adapted for ESP32-WROOM-32 + SSD1306 128x64 OLED.
// I2C: SDA=21, SCL=18. MPU6050 address=0x68 (optional).

constexpr int PIN_SDA = 21, PIN_SCL = 18;
constexpr uint8_t OLED_ADDR = 0x3C, MPU_ADDR = 0x68;
constexpr int W = 128, H = 64;
Adafruit_SSD1306 display(W, H, &Wire, -1);

enum Emotion : uint8_t { NEUTRAL, HAPPY, DELIGHTED, SURPRISED, ANGRY, SAD, SLEEPY, CURIOUS, SCARED };
Emotion requestedEmotion = NEUTRAL;
bool mpuPresent=false, waking=true, shakeRecovery=false;
uint32_t bootAt=0, lastFrame=0, lastMpu=0, emotionUntil=0, recoveryUntil=0, nextBlink=0, blinkEnd=0, nextIdle=0;
uint32_t nextLook=0, gazeUntil=0, lastMoodUpdate=0;
float prevAx=0,prevAy=0,prevAz=0, gazeX=0,gazeY=0;
float gazeTargetX=0, gazeTargetY=0;
int mood=60; // 0 = gloomy, 100 = delighted

// Animated face parameters. Every emotion changes targets, never hard-cuts.
struct Face {
  float eyeH, eyeW, eyeY, pupilX, pupilY, tilt, smile;
};
// smile: +1 = tiny smile, -1 = tiny frown, 0 = no mouth.
Face face={35,46,14,0,0,0,0};
Face target={35,46,14,0,0,0,0};

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
      // Anger: narrowed eyes with soft inward upper-corner cuts and a frown.
      target.eyeH=25; target.eyeY=20; target.tilt=1; target.smile=-1;
      break;
    case SAD:
      // Sadness: softly clipped outer upper corners, lowered pupils, tiny frown.
      target.eyeH=29; target.eyeY=18; target.pupilY=gazeY+7; target.smile=-0.75f;
      break;
    case SURPRISED: target.eyeH=45; target.eyeW=43; target.eyeY=8; break;
    case NEUTRAL: break;
    case HAPPY: target.eyeH=39; target.eyeY=12; target.smile=1; break;
    case DELIGHTED: target.eyeH=42; target.eyeY=10; target.smile=1; break;
    case SCARED: target.eyeH=43; target.eyeW=43; target.eyeY=9; target.tilt=1; break;
    case SLEEPY: target.eyeH=15; target.eyeY=28; break;
    case CURIOUS: target.eyeH=38; target.eyeY=12; target.tilt=2; break;
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
void drawOneEye(int cx,int cy,int ew,int eh,int tilt,int px,int py,bool closed,
                bool cutInnerTop,bool cutOuterTop){
  int x=cx-ew/2, y=cy-eh/2;
  if(closed || eh<5){
    // Rounded-looking closed eyelid: short horizontal line with soft end caps.
    display.drawFastHLine(x+5,cy,ew-10,SSD1306_WHITE);
    display.fillCircle(x+5,cy,1,SSD1306_WHITE);
    display.fillCircle(x+ew-5,cy,1,SSD1306_WHITE);
    return;
  }
  int radius=min(ew/3,eh/2);
  display.fillRoundRect(x,y,ew,eh,radius,SSD1306_WHITE);

  // Concave, fully rounded corner cuts create expression without sharp edges.
  // A black circle softly trims the selected upper corner of each eye.
  int cutR=max(5,min(9,eh/3));
  if(cutInnerTop){
    int cutX=(cx<64)?(x+ew+cutR-2):(x-cutR+2);
    display.fillCircle(cutX,y+2,cutR,SSD1306_BLACK);
  }
  if(cutOuterTop){
    int cutX=(cx<64)?(x-cutR+2):(x+ew+cutR-2);
    display.fillCircle(cutX,y+2,cutR,SSD1306_BLACK);
  }

  int pr=max(3,min(7,eh/5));
  int pupilCx=clampi(cx+px,x+pr+4,x+ew-pr-4);
  int pupilCy=clampi(cy+py,y+pr+3,y+eh-pr-3);
  display.fillCircle(pupilCx,pupilCy,pr,SSD1306_BLACK);
  display.fillCircle(pupilCx-2,pupilCy-2,1,SSD1306_WHITE);
}
void drawMouth(float amount){
  if(fabsf(amount)<0.08f)return;
  // Small smooth arc. Positive is a smile; negative is an inverted smile/frown.
  int width=13;
  int x0=64-width/2, x1=64+width/2;
  int baseY=55;
  int bend=(int)(3.0f*amount);
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
  bool blink=!reached(millis(),blinkEnd);
  int ew=(int)face.eyeW, eh=(int)face.eyeH;
  int cy=(int)(face.eyeY+eh/2);
  int tilt=(int)face.tilt;
  int leftCx=34, rightCx=94;
  bool angry=(requestedEmotion==ANGRY);
  bool sad=(requestedEmotion==SAD);
  // Angry: trim the upper inner corners (like brows). Sad: trim the upper outer corners.
  drawOneEye(leftCx,cy,ew,eh,tilt,(int)face.pupilX,(int)face.pupilY,blink,
             angry,sad);
  drawOneEye(rightCx,cy,ew,eh,-tilt,(int)face.pupilX,(int)face.pupilY,blink,
             angry,sad);
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
    requestedEmotion=SLEEPY;
    face.eyeH=smooth(face.eyeH,7,0.12f);
    face.eyeY=smooth(face.eyeY,31,0.12f);
    renderFace();return;
  }
  if(t<3400){
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
  }
  Serial.println(mpuPresent?"MPU6050 detected":"MPU6050 not detected; OLED mode");
}
void updateMotion(){
  uint32_t now=millis();
  if(!mpuPresent||now-lastMpu<35)return;
  lastMpu=now;
  float ax=mpuWord(0x3B)/16384.0f, ay=mpuWord(0x3D)/16384.0f, az=mpuWord(0x3F)/16384.0f;
  float delta=fabsf(ax-prevAx)+fabsf(ay-prevAy)+fabsf(az-prevAz);
  prevAx=ax;prevAy=ay;prevAz=az;
  if(delta>0.85f&&!shakeRecovery){
    setEmotion(SCARED,1500);mood=max(0,mood-12);
    shakeRecovery=true;recoveryUntil=now+1500;return;
  }
  if(!shakeRecovery){
    if(ax>0.55f){gazeTargetX=6;gazeTargetY=0;gazeUntil=now+900;setEmotion(CURIOUS,650);}
    else if(ax< -0.55f){gazeTargetX=-6;gazeTargetY=0;gazeUntil=now+900;setEmotion(CURIOUS,650);}
    if(ay>0.60f){gazeTargetY=3;gazeUntil=now+900;}
    if(reached(now,gazeUntil)){gazeTargetX=0;gazeTargetY=0;}
  }
}
void updateCharacter(){
  uint32_t now=millis();
  if(shakeRecovery&&reached(now,recoveryUntil)){
    shakeRecovery=false; mood=max(0,mood-5); setEmotion(SAD,3500);
  }
  if(emotionUntil&&reached(now,emotionUntil)){emotionUntil=0;setEmotion(NEUTRAL);}

  // Mood slowly drifts toward neutral, but interaction changes it immediately.
  if(now-lastMoodUpdate>12000){
    lastMoodUpdate=now;
    if(mood>55)mood--; else if(mood<45)mood++;
  }

  // Natural eye movement: glance left/right/up, pause, then return to center.
  if(reached(now,nextLook)&&!shakeRecovery&&reached(now,gazeUntil)){
    int glance=random(0,6);
    if(glance==0){gazeTargetX=-7;gazeTargetY=0;}
    else if(glance==1){gazeTargetX=7;gazeTargetY=0;}
    else if(glance==2){gazeTargetX=0;gazeTargetY=-3;}
    else if(glance==3){gazeTargetX=0;gazeTargetY=2;}
    else {gazeTargetX=0;gazeTargetY=0;}
    gazeUntil=now+random(500,1100);
    nextLook=now+random(1700,3600);
  }
  if(reached(now,gazeUntil)){gazeTargetX=0;gazeTargetY=0;}
  gazeX=smooth(gazeX,gazeTargetX,0.16f);
  gazeY=smooth(gazeY,gazeTargetY,0.16f);

  if(reached(now,nextBlink)){
    blinkEnd=now+110;
    nextBlink=now+random(2500,6500);
  }
  if(!shakeRecovery&&!emotionUntil&&reached(now,nextIdle)){
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
void serialCommands(){
  if(!Serial.available())return;
  char c=Serial.read();
  switch(c){
    case '0':setEmotion(NEUTRAL);break;
    case '1':setEmotion(HAPPY);break;
    case '2':setEmotion(ANGRY);break;
    case '3':setEmotion(SURPRISED);break;
    case '4':setEmotion(SLEEPY);break;
    case '5':setEmotion(SAD);break;
    case '6':setEmotion(DELIGHTED);break;
    case '7':setEmotion(CURIOUS);break;
    case '8':mood=min(100,mood+10);setEmotion(HAPPY,1800);break;
    case '9':mood=max(0,mood-10);setEmotion(SAD,1800);break;
    case 'b':blinkEnd=millis()+180;break;
    case 'l':gazeTargetX=-7;gazeUntil=millis()+1200;break;
    case 'r':gazeTargetX=7;gazeUntil=millis()+1200;break;
    case 'c':gazeTargetX=0;gazeTargetY=0;gazeUntil=0;break;
  }
}
void setup(){
  Serial.begin(115200);randomSeed(esp_random());
  Wire.begin(PIN_SDA,PIN_SCL);Wire.setClock(100000);
  if(!display.begin(SSD1306_SWITCHCAPVCC,OLED_ADDR)){
    Serial.println("OLED init failed; check SSD1306/address");while(true)delay(100);
  }
  display.clearDisplay();display.display();
  initMPU();bootAt=millis();lastMoodUpdate=bootAt;
  nextLook=millis()+1800;
  Serial.println("MOCHI 1.2 ready | 0 neutral,1 happy,2 angry,3 surprised,4 sleepy,5 sad,6 delighted,7 curious,8 mood+,9 mood-,b blink,l/r/c gaze");
}
void loop(){
  uint32_t now=millis();
  if(now-lastFrame>=33){
    lastFrame=now;
    if(waking)startup();
    else {updateMotion();updateCharacter();animateFace();renderFace();}
  }
  serialCommands();
}
