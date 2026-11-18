// Metal source for the reprojection (mesh pass + resolve) and for the synthetic test scene.
// Compiled at run time with precise maths (a pixel at W=5760 is 0.55 mrad, below fast-math trig error).

let shaderSource = #"""
#include <metal_stdlib>
using namespace metal;

// ---------------------------------------------------------------------------------------------------------
// Half-equirect convention. p is in pixel units (column + 0.5, row + 0.5 at pixel centres) of a W x H grid.
// az = -90 + 180 * p.x / W (left = -90), el = 90 - 180 * p.y / H (top = up). Rig frame: x right, y up, z forward.
static float3 dir_at(float2 p, float W, float H) {
    float az = M_PI_F * (p.x / W - 0.5f);
    float el = M_PI_F * (0.5f - p.y / H);
    float ce = precise::cos(el);
    return float3(ce * precise::sin(az), precise::sin(el), ce * precise::cos(az));
}
static float2 pix_of(float3 d, float W, float H) {
    float az = precise::atan2(d.x, d.z);
    float el = precise::atan2(d.y, precise::sqrt(d.x * d.x + d.z * d.z));
    return float2((az / M_PI_F + 0.5f) * W, (0.5f - el / M_PI_F) * H);
}

// ---------------------------------------------------------------------------------------------------------
// Mesh pass: one vertex per source pixel centre, two triangles per 2x2 block of pixels (unindexed, so each
// vertex knows its whole triangle, as in the compositor). Each vertex is the source pixel's 3D point
// P = dir / invr (invr = 1 / radial distance from the rig centre C; 0 for sky), drawn into the eye's own
// half-equirect: clip x = az_eye / 90deg, clip y from el_eye, depth from 1 / |P - eye| (reversed: larger = nearer).
struct MeshU {
    uint W, H, rowStart, bandStart;
    uint bandRows, outW, outH, skirt;
    float eyeX, tear, fillSkip, zc;
    float tearPx, mirror, pad1, pad2;
};

struct VOut {
    float4 position [[position]];
    float3 attr;             // source pixel coordinates (u, v) and inverse distance from C
    float flag [[flat]];     // 1: surface, 2: torn triangle (fill)
};

static float depth_curve(float invRE, float zc) { return invRE / (invRE + zc); }

// Where the point at direction dir (from C) and inverse distance iv lands in the eye's output grid, in sample
// pixels; also 1 / its distance from the eye.
static float2 to_eye(float3 dir, float iv, constant MeshU &u, thread float &invRE) {
    float3 o = dir - float3(u.eyeX * iv, 0, 0);          // (P - eye) * iv
    float len = max(length(o), 1e-9f);
    invRE = iv / len;
    return pix_of(o / len, float(u.outW), float(u.outH));
}
static float4 clip_of(float2 px, float z, constant MeshU &u) {
    return float4(px.x / float(u.outW) * 2.0f - 1.0f, 1.0f - 2.0f * (px.y - float(u.bandStart)) / float(u.bandRows), z, 1.0f);
}

