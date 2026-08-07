#include "Bindings.h"

#include <toml++/toml.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace
{
    using Bind = Bindings::Bind;
    using Device = Bindings::Bind::Device;

    constexpr Bind Key(int vk) { return { Device::Key, vk }; }
    constexpr Bind Mouse(int button) { return { Device::Mouse, button }; }

    // One row per action, and the only place any of these four facts is
    // written down: the order the settings screen shows, the key the TOML file
    // uses, the words the player reads, and what it's bound to out of the box.
    // A fifth place for any of them is a fifth place to disagree.
    struct ActionInfo
    {
        const char* key;  // TOML key
        const char* name; // player-facing
        Bind def;
    };

    constexpr ActionInfo kActions[Bindings::kActionCount] = {
        // The TOML keys are the ones already sitting in players' files and
        // stay put; the words changed when the walk did — forward is along
        // the soldier now, and the sideways pair is a strafe across it.
        { "move_forward", "MOVE FORWARD", Key('W') },
        { "move_back",    "MOVE BACK",    Key('S') },
        { "move_left",    "STRAFE LEFT",  Key('A') },
        { "move_right",   "STRAFE RIGHT", Key('D') },
        { "fire",         "FIRE",         Mouse(0) },
        { "reload",       "RELOAD",       Key('R') },
        { "grenade",      "GRENADE",      Key('Q') },
        { "melee",        "MELEE",        Key('E') },
        { "ability",      "ABILITY",      Key('F') },
        { "steady",       "STEADY",       Key(VK_SHIFT) },
        // F5, because that is where Infantry's callouts were and because the
        // three keys after it are where the next three are going. It is the
        // one binding on this list whose default is knowingly awkward: a call
        // for a medic is made while being shot at, by a hand that is also
        // holding WASD, and the function row is a reach from there. The zone's
        // layout wins the default anyway — anyone who played it will press F5
        // without being told, and everyone else can move it to a key their
        // thumb likes, which is what this screen is for.
        { "voice_medic",  "CALL MEDIC",   Key(VK_F5) },
        { "scoreboard",   "SCOREBOARD",   Key(VK_TAB) },
        // F11, which is where Infantry put it and where anyone who played it
        // will reach for it. Nothing else on this list is anywhere near the
        // function row, which is the other half of why it's the right key: the
        // class change is pressed between fights and must not be reachable by
        // the hand that is fighting.
        { "class_change", "CHANGE CLASS", Key(VK_F11) },
        // The bracket pair, because it's the one adjacent pair of keys left
        // that isn't under a hand playing the game: the radar is adjusted
        // between fights rather than during them, and putting its zoom
        // anywhere near WASD would be putting it somewhere it can be hit by
        // accident. Closing bracket zooms in, on the reading that the pair
        // runs left-to-right from wider to tighter.
        { "radar_in",     "RADAR ZOOM IN",  Key(VK_OEM_6) },
        { "radar_out",    "RADAR ZOOM OUT", Key(VK_OEM_4) },
        { "orbit_camera", "ORBIT CAMERA", Mouse(2) },
    };

    constexpr const char* kMouseNames[3] = { "LEFT MOUSE", "RIGHT MOUSE", "MIDDLE MOUSE" };

    // Keys whose name isn't just the character on them. Everything else — the
    // letters, the digits, the function row, the numpad — is spelled out
    // arithmetically below, and anything left over falls back to its code so
    // that no key on any keyboard is unbindable just because this table hasn't
    // heard of it.
    struct NamedKey
    {
        int vk;
        const char* name;
    };

    constexpr NamedKey kNamedKeys[] = {
        { VK_SPACE, "SPACE" },     { VK_RETURN, "ENTER" },    { VK_TAB, "TAB" },
        { VK_SHIFT, "SHIFT" },     { VK_CONTROL, "CTRL" },    { VK_MENU, "ALT" },
        { VK_BACK, "BACKSPACE" },  { VK_CAPITAL, "CAPS" },    { VK_LWIN, "LWIN" },
        { VK_RWIN, "RWIN" },       { VK_APPS, "MENU KEY" },
        { VK_LEFT, "LEFT" },       { VK_RIGHT, "RIGHT" },     { VK_UP, "UP" },
        { VK_DOWN, "DOWN" },       { VK_INSERT, "INSERT" },   { VK_DELETE, "DELETE" },
        { VK_HOME, "HOME" },       { VK_END, "END" },         { VK_PRIOR, "PAGE UP" },
        { VK_NEXT, "PAGE DOWN" },  { VK_SNAPSHOT, "PRTSCR" }, { VK_SCROLL, "SCROLL LOCK" },
        { VK_PAUSE, "PAUSE" },     { VK_NUMLOCK, "NUM LOCK" },
        { VK_MULTIPLY, "NUMPAD *" }, { VK_ADD, "NUMPAD +" },  { VK_SUBTRACT, "NUMPAD -" },
        { VK_DECIMAL, "NUMPAD ." },  { VK_DIVIDE, "NUMPAD /" },
        { VK_OEM_1, ";" },         { VK_OEM_PLUS, "=" },      { VK_OEM_COMMA, "," },
        { VK_OEM_MINUS, "-" },     { VK_OEM_PERIOD, "." },    { VK_OEM_2, "/" },
        { VK_OEM_3, "`" },         { VK_OEM_4, "[" },         { VK_OEM_5, "\\" },
        { VK_OEM_6, "]" },         { VK_OEM_7, "'" },
    };

    // The name of a key, and the only description of one. Parsing goes back
    // through this rather than through a second table (see ParseKeyLabel), so
    // there is no way for a key to save under one name and load under another.
    std::string KeyLabel(int vk)
    {
        if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9'))
            return std::string(1, static_cast<char>(vk));
        if (vk >= VK_F1 && vk <= VK_F24)
            return "F" + std::to_string(vk - VK_F1 + 1);
        if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9)
            return "NUMPAD " + std::to_string(vk - VK_NUMPAD0);
        for (const NamedKey& k : kNamedKeys)
            if (k.vk == vk)
                return k.name;

        char buf[16];
        std::snprintf(buf, sizeof(buf), "KEY 0X%02X", vk);
        return buf;
    }

    // The inverse, by asking KeyLabel about every key there is. Two hundred and
    // fifty-odd string compares, once, at load — cheap enough that it isn't
    // worth a second table to avoid, and a second table is exactly the thing
    // that would let the file stop round-tripping.
    int ParseKeyLabel(std::string_view name)
    {
        for (int vk = 1; vk < 256; ++vk)
            if (KeyLabel(vk) == name)
                return vk;
        return 0;
    }

    std::string ToUpper(std::string_view s)
    {
        std::string out(s);
        for (char& c : out)
            if (c >= 'a' && c <= 'z')
                c = static_cast<char>(c - 'a' + 'A');
        return out;
    }

    std::optional<Bind> ParseBind(std::string_view text)
    {
        const std::string name = ToUpper(text);
        for (int i = 0; i < 3; ++i)
            if (name == kMouseNames[i])
                return Mouse(i);
        if (const int vk = ParseKeyLabel(name))
            return Key(vk);
        return std::nullopt;
    }

    // Sides fold to the key the player thinks they pressed: binding SHIFT
    // should mean either shift, the way the old hardcoded VK_SHIFT did, rather
    // than committing the player to the one their hand happened to be nearest.
    int FoldSides(int vk)
    {
        switch (vk)
        {
        case VK_LSHIFT:
        case VK_RSHIFT:   return VK_SHIFT;
        case VK_LCONTROL:
        case VK_RCONTROL: return VK_CONTROL;
        case VK_LMENU:
        case VK_RMENU:    return VK_MENU;
        default:          return vk;
        }
    }
}

