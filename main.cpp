#include <mod/amlmod.h>
#include <mod/logger.h>
#include <mod/config.h>
#include <thread>
#include <sys/time.h>
#include <ctime>
#include <cmath>
#include <dlfcn.h>

#include <Events.h>
#include <engine/Font.h>
#include <engine/RsGlobal.h>
#include <base/Timer.h>
#include <entity/PlayerPed.h>

MYMODCFG(net.rusjj.gtasa.onlineradio, GTA:SA Online Radio, 1.2, RusJJ)
NEEDGAME(com.rockstargames.gtasa)
BEGIN_DEPLIST()
    ADD_DEPENDENCY(net.rusjj.basslib)
END_DEPLIST()

#define MAX_RADIOS 32

#include "ibass.h"
IBASS* BASS = nullptr;

#include "isautils.h"
ISAUtils* sautils = nullptr;

uintptr_t pGTASA = 0;
void* hGTASA = NULL;

struct timeval pTimeNow;
time_t lCurrentS;
time_t lCurrentMs;

ConfigEntry* pCurrentRadioIndex;
ConfigEntry* pRadioVolume;

uint32_t pCurrentRadio = 0;
const char** pRadioStreams;
const char** pRadioNames;
char nRadiosCount, nRadioIndex;
bool bIsRadioStarted = false;
bool bIsRadioShouldBeRendered = false;
GxtChar RadioGXT[256] { 0 };
CRGBA clrRadioLoading(255, 228, 181, 255);
CRGBA clrRadioPlaying(255, 255, 255, 255);

// Track if the player is in a vehicle
bool bPlayerInVehicle = false;

// Helper functions for current time
inline time_t GetCurrentTimeS()
{
    gettimeofday(&pTimeNow, nullptr);
    lCurrentS = pTimeNow.tv_sec;
    return lCurrentS;
}
inline time_t GetCurrentTimeMs()
{
    gettimeofday(&pTimeNow, nullptr);
    lCurrentMs = (1000 * pTimeNow.tv_sec) + (0.001f * pTimeNow.tv_usec);
    return lCurrentMs;
}

// Hook functions for game pause/resume
DECL_HOOK(bool, PauseGame, void* self)
{
    bIsRadioShouldBeRendered = false;
    if(pCurrentRadio != 0) BASS->ChannelPause(pCurrentRadio);
    return PauseGame(self);
}
DECL_HOOK(bool, ResumeGame, void* self)
{
    if(pCurrentRadio != 0)
    {
        bIsRadioShouldBeRendered = true;
        BASS->ChannelPlay(pCurrentRadio, false);
    }
    return ResumeGame(self);
}

// Volume control
void VolumeChanged(int oldVal, int newVal, void* data)
{
    pRadioVolume->SetInt(newVal);
    BASS->ChannelSetAttribute(pCurrentRadio, BASS_ATTRIB_VOL, 0.005f * newVal);
    cfg->Save();
}

