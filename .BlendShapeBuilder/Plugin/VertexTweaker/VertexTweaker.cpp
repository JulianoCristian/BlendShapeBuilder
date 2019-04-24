#include "pch.h"
#include "VertexTweaker.h"

#define npEpsilon 0.0000001f

struct npMeshData
{
    int         *indices = nullptr;
    float3      *vertices = nullptr;
    float3      *normals = nullptr;
    float4      *tangents = nullptr;
    float2      *uv = nullptr;
    float       *selection = nullptr;
    int         num_vertices = 0;
    int         num_triangles = 0;
    float4x4    transform = float4x4::identity();
};

struct npSkinData
{
    Weights4    *weights = nullptr;
    float4x4    *bones = nullptr;
    float4x4    *bindposes = nullptr;
    int         num_vertices = 0;
    int         num_bones = 0;
    float4x4    root = float4x4::identity();
};


inline static int Raycast(
    const npMeshData& model, const float3 pos, const float3 dir, int& tindex, float& distance)
{
    float4x4 itrans = invert(model.transform);
    float3 rpos = mul_p(itrans, pos);
    float3 rdir = normalize(mul_v(itrans, dir));
    float d;
    int hit = RayTrianglesIntersectionIndexed(rpos, rdir, model.vertices, model.indices, model.num_triangles, tindex, d);
    if (hit) {
        float3 hpos = rpos + rdir * d;
        distance = length(mul_p(model.transform, hpos) - pos);
    }
    return hit;
}

inline static int RaycastWithoutTransform(
    const npMeshData& model, const float3 pos, const float3 dir, int& tindex, float& distance)
{
    float d;
    int hit = RayTrianglesIntersectionIndexed(pos, dir, model.vertices, model.indices, model.num_triangles, tindex, d);
    if (hit) {
        float3 hpos = pos + dir * d;
        distance = length(hpos - pos);
    }
    return hit;
}

#define npVertexBlockSize 1024

template<class Body>
inline static int SelectInside(const npMeshData& model, float3 pos, float radius, const Body& body, bool parallel = false)
{
    auto num_vertices = model.num_vertices;
    auto vertices = model.vertices;
    auto transform = model.transform;

    float rq = radius * radius;
    auto do_select = [&](int vi) -> bool {
        float3 p = mul_p(transform, vertices[vi]);
        float dsq = length_sq(p - pos);
        if (dsq <= rq) {
            body(vi, std::sqrt(dsq), p);
            return true;;
        }
        return false;
    };

    if (parallel) {
        std::atomic_int ret{ 0 };
        parallel_for_blocked(0, num_vertices, npVertexBlockSize, [&](int vi, int vend) {
            int c = 0;
            for (; vi < vend; ++vi) {
                if (do_select(vi)) {
                    ++c;
                }
            }
            ret += c;
        });
        return ret;
    }
    else {
        int ret = 0;
        for (int vi = 0; vi < num_vertices; ++vi) {
            if (do_select(vi)) {
                ++ret;
            }
        }
        return ret;
    }
}

static bool GetFurthestDistance(const npMeshData& model, float3 pos, bool mask, int &vidx, float &dist)
{
    auto num_vertices = model.num_vertices;
    auto vertices = model.vertices;
    auto selection = model.selection;

    float furthest_sq = FLT_MIN;
    int furthest_vi;

    float3 lpos = mul_p(invert(model.transform), pos);
    for (int vi = 0; vi < num_vertices; ++vi) {
        if (!mask || selection[vi] > 0.0f) {
            float dsq = length_sq(vertices[vi] - lpos);
            if (dsq > furthest_sq) {
                furthest_sq = dsq;
                furthest_vi = vi;
            }
        }
    }

    if (furthest_sq > FLT_MIN) {
        dist = length(mul_p(model.transform, vertices[furthest_vi]) - pos);
        vidx = furthest_vi;
        return true;
    }
    return false;
}

static inline int GetBrushSampleIndex(float distance, float bradius, int num_bsamples)
{
    return int(clamp01(1.0f - distance / bradius) * (num_bsamples - 1));
}
static inline float GetBrushSample(float distance, float bradius, float bsamples[], int num_bsamples)
{
    return bsamples[GetBrushSampleIndex(distance, bradius, num_bsamples)];
}


npAPI int npRaycast(
    npMeshData *model, const float3 pos, const float3 dir, int *tindex, float *distance)
{
    return Raycast(*model, pos, dir, *tindex, *distance);
}

npAPI float3 npPickNormal(
    npMeshData *model, const float3 pos, int ti)
{
    auto indices = model->indices;
    auto points = model->vertices;
    auto normals = model->normals;

    float3 p[3]{ points[indices[ti * 3 + 0]], points[indices[ti * 3 + 1]], points[indices[ti * 3 + 2]] };
    float3 n[3]{ normals[indices[ti * 3 + 0]], normals[indices[ti * 3 + 1]], normals[indices[ti * 3 + 2]] };
    float3 lpos = mul_p(invert(model->transform), pos);
    float3 r = triangle_interpolation(lpos, p[0], p[1], p[2], n[0], n[1], n[2]);
    return normalize(mul_v(model->transform, r));
}

static bool npSelectNearestImpl(npMeshData *model, const float4x4 *mvp_, float2 rmin, float2 rmax, float3 campos, int frontface_only,
    int& pick_index)
{
    auto num_vertices = model->num_vertices;
    auto vertices = model->vertices;
    auto normals = model->normals;
    auto selection = model->selection;

    float4x4 mvp = *mvp_;
    float3 lcampos = mul_p(invert(model->transform), campos);
    float2 rcenter = (rmin + rmax) * 0.5f;

    const int max_inside = 64;
    std::pair<int, float> insider[max_inside];
    int num_inside = 0;

    {
        // gather vertices inside rect
        std::atomic_int num_inside_a{ 0 };
        parallel_for(0, num_vertices, [&](int vi) {
            float4 vp = mul4(mvp, vertices[vi]);
            float2 sp = float2{ vp.x, vp.y } / vp.w;
            if (sp.x >= rmin.x && sp.x <= rmax.x &&
                sp.y >= rmin.y && sp.y <= rmax.y && vp.z > 0.0f)
            {
                bool hit = false;
                if (frontface_only) {
                    float3 vpos = vertices[vi];
                    float3 dir = normalize(vpos - lcampos);
                    int ti;
                    float distance;
                    if (RaycastWithoutTransform(*model, lcampos, dir, ti, distance)) {
                        float3 hitpos = lcampos + dir * distance;
                        if (length(vpos - hitpos) < 0.01f) {
                            hit = true;
                        }
                    }
                }
                else {
                    hit = true;
                }

                if (hit) {
                    int ii = num_inside_a++;
                    if (ii < max_inside) {
                        insider[ii].first = vi;
                        insider[ii].second = length(sp - rcenter);
                    }
                }
            }
        });
        num_inside = std::min<int>(num_inside_a, max_inside);
    }

    if (num_inside > 0) {
        // search nearest from center of rect
        int nearest_index = 0;
        float nearest_distance = FLT_MAX;
        float nearest_facing = 1.0f;

        for (int ii = 0; ii < num_inside; ++ii) {
            int vi = insider[ii].first;
            float distance = insider[ii].second;
            float3 dir = normalize(vertices[vi] - lcampos);

            // if there are vertices with identical position, pick most camera-facing one 
            if (near_equal(distance, nearest_distance, npEpsilon)) {
                float facing = dot(normals[vi], dir);
                if (facing < nearest_facing) {
                    nearest_index = vi;
                    nearest_distance = distance;
                    nearest_facing = facing;
                }
            }
            else if (distance < nearest_distance) {
                nearest_index = vi;
                nearest_distance = distance;
                nearest_facing = dot(normals[vi], dir);
            }
        }

        pick_index = nearest_index;
        return true;
    }
    return false;
}

