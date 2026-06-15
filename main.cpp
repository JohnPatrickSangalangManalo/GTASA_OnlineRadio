#include <mod/amlmod.h>
#include <mod/logger.h>
#include <mod/config.h>

#include <thread>
#include <cmath>

#include <Events.h>
#include <engine/RsGlobal.h>
#include <base/Timer.h>

#include "ibass.h"
IBASS* BASS = nullptr;

#include "isautils.h"
ISAUtils* sautils = nullptr;

MYMODCFG(net.rusjj.gtasa.onlineradio, GTA:SA Online Radio IMGUI, 2.0, RusJJ)
NEEDGAME(com.rockstargames.gtasa)

#define MAX_RADIOS 32

uintptr_t pGTASA = 0;
void* hGTASA = NULL;

// ================= CONFIG =================
ConfigEntry* pCurrentRadioIndex;
ConfigEntry* pRadioVolume;

const char** pRadioStreams;
const char** pRadioNames;

int nRadiosCount = 0;
int nRadioIndex = 0;
uint32_t pCurrentRadio = 0;

bool bRadioPlaying = false;

// ================= IMGUI SAFETY =================
// Prevent compile crash if ImGui is missing
#ifdef IMGUI_VERSION
#include <imgui.h>
#define HAS_IMGUI 1
#else
#define HAS_IMGUI 0
#endif

bool g_showUI = true;

// ================= PLAYBACK =================
void PlayStation(int index)
{
    if(index < 0 || index >= nRadiosCount) return;

    nRadioIndex = index;
    pCurrentRadioIndex->SetInt(index);

    if(pCurrentRadio)
    {
        BASS->ChannelStop(pCurrentRadio);
        BASS->StreamFree(pCurrentRadio);
        pCurrentRadio = 0;
    }

    pCurrentRadio = BASS->StreamCreateURL(
        pRadioStreams[index],
        0,
        BASS_STREAM_BLOCK | BASS_STREAM_STATUS | BASS_STREAM_AUTOFREE | BASS_SAMPLE_FLOAT,
        0
    );

    if(pCurrentRadio)
    {
        BASS->ChannelSetAttribute(
            pCurrentRadio,
            BASS_ATTRIB_VOL,
            0.005f * pRadioVolume->GetInt()
        );

        if(!CTimer::IsPaused())
        {
            BASS->ChannelPlay(pCurrentRadio, false);
            bRadioPlaying = true;
        }
    }
}

void NextStation()
{
    PlayStation((nRadioIndex + 1) % nRadiosCount);
}

void PrevStation()
{
    PlayStation((nRadioIndex - 1 + nRadiosCount) % nRadiosCount);
}

// ================= VOLUME CALLBACK (FIXED) =================
void SetVolume(int oldVal, int newVal, void* data)
{
    if(pCurrentRadio)
        BASS->ChannelSetAttribute(pCurrentRadio, BASS_ATTRIB_VOL, 0.005f * newVal);

    pRadioVolume->SetInt(newVal);
    cfg->Save();
}

// ================= SAFE VEHICLE CHECK =================
uintptr_t GetPlayerVeh()
{
    return sautils ? sautils->GetPlayerVehicle(-1) : 0;
}

// ================= IMGUI UI =================
void DrawUI()
{
#if HAS_IMGUI
    if(!g_showUI) return;

    ImGui::Begin("🎵 Online Radio", &g_showUI);

    ImGui::Text("Station: %s", pRadioNames[nRadioIndex]);
    ImGui::Separator();

    if(ImGui::BeginListBox("Stations"))
    {
        for(int i = 0; i < nRadiosCount; i++)
        {
            bool selected = (i == nRadioIndex);
            if(ImGui::Selectable(pRadioNames[i], selected))
                PlayStation(i);
        }
        ImGui::EndListBox();
    }

    ImGui::Separator();

    if(ImGui::Button("⏮ Prev")) PrevStation();
    ImGui::SameLine();
    if(ImGui::Button(bRadioPlaying ? "⏸ Pause" : "▶ Play"))
    {
        if(bRadioPlaying && pCurrentRadio)
        {
            BASS->ChannelPause(pCurrentRadio);
            bRadioPlaying = false;
        }
        else
        {
            PlayStation(nRadioIndex);
        }
    }
    ImGui::SameLine();
    if(ImGui::Button("⏭ Next")) NextStation();

    ImGui::Separator();

    int vol = pRadioVolume->GetInt();
    if(ImGui::SliderInt("Volume", &vol, 0, 100))
        SetVolume(pRadioVolume->GetInt(), vol, nullptr);

    ImGui::Text("Status: %s", bRadioPlaying ? "Playing" : "Stopped");

    ImGui::End();
#endif
}

// ================= RADIO THREAD =================
void StartRadioThread()
{
    PlayStation(nRadioIndex);
}

// ================= HOOKS =================
DECL_HOOK(void, StartRadio, uintptr_t self, uintptr_t vehicleInfo)
{
    if(GetPlayerVeh() != 0)
        std::thread(StartRadioThread).detach();

    StartRadio(self, vehicleInfo);
}

DECL_HOOK(void, StopRadio, uintptr_t self, uintptr_t vehicleInfo, unsigned char flag)
{
    if(pCurrentRadio)
    {
        BASS->ChannelStop(pCurrentRadio);
        BASS->StreamFree(pCurrentRadio);
        pCurrentRadio = 0;
    }

    bRadioPlaying = false;

    StopRadio(self, vehicleInfo, flag);
}

// ================= MOD LOAD =================
ON_MOD_LOAD()
{
    pGTASA = aml->GetLib("libGTASA.so");
    hGTASA = aml->GetLibHandle("libGTASA.so");

    BASS = (IBASS*)GetInterface("BASS");
    sautils = (ISAUtils*)GetInterface("SAUtils");

    pCurrentRadioIndex = cfg->Bind("CurrentRadioIndex", 0);
    pRadioVolume = cfg->Bind("RadioVolume", 80);

    nRadiosCount = cfg->Bind("RadiosCount", 0)->GetInt();
    if(nRadiosCount > MAX_RADIOS) nRadiosCount = MAX_RADIOS;

    pRadioStreams = new const char*[nRadiosCount];
    pRadioNames   = new const char*[nRadiosCount];

    char key[64];

    for(int i = 0; i < nRadiosCount; i++)
    {
        sprintf(key, "Radio_%d", i + 1);

        pRadioStreams[i] = cfg->Bind(key, "", "URLs")->GetString();
        pRadioNames[i]   = cfg->Bind(key, "Untitled", "Names")->GetString();
    }

    cfg->Save();

    Events::drawHudEvent.after += []()
    {
        DrawUI();
    };
}
