#include "Sound.h"

#include <objbase.h>
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace DirectX;

namespace
{
    // Linear falloff to silence at CurveDistanceScaler (= Sound::kRange),
    // instead of X3DAudio's default inverse curve, which never reaches zero
    // and would make the far-range cull in callers audible as a pop.
    X3DAUDIO_DISTANCE_CURVE_POINT kFalloffPoints[2] = { { 0.0f, 1.0f }, { 1.0f, 0.0f } };
    X3DAUDIO_DISTANCE_CURVE kFalloffCurve = { kFalloffPoints, 2 };

    constexpr int kWindRate = 22050;         // Hz, mono 16-bit
    constexpr size_t kWindSamples = 4096;    // per generated buffer (~185 ms)
    constexpr float kWindVolume = 0.08f;

    // Shutdown waits this long, at most, for in-flight one-shots to finish.
    // Clips are half a second at the outside, so the cap only ever fires if
    // the device has stopped consuming buffers at all.
    constexpr int kDrainSteps = 100;
    constexpr DWORD kDrainStepMs = 10;

    // A comma-separated list of sounds to keep silent, from INFANTRY_MUTE.
    // "all" silences every sample in the bank; "wind" silences the generated
    // ambience, which is not a sample and answers to nothing else.
    //
    // This exists to answer "what am I hearing" by elimination, which is the
    // only way to answer it when the thing you can hear leaves no other trace.
    // Naming a sound here stops it at the one place every sound goes out, so
    // what is left is what remains audible — and turning the whole bank off
    // while a noise carries on is how you learn the noise was never a sample.
    bool Muted(const char* name)
    {
        static const std::string list = [] {
            const char* env = std::getenv("INFANTRY_MUTE");
            return std::string(env ? env : "");
        }();
        if (list.empty())
            return false;
        // Matched between commas so "fire" can't be found inside another name.
        for (size_t at = 0; (at = list.find(name, at)) != std::string::npos; at += 1)
        {
            const bool startOk = at == 0 || list[at - 1] == ',';
            const size_t end = at + std::strlen(name);
            if (startOk && (end == list.size() || list[end] == ','))
                return true;
        }
        return false;
    }

    bool MutedSample(const std::string& name)
    {
        return Muted("all") || Muted(name.c_str());
    }
}

Sound::~Sound()
{
    Shutdown();
}

void Sound::Init()
{
    // XAudio2 lives on COM; safe to call again if the thread already did it.
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    m_engine = std::make_unique<AudioEngine>(AudioEngine_EnvironmentalReverb |
                                             AudioEngine_ReverbUseFilters);
    // Open-air arena: a long, soft tail rather than a room echo.
    m_engine->SetReverb(Reverb_Mountains);

    m_bank = std::make_unique<WaveBank>(m_engine.get(), L"assets/sounds/sounds.xwb");

    if (!Muted("wind"))
    {
        m_wind = std::make_unique<DynamicSoundEffectInstance>(
            m_engine.get(), [this](DynamicSoundEffectInstance* i) { FeedWind(i); }, kWindRate, 1,
            16);
        m_wind->SetVolume(kWindVolume);
        m_wind->Play();
    }
}

// The engine is the last thing to go, and nothing may still be pointing into
// the graph when it does. Voices we hold are easy: dropping them destroys the
// underlying XAudio2 voice while the engine is still there to take it back.
// The one-shots from Play() are the trap — the engine owns those, each queued
// buffer carries a raw pointer back to the wave bank, and the engine only
// notices they finished inside Update(). Free the bank with one still in
// flight and XAudio2 delivers its completion callback into freed memory as it
// stops the engine, which reads as a crash on exit after a noisy session.
// So: pump until the bank reports no one-shots left, which means every
// callback has already been delivered, and only then take the bank away.
void Sound::Shutdown()
{
    if (!m_engine)
        return;

    if (m_wind)
    {
        m_wind->Stop(true);
        m_wind.reset();
    }
    m_active.clear();

    for (int i = 0; i < kDrainSteps; ++i)
    {
        if (!m_engine->Update()) // silent mode or a dead device: nothing to drain
            break;
        if (m_engine->GetStatistics().playingOneShots == 0)
            break;
        Sleep(kDrainStepMs);
    }
    // One more pass to retire the voices the last callbacks freed up, then
    // hand their now-idle voices back so none outlive the engine.
    m_engine->Update();
    m_engine->TrimVoicePool();

    m_bank.reset();
    m_engine.reset();
}

void Sound::Update()
{
    if (m_engine && !m_engine->Update() && m_engine->IsCriticalError())
    {
        m_engine->Reset(); // device unplugged/changed: rebind to the new default
        // A new mastering voice starts at full volume, so a mute has to be put
        // back on it — otherwise unplugging headphones is a way to make a
        // backgrounded game start shouting.
        m_engine->SetMasterVolume(m_muted ? 0.0f : 1.0f);
    }

    std::erase_if(m_active, [](const auto& inst) {
        return inst->GetState() == SoundState::STOPPED;
    });
}

