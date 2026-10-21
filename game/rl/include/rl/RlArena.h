// RlArena.h
// The arena as one triangle shell: floor, quarter-pipe ramps, walls, chamfered corners, ceiling and the two goals with their
// insides. The normals look INTO the arena (Jolt's mesh is one-sided: a ball outside the normals falls through). The same
// triangles go to the collision (Jolt MeshShape) and to the render (OBJ).

#pragma once

#include <rl/RlTuning.h>
#include <rl/RlTypes.h>

#include <cstdint>
#include <string>
#include <vector>

namespace rl
{
    class RlArena
    {
    public:
        explicit RlArena(const RlArenaTuning& tuning);

        // Welded vertices and triangles (three indices each), counter-clockwise seen from inside.
        const std::vector<RlVec3>& Vertices() const { return vertices_; }
        const std::vector<uint32_t>& Indices() const { return indices_; }
        size_t TriangleCount() const { return indices_.size() / 3; }

        // Wavefront OBJ of the same triangles with smoothed normals and box-projected UV (one unit of UV is uvMetres metres).
        // Vertices are (position - origin) / scale.
        std::string ToObj(float uvMetres = 8.0f, float scale = 1.0f, RlVec3 origin = {}) const;
        bool WriteObj(const std::string& path, float uvMetres = 8.0f, float scale = 1.0f, RlVec3 origin = {}) const;

        // The mesh of the scene (assets/models/rl_arena.obj). The renderer culls by a sphere of 0.87 * scale around the entity,
        // so the mesh is shrunk by kRenderScale around the middle of the arena and the entity RlArena sits there with that scale:
        // its sphere covers the whole shell. Normals do not change under a uniform scale.
        static constexpr float kRenderScale = 80.0f;
        RlVec3 RenderOrigin() const { return {0.0f, tuning_.height * 0.5f, 0.0f}; }
        std::string ToRenderObj() const { return ToObj(8.0f, kRenderScale, RenderOrigin()); }
        bool WriteRenderObj(const std::string& path) const { return WriteObj(path, 8.0f, kRenderScale, RenderOrigin()); }

        // Axis aligned bounds of the shell, goals included.
        RlVec3 BoundsMin() const { return boundsMin_; }
        RlVec3 BoundsMax() const { return boundsMax_; }

    private:
        RlArenaTuning tuning_;
        std::vector<RlVec3> vertices_;
        std::vector<uint32_t> indices_;
        RlVec3 boundsMin_{};
        RlVec3 boundsMax_{};
    };
}
