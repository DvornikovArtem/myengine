// RlArena.cpp
// The shell is built by sweeping one cross-section (floor ramp, wall, ceiling ramp) along the eight edges of the chamfered
// footprint; the corners are mitres, so the sweep is one continuous surface. The end walls are cut for the goal mouths.

#include <rl/RlArena.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <utility>

namespace rl
{
    namespace
    {
        constexpr float kPi = 3.14159265358979f;

        struct V3
        {
            float x = 0.0f;
            float y = 0.0f;
            float z = 0.0f;
        };

        V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
        V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
        V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
        float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
        V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
        float Length(V3 a) { return std::sqrt(Dot(a, a)); }
        V3 Normalized(V3 a)
        {
            const float length = Length(a);
            return length > 1e-12f ? a * (1.0f / length) : V3{};
        }
        V3 Lerp(V3 a, V3 b, float t) { return a + (b - a) * t; }

        // Triangles with welded vertices. A triangle is wound so that its normal looks towards the reference point.
        struct MeshBuilder
        {
            std::vector<V3> positions;
            std::vector<uint32_t> indices;
            std::map<std::array<long, 3>, uint32_t> weld;

            uint32_t Vertex(V3 p)
            {
                const std::array<long, 3> key = {std::lround(p.x * 1000.0f), std::lround(p.y * 1000.0f), std::lround(p.z * 1000.0f)};
                const auto it = weld.find(key);
                if (it != weld.end())
                {
                    return it->second;
                }
                const uint32_t index = static_cast<uint32_t>(positions.size());
                positions.push_back(p);
                weld.emplace(key, index);
                return index;
            }

            void Tri(V3 a, V3 b, V3 c, V3 ref)
            {
                const V3 normal = Cross(b - a, c - a);
                if (Dot(normal, normal) < 1e-10f)
                {
                    return;
                }
                const V3 centre = (a + b + c) * (1.0f / 3.0f);
                if (Dot(normal, ref - centre) < 0.0f)
                {
                    std::swap(b, c);
                }
                const uint32_t ia = Vertex(a);
                const uint32_t ib = Vertex(b);
                const uint32_t ic = Vertex(c);
                if (ia == ib || ib == ic || ia == ic)
                {
                    return;
                }
                indices.push_back(ia);
                indices.push_back(ib);
                indices.push_back(ic);
            }

            void Quad(V3 a, V3 b, V3 c, V3 d, V3 ref)
            {
                Tri(a, b, c, ref);
                Tri(a, c, d, ref);
            }
        };

        // A point of the cross-section: s metres in from the wall, y metres up.
        struct ProfilePoint
        {
            float s;
            float y;
        };

        struct Footprint
        {
            V3 v[8];
            V3 n[8];  // inward normal of edge i (from v[i] to v[i + 1])
            V3 m[8];  // mitre direction at v[i]: moving s inward moves the corner by s * m
        };

        Footprint BuildFootprint(const RlArenaTuning& t)
        {
            const float hx = t.halfWidth;
            const float hz = t.halfLength;
            const float c = t.cornerCut;
            Footprint f;
            f.v[0] = {hx, 0.0f, -(hz - c)};
            f.v[1] = {hx, 0.0f, hz - c};
            f.v[2] = {hx - c, 0.0f, hz};
            f.v[3] = {-(hx - c), 0.0f, hz};
            f.v[4] = {-hx, 0.0f, hz - c};
            f.v[5] = {-hx, 0.0f, -(hz - c)};
            f.v[6] = {-(hx - c), 0.0f, -hz};
            f.v[7] = {hx - c, 0.0f, -hz};
            for (int i = 0; i < 8; ++i)
            {
                const V3 d = Normalized(f.v[(i + 1) % 8] - f.v[i]);
                f.n[i] = {-d.z, 0.0f, d.x};  // left of the walking direction in the (x, z) plane: inwards
            }
            for (int i = 0; i < 8; ++i)
            {
                const V3 a = f.n[(i + 7) % 8];
                const V3 b = f.n[i];
                f.m[i] = (a + b) * (1.0f / (1.0f + Dot(a, b)));
            }
            return f;
        }

