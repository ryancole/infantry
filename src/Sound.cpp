#include "Sound.h"

#include <objbase.h>

void Sound::Init()
{
    // XAudio2 lives on COM; safe to call again if the thread already did it.
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    m_engine = std::make_unique<DirectX::AudioEngine>(DirectX::AudioEngine_Default);
}

void Sound::Update()
{
    if (m_engine && !m_engine->Update() && m_engine->IsCriticalError())
        m_engine->Reset(); // device unplugged/changed: rebind to the new default
}

void Sound::Load(const std::string& name)
{
    const std::wstring path =
        L"assets/sounds/" + std::wstring(name.begin(), name.end()) + L".wav";
    m_effects[name] = std::make_unique<DirectX::SoundEffect>(m_engine.get(), path.c_str());
}

void Sound::Play(const std::string& name, float volume, float pitch)
{
    if (const auto it = m_effects.find(name); it != m_effects.end())
        it->second->Play(volume, pitch, 0.0f);
}
