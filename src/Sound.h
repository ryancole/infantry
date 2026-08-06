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
    //
    // Twenty-six, down from forty-five, and it is the same number the
    // shortest-sighted soldier sees: you hear as far as a medic looks. What it
    // replaces had no relation to anything — it was three and a half times what
    // the camera showed in any direction, so a firefight two full screens away
    // arrived at full attention with nothing on screen to explain it. That was
    // survivable at five a side, when the odds of anyone fighting inside a
    // forty-five unit bubble were low. At twenty-five it is never not true, and
    // the arena sounded like a battle the player could neither see nor reach.
    //
    // It still reaches past the corner of the view (about 24 units at the zoom
    // the camera opens on), which is the point: a fight about to come around
    // the wall in front of you is worth hearing before it does. What it no
    // longer does is report a war being fought somewhere else.
    static constexpr float kRange = 26.0f;

    ~Sound();

    // Call once at startup; loads the wave bank and starts the ambience.
    // Throws std::runtime_error on real engine failure or a missing/corrupt
    // wave bank, not on missing audio hardware.
    void Init();

    // Tears the audio graph down in an order XAudio2 can survive; call at exit
    // before the rest of the game goes away. Safe to call twice, and the
    // destructor calls it, so an early-out startup path is still covered.
    void Shutdown();

    // Pump the engine and reap finished 3D voices; call once per frame.
    // Recovers when the default audio device changes or disappears.
    void Update();

    // Silences everything at the master out, rather than stopping the voices
    // under it: a muted game is still a game that's running, and the wind loop
    // and the one-shots in flight carry on exactly as they would with somebody
    // listening. Coming back is a volume change and nothing else, so nothing
    // has to be restarted and no sound is left half-played. Cheap enough to
    // call every frame — it only touches the engine when the answer changes.
    void SetMuted(bool muted);

    // Where the ear is and which world direction appears "up" on screen, so
    // stereo panning matches what the player sees. Call once per frame before
    // any Play3D.
    void SetListener(const DirectX::XMFLOAT3& pos, const DirectX::XMFLOAT3& screenUp);

    // pitch is in octaves, -1..+1; 0 plays as authored. A 3D voice's volume is
    // set before the 3D transform and multiplies with it rather than replacing
    // it: distance is still X3DAudio's answer, and this is how loud the thing
    // was where it happened — a footfall under a walk that is only half wound
    // up, against the same clip under a run.
    void Play(const std::string& name, float volume = 1.0f, float pitch = 0.0f);
    void Play3D(const std::string& name, const DirectX::XMFLOAT3& pos, float pitch = 0.0f,
                float volume = 1.0f);

private:
    void FeedWind(DirectX::DynamicSoundEffectInstance* instance);

    std::unique_ptr<DirectX::AudioEngine> m_engine;
    std::unique_ptr<DirectX::WaveBank> m_bank;
    // In-flight 3D one-shots. The 3D transform is applied once at spawn; our
    // clips are short enough (<= 0.5s) that listener motion during playback
    // is inaudible.
    std::vector<std::unique_ptr<DirectX::SoundEffectInstance>> m_active;
    DirectX::AudioListener m_listener;
    bool m_muted = false;

    // Wind ambience: submitted buffers must stay alive until the voice has
    // consumed them, so generation rotates through a small ring. The ring is
    // declared ahead of the voice so it also outlives it on the way out.
    std::array<std::vector<uint8_t>, 4> m_windRing;
    std::unique_ptr<DirectX::DynamicSoundEffectInstance> m_wind;
    size_t m_windNext = 0;
    float m_windLp1 = 0.0f; // two cascaded one-pole low-passes shape the noise
    float m_windLp2 = 0.0f;
    float m_windGust = 0.0f; // slow LFO phase for gusting
    std::minstd_rand m_windRng{ 1234 };
};