npAPI int npPickVertex(
    npMeshData *model, const float4x4 *mvp_, float2 rmin, float2 rmax, float3 campos, int frontface_only, int *vi, float3 *vpos)
{
    int pick_index = -1;
    if (npSelectNearestImpl(model, mvp_, rmin, rmax, campos, frontface_only, pick_index)) {
        *vi = pick_index;
        *vpos = mul_p(model->transform, model->vertices[pick_index]);
        return true;
    }
    return false;
}

npAPI int npSelectSingle(
    npMeshData *model, const float4x4 *mvp_, float2 rmin, float2 rmax, float3 campos, float strength, int frontface_only)
{
    int pick_index;
    if (npSelectNearestImpl(model, mvp_, rmin, rmax, campos, frontface_only, pick_index)) {
        auto selection = model->selection;
        selection[pick_index] = clamp01(selection[pick_index] + strength);
        return 1;
    }
    return 0;
}


npAPI int npSelectTriangle(
    npMeshData *model, const float3 pos, const float3 dir, float strength)
{
    auto indices = model->indices;
    auto selection = model->selection;

    int ti;
    float distance;
    if (Raycast(*model, pos, dir, ti, distance)) {
        for (int i = 0; i < 3; ++i) {
            selection[indices[ti * 3 + i]] = clamp01(selection[indices[ti * 3 + i]] + strength);
        }
        return 1;
    }
    return 0;
}

npAPI int npSelectEdge(
    npMeshData *model, float strength, int clear, int mask)
{
    auto indices = IArray<int>(model->indices, model->num_triangles * 3);
    auto vertices = IArray<float3>(model->vertices, model->num_vertices);
    auto selection = model->selection;
    int num_vertices = model->num_vertices;

    RawVector<int> targets;
    if (mask) {
        targets.reserve(num_vertices);
        for (int vi = 0; vi < num_vertices; ++vi) {
            if (selection[vi] > 0.0f) {
                targets.push_back(vi);
            }
        }
    }
    else {
        targets.resize(num_vertices);
        for (int vi = 0; vi < num_vertices; ++vi) {
            targets[vi] = vi;
        }
    }

    if (clear) { memset(selection, 0, num_vertices * 4); }

    int ret = 0;
    SelectEdge(indices, 3, vertices, targets, [&](int vi) {
        selection[vi] = clamp01(selection[vi] + strength);
        ++ret;
    });
    return ret;
}

npAPI int npSelectHole(
    npMeshData *model, float strength, int clear, int mask)
{
    auto indices = IArray<int>(model->indices, model->num_triangles * 3);
    auto vertices = IArray<float3>(model->vertices, model->num_vertices);
    auto selection = model->selection;
    int num_vertices = model->num_vertices;

    RawVector<int> targets;
    if (mask) {
        targets.reserve(num_vertices);
        for (int vi = 0; vi < num_vertices; ++vi) {
            if (selection[vi] > 0.0f) {
                targets.push_back(vi);
            }
        }
    }
    else {
        targets.resize(num_vertices);
        for (int vi = 0; vi < num_vertices; ++vi) {
            targets[vi] = vi;
        }
    }

    if (clear) { memset(selection, 0, num_vertices * 4); }

    int ret = 0;
    SelectHole(indices, 3, vertices, targets, [&](int vi) {
        selection[vi] = clamp01(selection[vi] + strength);
        ++ret;
    });
    return ret;
}

npAPI int npSelectConnected(
    npMeshData *model, float strength, int clear)
{
    auto indices = IArray<int>(model->indices, model->num_triangles * 3);
    auto vertices = IArray<float3>(model->vertices, model->num_vertices);
    auto selection = model->selection;
    int num_vertices = model->num_vertices;

    RawVector<int> targets;
    targets.reserve(num_vertices);
    for (int vi = 0; vi < num_vertices; ++vi) {
        if (selection[vi] > 0.0f) {
            targets.push_back(vi);
        }
    }

    if (clear) { memset(selection, 0, num_vertices * 4); }

    int ret = 0;
    SelectConnected(indices, 3, vertices, targets, [&](int vi) {
        selection[vi] = clamp01(selection[vi] + strength);
        ++ret;
    });
    return ret;
}

npAPI int npSelectRect(
    npMeshData *model,
    const float4x4 *mvp_, float2 rmin, float2 rmax, float3 campos, float strength, int frontface_only)
{
    auto num_vertices = model->num_vertices;
    auto vertices = model->vertices;
    auto normals = model->normals;
    auto selection = model->selection;

    float4x4 mvp = *mvp_;
    float3 lcampos = mul_p(invert(model->transform), campos);

    std::atomic_int ret{ 0 };
    parallel_for_blocked(0, num_vertices, npVertexBlockSize, [&](int vi, int vend) {
        int c = 0;
        for (; vi < vend; ++vi) {
            float4 vp = mul4(mvp, vertices[vi]);
            float2 sp = float2{ vp.x, vp.y } / vp.w;
            if (sp.x >= rmin.x && sp.x <= rmax.x &&
                sp.y >= rmin.y && sp.y <= rmax.y && vp.z > 0.0f)
            {
                bool hit = false;
                if (frontface_only) {
                    float3 vpos = vertices[vi];
                    float3 dir = normalize(vpos - lcampos);
                    int ti;
                    float distance;
                    if (RaycastWithoutTransform(*model, lcampos, dir, ti, distance)) {
                        float3 hitpos = lcampos + dir * distance;
                        if (length(vpos - hitpos) < 0.01f) {
                            hit = true;
                        }
                    }
                }
                else {
                    hit = true;
                }

                if (hit) {
                    selection[vi] = clamp01(selection[vi] + strength);
                    ++c;
                }
            }
        }
        ret += c;
    });
    return ret;
}

