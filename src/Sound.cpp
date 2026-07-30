#include "Sound.h"

#include <objbase.h>

using namespace DirectX;

namespace
{
    // Linear falloff to silence at CurveDistanceScaler (= Sound::kRange),
    // instead of X3DAudio's default inverse curve, which never reaches zero
    // and would make the far-range cull in callers audible as a pop.
    X3DAUDIO_DISTANCE_CURVE_POINT kFalloffPoints[2] = { { 0.0f, 1.0f }, { 1.0f, 0.0f } };
    X3DAUDIO_DISTANCE_CURVE kFalloffCurve = { kFalloffPoints, 2 };
}

void Sound::Init()
{
    // XAudio2 lives on COM; safe to call again if the thread already did it.
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    m_engine = std::make_unique<AudioEngine>(AudioEngine_Default);
}

void Sound::Update()
{
    if (m_engine && !m_engine->Update() && m_engine->IsCriticalError())
        m_engine->Reset(); // device unplugged/changed: rebind to the new default

    std::erase_if(m_active, [](const auto& inst) {
        return inst->GetState() == SoundState::STOPPED;
    });
}

void Sound::Load(const std::string& name)
{
    const std::wstring path =
        L"assets/sounds/" + std::wstring(name.begin(), name.end()) + L".wav";
    m_effects[name] = std::make_unique<SoundEffect>(m_engine.get(), path.c_str());
}

void Sound::SetListener(const XMFLOAT3& pos, const XMFLOAT3& screenUp)
{
    m_listener.SetPosition(pos);
    // screenUp lies on the ground plane, so it is orthogonal to world up —
    // exactly the orthonormal front/top pair X3DAudio expects.
    m_listener.SetOrientation(screenUp, XMFLOAT3{ 0.0f, 1.0f, 0.0f });
}

void Sound::Play(const std::string& name, float volume, float pitch)
{
    if (const auto it = m_effects.find(name); it != m_effects.end())
        it->second->Play(volume, pitch, 0.0f);
}

void Sound::Play3D(const std::string& name, const XMFLOAT3& pos, float pitch)
{
    const auto it = m_effects.find(name);
    if (it == m_effects.end())
        return;

    auto inst = it->second->CreateInstance(SoundEffectInstance_Use3D);

    AudioEmitter emitter;
    emitter.SetPosition(pos);
    emitter.CurveDistanceScaler = kRange;
    emitter.pVolumeCurve = &kFalloffCurve;
    // Within this radius panning eases toward center, so the player's own
    // muzzle blast doesn't hard-pan when they fire sideways.
    emitter.InnerRadius = 2.0f;

    inst->SetPitch(pitch);
    // rhcoords=false: the game world and X3DAudio are both left-handed.
    inst->Apply3D(m_listener, emitter, false);
    inst->Play();
    m_active.push_back(std::move(inst));
}
