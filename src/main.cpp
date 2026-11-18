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
float prevAx=0,prevAy=0,prevAz=0, gazeX=0,gazeY=0;
int mood=60;

// Animated face parameters. Every emotion changes targets, never hard-cuts.
struct Face {
  float eyeH, eyeW, eyeY, pupilX, pupilY, tilt, smile;
};
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
    case HAPPY: target.eyeH=39; target.eyeY=12; target.smile=1; break;
    case DELIGHTED: target.eyeH=42; target.eyeY=10; target.smile=1; break;
    case SURPRISED: target.eyeH=45; target.eyeW=43; target.eyeY=8; break;
    case SCARED: target.eyeH=43; target.eyeW=43; target.eyeY=9; target.tilt=1; break;
    case ANGRY: target.eyeH=25; target.eyeY=21; target.tilt=5; break;
    case SAD: target.eyeH=27; target.eyeY=19; target.tilt=-4; break;
    case SLEEPY: target.eyeH=15; target.eyeY=28; break;
    case CURIOUS: target.eyeH=38; target.eyeY=12; target.tilt=2; break;
    default: break;
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
void drawOneEye(int cx,int cy,int ew,int eh,int tilt,int px,int py,bool closed){
  int x=cx-ew/2, y=cy-eh/2;
  if(closed || eh<5){
    display.drawLine(x+3,cy,x+ew-3,cy,SSD1306_WHITE);
    return;
  }
  int radius=min(ew/3,eh/2);
  display.fillRoundRect(x,y,ew,eh,radius,SSD1306_WHITE);
  // Carve angled upper corners to suggest brow/expression using eye shape.
  if(tilt>0){
    for(int i=0;i<tilt;i++){
      display.drawFastHLine(x,y+i,ew/3-i/2,SSD1306_BLACK);
      display.drawFastHLine(x+ew-ew/3+i/2,y+i,ew/3-i/2,SSD1306_BLACK);
    }
  } else if(tilt<0){
    int n=-tilt;
    for(int i=0;i<n;i++){
      display.drawFastHLine(x,y+eh-n+i,ew/3-i/2,SSD1306_BLACK);
      display.drawFastHLine(x+ew-ew/3+i/2,y+eh-n+i,ew/3-i/2,SSD1306_BLACK);
    }
  }
  int pr=max(3,min(7,eh/5));
  int pupilCx=clampi(cx+px,x+pr+2,x+ew-pr-2);
  int pupilCy=clampi(cy+py,y+pr+2,y+eh-pr-2);
  display.fillCircle(pupilCx,pupilCy,pr,SSD1306_BLACK);
  display.fillCircle(pupilCx-2,pupilCy-2,1,SSD1306_WHITE);
}
void drawTinySmile(float amount){
  if(amount<0.08f)return;
  // Small, understated smile; fades in by drawing progressively.
  int width= (int)(9+amount*5);
  int x=64-width/2;
  display.drawPixel(x,55,SSD1306_WHITE);
  display.drawLine(x,55,x+width/2,58,SSD1306_WHITE);
  display.drawLine(x+width/2,58,x+width,55,SSD1306_WHITE);
}
void renderFace(){
  display.clearDisplay();
  bool blink=!reached(millis(),blinkEnd);
  int ew=(int)face.eyeW, eh=(int)face.eyeH;
  int cy=(int)(face.eyeY+eh/2);
  int tilt=(int)face.tilt;
  int leftCx=34, rightCx=94;
  drawOneEye(leftCx,cy,ew,eh,tilt,(int)face.pupilX,(int)face.pupilY,blink);
  drawOneEye(rightCx,cy,ew,eh,-tilt,(int)face.pupilX,(int)face.pupilY,blink);
  drawTinySmile(face.smile);
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
    setEmotion(SCARED,1500);mood=max(0,mood-8);
    shakeRecovery=true;recoveryUntil=now+1500;return;
  }
  if(!shakeRecovery){
    gazeX=ax>0.55f?5:(ax< -0.55f?-5:0);
    gazeY=ay>0.60f?3:0;
    if(fabsf(ax)>0.55f)setEmotion(CURIOUS,650);
  }
}
void updateCharacter(){
  uint32_t now=millis();
  if(shakeRecovery&&reached(now,recoveryUntil)){shakeRecovery=false;setEmotion(SAD,3500);}
  if(emotionUntil&&reached(now,emotionUntil)){emotionUntil=0;setEmotion(NEUTRAL);}
  if(reached(now,nextBlink)){
    blinkEnd=now+110;
    nextBlink=now+random(2500,6500);
  }
  if(!shakeRecovery&&!emotionUntil&&reached(now,nextIdle)){
    switch(random(0,5)){
      case 0:setEmotion(CURIOUS,1500);gazeX=random(-5,6);break;
      case 1:setEmotion(HAPPY,1700);mood=min(100,mood+2);break;
      case 2:setEmotion(SLEEPY,1800);break;
      case 3:setEmotion(DELIGHTED,1500);mood=min(100,mood+3);break;
      default:setEmotion(NEUTRAL,1300);gazeX=0;gazeY=0;break;
    }
    nextIdle=now+random(7500,14000);
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
    case 'b':blinkEnd=millis()+180;break;
    case 'l':gazeX=-6;break;
    case 'r':gazeX=6;break;
    case 'c':gazeX=0;gazeY=0;break;
  }
}
void setup(){
  Serial.begin(115200);randomSeed(esp_random());
  Wire.begin(PIN_SDA,PIN_SCL);Wire.setClock(100000);
  if(!display.begin(SSD1306_SWITCHCAPVCC,OLED_ADDR)){
    Serial.println("OLED init failed; check SSD1306/address");while(true)delay(100);
  }
  display.clearDisplay();display.display();
  initMPU();bootAt=millis();
  Serial.println("MOCHI 1.1 ready | 0 neutral,1 happy,2 angry,3 surprised,4 sleepy,5 sad,6 delighted,7 curious,b blink,l/r/c gaze");
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