void Sound::SetMuted(bool muted)
{
    if (muted == m_muted)
        return;
    m_muted = muted;
    // Held even without an engine, so a mute decided during a silent-mode
    // startup still stands if the engine ever comes back.
    if (m_engine)
        m_engine->SetMasterVolume(muted ? 0.0f : 1.0f);
}

// Keeps the wind voice fed: cascaded low-pass filtered white noise with a
// slow gust swell. Runs from AudioEngine::Update when buffers run low.
void Sound::FeedWind(DynamicSoundEffectInstance* instance)
{
    while (instance->GetPendingBufferCount() < static_cast<int>(m_windRing.size()) - 1)
    {
        std::vector<uint8_t>& buffer = m_windRing[m_windNext];
        m_windNext = (m_windNext + 1) % m_windRing.size();
        buffer.resize(kWindSamples * 2);

        auto* samples = reinterpret_cast<int16_t*>(buffer.data());
        std::uniform_real_distribution<float> white(-1.0f, 1.0f);
        for (size_t i = 0; i < kWindSamples; ++i)
        {
            m_windLp1 += 0.045f * (white(m_windRng) - m_windLp1);
            m_windLp2 += 0.045f * (m_windLp1 - m_windLp2);
            m_windGust += 0.35f / kWindRate;
            const float gust = 0.75f + 0.25f * std::sin(m_windGust * 6.2831853f);
            samples[i] = static_cast<int16_t>(m_windLp2 * gust * 12000.0f);
        }
        instance->SubmitBuffer(buffer.data(), buffer.size());
    }
}

void Sound::SetListener(const XMFLOAT3& pos, const XMFLOAT3& screenUp)
{
    m_listener.SetPosition(pos);
    // screenUp lies on the ground plane, so it is orthogonal to world up —
    // exactly the orthonormal front/top pair X3DAudio expects.
    m_listener.SetOrientation(screenUp, XMFLOAT3{ 0.0f, 1.0f, 0.0f });
}

// Every sample the game asks for, named and timestamped from launch. Off
// unless INFANTRY_SOUND_LOG names a file to write.
//
// It answers the question you cannot answer by listening: what was that. A
// sound heard without its cause on screen is one the player has to identify by
// ear, and a gunshot, a spawn and a grenade bounce are not that easy to tell
// apart through a speaker. So the log names it, says whether it played at the
// ear (2D — your own doing) or somewhere in the world (3D — everyone else's),
// and gives the spot it played at, which places it against the arena. The wind
// loop is not here: it is a generated voice rather than a sample, it starts at
// Init and never stops, and it is the one sound that has no event behind it.
static void LogSound(const char* how, const std::string& name, float x, float z)
{
    static const char* const path = std::getenv("INFANTRY_SOUND_LOG");
    if (!path)
        return;
    static std::FILE* f = std::fopen(path, "w");
    static const ULONGLONG t0 = GetTickCount64();
    if (!f)
        return;
    std::fprintf(f, "%8.2fs  %-2s  %-12s  at (%7.1f,%7.1f)\n",
                 (GetTickCount64() - t0) / 1000.0, how, name.c_str(), x, z);
    std::fflush(f);
}

void Sound::Play(const std::string& name, float volume, float pitch)
{
    LogSound("2D", name, 0.0f, 0.0f);
    if (MutedSample(name))
        return;
    if (m_bank)
        m_bank->Play(name.c_str(), volume, pitch, 0.0f);
}

void Sound::Play3D(const std::string& name, const XMFLOAT3& pos, float pitch, float volume)
{
    LogSound("3D", name, pos.x, pos.z);
    if (MutedSample(name))
        return;
    if (!m_bank)
        return;
    auto inst = m_bank->CreateInstance(name.c_str(), SoundEffectInstance_Use3D |
                                                         SoundEffectInstance_ReverbUseFilters);
    if (!inst)
        return;

    AudioEmitter emitter;
    emitter.SetPosition(pos);
    emitter.CurveDistanceScaler = kRange;
    emitter.pVolumeCurve = &kFalloffCurve;
    // Within this radius panning eases toward center, so the player's own
    // muzzle blast doesn't hard-pan when they fire sideways.
    emitter.InnerRadius = 2.0f;

    inst->SetPitch(pitch);
    // Voice volume and the 3D transform are separate settings on the same
    // voice — Apply3D writes an output matrix and a frequency ratio, and never
    // touches this — so the two multiply and the order doesn't matter.
    inst->SetVolume(volume);
    // rhcoords=false: the game world and X3DAudio are both left-handed.
    inst->Apply3D(m_listener, emitter, false);
    inst->Play();
    m_active.push_back(std::move(inst));
}
