#pragma once

#include <Audio.h>
#include <memory>
#include <string>
#include <unordered_map>

// Thin one-shot SFX layer over DirectXTK12's AudioEngine (XAudio2). On a
// machine with no audio device the engine comes up in silent mode and every
// Play() is a harmless no-op.
class Sound
{
public:
    // Call once at startup, before Load. Throws std::runtime_error only on
    // real engine failure, not on missing audio hardware.
    void Init();

    // Pump the engine; call once per frame. Recovers when the default audio
    // device changes or disappears mid-session.
    void Update();

    // Loads assets/sounds/<name>.wav and registers it under <name>.
    void Load(const std::string& name);

    // pitch is in octaves, -1..+1; 0 plays as authored.
    void Play(const std::string& name, float volume = 1.0f, float pitch = 0.0f);

private:
    std::unique_ptr<DirectX::AudioEngine> m_engine;
    std::unordered_map<std::string, std::unique_ptr<DirectX::SoundEffect>> m_effects;
};
