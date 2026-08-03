#include "Net.h"
#include "Server.h"
#include "Team.h"
#include "World.h"

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>

// The dedicated server: a Server, a clock, and a console. No window, no
// renderer, no sound device — this binary linking without them is still the
// proof that the simulation is severed from the machines it's watched on.
//
// Everything that runs a match lives in Server, which is also what a player
// deploying on their own machine starts behind the menus. What's left here is
// the things only a console process wants: a loop that sleeps, a status line
// once a second, and a run length so a smoke test can end on its own.
int main(int argc, char** argv)
{
    // A server's stdout is usually a pipe or a log file, and a line that sat
    // in a buffer while the process was killed is a line that never happened.
    // Unbuffered costs nothing at one line a second.
    setvbuf(stdout, nullptr, _IONBF, 0);

    // Optional run length in seconds, so a smoke test can end on its own.
    // Unattended is the default: a server's natural lifespan is "until told".
    const float runSeconds = argc > 1 ? static_cast<float>(std::atof(argv[1])) : 0.0f;

    // Optional level path, since there is more than one of them now. A client
    // still draws whatever its own copy loads, so this is for smoke tests and
    // for a host who knows what their players are running.
    const char* levelPath = argc > 2 ? argv[2] : nullptr;

    // A dedicated server is the public shape of the same class the client
    // embeds: the well-known port, every interface, listed on the LAN, and
    // narrating what happens to it.
    Server server;
    Server::Config config;
    config.port = Net::kPort;
    config.discoverable = true;
    config.log = true;
    if (levelPath)
        config.level = levelPath;
    try
    {
        if (!server.Start(config))
        {
            std::fprintf(stderr, "could not open port %u\n", config.port);
            return 1;
        }
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "startup failed: %s\n", e.what());
        return 1;
    }

    const World& world = server.Match();
    std::printf("infantry_server: port %u, arena %.0f half-units, %d a side, tick %d Hz, "
                "match %.0f min\n",
                server.Port(), world.ArenaHalf(), World::kTeamSize,
                static_cast<int>(1.0f / World::kTickDt + 0.5f), World::kMatchLength / 60.0f);

    LARGE_INTEGER freq, prev;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&prev);
    float sinceReport = 0.0f;

    for (;;)
    {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        float dt = static_cast<float>(now.QuadPart - prev.QuadPart) /
                   static_cast<float>(freq.QuadPart);
        prev = now;
        dt = std::min(dt, 0.1f); // avoid huge steps after stalls, same as the client

        server.Update(dt);

        sinceReport += dt;
        if (sinceReport >= 1.0f)
        {
            sinceReport = 0.0f;
            int alive[2] = { 0, 0 };
            for (const Unit& unit : world.Units())
                if (unit.hp > 0.0f)
                    ++alive[unit.team % 2];
            // Standing and killed, per side, against the clock — a headless
            // server's only window on the match it's running.
            const int left = static_cast<int>(
                std::ceil(world.MatchOver() ? world.Intermission() : world.MatchTime()));
            std::printf("%s %d:%02d  blue %d up %d killed  red %d up %d killed  shots %zu  "
                        "players %zu  events %lld\n",
                        world.MatchOver() ? "result" : "match", left / 60, left % 60, alive[0],
                        world.Score(0), alive[1], world.Score(1), world.Projectiles().size(),
                        server.Players(), server.EventsSeen());
        }

        if (runSeconds > 0.0f &&
            server.Ticks() >= static_cast<uint32_t>(runSeconds / World::kTickDt))
        {
            std::printf("done: %u ticks, %lld events\n", server.Ticks(), server.EventsSeen());
            return 0;
        }

        // The tick is 16.7ms of budget and the work is a fraction of it;
        // sleeping keeps a headless box from spinning a core to wait.
        Sleep(1);
    }
}
