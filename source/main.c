#include <stdio.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <sys/statvfs.h>
#include <arpa/inet.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <switch.h>

#define SCREEN_W  1280
#define SCREEN_H  720
#define PAD       28
#define ROW_H     36
#define TITLE_H   68
#define TABS_H    38
#define CONTENT_Y (TITLE_H + TABS_H + 2)
#define FOOTER_Y  (SCREEN_H - 44)
#define LABEL_X   (PAD + 6)
#define VALUE_X   (PAD + 340)
#define BAR_W     (SCREEN_W - VALUE_X - PAD - 10)

typedef SDL_Color Col;
#define MK(r,g,b) ((Col){r,g,b,255})
static const Col BG     = MK(12,12,18);
static const Col TITLEC = MK(20,20,30);
static const Col TABC   = MK(16,16,24);
static const Col SEPC   = MK(38,38,54);
static const Col ACCENT = MK(80,140,255);
static const Col WHITE  = MK(228,228,238);
static const Col GRAY   = MK(108,108,128);
static const Col GREEN  = MK(68,198,108);
static const Col YELLOW = MK(252,192,52);
static const Col RED    = MK(252,72,72);
static const Col CYAN   = MK(58,208,208);
static const Col PURPLE = MK(178,98,252);
static const Col ORANGE = MK(252,158,48);

typedef enum { T_SYSTEM=0,T_HARDWARE,T_POWER,T_STORAGE,T_NETWORK,T_CTRL,T_MOTION,T_COUNT } Tab;
static const char *TNAMES[T_COUNT] = {"System","Hardware","Power","Storage","Network","Controllers","Motion"};

static SDL_Renderer *R;
static TTF_Font *fSm,*fLbl,*fVal,*fTitle,*fTab;

static void fill(int x,int y,int w,int h,Col c){
    SDL_SetRenderDrawColor(R,c.r,c.g,c.b,255);
    SDL_Rect r={x,y,w,h}; SDL_RenderFillRect(R,&r);
}
static void hline(int y,Col c){
    SDL_SetRenderDrawColor(R,c.r,c.g,c.b,255);
    SDL_RenderDrawLine(R,0,y,SCREEN_W,y);
}
static void drawText(TTF_Font *f,const char *s,int x,int y,Col c){
    if(!s||!s[0])return;
    SDL_Surface *sf=TTF_RenderUTF8_Blended(f,s,c); if(!sf)return;
    SDL_Texture *t=SDL_CreateTextureFromSurface(R,sf);
    SDL_Rect d={x,y,sf->w,sf->h}; SDL_FreeSurface(sf);
    if(!t)return; SDL_RenderCopy(R,t,NULL,&d); SDL_DestroyTexture(t);
}
static int textW(TTF_Font *f,const char *s){int w=0;TTF_SizeUTF8(f,s,&w,NULL);return w;}
static void drawRow(int y,const char *lbl,const char *val,Col vc){
    SDL_SetRenderDrawColor(R,SEPC.r,SEPC.g,SEPC.b,255);
    SDL_RenderDrawLine(R,PAD,y+ROW_H-1,SCREEN_W-PAD,y+ROW_H-1);
    drawText(fLbl,lbl,LABEL_X,y+(ROW_H-19)/2,GRAY);
    drawText(fVal,val,VALUE_X,y+(ROW_H-22)/2,vc);
}
static void drawBar(int y,float pct,Col c){
    int by=y+(ROW_H-14)/2;
    if(pct<0)pct=0; if(pct>1)pct=1;
    fill(VALUE_X,by,BAR_W,14,SEPC);
    int fw=(int)(pct*BAR_W); if(fw>0)fill(VALUE_X,by,fw,14,c);
    SDL_SetRenderDrawColor(R,55,55,75,255);
    SDL_Rect o={VALUE_X,by,BAR_W,14}; SDL_RenderDrawRect(R,&o);
}
static void drawSwatch(int x,int y,u32 rgb){
    Col c={(rgb>>16)&0xFF,(rgb>>8)&0xFF,rgb&0xFF,255};
    fill(x,y+5,24,24,c);
    SDL_SetRenderDrawColor(R,80,80,100,255);
    SDL_Rect r={x,y+5,24,24}; SDL_RenderDrawRect(R,&r);
}