        // The point of edge `edge` at the fraction u of its length, s metres inwards. The ends of the edge are the mitre points
        // so the neighbours share their vertices; inside the edge the point just slides along the parallel line.
        V3 EdgePoint(const Footprint& f, int edge, float u, float s, float y)
        {
            V3 p;
            if (u <= 0.0f)
            {
                p = f.v[edge] + f.m[edge] * s;
            }
            else if (u >= 1.0f)
            {
                p = f.v[(edge + 1) % 8] + f.m[(edge + 1) % 8] * s;
            }
            else
            {
                p = Lerp(f.v[edge], f.v[(edge + 1) % 8], u) + f.n[edge] * s;
            }
            p.y = y;
            return p;
        }
    }

    RlArena::RlArena(const RlArenaTuning& t)
        : tuning_(t)
    {
        const Footprint foot = BuildFootprint(t);
        const float height = t.height;
        const float rf = t.floorRampRadius;
        const float rc = t.ceilingRampRadius;
        const int segments = std::max(t.rampSegments, 1);

        // cross-section from the floor to the ceiling
        std::vector<ProfilePoint> profile;
        for (int k = 0; k <= segments; ++k)
        {
            const float a = 0.5f * kPi * static_cast<float>(k) / static_cast<float>(segments);
            profile.push_back({rf - rf * std::sin(a), rf - rf * std::cos(a)});
        }
        int goalIndex = static_cast<int>(profile.size()) - 1;
        if (t.goalHeight > rf + 0.01f && t.goalHeight < height - rc - 0.01f)
        {
            profile.push_back({0.0f, t.goalHeight});
            goalIndex = static_cast<int>(profile.size()) - 1;
        }
        for (int k = 1; k <= segments; ++k)
        {
            const float a = 0.5f * kPi * static_cast<float>(k) / static_cast<float>(segments);
            profile.push_back({rc - rc * std::cos(a), height - rc + rc * std::sin(a)});
        }
        const int lastProfile = static_cast<int>(profile.size()) - 1;

        MeshBuilder mesh;
        const V3 inside = {0.0f, height * 0.5f, 0.0f};

        // the goal mouths: fractions of the end edges where the cut starts and ends
        const float endHalf = t.halfWidth - t.cornerCut;
        const float cutA = 0.5f * (1.0f - t.goalHalfWidth / endHalf);
        const float cutB = 0.5f * (1.0f + t.goalHalfWidth / endHalf);
        const auto isEndEdge = [](int edge) { return edge == 2 || edge == 6; };

        for (int edge = 0; edge < 8; ++edge)
        {
            struct Span
            {
                float u0;
                float u1;
                int firstProfile;
            };
            std::vector<Span> spans;
            if (isEndEdge(edge))
            {
                spans.push_back({0.0f, cutA, 0});
                spans.push_back({cutA, cutB, goalIndex});
                spans.push_back({cutB, 1.0f, 0});
            }
            else
            {
                spans.push_back({0.0f, 1.0f, 0});
            }
            for (const Span& span : spans)
            {
                for (int k = span.firstProfile; k < lastProfile; ++k)
                {
                    const ProfilePoint& p0 = profile[static_cast<size_t>(k)];
                    const ProfilePoint& p1 = profile[static_cast<size_t>(k) + 1];
                    mesh.Quad(EdgePoint(foot, edge, span.u0, p0.s, p0.y), EdgePoint(foot, edge, span.u1, p0.s, p0.y),
                              EdgePoint(foot, edge, span.u1, p1.s, p1.y), EdgePoint(foot, edge, span.u0, p1.s, p1.y), inside);
                }
            }
        }

        // floor and ceiling: a fan over the loop where the ramps end (the loop has the mouth cuts in it, no T-junctions)
        const auto fan = [&](float s, float y, V3 centre)
        {
            std::vector<V3> loop;
            for (int edge = 0; edge < 8; ++edge)
            {
                loop.push_back(EdgePoint(foot, edge, 0.0f, s, y));
                if (isEndEdge(edge))
                {
                    loop.push_back(EdgePoint(foot, edge, cutA, s, y));
                    loop.push_back(EdgePoint(foot, edge, cutB, s, y));
                }
            }
            for (size_t i = 0; i < loop.size(); ++i)
            {
                mesh.Tri(centre, loop[i], loop[(i + 1) % loop.size()], inside);
            }
        };
        fan(rf, 0.0f, {0.0f, 0.0f, 0.0f});
        fan(rc, height, {0.0f, height, 0.0f});

        // the floor runs on into the goal through the mouth; the cut ends of the floor ramp are closed
        for (const int edge : {2, 6})
        {
            const float z = foot.v[edge].z;
            mesh.Quad(EdgePoint(foot, edge, cutA, rf, 0.0f), EdgePoint(foot, edge, cutB, rf, 0.0f), EdgePoint(foot, edge, cutB, 0.0f, 0.0f),
                      EdgePoint(foot, edge, cutA, 0.0f, 0.0f), inside);
            for (const float u : {cutA, cutB})
            {
                const V3 mouthCentre = {0.0f, 3.0f, z};
                const V3 pivot = EdgePoint(foot, edge, u, 0.0f, 0.0f);
                for (int k = 0; k < segments; ++k)
                {
                    const ProfilePoint& p0 = profile[static_cast<size_t>(k)];
                    const ProfilePoint& p1 = profile[static_cast<size_t>(k) + 1];
                    mesh.Tri(pivot, EdgePoint(foot, edge, u, p0.s, p0.y), EdgePoint(foot, edge, u, p1.s, p1.y), mouthCentre);
                }
            }
        }

        // the two goals behind the end walls: the sweep of the sections of GoalProfile (a box when goalCornerRadius is 0), closed by the flat part of the back
        const std::vector<RlGoalSection> goalSections = GoalProfile(t);
        const int goalCorners = 4 * (std::max(t.rampSegments, 1) + 1);
        for (const float sign : {1.0f, -1.0f})
        {
            const float gh = t.goalHeight;
            const float z0 = sign * t.halfLength;
            const V3 cavity = {0.0f, gh * 0.5f, sign * (t.halfLength + 0.5f * t.goalDepth)};
            const auto point = [&](const RlGoalSection& section, int index)
            {
                const int steps = std::max(t.rampSegments, 1) + 1;
                const RlVec3 p = GoalSectionPoint(t, section, index / steps, index % steps);
                return V3{p.x, p.y, z0 + sign * section.along};
            };
            const int cornerSteps = std::max(t.rampSegments, 1) + 1;
            // the side edges of the mouth are split where the floor ramp of the end wall ends (the end wall has a vertex there): no T-junction
            const float split = std::min(rf, gh);
            for (size_t i = 0; i + 1 < goalSections.size(); ++i)
            {
                for (int j = 0; j < goalCorners; ++j)
                {
                    const int next = (j + 1) % goalCorners;
                    const V3 a0 = point(goalSections[i], j);
                    const V3 a1 = point(goalSections[i], next);
                    const V3 b0 = point(goalSections[i + 1], j);
                    const V3 b1 = point(goalSections[i + 1], next);
                    // the straight edge between corner 1 and 2 (the left side) and between 3 and 0 (the right side)
                    const bool sideEdge = j % cornerSteps == cornerSteps - 1 && ((j / cornerSteps) % 2 == 1);
                    if (i == 0 && sideEdge && std::fabs(a0.x - a1.x) < 1e-4f && std::min(a0.y, a1.y) < split - 1e-4f && std::max(a0.y, a1.y) > split + 1e-4f)
                    {
                        const V3 mid = {a0.x, split, a0.z};
                        mesh.Tri(a0, mid, b0, cavity);
                        mesh.Tri(mid, a1, b1, cavity);
                        mesh.Tri(mid, b1, b0, cavity);
                    }
                    else
                    {
                        mesh.Quad(a0, a1, b1, b0, cavity);
                    }
                }
            }
            // the back: the flat rectangle of the last section
            const RlGoalSection& back = goalSections.back();
            const V3 c0 = point(back, 0);
            for (int j = 1; j + 1 < goalCorners; ++j)
            {
                mesh.Tri(c0, point(back, j), point(back, j + 1), cavity);
            }
        }

        vertices_.reserve(mesh.positions.size());
        for (const V3& p : mesh.positions)
        {
            vertices_.push_back({p.x, p.y, p.z});
        }
        indices_ = std::move(mesh.indices);

        boundsMin_ = {1e9f, 1e9f, 1e9f};
        boundsMax_ = {-1e9f, -1e9f, -1e9f};
        for (const RlVec3& p : vertices_)
        {
            boundsMin_ = {std::min(boundsMin_.x, p.x), std::min(boundsMin_.y, p.y), std::min(boundsMin_.z, p.z)};
            boundsMax_ = {std::max(boundsMax_.x, p.x), std::max(boundsMax_.y, p.y), std::max(boundsMax_.z, p.z)};
        }
    }

