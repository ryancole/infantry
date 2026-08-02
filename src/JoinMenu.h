#pragma once

#include "Discovery.h"
#include "Input.h"
#include "Renderer.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// The server browser, at the scale there is to browse: every machine on the
// LAN that answered the last shout, and one row you can type into for a
// server the shout can't reach. Picking either hands back a host and the
// game does the rest — this screen never learns what a socket is, it just
// reads the scan's list the way the HUD reads its snapshot.
class JoinMenu
{
public:
    // Returns the host to connect to on the frame one is chosen. `servers`
    // is the scan's current answer; dt drives the caret and nothing else.
    std::optional<std::string> Update(const Input& input, float dt,
                                      const std::vector<Discovery::ServerInfo>& servers,
                                      uint32_t width, uint32_t height);

    // Draws the screen in screen-space; overrides the renderer's view-proj.
    void Render(Renderer& renderer, const std::vector<Discovery::ServerInfo>& servers);

private:
    struct Rect
    {
        float x, y, w, h;
        bool Contains(float px, float py) const
        {
            return px >= x && px <= x + w && py >= y && py <= y + h;
        }
    };

    static Rect RowRect(size_t index, float width, float height);

    // How many discovered rows the screen will show. More servers than this
    // on one LAN is a problem this prototype is happy to have someday.
    static constexpr size_t kMaxRows = 6;

    int m_selected = 0;
    // The typed row's contents, kept across visits to the screen: an address
    // you entered once is an address you'll enter again, and retyping it is
    // the whole cost this screen exists to remove.
    std::string m_address;
    float m_blink = 0.0f;
    std::vector<Vertex> m_tris;  // reused per frame
    std::vector<Vertex> m_lines;
};
