#pragma once

#include "Renderer.h"
#include "Vertex.h"
#include "Visibility.h"

#include <DirectXMath.h>
#include <vector>

// The ground the match is fought on: a sheet of turf over the whole arena and
// the grass standing on it. It replaces the ruled floor grid, which was a
// blockout stand-in — a level whose ground reads as a sheet of graph paper is
// a level nobody believes they are standing in, and the grid's real job (how
// far away is that) is done better by the props and the fog than by a rule
// every eight units.
//
// It is decoration and nothing else. The simulation's floor is the y = 0 plane
// it has always been, this is only what that plane looks like, and it is built
// from the level's own numbers rather than authored: an arena is a rectangle
// and a list of solid footprints, which is enough to say where turf goes and
// where grass would be growing through a crate.
//
// Two layers rather than one, because they are drawn at different moments and
// for different reasons:
//
//   turf   — flat at y = 0, opaque, laid down with the rest of the scene. It
//            writes depth, so everything above it sorts against it, and the
//            fog of war dims it exactly the way it dimmed the grid.
//   blades — a third of a unit tall, and therefore standing *above* the fog
//            sheet's plane. Drawn with the turf they would punch straight
//            through the darkness and speckle the unseen half of the map with
//            lit green, so they go down after the fog instead, through
//            Renderer::DrawGroundDetail, which dims them by hand to whatever
//            the sheet leaves the floor. The grass under the fog is as dark as
//            the ground it grows in and no darker.
namespace Ground
{
    // One square of ground: its two layers of geometry, and the sphere the
    // renderer culls the pair by. Chunks exist because the arena is 197 by 106
    // units and a camera showing twenty-six of them is looking at about one
    // percent of the grass — a chunk is small enough that most of the field is
    // rejected on a sphere test, and large enough that the whole visible set
    // still batches into a handful of draws.
    struct Chunk
    {
        DirectX::XMFLOAT3 center;
        float radius;
        std::vector<Vertex> turf;
        std::vector<Vertex> blades;
    };

    using Field = std::vector<Chunk>;

    // Builds the whole field. `solids` are the footprints of everything a
    // soldier can't walk through (World::Collider, flattened): no tuft is
    // planted inside one, so grass doesn't grow up through a trench wall or a
    // crate. The turf runs under them regardless — it's hidden, and cutting
    // holes in a sheet to hide it better is work for nothing.
    //
    // Deterministic: same arena, same solids, same field. There is no seed
    // because there is nothing a second one would be good for — the ground is
    // the ground, and a map that scattered its grass differently on each
    // machine would be two players describing different terrain to each other.
    Field Build(const DirectX::XMFLOAT2& arenaHalf, const std::vector<Visibility::Rect>& solids);

    // The two halves of the draw, each culling the field against the viewport
    // and batching what survives through `scratch` (the caller's reusable
    // buffer, cleared on entry and left empty). Call DrawTurf with the opaque
    // scene and DrawGrass after the fog of war has been laid down.
    //
    // `unseenTint` is how much of itself the fog leaves the ground — one minus
    // the sheet's alpha — and it is passed in rather than kept here because
    // the fog's darkness is the Game's number: the two have to agree, and the
    // way to make them agree is to have only one of them.
    void DrawTurf(Renderer& renderer, const Field& field, std::vector<Vertex>& scratch);
    void DrawGrass(Renderer& renderer, const Field& field, std::vector<Vertex>& scratch,
                   float unseenTint);
}