// Function to update the radio stream
static char szNewText[0xFF];
bool bRadioPending = false;
int retryCount = 0;
const int maxRetries = 3;
void DoRadio()
{
    if(bRadioPending) return;
    bRadioPending = true;

    nRadioIndex = pCurrentRadioIndex->GetInt();
    if(nRadioIndex < 0) nRadioIndex = nRadiosCount - 1;
    if(nRadioIndex >= nRadiosCount) nRadioIndex = 0;

    // Free previous stream
    if(pCurrentRadio)
    {
        BASS->ChannelStop(pCurrentRadio);
        BASS->StreamFree(pCurrentRadio);
        pCurrentRadio = 0;
    }

    sprintf(szNewText, "< Current radiostation >~n~%s", pRadioNames[nRadioIndex]);
    AsciiToGxtChar(szNewText, RadioGXT);
    char myIndex = nRadioIndex;

    // Retry logic
    bool streamLoaded = false;
    for(int attempt = 0; attempt < maxRetries; ++attempt)
    {
        auto stream = BASS->StreamCreateURL(pRadioStreams[nRadioIndex], 0, BASS_STREAM_BLOCK | BASS_STREAM_STATUS | BASS_STREAM_AUTOFREE | BASS_SAMPLE_FLOAT, 0);
        if(stream)
        {
            pCurrentRadio = stream;
            BASS->ChannelSetAttribute(pCurrentRadio, BASS_ATTRIB_VOL, 0.005f * pRadioVolume->GetInt());
            if(!CTimer::IsPaused())
                BASS->ChannelPlay(pCurrentRadio, true);
            streamLoaded = true;
            break; // Exit loop if success
        }
        else
        {
            logger->Error("Attempt %d: Failed to open stream! Error Code: %d", attempt + 1, BASS->ErrorGetCode());
            std::this_thread::sleep_for(std::chrono::seconds(1)); // wait before retry
        }
    }

    if(!streamLoaded)
    {
        // Failed after retries
        // Magbago ang kulay o mag-display ng message
        // Halimbawa: change color to red or maglagay ng message
        // For simplicity, gamitin natin ang kulay para ipakita ang error
        clrRadioLoading.SetRGBA(255, 0, 0, 255); // Red color indicating error
        bIsRadioShouldBeRendered = true; // Still show UI, pero may warning
        // Pwede ka ring maglagay ng text overlay dito para ipakita na error
        sprintf(szNewText, "< Stream load failed >~n~%s", pRadioNames[nRadioIndex]);
        AsciiToGxtChar(szNewText, RadioGXT);
    }
    else
    {
        // Successful load
        retryCount = 0; // reset retry counter
        // Reset color to normal
        clrRadioLoading.SetRGBA(255, 228, 181, 255);
        bIsRadioShouldBeRendered = true;
    }

    bRadioPending = false;
}

// Start radio hook
DECL_HOOK(void, StartRadio, uintptr_t self, uintptr_t vehicleInfo)
{
    if(FindPlayerVehicle(-1, false) != 0)
        std::thread(DoRadio).detach();
}

// Stop radio hook
DECL_HOOK(void, StopRadio, uintptr_t self, uintptr_t vehicleInfo, unsigned char flag)
{
    // Keep playing if player leaves vehicle
    if(!CTimer::IsPaused())
    {
        bIsRadioStarted = false;
        BASS->ChannelStop(pCurrentRadio);
        BASS->StreamFree(pCurrentRadio);
        pCurrentRadio = 0;
        nRadioIndex = -1;
    }
    bIsRadioShouldBeRendered = false;
    StopRadio(self, vehicleInfo, flag);
}

// Periodic check to see if player is in vehicle
void CheckPlayerVehicleState()
{
    bool inVehicle = FindPlayerVehicle(-1, false) != 0;
    if (inVehicle && !bPlayerInVehicle)
    {
        // Player just entered vehicle
        bPlayerInVehicle = true;
        // Resume radio if it was playing
        if(pCurrentRadio != 0)
        {
            BASS->ChannelPlay(pCurrentRadio, false);
        }
    }
    else if (!inVehicle && bPlayerInVehicle)
    {
        // Player just exited vehicle
        bPlayerInVehicle = false;
        // Optionally pause radio here if desired
        // BASS->ChannelPause(pCurrentRadio);
    }
}