// 18 vertices per 2x2 block of source pixels: for each of its two triangles, the triangle itself (3) and up to
// two skirt triangles (6). colTab / rowTab: (sin, cos) of each vertex column's azimuth / row's elevation (the
// outermost columns sit on the image edge, so the mesh reaches the hemisphere's left and right border).
vertex VOut mesh_vertex(uint vid [[vertex_id]], constant MeshU &u [[buffer(0)]],
                        const device float2 *colTab [[buffer(1)]], const device float2 *rowTab [[buffer(2)]],
                        texture2d<float, access::read> invr [[texture(0)]]) {
    const uint2 corners[6] = { uint2(0, 0), uint2(1, 0), uint2(0, 1), uint2(1, 0), uint2(1, 1), uint2(0, 1) };
    uint cell = vid / 18, r = vid % 18, tri = r / 9, slot = r % 9, first = tri * 3;
    uint2 c = uint2(cell % (u.W - 1), cell / (u.W - 1) + u.rowStart);
    float2 q[3]; float iv[3]; float3 d[3], o[3];
    for (uint j = 0; j < 3; j++) {
        uint2 p = c + corners[first + j];
        // The outermost columns sit on the image edge (their colour is the edge pixel's: lookups clamp).
        q[j] = float2(p.x == 0 ? 0.0f : (p.x == u.W - 1 ? float(u.W) : float(p.x) + 0.5f),
                      float(p.y) + 0.5f);   // not at the poles, where every column is the same point
        iv[j] = invr.read(p).r;
        float2 ca = colTab[p.x], re = rowTab[p.y];
        d[j] = float3(re.y * ca.x, re.x, re.y * ca.y);
        o[j] = normalize(d[j] - float3(u.eyeX * iv[j], 0, 0));
    }
    float iMin = min(iv[0], min(iv[1], iv[2])), iMax = max(iv[0], max(iv[1], iv[2]));
    // Tear test (compositor's): the far corner is more than (1 + tear) times as far as the near one. Offline we
    // also ask that the parallax actually opens (or folds) the triangle by more than tearPx output pixels: a
    // depth edge that moves less than that (far scenery against sky, say) stays one continuous surface, whose
    // interpolation reproduces the anti-aliased edge of the source.
    bool torn = iMax > iMin * (1.0f + u.tear);
    if (torn) {
        float st = max(abs(distance(o[0], o[1]) - distance(d[0], d[1])),
                   max(abs(distance(o[1], o[2]) - distance(d[1], d[2])), abs(distance(o[0], o[2]) - distance(d[0], d[2]))));
        torn = st * (float(u.W) / M_PI_F) > u.tearPx;
    }
    float lim = iMin * (1.0f + u.tear);
    VOut out;
    float invRE;
    if (slot < 3) {
        uint k = slot;
        float2 px = to_eye(d[k], iv[k], u, invRE);
        if (!torn) {
            out.position = clip_of(px, 0.5f + 0.5f * depth_curve(invRE, u.zc), u);
            out.attr = float3(q[k], iv[k]);
            out.flag = 1.0f;
            return out;
        }
        // Torn triangles are not dropped: they become fill, drawn behind every surface triangle (so they only
        // show where no surface lands, i.e. in disocclusions). Their near corners keep their true positions, so
        // the triangle spans exactly the gap the parallax opened, but they take the colour (and depth) of the far
        // corners: the background is stretched across the gap, never the foreground. fillSkip moves the colour
        // lookup that many pixels further into the background, past the anti-aliased edge pixel.
        float f0 = iv[0] <= lim ? 1.0f : 0.0f, f1 = iv[1] <= lim ? 1.0f : 0.0f, f2 = iv[2] <= lim ? 1.0f : 0.0f;
        float nf = f0 + f1 + f2, nn = 3.0f - nf;
        float2 farC = (q[0] * f0 + q[1] * f1 + q[2] * f2) / nf;
        float farI = (iv[0] * f0 + iv[1] * f1 + iv[2] * f2) / nf;
        float2 nearC = (q[0] * (1 - f0) + q[1] * (1 - f1) + q[2] * (1 - f2)) / nn;
        float2 uv = q[k];
        float ivFill = iv[k];
        float2 away = normalize(farC - nearC);
        if (iv[k] > lim) {
            uv = farC; ivFill = farI;
            if (u.mirror != 0) {
                // Mirror instead of smear: the near corner looks up the background as far beyond the edge as
                // the gap it spans is wide, so the gap shows the background beside it reflected.
                float3 oFar = normalize(o[0] * f0 + o[1] * f1 + o[2] * f2);
                uv += away * distance(o[k], oFar) * (float(u.W) / M_PI_F);
            }
        }
        uv += away * u.fillSkip;
        float invFill;
        to_eye(d[k], ivFill, u, invFill);
        // Fill depth in [0.25, 0.5): behind every surface (0.5 + ...), and above the cleared 0 even for sky.
        out.position = clip_of(px, 0.25f + 0.2499f * depth_curve(invFill, u.zc), u);
        out.attr = float3(uv, ivFill);
        out.flag = 2.0f;
        return out;
    }
    // Skirt: in a torn triangle the near surface is extended half way to the far corners, at the near corners'
    // depth, as surface. The silhouette then sits between the last near and first far pixel centre (where the
    // source's edge is) instead of on the near pixel centres, and a one-pixel-wide object keeps its width. This
    // is the per-pixel form of the compositor's nearest-depth dilation.
    out.attr = 0; out.flag = 0;
    out.position = float4(-2, -2, 0, 1);       // degenerate, outside the clip volume
    if (!torn || u.skirt == 0) return out;
    int nIdx[2] = { 0, 0 }, fIdx[2] = { 0, 0 };
    int nn = 0, nf = 0;
    for (int j = 0; j < 3; j++) {
        if (iv[j] > lim) { if (nn < 2) nIdx[nn] = j; nn++; } else { if (nf < 2) fIdx[nf] = j; nf++; }
    }
    uint s = slot - 3;
    int a, b = -1;
    if (nn == 1) {
        if (s >= 3) return out;
        a = nIdx[0];
        if (s > 0) b = fIdx[s - 1];
    } else {
        const int A[6] = { 0, 1, 1, 0, 1, 0 };  // which near corner (index into nIdx)
        const int B[6] = { 0, 0, 1, 0, 1, 1 };  // 1: midpoint towards the far corner
        a = nIdx[A[s]];
        if (B[s]) b = fIdx[0];
    }
    float2 uv = b < 0 ? q[a] : 0.5f * (q[a] + q[b]);
    float3 dir = b < 0 ? d[a] : dir_at(uv, float(u.W), float(u.H));
    float2 px = to_eye(dir, iv[a], u, invRE);
    out.position = clip_of(px, 0.5f + 0.5f * depth_curve(invRE, u.zc), u);
    out.attr = float3(uv, iv[a]);
    out.flag = 1.0f;
    return out;
}

