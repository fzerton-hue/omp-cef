#include "game_settings.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

#include <nlohmann/json.hpp>

#include <game_sa/CMenuManager.h>
#include <game_sa/CControllerConfigManager.h>
#include <game_sa/CCamera.h>
#include <game_sa/CVehicle.h>
#include <game_sa/CRenderer.h>
#include <game_sa/CGamma.h>
#include <game_sa/Fx_c.h>

#include "browser/manager.hpp"
#include "browser/player_stats.hpp"
#include "system/logger.hpp"

/* =====================================================================
   Next-Vision: setarile GTA din meniul ESC (taste, mouse, sunet, grafica)

   Adrese si structuri: plugin-sdk (game_sa/) - FrontEndMenuManager, ControlsManager, CCamera,
   CVehicle, CRenderer, CGamma, Fx_c. Volumul: CAudioEngine (MTA:SA, CAudioEngineSA.h).
   Totul se aplica pe firul jocului (Tick din App::Tick), apoi CMenuManager::SaveSettings
   scrie gta_sa.set (setari + taste), ca meniul original.
   ===================================================================== */

namespace
{
    // CAudioEngine nu are header in game_sa/: adresele sunt din MTA:SA (CAudioEngineSA.h)
    constexpr uintptr_t kAudioEngine          = 0xB6BC90;
    constexpr uintptr_t kSetEffectsVolume     = 0x506E10;   // void __thiscall (char)
    constexpr uintptr_t kSetMusicVolume       = 0x506DE0;   // void __thiscall (char)

    constexpr float kMouseMax = 0.005f;                      // sensibilitatea maxima din meniul GTA

    // tastele din meniu -> actiunile GTA
    struct KeyMap { const char* ui; e_ControllerAction action; };
    constexpr KeyMap kKeys[] = {
        {"foot.forward", CA_GO_FORWARD},          {"foot.back", CA_GO_BACK},
        {"foot.left", CA_GO_LEFT},                {"foot.right", CA_GO_RIGHT},
        {"foot.fire", CA_PED_FIREWEAPON},         {"foot.aim", CA_PED_LOCK_TARGET},
        {"foot.enter", CA_VEHICLE_ENTER_EXIT},    {"foot.sprint", CA_PED_SPRINT},
        {"foot.jump", CA_PED_JUMPING},            {"foot.crouch", CA_PED_DUCK},
        {"foot.next", CA_PED_CYCLE_WEAPON_RIGHT}, {"foot.prev", CA_PED_CYCLE_WEAPON_LEFT},
        {"car.accel", CA_VEHICLE_ACCELERATE},     {"car.brake", CA_VEHICLE_BRAKE},
        {"car.left", CA_VEHICLE_STEERLEFT},       {"car.right", CA_VEHICLE_STEERRIGHT},
        {"car.handbrake", CA_VEHICLE_HANDBRAKE},  {"car.enter", CA_VEHICLE_ENTER_EXIT},
        {"car.lookleft", CA_VEHICLE_LOOKLEFT},    {"car.lookright", CA_VEHICLE_LOOKRIGHT},
        {"car.fire", CA_VEHICLE_FIREWEAPON},      {"car.horn", CA_VEHICLE_HORN},
        {"car.radionext", CA_VEHICLE_RADIO_STATION_UP}, {"car.radioprev", CA_VEHICLE_RADIO_STATION_DOWN},
    };

    // tipurile de control din CControllerAction::keys[]
    constexpr int kKeyboard = 0, kExtraKey = 1, kMouse = 2;