// Main mod load
ON_MOD_LOAD()
{
    pGTASA = aml->GetLib("libGTASA.so");
    hGTASA = aml->GetLibHandle("libGTASA.so");

    BASS = (IBASS*)GetInterface("BASS");
    BASS->SetConfig(BASS_CONFIG_NET_TIMEOUT, 5000);

    pCurrentRadioIndex = cfg->Bind("CurrentRadioIndex", 0);
    pRadioVolume = cfg->Bind("RadioVolume", 80);
    nRadiosCount = cfg->Bind("RadiosCount", 0)->GetInt();

    if(nRadiosCount > 0)
    {
        if(nRadiosCount > MAX_RADIOS) nRadiosCount = MAX_RADIOS;
        if(pCurrentRadioIndex->GetInt() < 0) pCurrentRadioIndex->SetInt(0);
        if(pCurrentRadioIndex->GetInt() > MAX_RADIOS) pCurrentRadioIndex->SetInt(MAX_RADIOS);

        nRadioIndex = -1;
        pRadioStreams = new const char*[nRadiosCount];
        pRadioNames = new const char*[nRadiosCount];

        char szTemp[16];
        for(int i = 0; i < nRadiosCount; ++i)
        {
            sprintf(szTemp, "Radio_%d", i+1);
            pRadioStreams[i] = cfg->Bind(szTemp, "", "URLs")->GetString();
            pRadioNames[i] = cfg->Bind(szTemp, "Untitled Radio", "Names")->GetString();
        }

        if(pRadioVolume->GetInt() > 100) pRadioVolume->SetInt(100);
        else if(pRadioVolume->GetInt() < 0) pRadioVolume->SetInt(0);
        cfg->Save();
    }
    else 
    {
        logger->Error("There is no radios in the config! Mod is not loaded.");
        return;
    }

    // Hook game functions
    HOOKPLT(PauseGame,          pGTASA + BYBIT(0x672644, 0x844230));
    HOOKPLT(ResumeGame,         pGTASA + BYBIT(0x67056C, 0x840CB0));
    HOOKPLT(StartRadio,         pGTASA + BYBIT(0x66F738, 0x83F5C0));
    HOOK(StopRadio,             aml->GetSym(hGTASA, "_ZN20CAERadioTrackManager9StopRadioEP21tVehicleAudioSettingsh"));

    // Remove radio from audio settings
    aml->PlaceB(pGTASA + BYBIT(0x2A4D28 + 0x1, 0x3638A4), pGTASA + BYBIT(0x2A4D3C + 0x1, 0x3638C0));

    // Add volume slider
    sautils = (ISAUtils*)GetInterface("SAUtils");
    if(sautils)
    {
        sautils->AddSliderItem(SetType_Audio, "Online-Radio Volume", pRadioVolume->GetInt(), 0, 100, VolumeChanged);
    }

    // Draw HUD event
    Events::drawHudEvent.after += []
    {
        if(bIsRadioShouldBeRendered)
        {
            float flScale = (float)RsGlobal.maximumHeight / 540.0f;
            CFont::SetScale(flScale);
            CFont::SetColor(bIsRadioStarted ? clrRadioPlaying : clrRadioLoading);
            CFont::SetFontStyle(FO_FONT_STYLE_HEADING);
            CFont::SetEdge(1);
            CFont::SetOrientation(ALIGN_CENTER);
            CFont::PrintString(0.5f * RsGlobal.maximumWidth, 0.02f * RsGlobal.maximumHeight, RadioGXT);
            CFont::RenderFontBuffer();
        }
    };

    // Touch screen event for switching radios
    Events::touchScreenEvent.after += [](int type, int finger, int x, int y)
    {
        if(bIsRadioShouldBeRendered && type == 2 /*TOUCH_PRESS*/)
        {
            if(!bRadioPending &&
                y < (RsGlobal.maximumHeight * 0.135f) &&
                x > (RsGlobal.maximumWidth * 0.33f) &&
                x < (RsGlobal.maximumWidth * 0.66f) )
            {
                if(x > (RsGlobal.maximumWidth * 0.5f))
                {
                    ++nRadioIndex;
                }
                else
                {
                    --nRadioIndex;
                }
                pCurrentRadioIndex->SetInt(nRadioIndex);
                cfg->Save();
                std::thread(DoRadio).detach();
            }
        }
    };

    // Periodic check for vehicle state
    // Use a timer or game loop to call this regularly
    // For simplicity, assume a thread here:
    std::thread([]()
    {
        while(true)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            CheckPlayerVehicleState();
        }
    }).detach();
}