fragment float4 mesh_fragment(VOut in [[stage_in]]) {
    return float4(in.attr, in.flag);
}

// ---------------------------------------------------------------------------------------------------------
// Resolve: per output pixel, ss x ss samples of the mesh pass; each sample's source position is optionally
// refined (compositor's per-pixel depth search) and its colour fetched with a depth-aware Catmull-Rom filter
// in linear light (taps on another surface than the sample's own are left out). Box average, sRGB encode.
struct ResolveU {
    uint W, H, outW, outH;
    uint ss, bandStart, bandRows, refine;
    float eyeX, tear, zc, pad0;
    uint dumpUV, cubic, pad1, pad2;
};

static bool same_surface(float a, float b, float tear) {
    return max(a, b) <= min(a, b) * (1.0f + tear);
}

static void catmull_rom(float t, thread float *w) {
    float t2 = t * t, t3 = t2 * t;
    w[0] = -0.5f * t + t2 - 0.5f * t3;
    w[1] = 1.0f - 2.5f * t2 + 1.5f * t3;
    w[2] = 0.5f * t + 2.0f * t2 - 1.5f * t3;
    w[3] = -0.5f * t2 + 0.5f * t3;
}

static float3 sample_colour(texture2d<float, access::read> colour, texture2d<float, access::read> invr,
                            float2 uv, float iv, bool aware, constant ResolveU &u) {
    int2 hi = int2(u.W - 1, u.H - 1);
    float2 x = uv - 0.5f;
    float2 b = floor(x);
    float2 f = x - b;
    int2 base = int2(b);
    float3 acc = 0; float wsum = 0;
    float3 bil = 0; float bsum = 0;
    float3 nearest = 0; float nearestErr = INFINITY;
    if (u.cubic == 2) {
        // Lanczos-3: 6 x 6 taps.
        float wx[6], wy[6];
        for (int i = 0; i < 6; i++) {
            float tx = float(i - 2) - f.x, ty = float(i - 2) - f.y;
            wx[i] = abs(tx) < 1e-5f ? 1.0f : 3.0f * sin(M_PI_F * tx) * sin(M_PI_F * tx / 3.0f) / (M_PI_F * M_PI_F * tx * tx);
            wy[i] = abs(ty) < 1e-5f ? 1.0f : 3.0f * sin(M_PI_F * ty) * sin(M_PI_F * ty / 3.0f) / (M_PI_F * M_PI_F * ty * ty);
        }
        float total = 0;
        for (int j = 0; j < 6; j++) {
            for (int i = 0; i < 6; i++) {
                float w = wx[i] * wy[j];
                total += w;
                uint2 q = uint2(clamp(base + int2(i - 2, j - 2), int2(0), hi));
                if (aware && !same_surface(invr.read(q).r, iv, u.tear)) continue;
                acc += w * colour.read(q).rgb; wsum += w;
            }
        }
        if (wsum > 0.5f * total) return max(acc / wsum, 0.0f);
    } else if (u.cubic != 0) {
        float wx[4], wy[4];
        catmull_rom(f.x, wx); catmull_rom(f.y, wy);
        for (int j = 0; j < 4; j++) {
            for (int i = 0; i < 4; i++) {
                uint2 q = uint2(clamp(base + int2(i - 1, j - 1), int2(0), hi));
                if (aware && !same_surface(invr.read(q).r, iv, u.tear)) continue;
                float w = wx[i] * wy[j];
                acc += w * colour.read(q).rgb; wsum += w;
            }
        }
        if (wsum > 0.5f) return max(acc / wsum, 0.0f);
    }
    for (int j = 0; j < 2; j++) {
        for (int i = 0; i < 2; i++) {
            uint2 q = uint2(clamp(base + int2(i, j), int2(0), hi));
            float t = invr.read(q).r;
            float3 c = colour.read(q).rgb;
            float e = abs(t - iv) / max(max(t, iv), 1e-12f);
            if (e < nearestErr) { nearestErr = e; nearest = c; }
            if (aware && !same_surface(t, iv, u.tear)) continue;
            float w = (i == 0 ? 1.0f - f.x : f.x) * (j == 0 ? 1.0f - f.y : f.y);
            bil += w * c; bsum += w;
        }
    }
    if (bsum > 1e-4f) return bil / bsum;
    return nearest;
}