    // RsKeyCodes (game_sa/rw/skeleton.h)
    struct KName { const char* name; unsigned int code; };
    constexpr KName kSpecialKeys[] = {
        {"ESC", rsESC}, {"F1", rsF1}, {"F2", rsF2}, {"F3", rsF3}, {"F4", rsF4}, {"F5", rsF5}, {"F6", rsF6},
        {"F7", rsF7}, {"F8", rsF8}, {"F9", rsF9}, {"F10", rsF10}, {"F11", rsF11}, {"F12", rsF12},
        {"INSERT", rsINS}, {"DELETE", rsDEL}, {"HOME", rsHOME}, {"END", rsEND}, {"PAGEUP", rsPGUP}, {"PAGEDOWN", rsPGDN},
        {"UP", rsUP}, {"DOWN", rsDOWN}, {"LEFT", rsLEFT}, {"RIGHT", rsRIGHT},
        {"NUMLOCK", rsNUMLOCK}, {"NUM /", rsDIVIDE}, {"NUM *", rsTIMES}, {"NUM -", rsMINUS}, {"NUM +", rsPLUS},
        {"NUM ENTER", rsPADENTER}, {"BACKSPACE", rsBACKSP}, {"TAB", rsTAB}, {"CAPS LOCK", rsCAPSLK},
        {"ENTER", rsENTER}, {"L SHIFT", rsLSHIFT}, {"R SHIFT", rsRSHIFT}, {"L CTRL", rsLCTRL}, {"R CTRL", rsRCTRL},
        {"L ALT", rsLALT}, {"R ALT", rsRALT}, {"SPACE", ' '},
    };
    // butoanele mouse-ului (controller MOUSE): 1 stanga, 2 rotita apasata, 3 dreapta, 4/5 rotita, 6/7 laterale
    constexpr KName kMouseKeys[] = {
        {"MOUSE 1", 1}, {"MOUSE 3", 2}, {"MOUSE 2", 3}, {"WHEEL UP", 4}, {"WHEEL DOWN", 5}, {"MOUSE 4", 6}, {"MOUSE 5", 7},
    };

    int Clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
    int Round(float f) { return static_cast<int>(std::lround(f)); }
    int ToInt(const std::string& s) { try { return std::stoi(s); } catch (...) { return 0; } }
    bool Finite(float f, float lo, float hi) { return std::isfinite(f) && f >= lo && f <= hi; }

    // literele: GTA le tine fie mari, fie mici - urmam ce gasim in tabela (W de la "mergi inainte")
    bool LowercaseLetters()
    {
        for (const auto& k : ControlsManager.m_actions[CA_GO_FORWARD].keys)
            if (k.keyCode >= 'a' && k.keyCode <= 'z') return true;
        return false;
    }

    std::string KeyName(unsigned int code)
    {
        for (const auto& k : kSpecialKeys) if (k.code == code) return k.name;
        if (code > 32 && code < 127)
            return std::string(1, static_cast<char>(std::toupper(static_cast<int>(code))));
        return "-";
    }

    std::string ActionKey(e_ControllerAction a)
    {
        const auto& keys = ControlsManager.m_actions[a].keys;
        if (keys[kKeyboard].keyCode) return KeyName(keys[kKeyboard].keyCode);
        if (keys[kMouse].keyCode)
            for (const auto& m : kMouseKeys) if (m.code == keys[kMouse].keyCode) return m.name;
        if (keys[kExtraKey].keyCode) return KeyName(keys[kExtraKey].keyCode);
        return "-";
    }

    // tasta noua devine prima (prioritatea 1); celelalte isi pastreaza ordinea dupa ea
    void SetActionKey(e_ControllerAction a, const std::string& name)
    {
        unsigned int code = 0;
        int type = -1;
        for (const auto& m : kMouseKeys) if (name == m.name) { code = m.code; type = kMouse; }
        if (type < 0) for (const auto& k : kSpecialKeys) if (name == k.name) { code = k.code; type = kKeyboard; }
        if (type < 0 && name.size() == 1 && name[0] > 32 && name[0] < 127)
        {
            int c = static_cast<unsigned char>(name[0]);
            code = static_cast<unsigned int>(LowercaseLetters() ? std::tolower(c) : std::toupper(c));
            type = kKeyboard;
        }
        if (type < 0) return;

        auto& keys = ControlsManager.m_actions[a].keys;
        const int other = (type == kKeyboard) ? kMouse : kKeyboard;
        keys[other].keyCode = 0;
        keys[other].priority = 0;

        unsigned int old[4];
        for (int t = 0; t < 4; ++t) old[t] = keys[t].priority;
        keys[type].keyCode = code;
        unsigned int next = 2;
        for (unsigned int o = 1; o <= 4; ++o)
            for (int t = 0; t < 4; ++t)
                if (t != type && keys[t].keyCode && old[t] == o) keys[t].priority = next++;
        keys[type].priority = 1;
    }