npAPI int npSelectLasso(
    npMeshData *model,
    const float4x4 *mvp_, const float2 lasso[], int num_lasso_points, float3 campos, float strength, int frontface_only)
{
    if (num_lasso_points < 3) { return 0; }

    auto num_vertices = model->num_vertices;
    auto vertices = model->vertices;
    auto normals = model->normals;
    auto selection = model->selection;

    float4x4 mvp = *mvp_;
    float3 lcampos = mul_p(invert(model->transform), campos);

    float2 minp, maxp;
    MinMax(lasso, num_lasso_points, minp, maxp);

    RawVector<float> polyx, polyy;
    polyx.resize(num_lasso_points); polyy.resize(num_lasso_points);
    for (int i = 0; i < num_lasso_points; ++i) {
        polyx[i] = lasso[i].x;
        polyy[i] = lasso[i].y;
    }

    std::atomic_int ret{ 0 };
    parallel_for_blocked(0, num_vertices, npVertexBlockSize, [&](int vi, int vend) {
        int c = 0;
        for (; vi < vend; ++vi) {
            float4 vp = mul4(mvp, vertices[vi]);
            float2 sp = float2{ vp.x, vp.y } / vp.w;
            if (PolyInside(polyx.data(), polyy.data(), num_lasso_points, minp, maxp, sp)) {
                bool hit = false;
                if (frontface_only) {
                    float3 vpos = vertices[vi];
                    float3 dir = normalize(vpos - lcampos);
                    int ti;
                    float distance;
                    if (RaycastWithoutTransform(*model, lcampos, dir, ti, distance)) {
                        float3 hitpos = lcampos + dir * distance;
                        if (length(vpos - hitpos) < 0.01f) {
                            hit = true;
                        }
                    }
                }
                else {
                    hit = true;
                }

                if (hit) {
                    selection[vi] = clamp01(selection[vi] + strength);
                    ++c;
                }
            }
        }
        ret += c;
    });
    return ret;
}

npAPI int npSelectBrush(
    npMeshData *model,
    const float3 pos, float radius, float strength, int num_bsamples, float bsamples[])
{
    auto selection = model->selection;

    return SelectInside(*model, pos, radius, [&](int vi, float d, float3 p) {
        float s = GetBrushSample(d, radius, bsamples, num_bsamples) * strength;
        selection[vi] = clamp01(selection[vi] + s);
    }, true);
}

npAPI int npUpdateSelection(
    npMeshData *model,
    float3 *selection_pos, float3 *selection_normal)
{
    auto num_vertices = model->num_vertices;
    auto vertices = model->vertices;
    auto normals = model->normals;
    auto selection = model->selection;

    float st = 0.0f;
    int num_selected = 0;
    float3 spos = float3::zero();
    float3 snormal = float3::zero();
    quatf srot = quatf::identity();

    for (int vi = 0; vi < num_vertices; ++vi) {
        float s = selection[vi];
        if (s > 0.0f) {
            spos += vertices[vi] * s;
            snormal += normals[vi] * s;
            ++num_selected;
            st += s;
        }
    }

    if (num_selected > 0) {
        auto trans = model->transform;
        spos /= st;
        spos = mul_p(trans, spos);
        snormal = normalize(mul_v(trans, snormal));
        srot = to_quat(look33(snormal, {0,1,0}));
    }

    *selection_pos = spos;
    *selection_normal = snormal;
    return num_selected;
}


npAPI void npAssignVertices(
    npMeshData *model, float3 value, float4x4 trans, int xyz, int mask)
{
    auto num_vertices = model->num_vertices;
    auto vertices = model->vertices;
    auto selection = model->selection;
    auto itrans = invert(trans);

    for (int vi = 0; vi < num_vertices; ++vi) {
        float s = mask ? selection[vi] : 1.0f;
        if (s == 0.0f) continue;

        float3 v1 = mul_p(trans, vertices[vi]);
        float3 v2 = v1;
        if (xyz & 1) v2.x = value.x;
        if (xyz & 2) v2.y = value.y;
        if (xyz & 4) v2.z = value.z;
        vertices[vi] = mul_p(itrans, lerp(v1, v2, s));
    }
}

npAPI void npMoveVertices(
    npMeshData *model, float3 value, int mask)
{
    auto num_vertices = model->num_vertices;
    auto vertices = model->vertices;
    auto selection = model->selection;

    value = mul_v(invert(model->transform), value);
    for (int vi = 0; vi < num_vertices; ++vi) {
        float s = mask ? selection[vi] : 1.0f;
        if (s == 0.0f) continue;

        vertices[vi] = (vertices[vi] + value * s);
    }
}

npAPI void npRotatePivotVertices(
    npMeshData *model, quatf value, float3 pivot_pos, quatf pivot_rot, int mask)
{
    float3 axis;
    float angle;
    to_axis_angle(value, axis, angle);
    if (near_equal(angle, 0.0f) || std::isnan(angle)) {
        return;
    }

    auto num_vertices = model->num_vertices;
    auto vertices = model->vertices;
    auto selection = model->selection;

    auto ptrans = to_mat4x4(invert(pivot_rot)) * translate(pivot_pos);
    auto iptrans = invert(ptrans);
    auto trans = model->transform;
    auto itrans = invert(trans);

    auto to_pivot_space = trans * iptrans;
    auto to_local_space = ptrans * itrans;
    auto rotation = to_pivot_space * to_mat4x4(value) * to_local_space;

    for (int vi = 0; vi < num_vertices; ++vi) {
        float s = mask ? selection[vi] : 1.0f;
        if (s == 0.0f) continue;
        vertices[vi] = lerp(vertices[vi], mul_p(rotation, vertices[vi]), s);
    }
}

npAPI void npScaleVertices(
    npMeshData *model, float3 value, float3 pivot_pos, quatf pivot_rot, int mask)
{
    auto num_vertices = model->num_vertices;
    auto vertices = model->vertices;
    auto selection = model->selection;

    auto ptrans = to_mat4x4(invert(pivot_rot)) * translate(pivot_pos);
    auto iptrans = invert(ptrans);
    auto trans = model->transform;
    auto itrans = invert(trans);

    auto to_pivot_space = trans * iptrans;
    auto to_local_space = ptrans * itrans;
    auto scale = to_pivot_space * scale44(value) * to_local_space;

    for (int vi = 0; vi < num_vertices; ++vi) {
        float s = mask ? selection[vi] : 1.0f;
        if (s == 0.0f) continue;
        vertices[vi] = lerp(vertices[vi], mul_p(scale, vertices[vi]), s);
    }
}