void Bindings::ResetToDefaults()
{
    for (size_t i = 0; i < kActionCount; ++i)
        m_binds[i] = kActions[i].def;
    RefreshLabels();
}

void Bindings::Set(Action action, Bind bind)
{
    const size_t slot = static_cast<size_t>(action);
    if (bind.device == Bind::Device::None || m_binds[slot] == bind)
        return;

    for (size_t i = 0; i < kActionCount; ++i)
        if (i != slot && m_binds[i] == bind)
            m_binds[i] = m_binds[slot]; // the swap; see the header

    m_binds[slot] = bind;
    RefreshLabels();
}

void Bindings::RefreshLabels()
{
    for (size_t i = 0; i < kActionCount; ++i)
        m_labels[i] = Label(m_binds[i]);
}

bool Bindings::Down(const Input& input, Action action) const
{
    const Bind b = m_binds[static_cast<size_t>(action)];
    switch (b.device)
    {
    case Bind::Device::Key:   return input.Key(b.code);
    case Bind::Device::Mouse: return input.MouseDown(b.code);
    default:                  return false;
    }
}

bool Bindings::Pressed(const Input& input, Action action) const
{
    const Bind b = m_binds[static_cast<size_t>(action)];
    switch (b.device)
    {
    case Bind::Device::Key:   return input.KeyPressed(b.code);
    case Bind::Device::Mouse: return input.MousePressed(b.code);
    default:                  return false;
    }
}