    // obiectul CGamma (game_sa/CGamma.cpp: 0xC92134); luat asa ca sa nu se confunde cu functia gamma() din <cmath>
    CGamma& GammaObject() { return *reinterpret_cast<CGamma*>(0xC92134); }

    void SetAudio(uintptr_t fn, int volume64)
    {
        using Fn = void(__thiscall*)(void*, char);
        reinterpret_cast<Fn>(fn)(reinterpret_cast<void*>(kAudioEngine), static_cast<char>(volume64));
    }
}

GameSettings& GameSettings::Instance()
{
    static GameSettings instance;
    return instance;
}

void GameSettings::Enqueue(int browserId, std::string key, std::string value)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.size() < 256)
        queue_.push_back({ browserId, std::move(key), std::move(value) });
}

// verificari de bun-simt (setarile sunt incarcate, tabela de taste arata cum trebuie)
bool GameSettings::Validate()
{
    auto& m = FrontEndMenuManager;
    if (!Finite(m.m_fDrawDistance, 0.3f, 50.0f) || m.m_nBrightness < 0 || m.m_nBrightness > 4096)
        return false;

    int keyboardKeys = 0;
    keys_ok_ = true;
    for (int i = 0; i < 59 && keys_ok_; ++i)
        for (int t = 0; t < 4; ++t)
        {
            const auto& k = ControlsManager.m_actions[i].keys[t];
            if (k.priority > 4 || (t <= kExtraKey && k.keyCode > 255) || (t == kMouse && k.keyCode > 7)) { keys_ok_ = false; break; }
            if (t <= kExtraKey && k.keyCode && i < 53) ++keyboardKeys;
        }
    if (keyboardKeys < 15) keys_ok_ = false;

    orig_vertical_ = CCamera::m_fMouseAccelVertical;
    LOG_INFO("[CEF] GameSettings: setari GTA disponibile (taste: {}).", keys_ok_ ? "da" : "nu");
    return true;
}

void GameSettings::Apply(const std::string& key, const std::string& value)
{
    auto& m = FrontEndMenuManager;
    const int n = ToInt(value);

    if (key.rfind("key.", 0) == 0)
    {
        if (!keys_ok_) return;
        const std::string ui = key.substr(4);
        for (const auto& k : kKeys)
            if (ui == k.ui) { SetActionKey(k.action, value); break; }
    }
    else if (key == "invertMouse")  CMenuManager::bInvertMouseY = n != 0;
    else if (key == "mouseSens")
    {
        const float h = static_cast<float>(Clamp(n, 1, 16)) * (kMouseMax / 16.0f);
        const bool same = std::fabs(CCamera::m_fMouseAccelVertical - CCamera::m_fMouseAccelHorzntl) < 1e-7f;
        CCamera::m_fMouseAccelHorzntl = h;
        if (same) CCamera::m_fMouseAccelVertical = h;
    }
    else if (key == "mouseFix")     CCamera::m_fMouseAccelVertical = n ? CCamera::m_fMouseAccelHorzntl : orig_vertical_;
    else if (key == "steerMouse")   CVehicle::m_bEnableMouseSteering = n != 0;
    else if (key == "flyMouse")     CVehicle::m_bEnableMouseFlying = n != 0;
    else if (key == "sfxVol")       { const int v = Clamp(n, 0, 100) * 64 / 100; m.m_nSfxVolume = static_cast<char>(v);   SetAudio(kSetEffectsVolume, v); }
    else if (key == "radioVol")     { const int v = Clamp(n, 0, 100) * 64 / 100; m.m_nRadioVolume = static_cast<char>(v); SetAudio(kSetMusicVolume, v); }
    else if (key == "brightness")   { m.m_nBrightness = Clamp(n, 0, 100) * 384 / 100; GammaObject().SetGamma(static_cast<float>(m.m_nBrightness) / 512.0f, 1); }
    else if (key == "drawDist")
    {
        const float lod = 0.925f + static_cast<float>(Clamp(n, 0, 100)) * (0.875f / 100.0f);
        m.m_fDrawDistance = lod;
        CRenderer::ms_lodDistScale = lod;
    }
    else if (key == "fxQuality")    g_fx.SetFxQuality(static_cast<FxQuality_e>(Clamp(n, 0, 3)));
    else if (key == "widescreen")   m.m_bWidescreenOn = n != 0;
    else if (key == "frameLimiter") m.m_bFrameLimiterOn = n != 0;
    else if (key == "mipMap")       m.m_bMipMapping = n != 0;                      // dupa repornire
    else if (key == "aa")           m.m_nAntiAliasingLevel = Clamp(n, 0, 3) + 1;   // dupa repornire
    else return;

    save_at_ = ::GetTickCount64() + 1500;
}