// Inverse distance at uv, bilinear over the 2x2 taps on the same surface as ref; -1 if none is.
static float invr_on_surface(texture2d<float, access::read> invr, float2 uv, float ref, constant ResolveU &u) {
    int2 hi = int2(u.W - 1, u.H - 1);
    float2 x = uv - 0.5f;
    float2 b = floor(x);
    float2 f = x - b;
    float acc = 0, wsum = 0;
    for (int j = 0; j < 2; j++) {
        for (int i = 0; i < 2; i++) {
            uint2 q = uint2(clamp(int2(b) + int2(i, j), int2(0), hi));
            float t = invr.read(q).r;
            if (!same_surface(t, ref, u.tear)) continue;
            float w = (i == 0 ? 1.0f - f.x : f.x) * (j == 0 ? 1.0f - f.y : f.y) + 1e-6f;
            acc += w * t; wsum += w;
        }
    }
    return wsum > 0 ? acc / wsum : -1.0f;
}

// The compositor's per-pixel depth search, in equirect: take the depth of the source pixel the eye's ray is
// thought to show, move along the ray to that distance from C, look up which source pixel that is; repeat.
// A ray that ends on a different surface than it started on reverts to the mesh's answer.
static float2 refine_uv(float2 uv0, float iv0, float2 pOut, texture2d<float, access::read> invr, constant ResolveU &u) {
    float3 d = dir_at(pOut, float(u.W), float(u.H));
    float ad = u.eyeX * d.x, ee = u.eyeX * u.eyeX;
    float2 uv = uv0;
    float cur = iv0;
    for (uint k = 0; k < u.refine; k++) {
        float si = invr_on_surface(invr, uv, cur, u);
        if (si < 0) return uv0;
        float den = -ad * si + sqrt(max((ad * ad - ee) * si * si + 1.0f, 0.0f));
        float s = si / max(den, 1e-9f);
        uv = pix_of(normalize(float3(u.eyeX * s, 0, 0) + d), float(u.W), float(u.H));
        cur = si;
    }
    return invr_on_surface(invr, uv, cur, u) < 0 ? uv0 : uv;
}

static float srgb_encode(float c) {
    c = saturate(c);
    return c <= 0.0031308f ? c * 12.92f : 1.055f * precise::pow(c, 1.0f / 2.4f) - 0.055f;
}

