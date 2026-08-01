#pragma once

#include "Input.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// What the player's hands are wired to, and the one place that answers it.
//
// The keys used to be constants next to the code that read them — 'W' in the
// movement block, 'R' in the reload block, a table of key caps in the HUD that
// spelled the same letters a third time. That's fine right up until a player
// wants their own layout, at which point every one of those places is a place
// the game can disagree with itself about what R does.
//
// So gameplay asks for an *action* and never names a key: Down(input,
// Action::Grenade) rather than input.KeyPressed('F'). The binding behind it is
// data, it round-trips through a TOML file the player can hand-edit, and the
// HUD draws its key caps off the same table the simulation reads. There is one
// answer to "what throws a grenade" and everything gets it from here.
//
// Menu navigation is deliberately not in the action list. Arrows, Enter and
// Escape stay nailed down, because a player who binds their way out of the
// menus has no way back in to fix it.
class Bindings
{
public:
    // Everything a player can rebind. The order is the order the settings
    // screen lists them in, which is why movement leads and the debug spawn
    // key brings up the rear.
    enum class Action
    {
        MoveForward,
        MoveBack,
        MoveLeft,
        MoveRight,
        Fire,
        Reload,
        Grenade,
        Melee,
        Ability,
        Steady,
        OrbitCamera,
        SpawnNpc,
        Count
    };

    static constexpr size_t kActionCount = static_cast<size_t>(Action::Count);

    // One physical control: a keyboard key or a mouse button. The gamepad isn't
    // in here — a pad's face buttons are bound by the shape of the pad, and
    // every screen that reads one already asks for the button by name.
    struct Bind
    {
        enum class Device : uint8_t
        {
            None,
            Key,   // `code` is a Win32 virtual-key code
            Mouse, // `code` is 0 left, 1 right, 2 middle
        };

        Device device = Device::None;
        int code = 0;

        bool operator==(const Bind&) const = default;
    };

    Bindings() { ResetToDefaults(); }

    // The three questions gameplay asks of a control: is it held, did it go
    // down this frame, did it come up this frame. Which device is behind it is
    // this class's business and not the caller's.
    bool Down(const Input& input, Action action) const;
    bool Pressed(const Input& input, Action action) const;
    bool Released(const Input& input, Action action) const;

    Bind Get(Action action) const { return m_binds[static_cast<size_t>(action)]; }

    // Binds `bind` to `action`. If another action already held it the two
    // swap, so a rebind can never leave something unbound: an action with no
    // control is a hole the player has to notice on their own, in the middle of
    // a firefight, and a swap says what happened on the screen where they did
    // it.
    void Set(Action action, Bind bind);
    void ResetToDefaults();

    // The player-facing name of the action ("MOVE FORWARD") and of what it's
    // bound to ("W", "LEFT MOUSE", "SHIFT"). The action label is cached because
    // the HUD wants a stable pointer to it every frame; it stays valid until
    // the next Set or ResetToDefaults.
    static const char* Name(Action action);
    const std::string& Label(Action action) const
    {
        return m_labels[static_cast<size_t>(action)];
    }
    static std::string Label(Bind bind);

    // The control the player just pressed, for the rebinding screen to capture.
    // Escape is never reported: it's how the screen backs out of a capture, so
    // it can't also be a thing to capture.
    static std::optional<Bind> CaptureAny(const Input& input);

    // %LOCALAPPDATA%\Infantry\bindings.toml — a user's layout is theirs and
    // survives the build directory being thrown away, which is the one place
    // the game's own files live.
    static std::string FilePath();
    // False when there's nothing to read or it's unusable, in which case the
    // defaults stand: a bindings file the player has broken by hand loses them
    // their layout, and losing the game on top of that helps nobody. Keys the
    // file doesn't mention keep their default, so a partial file is legal and a
    // player can write down only what they changed.
    bool Load();
    bool Save() const;

private:
    void RefreshLabels();

    std::array<Bind, kActionCount> m_binds{};
    std::array<std::string, kActionCount> m_labels;
};