std::string GameSettings::BuildState() const
{
    const auto& m = FrontEndMenuManager;
    nlohmann::json v;
    v["invertMouse"]  = CMenuManager::bInvertMouseY ? 1 : 0;
    v["mouseSens"]    = Clamp(Round(CCamera::m_fMouseAccelHorzntl / (kMouseMax / 16.0f)), 1, 16);
    v["mouseFix"]     = std::fabs(CCamera::m_fMouseAccelVertical - CCamera::m_fMouseAccelHorzntl) < 1e-7f ? 1 : 0;
    v["steerMouse"]   = CVehicle::m_bEnableMouseSteering ? 1 : 0;
    v["flyMouse"]     = CVehicle::m_bEnableMouseFlying ? 1 : 0;
    v["sfxVol"]       = Clamp(Round(static_cast<float>(static_cast<unsigned char>(m.m_nSfxVolume)) * 100.0f / 64.0f), 0, 100);
    v["radioVol"]     = Clamp(Round(static_cast<float>(static_cast<unsigned char>(m.m_nRadioVolume)) * 100.0f / 64.0f), 0, 100);
    v["brightness"]   = Clamp(Round(static_cast<float>(m.m_nBrightness) * 100.0f / 384.0f), 0, 100);
    v["drawDist"]     = Clamp(Round((m.m_fDrawDistance - 0.925f) * 100.0f / 0.875f), 0, 100);
    v["fxQuality"]    = Clamp(static_cast<int>(g_fx.m_fxQuality), 0, 3);
    v["widescreen"]   = m.m_bWidescreenOn ? 1 : 0;
    v["frameLimiter"] = m.m_bFrameLimiterOn ? 1 : 0;
    v["mipMap"]       = m.m_bMipMapping ? 1 : 0;
    v["aa"]           = Clamp(m.m_nAntiAliasingLevel - 1, 0, 3);

    nlohmann::json keys = nlohmann::json::object();
    if (keys_ok_)
        for (const auto& k : kKeys) keys[k.ui] = ActionKey(k.action);

    nlohmann::json sup = {
        {"keys", keys_ok_ ? 1 : 0}, {"res", 0}, {"voiceVol", 0}
    };

    nlohmann::json state = {
        {"t", "state"}, {"src", "cef"}, {"v", v}, {"keys", keys}, {"sup", sup},
        {"restart", nlohmann::json::array({"mipMap", "aa"})}
    };
    return state.dump();
}

void GameSettings::Tick(BrowserManager& manager)
{
    std::vector<Request> batch;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty() && !save_at_) return;
        batch.swap(queue_);
    }

    if (!validated_)
    {
        validated_ = Validate();
        if (!validated_)
        {
            // jocul nu si-a incarcat inca setarile: punem cererile inapoi si incercam la urmatorul cadru
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.insert(queue_.begin(), batch.begin(), batch.end());
            return;
        }
    }

    std::unordered_set<int> reply;
    for (const auto& r : batch)
    {
        if (!r.key.empty()) Apply(r.key, r.value);
        reply.insert(r.browserId);
    }

    if (!reply.empty())
    {
        const std::string json = BuildState();
        for (int id : reply)
            if (auto* inst = manager.GetBrowserInstance(id))
                PlayerStats::EmitJson(inst, "gta:state", json);
    }

    if (save_at_ && ::GetTickCount64() >= save_at_)
    {
        save_at_ = 0;
        FrontEndMenuManager.SaveSettings();          // gta_sa.set: setari + taste
    }
}