kernel void resolve(uint2 g [[thread_position_in_grid]], constant ResolveU &u [[buffer(0)]],
                    device atomic_uint *stats [[buffer(1)]], device float4 *dump [[buffer(2)]],
                    texture2d<float, access::read> mesh [[texture(0)]],
                    texture2d<float, access::read> colour [[texture(1)]],
                    texture2d<float, access::read> invr [[texture(2)]],
                    texture2d<float, access::write> out [[texture(3)]]) {
    uint rows = u.bandRows / u.ss;
    uint oy = u.bandStart / u.ss + g.y;
    bool inside = g.x < u.W && g.y < rows && oy < u.H;
    uint holes = 0, fills = 0;
    if (inside) {
        float3 sum = 0;
        float4 first = 0;
        for (uint sy = 0; sy < u.ss; sy++) {
            for (uint sx = 0; sx < u.ss; sx++) {
                float4 m = mesh.read(uint2(g.x * u.ss + sx, g.y * u.ss + sy));
                float2 pOut = (float2(g.x * u.ss + sx, oy * u.ss + sy) + 0.5f) / float(u.ss);
                int flag = int(m.w + 0.5f);
                float2 uv;
                float3 c;
                if (flag == 0) {
                    // Nothing landed here: the compositor's backplate, i.e. the image at infinite distance
                    // (same direction).
                    uv = pOut;
                    c = sample_colour(colour, invr, uv, 0, false, u);
                    holes++;
                } else {
                    uv = m.xy;
                    if (flag == 1 && u.refine > 0) uv = refine_uv(uv, m.z, pOut, invr, u);
                    if (flag == 2) fills++;
                    c = sample_colour(colour, invr, uv, m.z, true, u);
                }
                if (sx == 0 && sy == 0) first = float4(uv, float(flag), m.z);
                sum += c;
            }
        }
        float3 c = sum / float(u.ss * u.ss);
        out.write(float4(srgb_encode(c.r), srgb_encode(c.g), srgb_encode(c.b), 1.0f), uint2(g.x, oy));
        if (u.dumpUV != 0) dump[oy * u.W + g.x] = first;
    }
    uint h = simd_sum(holes), fl = simd_sum(fills);
    if (simd_is_first()) {
        if (h) atomic_fetch_add_explicit(&stats[0], h, memory_order_relaxed);
        if (fl) atomic_fetch_add_explicit(&stats[1], fl, memory_order_relaxed);
    }
}

// ---------------------------------------------------------------------------------------------------------
// Synthetic scene, in game units (70 = 1 m), ray-cast analytically. A textured sphere ~1.2 m away, a rotated
// textured box ~2.4 m, a 3 cm rod ~1.7 m, a textured ground 1.6 m below, a brick cylinder backdrop 30 m away
// and sky (infinitely far) above it.
struct SynthU { uint W, H, spp, view; float eyeX, skyDist, pad0, pad1; };

struct Hit { float t; int id; float3 p; };

constant float3 SPH_C = float3(-38.0f, -8.0f, 78.0f);
constant float SPH_R = 17.0f;
constant float3 BOX_C = float3(48.0f, -22.0f, 165.0f);
constant float3 BOX_H = float3(24.0f, 24.0f, 24.0f);
constant float3 ROD_C = float3(8.0f, 0.0f, 118.0f);
constant float ROD_R = 1.0f;
constant float GROUND_Y = -112.0f;
constant float BACK_R = 2100.0f;
constant float BACK_TOP = 800.0f;

static float3x3 box_rot() {   // world -> box local
    float a = 35.0f * M_PI_F / 180.0f, b = 20.0f * M_PI_F / 180.0f;
    float3x3 ry = float3x3(float3(cos(a), 0, -sin(a)), float3(0, 1, 0), float3(sin(a), 0, cos(a)));
    float3x3 rx = float3x3(float3(1, 0, 0), float3(0, cos(b), sin(b)), float3(0, -sin(b), cos(b)));
    return transpose(ry * rx);
}