npAPI void npSmooth(
    npMeshData *model, float radius, float strength, int mask)
{
    auto num_vertices = model->num_vertices;
    auto vertices = model->vertices;
    auto normals = model->normals;
    auto selection = model->selection;

    RawVector<float3> tvertices;
    tvertices.resize(num_vertices);
    parallel_for(0, num_vertices, [&](int vi) {
        tvertices[vi] = mul_p(model->transform, vertices[vi]);
    });

    float rsq = radius * radius;
    parallel_for(0, num_vertices, [&](int vi) {
        float s = mask ? selection[vi] : 1.0f;
        if (s == 0.0f) { return; }

        float3 p = tvertices[vi];
        float3 average = float3::zero();
        for (int i = 0; i < num_vertices; ++i) {
            float s2 = selection ? selection[i] : 1.0f;
            float dsq = length_sq(tvertices[i] - p);
            if (dsq <= rsq) {
                average += normals[i] * s2;
            }
        }
        average = normalize(average);
        normals[vi] = normalize(normals[vi] + average * (strength * s));
    });
}

npAPI int npWeld(
    npMeshData *model, int smoothing, float weld_angle, int mask)
{
    auto num_vertices = model->num_vertices;
    auto vertices = model->vertices;
    auto normals = model->normals;
    auto selection = model->selection;

    RawVector<bool> checked;
    checked.resize(num_vertices);
    checked.zeroclear();

    int ret = 0;
    RawVector<int> shared;
    for (int vi = 0; vi < num_vertices; ++vi) {
        if (checked[vi]) { continue; }
        float s = mask ? selection[vi] : 1.0f;
        if (s == 0.0f) { continue; }

        float3 p = vertices[vi];
        float3 n = normals[vi];
        for (int i = 0; i < num_vertices; ++i) {
            if (vi != i && !checked[i] &&
                length(vertices[i] - p) < npEpsilon &&
                angle_between(n, normals[i]) * Rad2Deg <= weld_angle)
            {
                if (smoothing) n += normals[i];
                shared.push_back(i);
                checked[i] = true;
            }
        }

        if (!shared.empty()) {
            n = normalize(n);
            normals[vi] = n;
            for (int si : shared) {
                normals[si] = n;
            }
            shared.clear();
            ++ret;
        }
    }

    return ret;
}


npAPI int npWeld2(
    npMeshData *model, int num_targets, npMeshData targets[],
    int weld_mode, float weld_angle, int mask)
{
    auto num_vertices = model->num_vertices;
    auto vertices = model->vertices;
    auto normals = model->normals;
    auto selection = model->selection;

    float4x4 trans = model->transform;
    float4x4 itrans = invert(trans);

    RawVector<float4x4> titrans;
    RawVector<float3> wvertices, wnormals;
    std::vector<RawVector<float3>> twvertices, twnormals;

    // generate world space vertices
    wvertices.resize(num_vertices);
    wnormals.resize(num_vertices);
    for (int vi = 0; vi < num_vertices; ++vi) {
        wvertices[vi] = mul_p(trans, vertices[vi]);
        wnormals[vi] = mul_v(trans, normals[vi]);
    }

    titrans.resize(num_targets);
    twvertices.resize(num_targets);
    twnormals.resize(num_targets);
    for (int ti = 0; ti < num_targets; ++ti) {
        auto tt = targets[ti].transform;
        titrans[ti] = invert(tt);

        auto tva = targets[ti].vertices;
        auto tna = targets[ti].normals;
        auto& twva = twvertices[ti];
        auto& twna = twnormals[ti];
        int num_tv = targets[ti].num_vertices;
        twva.resize(num_tv);
        twna.resize(num_tv);
        for (int tvi = 0; tvi < num_tv; ++tvi) {
            twva[tvi] = mul_p(tt, tva[tvi]);
            twna[tvi] = mul_v(tt, tna[tvi]);
        }
    }

    std::vector<RawVector<std::pair<int, int>>> weld_maps;
    weld_maps.resize(num_targets);

    // generate weld maps
    for (int ti = 0; ti < num_targets; ++ti) {
        auto& weld_map = weld_maps[ti];

        for (int vi = 0; vi < num_vertices; ++vi) {
            float s = mask ? selection[vi] : 1.0f;
            if (s == 0.0f) { continue; }

            auto p = wvertices[vi];
            auto n = wnormals[vi];
            auto& twva = twvertices[ti];
            auto& twna = twnormals[ti];
            int num_tv = targets[ti].num_vertices;
            for (int tvi = 0; tvi < num_tv; ++tvi) {
                if (length(twva[tvi] - p) < npEpsilon && angle_between(n, twna[tvi]) * Rad2Deg <= weld_angle) {
                    weld_map.push_back({ vi, tvi });
                }
            }
        }
    }

    int ret = 0;
    for (auto& map : weld_maps) { ret += (int)map.size(); }
    if (ret == 0) { return 0; } // no vertices to weld


    if (weld_mode == 0) {
        // copy to targets
        for (int ti = 0; ti < num_targets; ++ti) {
            auto& weld_map = weld_maps[ti];
            auto it = titrans[ti];
            auto tna = targets[ti].normals;
            for (auto& rel : weld_map) {
                tna[rel.second] = mul_v(it, wnormals[rel.first]);
            }
        }
    }
    else if (weld_mode == 1) {
        // copy from targets
        for (int ti = 0; ti < num_targets; ++ti) {
            auto& weld_map = weld_maps[ti];
            auto& twna = twnormals[ti];
            for (auto& rel : weld_map) {
                normals[rel.first] = mul_v(itrans, twna[rel.second]);
            }
        }
    }
    else if (weld_mode == 2) {
        // smooth
        RawVector<float3> tmp_wnormals = wnormals;

        for (int ti = 0; ti < num_targets; ++ti) {
            auto& weld_map = weld_maps[ti];
            auto& twna = twnormals[ti];
            for (auto& rel : weld_map) {
                tmp_wnormals[rel.first] += twna[rel.second];
            }
        }
        for (auto& n : tmp_wnormals) {
            n = normalize(n);
        }

        for (int ti = 0; ti < num_targets; ++ti) {
            auto& weld_map = weld_maps[ti];
            auto it = titrans[ti];
            auto tna = targets[ti].normals;
            for (auto& rel : weld_map) {
                normals[rel.first] = mul_v(itrans, tmp_wnormals[rel.first]);
                tna[rel.second] = mul_v(it, tmp_wnormals[rel.first]);
            }
        }
    }
    return ret;
}


npAPI int npBrushReplace(
    npMeshData *model,
    const float3 pos, float radius, float strength, int num_bsamples, float bsamples[], float3 value, int mask)
{
    auto normals = model->normals;
    auto selection = model->selection;
    auto sign = strength < 0.0f ? -1.0f : 1.0f;

    return SelectInside(*model, pos, radius, [&](int vi, float d, float3 p) {
        float s = GetBrushSample(d, radius, bsamples, num_bsamples) * abs(strength);
        if (mask) s *= selection[vi];

        normals[vi] = normalize(normals[vi] + value * (s * sign));
    }, true);
    return 0;
}

