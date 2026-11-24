#pragma once

// Setarile jocului (taste, mouse, sunet, grafica) schimbate direct din pagini, local, fara server.
//
// JS:
//   cef.emit('gta:get')                     -> raspuns: cef.on('gta:state', json)
//   cef.emit('gta:set', 'sfxVol', '70')     -> se aplica pe firul jocului, apoi 'gta:state'
//   cef.emit('gta:set', 'key.foot.jump', 'L SHIFT')
//
// Cererile vin pe firul UI al CEF (OnProcessMessageReceived) si se aplica in Tick(), pe firul jocului.

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

class BrowserManager;

class GameSettings
{
public:
    static GameSettings& Instance();

    // firul UI (CEF): pune cererea in coada
    void Enqueue(int browserId, std::string key, std::string value);

    // firul jocului: aplica cererile, raspunde cu starea, salveaza gta_sa.set
    void Tick(BrowserManager& manager);

    static bool IsSettingsEvent(const std::string& eventName)
    {
        return eventName == "gta:get" || eventName == "gta:set";
    }

private:
    struct Request
    {
        int browserId = -1;
        std::string key;     // gol = doar starea
        std::string value;
    };

    void Apply(const std::string& key, const std::string& value);
    std::string BuildState() const;
    bool Validate();

    std::mutex mutex_;
    std::vector<Request> queue_;
    uint64_t save_at_ = 0;
    bool validated_ = false;
    bool keys_ok_ = false;
    float orig_vertical_ = 0.0015f;
};