int main(void){
    SDL_Init(SDL_INIT_VIDEO|SDL_INIT_JOYSTICK);
    TTF_Init();
    SDL_Window *win=SDL_CreateWindow("sysinfo",0,0,SCREEN_W,SCREEN_H,SDL_WINDOW_FULLSCREEN);
    R=SDL_CreateRenderer(win,-1,SDL_RENDERER_ACCELERATED|SDL_RENDERER_PRESENTVSYNC);

    plInitialize(PlServiceType_User);
    PlFontData fd; plGetSharedFontByType(&fd,PlSharedFontType_Standard);
    fSm    = TTF_OpenFontRW(SDL_RWFromMem(fd.address,fd.size),1,15);
    fLbl   = TTF_OpenFontRW(SDL_RWFromMem(fd.address,fd.size),1,18);
    fVal   = TTF_OpenFontRW(SDL_RWFromMem(fd.address,fd.size),1,21);
    fTitle = TTF_OpenFontRW(SDL_RWFromMem(fd.address,fd.size),1,30);
    fTab   = TTF_OpenFontRW(SDL_RWFromMem(fd.address,fd.size),1,16);

    padConfigureInput(8,HidNpadStyleSet_NpadStandard);
    PadState pad; padInitializeAny(&pad);

    bool psmOk  = R_SUCCEEDED(psmInitialize());
    bool clkOk  = R_SUCCEEDED(clkrstInitialize());
    bool tsOk   = R_SUCCEEDED(tsInitialize());
    bool sysOk  = R_SUCCEEDED(setsysInitialize());
    bool apmOk  = R_SUCCEEDED(apmInitialize());
    bool lblOk  = R_SUCCEEDED(lblInitialize());
    bool nifmOk = R_SUCCEEDED(nifmInitialize(NifmServiceType_System));
    bool timeOk = R_SUCCEEDED(timeInitialize());
    bool setOk  = R_SUCCEEDED(setInitialize());

    ClkrstSession cpuS={0},gpuS={0},emcS={0};
    if(clkOk){
        clkrstOpenSession(&cpuS,PcvModuleId_CpuBus,3);
        clkrstOpenSession(&gpuS,PcvModuleId_GPU,3);
        clkrstOpenSession(&emcS,PcvModuleId_EMC,3);
    }

    HidSixAxisSensorHandle gyroH[2]={0}; int gyroN=0;
    if(R_SUCCEEDED(hidGetSixAxisSensorHandles(gyroH,2,HidNpadIdType_No1,HidNpadStyleTag_NpadJoyDual))){
        hidStartSixAxisSensor(gyroH[0]); hidStartSixAxisSensor(gyroH[1]); gyroN=2;
    } else if(R_SUCCEEDED(hidGetSixAxisSensorHandles(gyroH,1,HidNpadIdType_Handheld,HidNpadStyleTag_NpadHandheld))){
        hidStartSixAxisSensor(gyroH[0]); gyroN=1;
    }

    SetSysFirmwareVersion fw={0}; if(sysOk) setsysGetFirmwareVersion(&fw);
    SetSysDeviceNickName nick={0}; if(sysOk) setsysGetDeviceNickname(&nick);
    SetSysProductModel model=SetSysProductModel_Invalid; if(sysOk) setsysGetProductModel(&model);
    u64 langCode=0; if(setOk) setGetLanguageCode(&langCode);
    char langStr[9]={0}; memcpy(langStr,&langCode,8);
    SetRegion region=0; if(setOk) setGetRegionCode(&region);

    int curTab=0;
    char buf[256];

    while(appletMainLoop()){
        padUpdate(&pad);
        u64 kd=padGetButtonsDown(&pad);
        if(kd&HidNpadButton_Plus) break;
        if(kd&(HidNpadButton_R|HidNpadButton_Right)) curTab=(curTab+1)%T_COUNT;
        if(kd&(HidNpadButton_L|HidNpadButton_Left))  curTab=(curTab-1+T_COUNT)%T_COUNT;

        u32 batPct=0; PsmChargerType charger=PsmChargerType_Unconnected;
        if(psmOk){psmGetBatteryChargePercentage(&batPct); psmGetChargerType(&charger);}

        u32 cpuHz=0,gpuHz=0,emcHz=0;
        if(clkOk){clkrstGetClockRate(&cpuS,&cpuHz);clkrstGetClockRate(&gpuS,&gpuHz);clkrstGetClockRate(&emcS,&emcHz);}

        s32 tempMC=0,temp2MC=0;
        if(tsOk){tsGetTemperatureMilliC(TsLocation_Internal,&tempMC);tsGetTemperatureMilliC(TsLocation_External,&temp2MC);}

        u64 totalMem=0,usedMem=0;
        svcGetInfo(&totalMem,InfoType_TotalMemorySize,CUR_PROCESS_HANDLE,0);
        svcGetInfo(&usedMem, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE,0);

        ApmPerformanceMode perfMode=ApmPerformanceMode_Invalid;
        if(apmOk) apmGetPerformanceMode(&perfMode);
        AppletOperationMode opMode=appletGetOperationMode();

        float bright=0; if(lblOk) lblGetCurrentBrightnessSetting(&bright);

        char timeStr[48]="N/A";
        if(timeOk){
            u64 posix=0;
            if(R_SUCCEEDED(timeGetCurrentTime(TimeType_UserSystemClock,&posix))){
                time_t t=(time_t)posix; struct tm *tm=localtime(&t);
                strftime(timeStr,sizeof(timeStr),"%Y-%m-%d  %H:%M:%S",tm);
            }
        }

        char sdTot[32]="N/A",sdFree[32]="N/A",sdUsed[32]="N/A"; float sdPct=0;
        struct statvfs sv;
        if(statvfs("sdmc:/",&sv)==0){
            u64 tot=(u64)sv.f_blocks*sv.f_frsize, fr=(u64)sv.f_bfree*sv.f_frsize, us=tot-fr;
            sdPct=tot>0?(float)us/tot:0;
            snprintf(sdTot, sizeof(sdTot), "%.2f GB",tot/1e9);
            snprintf(sdFree,sizeof(sdFree),"%.2f GB",fr/1e9);
            snprintf(sdUsed,sizeof(sdUsed),"%.2f GB",us/1e9);
        }

        NifmInternetConnectionType connType=0; u32 wifiSig=0;
        NifmInternetConnectionStatus connStat=0; char ipStr[32]="N/A"; bool netOk=false;
        if(nifmOk&&R_SUCCEEDED(nifmGetInternetConnectionStatus(&connType,&wifiSig,&connStat))){
            netOk=(connStat==NifmInternetConnectionStatus_Connected);
            if(netOk){u32 ip=0;if(R_SUCCEEDED(nifmGetCurrentIpAddress(&ip))){struct in_addr a;a.s_addr=ip;strncpy(ipStr,inet_ntoa(a),sizeof(ipStr)-1);}}
        }

        HidSixAxisSensorState gst={0};
        if(gyroN>0) hidGetSixAxisSensorStates(gyroH[0],&gst,1);

        // --- RENDER ---
        SDL_SetRenderDrawColor(R,BG.r,BG.g,BG.b,255); SDL_RenderClear(R);

        // Title bar
        fill(0,0,SCREEN_W,TITLE_H,TITLEC);
        fill(0,0,4,TITLE_H,ACCENT);
        drawText(fTitle,"SysInfo",PAD+8,16,WHITE);
        snprintf(buf,sizeof(buf),"FW %s",fw.display_version);
        drawText(fSm,buf,SCREEN_W-PAD-textW(fSm,buf)-4,26,GRAY);

        // Tab bar
        fill(0,TITLE_H,SCREEN_W,TABS_H,TABC);
        int tw=SCREEN_W/T_COUNT;
        for(int i=0;i<T_COUNT;i++){
            int tx=i*tw;
            if(i==curTab) fill(tx,TITLE_H,tw,TABS_H,ACCENT);
            Col tc=i==curTab?WHITE:GRAY;
            int sw=textW(fTab,TNAMES[i]);
            drawText(fTab,TNAMES[i],tx+(tw-sw)/2,TITLE_H+(TABS_H-16)/2,tc);
        }
        hline(TITLE_H+TABS_H,ACCENT);
        hline(TITLE_H+TABS_H+1,SEPC);

        int cy=CONTENT_Y+2, r=0;
#define ROW(l,v,c) drawRow(cy+(r++)*ROW_H,l,v,c)
#define BAR(p,c)   drawBar(cy+(r++)*ROW_H,p,c)

        switch(curTab){

        case T_SYSTEM:{
            const char *mdl="Unknown";
            switch(model){
                case SetSysProductModel_Nx:     mdl="Nintendo Switch (HAC-001)"; break;
                case SetSysProductModel_Copper:  mdl="Nintendo Switch (Dev Unit)"; break;
                case SetSysProductModel_Iowa:    mdl="Nintendo Switch Lite (HDH-001)"; break;
                case SetSysProductModel_Hoag:    mdl="Nintendo Switch (HAC-001-01)"; break;
                case SetSysProductModel_Calcio:  mdl="Nintendo Switch OLED (HEG-001)"; break;
                case SetSysProductModel_Aula:    mdl="Nintendo Switch OLED (Revised)"; break;
                default: break;
            }
            ROW("Model",    mdl,                CYAN);
            ROW("Nickname", nick.nickname,        WHITE);
            ROW("Firmware", fw.display_version, WHITE);
            ROW("Language", langStr,            WHITE);
            const char *reg="Unknown";
            switch(region){
                case SetRegion_JPN: reg="Japan"; break;
                case SetRegion_USA: reg="Americas"; break;
                case SetRegion_EUR: reg="Europe"; break;
                case SetRegion_AUS: reg="Australia / NZ"; break;
                case SetRegion_HTK: reg="HK / TW / Korea"; break;
                case SetRegion_CHN: reg="China"; break;
                default: break;
            }
            ROW("Region",   reg, WHITE);
            ROW("Date / Time", timeStr, GREEN);
            snprintf(buf,sizeof(buf),"%.0f%%",bright*100.f);
            ROW("Brightness", buf, YELLOW);
            ROW("Mode", opMode==AppletOperationMode_Handheld?"Handheld":"Docked (TV)", WHITE);
            const char *perf="Unknown";
            switch(perfMode){
                case ApmPerformanceMode_Normal: perf="Normal"; break;
                case ApmPerformanceMode_Boost:  perf="Boost (Docked)"; break;
                default: break;
            }
            ROW("Perf Mode", perf, ACCENT);
            break;}

        case T_HARDWARE:{
            snprintf(buf,sizeof(buf),"%u MHz",cpuHz/1000000);
            ROW("CPU Clock",buf,ACCENT); BAR((float)cpuHz/1785600000.f,ACCENT);
            snprintf(buf,sizeof(buf),"%u MHz",gpuHz/1000000);
            ROW("GPU Clock",buf,PURPLE); BAR((float)gpuHz/921600000.f,PURPLE);
            snprintf(buf,sizeof(buf),"%u MHz",emcHz/1000000);
            ROW("EMC Clock",buf,CYAN);   BAR((float)emcHz/1600000000.f,CYAN);
            float mp=totalMem>0?(float)usedMem/totalMem:0;
            snprintf(buf,sizeof(buf),"%llu / %llu MB  (%.0f%%)",
                (unsigned long long)(usedMem/1048576),(unsigned long long)(totalMem/1048576),mp*100.f);
            ROW("RAM",buf,WHITE); BAR(mp,GREEN);
            snprintf(buf,sizeof(buf),"%.1f C  (SoC)",  tempMC/1000.f);
            Col tc=tempMC<50000?GREEN:(tempMC<70000?YELLOW:RED);
            ROW("Temp (Internal)",buf,tc);
            snprintf(buf,sizeof(buf),"%.1f C  (Board)",temp2MC/1000.f);
            Col tc2=temp2MC<50000?GREEN:(temp2MC<70000?YELLOW:RED);
            ROW("Temp (External)",buf,tc2);
            ROW("Op Mode",opMode==AppletOperationMode_Handheld?"Handheld":"Docked",WHITE);
            break;}

        case T_POWER:{
            snprintf(buf,sizeof(buf),"%u%%",batPct);
            Col bc=batPct>50?GREEN:(batPct>20?YELLOW:RED);
            ROW("Battery",buf,bc); BAR(batPct/100.f,bc);
            const char *chrg; Col cc;
            switch(charger){
                case PsmChargerType_Unconnected:     chrg="Not Charging";          cc=GRAY;   break;
                case PsmChargerType_EnoughPower:  chrg="Charging (AC Adapter)"; cc=GREEN;  break;
                case PsmChargerType_LowPower:     chrg="Charging (Low Power)";  cc=YELLOW; break;
                default:                              chrg="Charging";              cc=GREEN;  break;
            }
            ROW("Charger",chrg,cc);
            snprintf(buf,sizeof(buf),"%.0f%%",bright*100.f);
            ROW("Brightness",buf,YELLOW); BAR(bright,YELLOW);
            ROW("Op Mode",opMode==AppletOperationMode_Handheld?"Handheld":"Docked (TV)",WHITE);
            const char *perf="Unknown";
            switch(perfMode){
                case ApmPerformanceMode_Normal: perf="Normal  (1020 MHz CPU / 307 MHz GPU)"; break;
                case ApmPerformanceMode_Boost:  perf="Boost   (1785 MHz CPU / 768 MHz GPU)"; break;
                default: break;
            }
            ROW("Perf Profile",perf,ACCENT);
            break;}

        case T_STORAGE:{
            snprintf(buf,sizeof(buf),"%s used  /  %s free  /  %s total",sdUsed,sdFree,sdTot);
            ROW("SD Card",buf,WHITE); BAR(sdPct,ACCENT);
            snprintf(buf,sizeof(buf),"%.1f%%",sdPct*100.f);
            ROW("SD Used",buf,ACCENT);
            snprintf(buf,sizeof(buf),"%llu MB / %llu MB",
                (unsigned long long)(usedMem/1048576),(unsigned long long)(totalMem/1048576));
            ROW("RAM",buf,CYAN); BAR(totalMem>0?(float)usedMem/totalMem:0,CYAN);
            break;}

        case T_NETWORK:{
            if(!nifmOk){ROW("Status","Service unavailable",RED);break;}
            ROW("Internet",netOk?"Connected":"Disconnected",netOk?GREEN:RED);
            const char *ct=connType==NifmInternetConnectionType_WiFi?"WiFi":
                           connType==NifmInternetConnectionType_Ethernet?"Ethernet":"None";
            ROW("Type",ct,WHITE);
            if(connType==NifmInternetConnectionType_WiFi){
                snprintf(buf,sizeof(buf),"%u / 3 bars",wifiSig);
                Col sc=wifiSig>=2?GREEN:(wifiSig==1?YELLOW:RED);
                ROW("WiFi Signal",buf,sc); BAR(wifiSig/3.f,sc);
            }
            ROW("IP Address",netOk?ipStr:"N/A",CYAN);
            break;}

        case T_CTRL:{
            HidNpadIdType ids[]={HidNpadIdType_No1,HidNpadIdType_No2,HidNpadIdType_No3,HidNpadIdType_No4,HidNpadIdType_Handheld};
            const char *inames[]={"P1","P2","P3","P4","Handheld"};
            int found=0;
            for(int i=0;i<5&&r<13;i++){
                u32 style=hidGetNpadStyleSet(ids[i]); if(!style) continue; found++;
                const char *type="Unknown";
                if(style&HidNpadStyleTag_NpadJoyDual)      type="Joy-Con Pair";
                else if(style&HidNpadStyleTag_NpadFullKey)  type="Pro Controller";
                else if(style&HidNpadStyleTag_NpadJoyLeft)  type="Joy-Con (L)";
                else if(style&HidNpadStyleTag_NpadJoyRight) type="Joy-Con (R)";
                else if(style&HidNpadStyleTag_NpadHandheld) type="Handheld";
                else if(style&HidNpadStyleTag_NpadSystemExt) type="Other Controller";
                char lbl[48]; snprintf(lbl,sizeof(lbl),"[%s] Type",inames[i]);
                ROW(lbl,type,ACCENT);
                HidPowerInfo pl={0},pr={0};
                if(style&HidNpadStyleTag_NpadJoyDual){
                    hidGetNpadPowerInfoSplit(ids[i],&pl,&pr);
                    snprintf(buf,sizeof(buf),"L: %u/4%s   R: %u/4%s",
                        pl.battery_level,pl.is_charging?" (CHG)":"",
                        pr.battery_level,pr.is_charging?" (CHG)":"");
                } else {
                    hidGetNpadPowerInfoSingle(ids[i],&pl);
                    snprintf(buf,sizeof(buf),"%u / 4%s",pl.battery_level,pl.is_charging?"  (Charging)":"");
                }
                snprintf(lbl,sizeof(lbl),"[%s] Battery",inames[i]); ROW(lbl,buf,GREEN);
                if(style&(HidNpadStyleTag_NpadJoyDual|HidNpadStyleTag_NpadJoyLeft|HidNpadStyleTag_NpadJoyRight)){
                    HidNpadControllerColor cl={0},cr={0};
                    if(R_SUCCEEDED(hidGetNpadControllerColorSplit(ids[i],&cl,&cr))){
                        int ry=cy+r*ROW_H;
                        snprintf(buf,sizeof(buf),"#%06X                #%06X",cl.main&0xFFFFFF,cr.main&0xFFFFFF);
                        snprintf(lbl,sizeof(lbl),"[%s] Colors",inames[i]); ROW(lbl,buf,WHITE);
                        drawSwatch(VALUE_X-2,ry,cl.main&0xFFFFFF);
                        drawSwatch(VALUE_X+118,ry,cr.main&0xFFFFFF);
                    }
                } else if(style&HidNpadStyleTag_NpadFullKey){
                    HidNpadControllerColor cs={0};
                    if(R_SUCCEEDED(hidGetNpadControllerColorSingle(ids[i],&cs))){
                        int ry=cy+r*ROW_H;
                        snprintf(buf,sizeof(buf),"#%06X",cs.main&0xFFFFFF);
                        snprintf(lbl,sizeof(lbl),"[%s] Color",inames[i]); ROW(lbl,buf,WHITE);
                        drawSwatch(VALUE_X-2,ry,cs.main&0xFFFFFF);
                    }
                }
            }
            if(!found) ROW("Controllers","None connected",GRAY);
            break;}

        case T_MOTION:{
            if(!gyroN){ROW("Gyro","No sensor available",RED);break;}
            float ax=gst.acceleration.x, ay=gst.acceleration.y, az=gst.acceleration.z;
            float gx=gst.angular_velocity.x, gy=gst.angular_velocity.y, gz=gst.angular_velocity.z;
            float rx=gst.angle.x, ry2=gst.angle.y, rz=gst.angle.z;
            snprintf(buf,sizeof(buf),"X %+.4f   Y %+.4f   Z %+.4f  G",ax,ay,az);
            ROW("Accelerometer",buf,CYAN);
            BAR((ax+2.f)/4.f,CYAN); BAR((ay+2.f)/4.f,GREEN); BAR((az+2.f)/4.f,PURPLE);
            snprintf(buf,sizeof(buf),"X %+.4f   Y %+.4f   Z %+.4f  rad/s",gx,gy,gz);
            ROW("Angular Velocity",buf,ORANGE);
            BAR((gx+5.f)/10.f,ORANGE); BAR((gy+5.f)/10.f,YELLOW); BAR((gz+5.f)/10.f,RED);
            snprintf(buf,sizeof(buf),"Roll %+.3f   Pitch %+.3f   Yaw %+.3f  rad",rx,ry2,rz);
            ROW("Angle",buf,YELLOW);
            break;}
        }

        // Footer
        fill(0,FOOTER_Y,SCREEN_W,SCREEN_H-FOOTER_Y,TITLEC);
        hline(FOOTER_Y,ACCENT);
        drawText(fSm,"[L][R] or [<][>] Switch Tab     [+] Exit",PAD,FOOTER_Y+(44-15)/2,GRAY);
        int dotX=SCREEN_W/2-T_COUNT*12;
        for(int i=0;i<T_COUNT;i++){
            Col dc=i==curTab?ACCENT:SEPC;
            fill(dotX+i*24,FOOTER_Y+15,12,12,dc);
        }

        SDL_RenderPresent(R);
        svcSleepThread(33333333ULL);
    }

    for(int i=0;i<gyroN;i++) hidStopSixAxisSensor(gyroH[i]);
    if(clkOk){clkrstCloseSession(&cpuS);clkrstCloseSession(&gpuS);clkrstCloseSession(&emcS);clkrstExit();}
    if(psmOk)  psmExit();
    if(tsOk)   tsExit();
    if(sysOk)  setsysExit();
    if(apmOk)  apmExit();
    if(lblOk)  lblExit();
    if(nifmOk) nifmExit();
    if(timeOk) timeExit();
    if(setOk)  setExit();
    plExit();
    TTF_CloseFont(fSm);TTF_CloseFont(fLbl);TTF_CloseFont(fVal);TTF_CloseFont(fTitle);TTF_CloseFont(fTab);
    TTF_Quit();
    SDL_DestroyRenderer(R); SDL_DestroyWindow(win); SDL_Quit();
    return 0;
}