    std::vector<RlGoalSection> RlArena::GoalProfile(const RlArenaTuning& t)
    {
        const int segments = std::max(t.rampSegments, 1);
        const float gh = t.goalHeight;
        const float depth = std::max(t.goalDepth, 0.1f);
        // the radius fits the goal: no more than half of the height and of the width, and the back has to fit in the depth
        const float radius = std::clamp(t.goalCornerRadius, 0.0f, std::min({0.5f * gh - 0.01f, t.goalHalfWidth - 0.01f, depth - 0.01f}));
        const float innerX = t.goalHalfWidth - radius;
        const float innerY = 0.5f * gh - radius;
        const float blend = radius > 0.0f ? std::clamp(t.goalMouthBlend, 0.01f, depth - radius) : 0.0f;
        std::vector<RlGoalSection> sections;
        if (radius <= 0.0f)
        {
            sections.push_back({0.0f, t.goalHalfWidth, 0.5f * gh, 0.0f});
            sections.push_back({depth, t.goalHalfWidth, 0.5f * gh, 0.0f});
            return sections;
        }
        // the mouth: the outside is the rectangle of the opening, the radius grows from nothing (smoothstep: the corners do not step in at the mouth)
        const int blendSteps = std::max(segments, 4);
        for (int k = 0; k <= blendSteps; ++k)
        {
            const float f = static_cast<float>(k) / static_cast<float>(blendSteps);
            const float rho = radius * f * f * (3.0f - 2.0f * f);
            sections.push_back({blend * f, t.goalHalfWidth - rho, 0.5f * gh - rho, rho});
        }
        // the straight part ends where the quarter circle of the back starts
        const float backStart = depth - radius;
        if (backStart > blend + 1e-4f)
        {
            sections.push_back({backStart, innerX, innerY, radius});
        }
        // the back: a quarter circle in the z direction, the radius shrinks to nothing
        for (int k = 1; k <= segments; ++k)
        {
            const float a = 0.5f * kPi * static_cast<float>(k) / static_cast<float>(segments);
            sections.push_back({backStart + radius * std::sin(a), innerX, innerY, radius * std::cos(a)});
        }
        return sections;
    }