npAPI int npBrushPaint(
    npMeshData *model,
    const float3 pos, float radius, float strength, int num_bsamples, float bsamples[], float3 n, int blend_mode, int mask)
{
    auto normals = model->normals;
    auto selection = model->selection;
    auto sign = strength < 0.0f ? -1.0f : 1.0f;

    float3 ln = n;
    n = normalize(mul_v(model->transform, n));
    auto itrans = invert(model->transform);
    return SelectInside(*model, pos, radius, [&](int vi, float d, float3 p) {
        int bsi = GetBrushSampleIndex(d, radius, num_bsamples);
        float s = saturate(bsamples[bsi] * abs(strength) * 2.0f);
        if (mask) s *= selection[vi];

        float slope;
        if (bsi == 0) {
            slope = (bsamples[bsi+1] - bsamples[bsi  ]) / (1.0f / (num_bsamples - 1));
        }
        else if (bsi == num_bsamples - 1) {
            slope = (bsamples[bsi  ] - bsamples[bsi-1]) / (1.0f / (num_bsamples - 1));
        }
        else {
            slope = (bsamples[bsi+1] - bsamples[bsi-1]) / (1.0f / (num_bsamples - 1) * 2.0f);
        }

        float3 t;
        {
            float3 p1 = pos - n * plane_distance(pos, n);
            float3 p2 = p - n * plane_distance(p, n);
            t = normalize(p2 - p1);
        }
        if (slope < 0.0f) {
            t *= -1.0f;
            slope *= -1.0f;
        }

        float3 vn = normals[vi];
        float3 r = lerp(n, t * sign, clamp01(slope * 0.5f));
        r = normalize(mul_v(itrans, r));

        // maybe add something here later
        //switch (blend_mode) {
        //}
        r = lerp(vn, r, s);

        normals[vi] = normalize(vn + r * s);
    }, true);
}

npAPI int npBrushLerp(
    npMeshData *model,
    const float3 pos, float radius, float strength, int num_bsamples, float bsamples[], const float3 n0[], const float3 n1[], int mask)
{
    auto normals = model->normals;
    auto selection = model->selection;
    auto sign = strength < 0.0f ? -1.0f : 1.0f;

    return SelectInside(*model, pos, radius, [&](int vi, float d, float3 p) {
        float s = GetBrushSample(d, radius, bsamples, num_bsamples) * abs(strength);
        if (mask) s *= selection[vi];

        normals[vi] = normalize(lerp(n1[vi], n0[vi] * sign, s));
    }, true);
}

npAPI int npBrushSmooth(
    npMeshData *model,
    const float3 pos, float radius, float strength, int num_bsamples, float bsamples[], int mask)
{
    auto normals = model->normals;
    auto selection = model->selection;

    RawVector<std::pair<int, float>> inside;
    SelectInside(*model, pos, radius, [&](int vi, float d, float3 p) {
        inside.push_back({ vi, d });
    });

    float3 average = float3::zero();
    for (auto& p : inside) {
        average += normals[p.first];
    }
    average = normalize(average);

    for (auto& p : inside) {
        // ignore sign of strength
        float s = GetBrushSample(p.second, radius, bsamples, num_bsamples) * abs(strength);
        if (mask) s *= selection[p.first];

        normals[p.first] = normalize(normals[p.first] + average * s);
    }
    return (int)inside.size();
}

template<class RayDirs>
inline int BrushProjectionImpl(
    npMeshData *model,
    const float3 pos, float radius, float strength, int num_bsamples, float bsamples[], int mask,
    npMeshData *normal_source, const RayDirs& ray_dirs)
{
    auto vertices = model->vertices;
    auto normals = model->normals;
    auto selection = model->selection;

    auto pnum_triangles = normal_source->num_triangles;
    auto pnormals = normal_source->normals;
    auto pindices = normal_source->indices;

    auto to_local = normal_source->transform * invert(model->transform);
    RawVector<float3> pvertices;
    pvertices.resize(normal_source->num_vertices);
    for (int vi = 0; vi < normal_source->num_vertices; ++vi) {
        pvertices[vi] = mul_p(to_local, normal_source->vertices[vi]);
    }

    auto sign = strength < 0.0f ? -1.0f : 1.0f;

    return SelectInside(*model, pos, radius, [&](int vi, float d, float3 p) {
        float s = GetBrushSample(d, radius, bsamples, num_bsamples) * abs(strength);
        if (mask) s *= selection[vi];

        float3 rpos = vertices[vi];
        float3 rdir = ray_dirs[vi];
        int ti;
        float distance;
        int num_hit = RayTrianglesIntersectionIndexed(rpos, rdir, pvertices.data(), pindices, pnum_triangles, ti, distance);

        if (num_hit > 0) {
            float3 result = triangle_interpolation(
                rpos + rdir * distance,
                { pvertices[pindices[ti * 3 + 0]] },
                { pvertices[pindices[ti * 3 + 1]] },
                { pvertices[pindices[ti * 3 + 2]] },
                pnormals[pindices[ti * 3 + 0]],
                pnormals[pindices[ti * 3 + 1]],
                pnormals[pindices[ti * 3 + 2]]);

            result = normalize(mul_v(to_local, result));
            normals[vi] = normalize(lerp(normals[vi], result * sign, s));
        }
    }, true);
}

npAPI int npBrushProjection(
    npMeshData *model,
    const float3 pos, float radius, float strength, int num_bsamples, float bsamples[], int mask,
    npMeshData *normal_source, float3 ray_dirs[])
{
    return BrushProjectionImpl(model, pos, radius, strength, num_bsamples, bsamples, mask, normal_source, ray_dirs);
}

npAPI int npBrushProjection2(
    npMeshData *model,
    const float3 pos, float radius, float strength, int num_bsamples, float bsamples[], int mask,
    npMeshData *normal_source, float3 ray_dir)
{
    struct RayDir
    {
        float3 ray_dir;
        const float3& operator[](int) const { return ray_dir; }
    } ray_dirs = { ray_dir };
    return BrushProjectionImpl(model, pos, radius, strength, num_bsamples, bsamples, mask, normal_source, ray_dirs);
}