bool Bindings::Released(const Input& input, Action action) const
{
    const Bind b = m_binds[static_cast<size_t>(action)];
    switch (b.device)
    {
    case Bind::Device::Key:   return input.KeyReleased(b.code);
    case Bind::Device::Mouse: return input.MouseReleased(b.code);
    default:                  return false;
    }
}

const char* Bindings::Name(Action action)
{
    return kActions[static_cast<size_t>(action)].name;
}

std::string Bindings::Label(Bind bind)
{
    switch (bind.device)
    {
    case Bind::Device::Key:
        return KeyLabel(bind.code);
    case Bind::Device::Mouse:
        return bind.code >= 0 && bind.code < 3 ? kMouseNames[bind.code] : "MOUSE";
    default:
        return "UNBOUND";
    }
}

std::optional<Bind> Bindings::CaptureAny(const Input& input)
{
    for (int button = 0; button < 3; ++button)
        if (input.MousePressed(button))
            return Mouse(button);

    for (int vk = 1; vk < 256; ++vk)
    {
        if (vk == VK_ESCAPE)
            continue;
        if (input.KeyPressed(vk))
            return Key(FoldSides(vk));
    }
    return std::nullopt;
}

std::string Bindings::FilePath()
{
    const char* local = std::getenv("LOCALAPPDATA");
    const std::filesystem::path dir =
        local ? std::filesystem::path(local) / "Infantry" : std::filesystem::path(".");
    return (dir / "bindings.toml").string();
}

bool Bindings::Load()
{
    toml::table root;
    try
    {
        root = toml::parse_file(FilePath());
    }
    catch (const toml::parse_error&)
    {
        return false; // no file, or one that's been edited into nonsense
    }

    const toml::table* binds = root["binds"].as_table();
    if (!binds)
        return false;

    for (size_t i = 0; i < kActionCount; ++i)
    {
        const std::optional<std::string> text = (*binds)[kActions[i].key].value<std::string>();
        if (!text)
            continue; // unmentioned: keep the default
        if (const std::optional<Bind> bind = ParseBind(*text))
            m_binds[i] = *bind;
    }

    // Straight into the array rather than through Set, so a file that names one
    // key for two actions doesn't get half-swapped on the way in. Two actions
    // on one control is a state the player can only reach by hand-editing, and
    // the honest thing is to load what they wrote and let the screen show them
    // both rows saying the same key.
    RefreshLabels();
    return true;
}

bool Bindings::Save() const
{
    const std::filesystem::path path = FilePath();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    toml::table binds;
    for (size_t i = 0; i < kActionCount; ++i)
        binds.insert(kActions[i].key, Label(m_binds[i]));

    toml::table root;
    root.insert("binds", std::move(binds));

    std::ofstream file(path, std::ios::trunc);
    if (!file)
        return false;
    file << "# Infantry key bindings. Written by the game; safe to edit by hand.\n"
         << "# Values are the names the settings screen shows: a key (\"W\", \"SHIFT\",\n"
         << "# \"F5\", \"NUMPAD 4\") or a mouse button (\"LEFT MOUSE\"). Anything this\n"
         << "# file doesn't mention keeps the game's default.\n\n"
         << root << '\n';
    return static_cast<bool>(file);
}