static Hit trace(float3 o, float3 d) {
    Hit h; h.t = INFINITY; h.id = 0;
    // sphere
    {
        float3 oc = o - SPH_C;
        float bb = dot(oc, d), cc = dot(oc, oc) - SPH_R * SPH_R, disc = bb * bb - cc;
        if (disc >= 0) { float t = -bb - sqrt(disc); if (t > 1e-3f && t < h.t) { h.t = t; h.id = 1; } }
    }
    // box (slab test in local frame)
    {
        float3x3 R = box_rot();
        float3 lo = R * (o - BOX_C), ld = R * d;
        float3 inv = 1.0f / ld;
        float3 t0 = (-BOX_H - lo) * inv, t1 = (BOX_H - lo) * inv;
        float3 tmin = min(t0, t1), tmax = max(t0, t1);
        float tn = max(tmin.x, max(tmin.y, tmin.z)), tf = min(tmax.x, min(tmax.y, tmax.z));
        if (tn <= tf && tn > 1e-3f && tn < h.t) { h.t = tn; h.id = 2; }
    }
    // rod: vertical cylinder from the ground up to y = 60
    {
        float2 oc = o.xz - ROD_C.xz, dd = d.xz;
        float a = dot(dd, dd), bb = dot(oc, dd), cc = dot(oc, oc) - ROD_R * ROD_R, disc = bb * bb - a * cc;
        if (a > 0 && disc >= 0) {
            float t = (-bb - sqrt(disc)) / a;
            float y = o.y + t * d.y;
            if (t > 1e-3f && t < h.t && y > GROUND_Y && y < 60.0f) { h.t = t; h.id = 3; }
        }
    }
    // ground
    if (d.y < 0) {
        float t = (GROUND_Y - o.y) / d.y;
        float3 p = o + t * d;
        if (t > 1e-3f && t < h.t && length(p.xz) < BACK_R) { h.t = t; h.id = 4; }
    }
    // backdrop cylinder (from inside)
    {
        float2 dd = d.xz, oo = o.xz;
        float a = dot(dd, dd), bb = dot(oo, dd), cc = dot(oo, oo) - BACK_R * BACK_R;
        if (a > 0) {
            float t = (-bb + sqrt(max(bb * bb - a * cc, 0.0f))) / a;
            float y = o.y + t * d.y;
            if (t > 1e-3f && t < h.t && y >= GROUND_Y && y < BACK_TOP) { h.t = t; h.id = 5; }
        }
    }
    h.p = o + h.t * d;
    return h;
}

static uint hash3(int3 p) {
    uint h = uint(p.x) * 0x8da6b343u ^ uint(p.y) * 0xd8163841u ^ uint(p.z) * 0xcb1ab31fu;
    h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu; h ^= h >> 16;
    return h;
}
static float rnd(int3 p) { return float(hash3(p) & 0xffffffu) / 16777215.0f; }
static float vnoise(float3 x) {
    float3 i = floor(x), f = x - i;
    f = f * f * (3.0f - 2.0f * f);
    int3 b = int3(i);
    float n000 = rnd(b), n100 = rnd(b + int3(1, 0, 0)), n010 = rnd(b + int3(0, 1, 0)), n110 = rnd(b + int3(1, 1, 0));
    float n001 = rnd(b + int3(0, 0, 1)), n101 = rnd(b + int3(1, 0, 1)), n011 = rnd(b + int3(0, 1, 1)), n111 = rnd(b + int3(1, 1, 1));
    return mix(mix(mix(n000, n100, f.x), mix(n010, n110, f.x), f.y), mix(mix(n001, n101, f.x), mix(n011, n111, f.x), f.y), f.z);
}
static float fbm(float3 x) { return 0.5f * vnoise(x) + 0.25f * vnoise(2.03f * x) + 0.125f * vnoise(4.07f * x) + 0.0625f * vnoise(8.1f * x); }
static float checker(float2 x) { int2 c = int2(floor(x)); return float((c.x + c.y) & 1); }