npAPI int npBuildMirroringRelation(
    npMeshData *model, float3 mirror_plane, float epsilon, int relation[])
{
    if (!relation) { return 0; }

    // per-vertex detection
    auto num_vertices = model->num_vertices;
    auto vertices = model->vertices;
    auto normals = model->normals;

    RawVector<float> distances;
    distances.resize(num_vertices);
    parallel_for(0, num_vertices, [&](int vi) {
        distances[vi] = plane_distance(vertices[vi], mirror_plane);
    });

    std::atomic_int ret{ 0 };
    parallel_for(0, num_vertices, [&](int vi) {
        int rel = -1;
        float d1 = distances[vi];
        if (d1 < 0.0f) {
            for (int i = 0; i < num_vertices; ++i) {
                float d2 = distances[i];
                if (d2 > 0.0f &&
                    length(vertices[vi] - (vertices[i] - mirror_plane * (d2 * 2.0f))) < epsilon)
                {
                    float3 n1 = normals[vi];
                    float3 n2 = plane_mirror(normals[i], mirror_plane);
                    if (dot(n1, n2) >= 0.99f) {
                        rel = i;
                        ++ret;
                        break;
                    }
                }
            }
        }
        else if (near_equal(d1, 0.0f)) {
            rel = -2; // -2: on mirror plane
        }
        relation[vi] = rel;
    });

#if 0
    // per-triangle detection
    auto num_triangles = model->num_triangles;
    auto indices = model->indices;
    auto vertices = model->vertices;

    RawVector<float3> tri_normals(num_triangles);
    RawVector<float3> tri_position(num_triangles);
    RawVector<float> tri_distances(num_triangles);
    RawVector<int> tri_relations(num_triangles);

    parallel_for(0, num_triangles, [&](int ti) {
        int i0 = indices[ti * 3 + 0];
        int i1 = indices[ti * 3 + 1];
        int i2 = indices[ti * 3 + 2];
        auto v0 = vertices[i0];
        auto v1 = vertices[i1];
        auto v2 = vertices[i2];
        float3 tri_pos = (v0 + v1 + v2) * 0.33333333333f;
        tri_normals[ti] = normalize(cross(v1 - v0, v2 - v0));
        tri_position[ti] = tri_pos;
        tri_distances[ti] = plane_distance(tri_pos, mirror_plane);
    });

    std::atomic_int ret{ 0 };
    parallel_for(0, num_triangles, [&](int ti1) {
        int rel = -1;
        float d1 = tri_distances[ti1];
        if (d1 < 0.0f) {
            for (int ti2 = 0; ti2 < num_triangles; ++ti2) {
                float d2 = tri_distances[ti2];
                if (d2 > 0.0f && near_equal(abs(d1), d2, epsilon))
                {
                    float3 n1 = tri_normals[ti1];
                    float3 n2 = plane_mirror(tri_normals[ti2], mirror_plane);
                    if (dot(n1, n2) >= 0.99f &&
                        length(tri_position[ti1] - plane_mirror(tri_position[ti2], mirror_plane)) < epsilon)
                    {
                        rel = ti2;
                        ++ret;
                        break;
                    }
                }
            }
        }
        tri_relations[ti1] = rel;
    });

    for (int ti = 0; ti < num_triangles; ++ti) {
        int trel = tri_relations[ti];
        int i0 = indices[ti * 3 + 0];
        int i1 = indices[ti * 3 + 1];
        int i2 = indices[ti * 3 + 2];

        int ri0, ri1, ri2;
        if (trel == -1) {
            ri0 = ri1 = ri2 = -1;
        }
        else {
            auto search_nearest = [&](int vi) {
                const float3& pos = vertices[vi];
                int nearest_index;
                float nearest_distance;
                for (int ci = 0; ci < 3; ++ci) {
                    int idx = indices[trel * 3 + ci];
                    float3 rpos = vertices[idx];
                    float d1 = plane_distance(pos, mirror_plane);
                    float d = length(pos - (rpos + mirror_plane * (d1 * 2.0f)));
                    if (ci == 0 || d < nearest_distance) {
                        nearest_index = idx;;
                        nearest_distance = d;
                    }
                }
                return nearest_distance <= epsilon ? nearest_index : -1;
            };
            ri0 = search_nearest(i0);
            ri1 = search_nearest(i1);
            ri2 = search_nearest(i2);
        }
        relation[i0] = ri0;
        relation[i1] = ri1;
        relation[i2] = ri2;
    }
#endif

    return ret;
}

npAPI void npApplyMirroring(
    npMeshData *model, const int relation[], float3 mirror_plane, float3 *vertices, float3 *normals, float4 *tangents)
{
    if (!relation) { return; }

    auto num_vertices = model->num_vertices;
    if (vertices) {
        for (int vi = 0; vi < num_vertices; ++vi) {
            int ri = relation[vi];
            if (ri >= 0) {
                vertices[ri] = plane_mirror(vertices[vi], mirror_plane);
            }
            else if (ri == -2) {
                // project to mirror plane
                float3 v = vertices[vi];
                float d = plane_distance(v, mirror_plane);
                vertices[vi] = v - (mirror_plane * d);
            }
        }
    }
    if (normals) {
        for (int vi = 0; vi < num_vertices; ++vi) {
            int ri = relation[vi];
            if (ri >= 0)
                normals[ri] = plane_mirror(normals[vi], mirror_plane);
        }
    }
    if (tangents) {
        for (int vi = 0; vi < num_vertices; ++vi) {
            int ri = relation[vi];
            if (ri >= 0)
                (float3&)tangents[ri] = plane_mirror((float3&)tangents[vi], mirror_plane);
        }
    }
}


template<class RayDirs>
inline void ProjectNormalsImpl(
    npMeshData *model, npMeshData *target, const RayDirs& ray_dirs, int mask)
{
    auto num_vertices = model->num_vertices;
    auto vertices = model->vertices;
    auto normals = model->normals;
    auto selection = model->selection;

    auto pnum_triangles = target->num_triangles;
    auto pvertices = target->vertices;
    auto pnormals = target->normals;
    auto pindices = target->indices;

    auto to_local = target->transform * invert(model->transform);
    RawVector<float> soa[9]; // flattened + SoA-nized vertices (faster on CPU)

    // flatten + SoA-nize
    {
        for (int i = 0; i < 9; ++i) {
            soa[i].resize(pnum_triangles);
        }
        for (int ti = 0; ti < pnum_triangles; ++ti) {
            for (int i = 0; i < 3; ++i) {
                auto p = mul_p(to_local, pvertices[pindices[ti * 3 + i]]);
                soa[i * 3 + 0][ti] = p.x;
                soa[i * 3 + 1][ti] = p.y;
                soa[i * 3 + 2][ti] = p.z;
            }
        }
    }

    parallel_for(0, num_vertices, [&](int vi) {
        float s = mask ? selection[vi] : 1.0f;
        if (s == 0.0f) { return; }

        float3 rpos = vertices[vi];
        float3 rdir = ray_dirs[vi];
        int ti;
        float distance;
        int num_hit = RayTrianglesIntersectionSoA(rpos, rdir,
            soa[0].data(), soa[1].data(), soa[2].data(),
            soa[3].data(), soa[4].data(), soa[5].data(),
            soa[6].data(), soa[7].data(), soa[8].data(),
            pnum_triangles, ti, distance);

        if (num_hit > 0) {
            float3 result = triangle_interpolation(
                rpos + rdir * distance,
                { soa[0][ti], soa[1][ti], soa[2][ti] },
                { soa[3][ti], soa[4][ti], soa[5][ti] },
                { soa[6][ti], soa[7][ti], soa[8][ti] },
                pnormals[pindices[ti * 3 + 0]],
                pnormals[pindices[ti * 3 + 1]],
                pnormals[pindices[ti * 3 + 2]]);

            result = normalize(mul_v(to_local, result));
            normals[vi] = normalize(lerp(normals[vi], result, s));
        }
    });
}

