#include "Level.h"
#include "World.h"

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <vector>

// The dedicated server: a World, a clock, and nothing else. No window, no
// renderer, no sound device — this file existing and linking is the proof
// that the simulation really is severed from the machine it's watched on.
//
// Today it runs the match the client would have run, AI in every slot, and
// counts what happens. What it's waiting for is a socket: connected players
// will claim slots the way the local player does on a client, their Commands
// will arrive where this loop passes nullptr, and the Events it drains will
// be forwarded instead of tallied. The loop is already the server's real
// loop; only the audience is missing.
int main(int argc, char** argv)
{
    // Optional run length in seconds, so a smoke test can end on its own.
    // Unattended is the default: a server's natural lifespan is "until told".
    const float runSeconds = argc > 1 ? static_cast<float>(std::atof(argv[1])) : 0.0f;

    World world;
    try
    {
        world.Init(LevelData::Load("assets/levels/arena01.json"));
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "startup failed: %s\n", e.what());
        return 1;
    }

    // No local class: every slot on both sides is the AI's until players
    // arrive over the wire.
    world.StartMatch(nullptr, 0);

    std::printf("infantry_server: arena %.0f half-units, %d a side, tick %d Hz\n",
                world.ArenaHalf(), World::kTeamSize,
                static_cast<int>(1.0f / World::kTickDt + 0.5f));

    LARGE_INTEGER freq, prev;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&prev);

    std::vector<Event> events;
    float accum = 0.0f;
    float sinceReport = 0.0f;
    long long ticks = 0;
    long long eventsSeen = 0;

    for (;;)
    {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        float dt = static_cast<float>(now.QuadPart - prev.QuadPart) /
                   static_cast<float>(freq.QuadPart);
        prev = now;
        dt = std::min(dt, 0.1f); // avoid huge steps after stalls, same as the client

        accum += dt;
        while (accum >= World::kTickDt)
        {
            accum -= World::kTickDt;
            world.Tick(nullptr, events);
            ++ticks;
        }

        // A real server forwards these to whoever is watching; this one just
        // proves they flow.
        eventsSeen += static_cast<long long>(events.size());
        events.clear();

        sinceReport += dt;
        if (sinceReport >= 1.0f)
        {
            sinceReport = 0.0f;
            int alive[2] = { 0, 0 };
            for (const Unit& unit : world.Units())
                if (unit.hp > 0.0f)
                    ++alive[unit.team % 2];
            std::printf("tick %lld  blue %d  red %d  shots in air %zu  events %lld\n", ticks,
                        alive[0], alive[1], world.Projectiles().size(), eventsSeen);
        }

        if (runSeconds > 0.0f && ticks >= static_cast<long long>(runSeconds / World::kTickDt))
        {
            std::printf("done: %lld ticks, %lld events\n", ticks, eventsSeen);
            return 0;
        }

        // The tick is 16.7ms of budget and the work is a fraction of it;
        // sleeping keeps a headless box from spinning a core to wait.
        Sleep(1);
    }
}