static float3 shade(Hit h, float3 d) {
    float3 p = h.p;
    if (h.id == 0) {   // sky: by direction only
        float g = saturate(d.y);
        float3 base = mix(float3(0.75f, 0.82f, 0.92f), float3(0.25f, 0.45f, 0.85f), g);
        float cl = smoothstep(0.45f, 0.75f, fbm(d * 9.0f));
        return mix(base, float3(0.97f), cl * 0.8f);
    }
    if (h.id == 1) {
        float3 n = normalize(p - SPH_C);
        float lon = atan2(n.x, n.z), lat = asin(n.y);
        float c = checker(float2(lon, lat) * (8.0f / M_PI_F));
        float3 col = mix(float3(0.85f, 0.35f, 0.1f), float3(0.1f, 0.3f, 0.8f), c);
        col *= 0.8f + 0.2f * sin(lat * 60.0f);
        return col * (0.55f + 0.45f * saturate(dot(n, normalize(float3(-0.4f, 0.8f, -0.4f)))));
    }
    if (h.id == 2) {
        float3 l = box_rot() * (p - BOX_C);
        float3 a = abs(l) / BOX_H;
        float2 f = a.x > a.y && a.x > a.z ? l.yz : (a.y > a.z ? l.xz : l.xy);
        float3 tint = a.x > a.y && a.x > a.z ? float3(0.9f, 0.9f, 0.3f) : (a.y > a.z ? float3(0.3f, 0.9f, 0.5f) : float3(0.9f, 0.4f, 0.7f));
        float c = checker(f / 8.0f);
        return tint * (0.35f + 0.5f * c) * (0.85f + 0.3f * vnoise(l * 0.7f));
    }
    if (h.id == 3) {
        return int(floor(p.y / 10.0f)) & 1 ? float3(0.95f, 0.95f, 0.95f) : float3(0.8f, 0.1f, 0.1f);
    }
    if (h.id == 4) {
        float c = checker(p.xz / 35.0f);
        float n = fbm(float3(p.xz / 6.0f, 0.5f));
        return mix(float3(0.25f, 0.4f, 0.2f), float3(0.55f, 0.55f, 0.5f), c) * (0.6f + 0.8f * n);
    }
    // backdrop: bricks along the arc
    float s = atan2(p.x, p.z) * BACK_R, y = p.y - GROUND_Y;
    float row = floor(y / 30.0f);
    float sx = s + (int(row) & 1 ? 30.0f : 0.0f);
    float col = floor(sx / 60.0f);
    float mx = sx - col * 60.0f, my = y - row * 30.0f;
    float mortar = (mx < 4.0f || my < 4.0f) ? 1.0f : 0.0f;
    float v = rnd(int3(int(col), int(row), 7));
    float3 brick = float3(0.55f + 0.3f * v, 0.25f + 0.15f * v, 0.18f) * (0.8f + 0.4f * fbm(float3(s, y, 0) / 9.0f));
    return mix(brick, float3(0.7f, 0.7f, 0.65f), mortar);
}

static float3 srgb3(float3 c) { return float3(srgb_encode(c.r), srgb_encode(c.g), srgb_encode(c.b)); }

// view: 0 = C (writes colour + dist), 1 = eye (writes colour + truth). truth = (source u, v as seen from C,
// visible-from-C flag, object id) for the pixel-centre ray.
kernel void synth(uint2 g [[thread_position_in_grid]], constant SynthU &u [[buffer(0)]],
                  device uchar4 *rgba [[buffer(1)]], device float *dist [[buffer(2)]], device float4 *truth [[buffer(3)]]) {
    if (g.x >= u.W || g.y >= u.H) return;
    float3 eye = float3(u.eyeX, 0, 0);
    float3 acc = 0;
    for (uint j = 0; j < u.spp; j++) {
        for (uint i = 0; i < u.spp; i++) {
            float2 p = float2(g) + (float2(i, j) + 0.5f) / float(u.spp);
            float3 d = dir_at(p, float(u.W), float(u.H));
            acc += shade(trace(eye, d), d);
        }
    }
    acc /= float(u.spp * u.spp);
    uint idx = g.y * u.W + g.x;
    rgba[idx] = uchar4(uchar3(round(srgb3(acc) * 255.0f)), 255);
    float3 d = dir_at(float2(g) + 0.5f, float(u.W), float(u.H));
    Hit h = trace(eye, d);
    if (u.view == 0) {
        dist[idx] = h.id == 0 ? u.skyDist : h.t;
    } else {
        if (h.id == 0) {
            // Sky is at infinity: the same direction from C, seen there unless something near is in the way.
            truth[idx] = float4(pix_of(d, float(u.W), float(u.H)), trace(float3(0), d).id == 0 ? 1.0f : 0.0f, 0.0f);
        } else {
            float r = length(h.p);
            float3 dc = h.p / r;
            Hit hc = trace(float3(0), dc);
            bool vis = hc.id != 0 && abs(hc.t - r) < 1e-3f * r + 0.05f;
            truth[idx] = float4(pix_of(dc, float(u.W), float(u.H)), vis ? 1.0f : 0.0f, float(h.id));
        }
    }
}
"""#