npAPI void npProjectNormals(
    npMeshData *model, npMeshData *target, const float3 ray_dirs[], int mask)
{
    ProjectNormalsImpl(model, target, ray_dirs, mask);
}

npAPI void npProjectNormals2(
    npMeshData *model, npMeshData *target, const float3 ray_dir, int mask)
{
    struct RayDir
    {
        float3 ray_dir;
        const float3& operator[](int) const { return ray_dir; }
    } ray_dirs = { ray_dir };
    ProjectNormalsImpl(model, target, ray_dirs, mask);
}

enum class npProjectVerticesMode
{
    Forward,
    Backward,
    ForwardAndBackward,
};

template<class RayDirs>
void npProjectVerticesImpl(
    npMeshData *model, npMeshData *target, const RayDirs& ray_dirs, npProjectVerticesMode mode, float max_distance, int PNT, bool mask)
{
    auto num_vertices = model->num_vertices;
    auto vertices = model->vertices;
    auto normals = model->normals;
    auto tangents = model->tangents;
    auto selection = model->selection;

    auto pnum_triangles = target->num_triangles;
    auto pvertices = target->vertices;
    auto pnormals = target->normals;
    auto ptangents = target->tangents;
    auto pindices = target->indices;

    auto to_local = target->transform * invert(model->transform);
    RawVector<float> soa[9]; // flattened + SoA-nized vertices (faster on CPU)

                             // flatten + SoA-nize
    {
        for (int i = 0; i < 9; ++i) {
            soa[i].resize(pnum_triangles);
        }
        for (int ti = 0; ti < pnum_triangles; ++ti) {
            for (int i = 0; i < 3; ++i) {
                auto p = mul_p(to_local, pvertices[pindices[ti * 3 + i]]);
                soa[i * 3 + 0][ti] = p.x;
                soa[i * 3 + 1][ti] = p.y;
                soa[i * 3 + 2][ti] = p.z;
            }
        }
    }

    parallel_for(0, num_vertices, [&](int vi) {
        float s = mask ? selection[vi] : 1.0f;
        if (s == 0.0f) { return; }

        float3 rpos = vertices[vi];
        float3 rdir = ray_dirs[vi];
        int ti;
        float distance;

        float3 rvertex;
        float3 rnormal = float3::zero();
        float4 rtangents = float4::zero();
        float min_distance;
        bool hit = false;

        auto gen_normal = [&soa, &to_local, pnormals, pindices](const float3& pos, int ti) {
            if (pnormals) {
                float3 ret = triangle_interpolation(
                    pos,
                    { soa[0][ti], soa[1][ti], soa[2][ti] },
                    { soa[3][ti], soa[4][ti], soa[5][ti] },
                    { soa[6][ti], soa[7][ti], soa[8][ti] },
                    pnormals[pindices[ti * 3 + 0]],
                    pnormals[pindices[ti * 3 + 1]],
                    pnormals[pindices[ti * 3 + 2]]);
                ret = normalize(mul_v(to_local, ret));
                return ret;
            }
            else {
                return float3::zero();
            }
        };
        auto gen_tangent = [&soa, &to_local, ptangents, pindices](const float3& pos, int ti) {
            if (ptangents) {
                float4 ret = triangle_interpolation(
                    pos,
                    { soa[0][ti], soa[1][ti], soa[2][ti] },
                    { soa[3][ti], soa[4][ti], soa[5][ti] },
                    { soa[6][ti], soa[7][ti], soa[8][ti] },
                    ptangents[pindices[ti * 3 + 0]],
                    ptangents[pindices[ti * 3 + 1]],
                    ptangents[pindices[ti * 3 + 2]]);
                (float3&)ret = normalize(mul_v(to_local, (float3&)ret));
                return ret;
            }
            else {
                return float4::zero();
            }
        };

        if (mode == npProjectVerticesMode::Forward || mode == npProjectVerticesMode::ForwardAndBackward) {
            int num_hit = RayTrianglesIntersectionSoA(rpos, rdir,
                soa[0].data(), soa[1].data(), soa[2].data(),
                soa[3].data(), soa[4].data(), soa[5].data(),
                soa[6].data(), soa[7].data(), soa[8].data(),
                pnum_triangles, ti, distance);
            if (num_hit > 0 && distance < max_distance) {
                hit = true;
                min_distance = distance;
                rvertex = rpos + rdir * distance;
                rnormal = gen_normal(rpos + rdir * distance, ti);
                rtangents = gen_tangent(rpos + rdir * distance, ti);
            }
        }
        if (mode == npProjectVerticesMode::Backward || mode == npProjectVerticesMode::ForwardAndBackward) {
            int num_hit = RayTrianglesIntersectionSoA(rpos, -rdir,
                soa[0].data(), soa[1].data(), soa[2].data(),
                soa[3].data(), soa[4].data(), soa[5].data(),
                soa[6].data(), soa[7].data(), soa[8].data(),
                pnum_triangles, ti, distance);
            if (num_hit > 0 && distance < max_distance && (!hit || distance < min_distance)) {
                hit = true;
                min_distance = distance;
                rvertex = rpos + -rdir * distance;
                rnormal = gen_normal(rpos + -rdir * distance, ti);
                rtangents = gen_tangent(rpos + -rdir * distance, ti);
            }
        }
        if (hit) {
            if (PNT & 1) {
                vertices[vi] = lerp(vertices[vi], rvertex, s);
            }
            if (normals && (PNT & 2)) {
                normals[vi] = normalize(lerp(normals[vi], rnormal, s));
            }
            if (tangents && (PNT & 4)) {
                (float3&)tangents[vi] = normalize(lerp((float3&)tangents[vi], (float3&)rtangents, s));
            }
        }
    });
}

npAPI void npProjectVertices(
    npMeshData *model, npMeshData *target, const float3 ray_dirs[], npProjectVerticesMode mode, float max_distance, int PNT, int mask)
{
    npProjectVerticesImpl(model, target, ray_dirs, mode, max_distance, PNT, mask);
}