    RlVec3 RlArena::GoalSectionPoint(const RlArenaTuning& t, const RlGoalSection& section, int corner, int step)
    {
        const int segments = std::max(t.rampSegments, 1);
        static const float kSignX[4] = {1.0f, -1.0f, -1.0f, 1.0f};
        static const float kSignY[4] = {1.0f, 1.0f, -1.0f, -1.0f};
        const float angle = 0.5f * kPi * (static_cast<float>(corner) + static_cast<float>(step) / static_cast<float>(segments));
        return {kSignX[corner] * section.innerX + section.radius * std::cos(angle), 0.5f * t.goalHeight + kSignY[corner] * section.innerY + section.radius * std::sin(angle), 0.0f};
    }

    std::string RlArena::ToObj(float uvMetres, float scale, RlVec3 origin) const
    {
        const size_t triangleCount = TriangleCount();
        const auto position = [&](uint32_t index) { return V3{vertices_[index].x, vertices_[index].y, vertices_[index].z}; };

        std::vector<V3> faceNormal(triangleCount);
        std::vector<float> faceArea(triangleCount);
        std::vector<std::vector<uint32_t>> vertexFaces(vertices_.size());
        for (size_t t = 0; t < triangleCount; ++t)
        {
            const V3 a = position(indices_[t * 3]);
            const V3 b = position(indices_[t * 3 + 1]);
            const V3 c = position(indices_[t * 3 + 2]);
            const V3 cross = Cross(b - a, c - a);
            faceArea[t] = 0.5f * Length(cross);
            faceNormal[t] = Normalized(cross);
            for (size_t k = 0; k < 3; ++k)
            {
                vertexFaces[indices_[t * 3 + k]].push_back(static_cast<uint32_t>(t));
            }
        }

        // smooth the normals over the faces within 40 degrees of each other: the ramps look round, the corners stay sharp
        constexpr float kSmoothCos = 0.766f;
        const float invUv = 1.0f / std::max(uvMetres, 0.001f);
        const float invScale = 1.0f / std::max(scale, 1e-6f);
        std::string obj = "# RL arena shell (game/rl RlArena), normals look into the arena\no rl_arena\n";
        char line[160];
        std::string faces;
        uint32_t next = 1;
        for (size_t t = 0; t < triangleCount; ++t)
        {
            const V3 n = faceNormal[t];
            const float ax = std::fabs(n.x);
            const float ay = std::fabs(n.y);
            const float az = std::fabs(n.z);
            uint32_t firstOfFace = next;
            for (size_t k = 0; k < 3; ++k)
            {
                const uint32_t vertex = indices_[t * 3 + k];
                V3 sum;
                for (const uint32_t other : vertexFaces[vertex])
                {
                    if (Dot(faceNormal[other], n) > kSmoothCos)
                    {
                        sum = sum + faceNormal[other] * faceArea[other];
                    }
                }
                const V3 vn = Normalized(sum);
                const V3 p = position(vertex);
                float u;
                float v;
                if (ay >= ax && ay >= az)
                {
                    u = p.x;
                    v = p.z;
                }
                else if (ax >= az)
                {
                    u = p.z;
                    v = p.y;
                }
                else
                {
                    u = p.x;
                    v = p.y;
                }
                std::snprintf(line, sizeof(line), "v %.6f %.6f %.6f\nvt %.5f %.5f\nvn %.5f %.5f %.5f\n", (p.x - origin.x) * invScale,
                              (p.y - origin.y) * invScale, (p.z - origin.z) * invScale, u * invUv, v * invUv, vn.x, vn.y, vn.z);
                obj += line;
                ++next;
            }
            std::snprintf(line, sizeof(line), "f %u/%u/%u %u/%u/%u %u/%u/%u\n", firstOfFace, firstOfFace, firstOfFace, firstOfFace + 1, firstOfFace + 1,
                          firstOfFace + 1, firstOfFace + 2, firstOfFace + 2, firstOfFace + 2);
            faces += line;
        }
        obj += faces;
        return obj;
    }

    bool RlArena::WriteObj(const std::string& path, float uvMetres, float scale, RlVec3 origin) const
    {
        const std::string text = ToObj(uvMetres, scale, origin);
        {
            // do not touch the file when it is already up to date (it is in the assets folder, the file watcher looks at it)
            std::ifstream existing(path, std::ios::binary);
            if (existing)
            {
                const std::string old((std::istreambuf_iterator<char>(existing)), std::istreambuf_iterator<char>());
                if (old == text)
                {
                    return true;
                }
            }
        }
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file)
        {
            return false;
        }
        file.write(text.data(), static_cast<std::streamsize>(text.size()));
        return static_cast<bool>(file);
    }
}
