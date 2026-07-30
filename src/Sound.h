#pragma once

#include <Audio.h>
#include <DirectXMath.h>
#include <array>
#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <vector>

// SFX layer over DirectXTK12's AudioEngine (XAudio2). All clips live in one
// wave bank (assets/sounds/sounds.xwb, built by etc/make_wavebank.ps1) and
// play by name. Play() fires plain one-shots for sounds at the listener (UI,
// the player getting hit); Play3D() spawns positional voices panned and
// attenuated by X3DAudio against the per-frame listener pose, with
// environmental reverb layered on top. A procedurally generated wind loop
// (DynamicSoundEffectInstance fed with filtered noise) runs underneath.
// On a machine with no audio device the engine comes up in silent mode and
// every play call is a harmless no-op.
class Sound
{
public:
    // Positional sounds fade linearly to silence at this distance; callers
    // can skip Play3D entirely beyond it.
    static constexpr float kRange = 45.0f;

    // Call once at startup; loads the wave bank and starts the ambience.
    // Throws std::runtime_error on real engine failure or a missing/corrupt
    // wave bank, not on missing audio hardware.
    void Init();

    // Pump the engine and reap finished 3D voices; call once per frame.
    // Recovers when the default audio device changes or disappears.
    void Update();

    // Where the ear is and which world direction appears "up" on screen, so
    // stereo panning matches what the player sees. Call once per frame before
    // any Play3D.
    void SetListener(const DirectX::XMFLOAT3& pos, const DirectX::XMFLOAT3& screenUp);

    // pitch is in octaves, -1..+1; 0 plays as authored.
    void Play(const std::string& name, float volume = 1.0f, float pitch = 0.0f);
    void Play3D(const std::string& name, const DirectX::XMFLOAT3& pos, float pitch = 0.0f);

private:
    void FeedWind(DirectX::DynamicSoundEffectInstance* instance);

    std::unique_ptr<DirectX::AudioEngine> m_engine;
    std::unique_ptr<DirectX::WaveBank> m_bank;
    // In-flight 3D one-shots. The 3D transform is applied once at spawn; our
    // clips are short enough (<= 0.5s) that listener motion during playback
    // is inaudible.
    std::vector<std::unique_ptr<DirectX::SoundEffectInstance>> m_active;
    DirectX::AudioListener m_listener;

    // Wind ambience: submitted buffers must stay alive until the voice has
    // consumed them, so generation rotates through a small ring.
    std::unique_ptr<DirectX::DynamicSoundEffectInstance> m_wind;
    std::array<std::vector<uint8_t>, 4> m_windRing;
    size_t m_windNext = 0;
    float m_windLp1 = 0.0f; // two cascaded one-pole low-passes shape the noise
    float m_windLp2 = 0.0f;
    float m_windGust = 0.0f; // slow LFO phase for gusting
    std::minstd_rand m_windRng{ 1234 };
};