npAPI void npProjectVerticesRadial(
    npMeshData *model, npMeshData *target, const float3 center, npProjectVerticesMode mode, float max_distance, int PNT, int mask)
{
    auto to_local = invert(model->transform);

    struct RayDir
    {
        const float3 *vertices;
        const float3 *normals;
        float3 center;
        float3 operator[](int vi) const
        {
            float3 ray_dir = normalize(vertices[vi] - center);
            float3 n = normals[vi];
            return dot(ray_dir, n) > 0 ? ray_dir : -ray_dir;
        }
    } ray_dirs = { model->vertices, model->normals, mul_p(to_local, center) };
    npProjectVerticesImpl(model, target, ray_dirs, mode, max_distance, PNT, mask);
}

npAPI void npProjectVerticesDirectional(
    npMeshData *model, npMeshData *target, const float3 ray_dir, npProjectVerticesMode mode, float max_distance, int PNT, int mask)
{
    auto to_local = invert(model->transform);

    struct RayDir
    {
        const float3 *normals;
        float3 ray_dir;
        float3 operator[](int vi) const {
            float3 n = normals[vi];
            return dot(ray_dir, n) > 0 ? ray_dir : -ray_dir;
        }
    } ray_dirs = { model->normals, normalize(mul_v(to_local, ray_dir)) };
    npProjectVerticesImpl(model, target, ray_dirs, mode, max_distance, PNT, mask);
}

template<int NumInfluence>
static void SkinningImpl(
    int num_vertices, const RawVector<float4x4>& poses, const Weights<NumInfluence> weights[],
    const float3 ipoints[], const float3 inormals[], const float4 itangents[],
    float3 opoints[], float3 onormals[], float4 otangents[])
{
    parallel_invoke(
    [&]() {
        if (ipoints && opoints) {
            for (int vi = 0; vi < num_vertices; ++vi) {
                const auto& w = weights[vi];
                float3 p = ipoints[vi];
                float3 rp = float3::zero();
                for (int bi = 0; bi < NumInfluence; ++bi) {
                    rp += mul_p(poses[w.indices[bi]], p) * w.weights[bi];
                }
                opoints[vi] = rp;
            }
        }
    },
    [&]() {
        if (inormals && onormals) {
            for (int vi = 0; vi < num_vertices; ++vi) {
                const auto& w = weights[vi];
                float3 n = inormals[vi];
                float3 rn = float3::zero();
                for (int bi = 0; bi < NumInfluence; ++bi) {
                    rn += mul_v(poses[w.indices[bi]], n) * w.weights[bi];
                }
                onormals[vi] = normalize(rn);
            }
        }
    },
    [&]() {
        if (itangents && otangents) {
            for (int vi = 0; vi < num_vertices; ++vi) {
                const auto& w = weights[vi];
                float4 t = itangents[vi];
                float4 rt = float4::zero();
                for (int bi = 0; bi < NumInfluence; ++bi) {
                    rt += mul_v(poses[w.indices[bi]], t) * w.weights[bi];
                }
                otangents[vi] = rt;
            }
        }
    });
}

npAPI void npApplySkinning(
    npSkinData *skin,
    const float3 ipoints[], const float3 inormals[], const float4 itangents[],
    float3 opoints[], float3 onormals[], float4 otangents[])
{
    RawVector<float4x4> poses;
    poses.resize(skin->num_bones);

    auto iroot = invert(skin->root);
    for (int bi = 0; bi < skin->num_bones; ++bi) {
        poses[bi] = skin->bindposes[bi] * skin->bones[bi] * iroot;
    }
    SkinningImpl(skin->num_vertices, poses, skin->weights, ipoints, inormals, itangents, opoints, onormals, otangents);
}

npAPI void npApplyReverseSkinning(
    npSkinData *skin,
    const float3 ipoints[], const float3 inormals[], const float4 itangents[],
    float3 opoints[], float3 onormals[], float4 otangents[])
{
    RawVector<float4x4> poses;
    poses.resize(skin->num_bones);

    auto iroot = invert(skin->root);
    for (int bi = 0; bi < skin->num_bones; ++bi) {
        poses[bi] = invert(skin->bindposes[bi] * skin->bones[bi] * iroot);
    }
    SkinningImpl(skin->num_vertices, poses, skin->weights, ipoints, inormals, itangents, opoints, onormals, otangents);
}


npAPI void npGenerateNormals(npMeshData *model, float3 dst[])
{
    if (!dst) dst = model->normals;
    if (!dst || !model->vertices || !model->indices) return;
    GenerateNormalsTriangleIndexed(dst, model->vertices, model->indices, model->num_triangles, model->num_vertices);
}

npAPI void npGenerateTangents(npMeshData *model, float4 dst[])
{
    if (!dst) dst = model->tangents;
    if (!dst || !model->vertices || !model->uv || !model->normals || !model->indices) return;
    GenerateTangentsTriangleIndexed(dst,
        model->vertices, model->uv, model->normals, model->indices, model->num_triangles, model->num_vertices);
}

npAPI void npGenerateTerrainMesh(
    const float heightmap[], int width, int height, float3 size,
    float3 dst_vertices[], float3 dst_normals[], float2 dst_uv[], int dst_indices[])
{
    int num_vertices = width * height;
    int num_triangles = (width - 1) * (height - 1) * 2;
    auto size_unit = float3{ 1.0f / (width - 1), 1.0f, 1.0f / (height - 1) } *size;
    auto uv_unit = float2{ 1.0f / (width - 1), 1.0f / (height - 1) };

    parallel_invoke(
    [&]() {
        for (int iy = 0; iy < height; ++iy) {
            for (int ix = 0; ix < width; ++ix) {
                int i = iy * width + ix;
                dst_vertices[i] = float3{ (float)ix, heightmap[i], (float)iy } * size_unit;
                dst_uv[i] = float2{ (float)ix, (float)iy } * uv_unit;
            }
        }
    },
    [&]() {
        for (int iy = 0; iy < height - 1; ++iy) {
            for (int ix = 0; ix < width - 1; ++ix) {
                int i6 = (iy * width + ix) * 6;
                dst_indices[i6 + 0] = width*iy + ix;
                dst_indices[i6 + 1] = width*(iy + 1) + ix;
                dst_indices[i6 + 2] = width*(iy + 1) + (ix + 1);

                dst_indices[i6 + 3] = width*iy + ix;
                dst_indices[i6 + 4] = width*(iy + 1) + (ix + 1);
                dst_indices[i6 + 5] = width*iy + (ix + 1);
            }
        }
    });

    GenerateNormalsTriangleIndexed(dst_normals, dst_vertices, dst_indices, num_triangles, num_vertices);
}


float g_pen_pressure = 1.0f;

npAPI float npGetPenPressure()
{
    return g_pen_pressure;
}

void npInitializePenInput_Win();

npAPI void npInitializePenInput()
{
#ifdef npEnablePenTablet
#ifdef _WIN32
    npInitializePenInput_Win();
#endif
#endif
}

