#pragma once

#include <Audio.h>
#include <DirectXMath.h>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// SFX layer over DirectXTK12's AudioEngine (XAudio2). Play() fires plain
// one-shots for sounds at the listener (UI, the player getting hit); Play3D()
// spawns positional voices panned and attenuated by X3DAudio against the
// per-frame listener pose. On a machine with no audio device the engine comes
// up in silent mode and every play call is a harmless no-op.
class Sound
{
public:
    // Positional sounds fade linearly to silence at this distance; callers
    // can skip Play3D entirely beyond it.
    static constexpr float kRange = 45.0f;

    // Call once at startup, before Load. Throws std::runtime_error only on
    // real engine failure, not on missing audio hardware.
    void Init();

    // Pump the engine and reap finished 3D voices; call once per frame.
    // Recovers when the default audio device changes or disappears.
    void Update();

    // Loads assets/sounds/<name>.wav and registers it under <name>.
    void Load(const std::string& name);

    // Where the ear is and which world direction appears "up" on screen, so
    // stereo panning matches what the player sees. Call once per frame before
    // any Play3D.
    void SetListener(const DirectX::XMFLOAT3& pos, const DirectX::XMFLOAT3& screenUp);

    // pitch is in octaves, -1..+1; 0 plays as authored.
    void Play(const std::string& name, float volume = 1.0f, float pitch = 0.0f);
    void Play3D(const std::string& name, const DirectX::XMFLOAT3& pos, float pitch = 0.0f);

private:
    std::unique_ptr<DirectX::AudioEngine> m_engine;
    std::unordered_map<std::string, std::unique_ptr<DirectX::SoundEffect>> m_effects;
    // In-flight 3D one-shots. The 3D transform is applied once at spawn; our
    // clips are short enough (<= 0.5s) that listener motion during playback
    // is inaudible.
    std::vector<std::unique_ptr<DirectX::SoundEffectInstance>> m_active;
    DirectX::AudioListener m_listener;
};
