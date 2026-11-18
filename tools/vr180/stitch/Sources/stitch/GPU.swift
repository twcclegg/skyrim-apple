import Foundation
import Metal
import StitchKit

/// Must match `Params` in the shader: only 4-byte scalars, so Swift and MSL layouts agree.
struct Params {
    var W: UInt32 = 0
    var S: UInt32 = 0
    var rowOffset: UInt32 = 0
    var hasDepth: UInt32 = 0
    var filterRadius: Float = 3
    var filterType: UInt32 = 0
    var maxScale: Float = 4
    var antiring: Float = 1
    var depthTol: Float = 0.02
    var distTol: Float = 0.03
    var skyDist: Float = .infinity
    var skyDepth: Float = 1
    var t: (Float, Float, Float, Float, Float) = (1, 1, 1, 1, 1)
    var tE: (Float, Float, Float, Float, Float) = (1, 1, 1, 1, 1)
    var near: (Float, Float, Float, Float, Float) = (15, 15, 15, 15, 15)
    var uLim: (Float, Float, Float, Float, Float) = (1, 1, 1, 1, 1)
}

let shaderSource = """
#include <metal_stdlib>
using namespace metal;

struct Params {
    uint W; uint S; uint rowOffset; uint hasDepth;
    float filterRadius; uint filterType; float maxScale; float antiring;
    float depthTol; float distTol; float skyDist; float skyDepth;
    float t[5]; float tE[5]; float nearZ[5]; float uLim[5];
};

constant float3 FWD[5]   = { float3(0,0,1), float3(-1,0,0), float3(1,0,0), float3(0,1,0), float3(0,-1,0) };
constant float3 UPV[5]   = { float3(0,1,0), float3(0,1,0),  float3(0,1,0), float3(0,0,-1), float3(0,0,1) };
constant float3 RIGHT[5] = { float3(1,0,0), float3(0,0,1),  float3(0,0,-1), float3(1,0,0), float3(1,0,0) };

#define MAXTAPS 40

static inline float kern(float x, uint type, float R) {
    x = fabs(x);
    if (x >= R) return 0.0f;
    if (type == 0) {                       // Lanczos-R
        if (x < 1e-5f) return 1.0f;
        float px = M_PI_F * x;
        return R * sin(px) * sin(px / R) / (px * px);
    } else {                               // Catmull-Rom (R = 2)
        if (x < 1.0f) return (1.5f * x - 2.5f) * x * x + 1.0f;
        return ((-0.5f * x + 2.5f) * x - 4.0f) * x + 2.0f;
    }
}

// Blend weight along one tangent axis: 1 for |a| <= 1/tE, smoothly 0 at |a| >= tE.
static inline float bandWeight(float a, float tE) {
    float lo = 1.0f / tE;
    float e = clamp((tE - fabs(a)) / (tE - lo), 0.0f, 1.0f);
    return e * e * e * (e * (e * 6.0f - 15.0f) + 10.0f);   // smootherstep
}

static inline float3 sampleFace(texture2d_array<float, access::read> tex, uint k, float x, float y,
                                float sx, float sy, constant Params& P) {
    float R = P.filterRadius;
    int S = int(P.S);
    int x0 = int(ceil(x - R * sx)), x1 = int(floor(x + R * sx));
    int y0 = int(ceil(y - R * sy)), y1 = int(floor(y + R * sy));
    int nx = min(x1 - x0 + 1, MAXTAPS), ny = min(y1 - y0 + 1, MAXTAPS);
    float wx[MAXTAPS], wy[MAXTAPS];
    float sumx = 0, sumy = 0;
    for (int i = 0; i < nx; i++) { wx[i] = kern((float(x0 + i) - x) / sx, P.filterType, R); sumx += wx[i]; }
    for (int j = 0; j < ny; j++) { wy[j] = kern((float(y0 + j) - y) / sy, P.filterType, R); sumy += wy[j]; }
    float3 acc = 0, mn = float3(1e9f), mx = float3(-1e9f);
    for (int j = 0; j < ny; j++) {
        int yy = clamp(y0 + j, 0, S - 1);
        bool yin = fabs(float(y0 + j) - y) <= sy;
        float3 row = 0;
        for (int i = 0; i < nx; i++) {
            int xx = clamp(x0 + i, 0, S - 1);
            float3 c = tex.read(uint2(xx, yy), k).rgb;
            row += wx[i] * c;
            if (yin && fabs(float(x0 + i) - x) <= sx) { mn = min(mn, c); mx = max(mx, c); }
        }
        acc += wy[j] * row;
    }
    acc /= (sumx * sumy);
    if (P.antiring > 0) acc = mix(acc, clamp(acc, mn, mx), P.antiring);
    return acc;
}

// View-space z along the face's forward axis. Returns INFINITY for sky.
// Bilinear in raw depth d (affine in 1/z, exact for planes), using only taps that agree
// with the nearest tap within depthTol, so no in-between depths across discontinuities.
static inline float sampleZ(texture2d_array<float, access::read> dt, uint k, float x, float y, constant Params& P) {
    int S = int(P.S);
    float nearZ = P.nearZ[k];
    int ix = int(floor(x)), iy = int(floor(y));
    float fx = x - float(ix), fy = y - float(iy);
    float d[4]; float w[4] = { (1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy };
    for (int q = 0; q < 4; q++) {
        int xx = clamp(ix + (q & 1), 0, S - 1), yy = clamp(iy + (q >> 1), 0, S - 1);
        d[q] = dt.read(uint2(xx, yy), k).r;
    }
    int nq = (fx < 0.5f ? 0 : 1) + (fy < 0.5f ? 0 : 2);
    if (d[nq] >= P.skyDepth) return INFINITY;
    float zn = nearZ / (1.0f - d[nq]);
    float ds = 0, ws = 0;
    for (int q = 0; q < 4; q++) {
        if (d[q] >= P.skyDepth) continue;
        float z = nearZ / (1.0f - d[q]);
        if (fabs(z - zn) <= P.depthTol * zn) { ds += w[q] * d[q]; ws += w[q]; }
    }
    float dd = (ws > 0) ? ds / ws : d[nq];
    return nearZ / (1.0f - dd);
}

kernel void stitch(texture2d_array<float, access::read> faces [[texture(0)]],
                   texture2d_array<float, access::read> depth [[texture(1)]],
                   device uchar4* outColor [[buffer(0)]],
                   device float* outDist [[buffer(1)]],
                   constant Params& P [[buffer(2)]],
                   uint2 gid [[thread_position_in_grid]])
{
    uint c = gid.x, r = gid.y + P.rowOffset;
    if (c >= P.W || r >= P.W) return;
    float W = float(P.W);
    float az = M_PI_F * ((float(c) + 0.5f) / W - 0.5f);
    float el = M_PI_F * (0.5f - (float(r) + 0.5f) / W);
    float sa = sin(az), ca = cos(az), se = sin(el), ce = cos(el);
    float3 dir = float3(ce * sa, se, ce * ca);
    float stp = M_PI_F / W;
    float3 dC = stp * float3(ce * ca, 0, -ce * sa);        // d dir / d column
    float3 dR = -stp * float3(-se * sa, ce, -se * ca);     // d dir / d row
    float S = float(P.S);

    float3 acc = 0; float wsum = 0;
    float wK[5]; float distK[5];
    for (uint k = 0; k < 5; k++) {
        wK[k] = 0; distK[k] = INFINITY;
        float fz = dot(dir, FWD[k]);
        if (fz <= 1e-4f) continue;
        float a = dot(dir, RIGHT[k]) / fz, b = dot(dir, UPV[k]) / fz;
        float t = P.t[k];
        float u = a / t, v = b / t;
        if (fabs(u) >= P.uLim[k] || fabs(v) >= P.uLim[k]) continue;
        float w = bandWeight(a, P.tE[k]) * bandWeight(b, P.tE[k]);
        if (w <= 0) continue;
        float x = (u + 1.0f) * 0.5f * S - 0.5f;
        float y = (1.0f - v) * 0.5f * S - 0.5f;
        // Footprint of one output pixel in face pixels -> filter scale (>= 1: minify only).
        float3 ga = (RIGHT[k] - a * FWD[k]) / fz;
        float3 gb = (UPV[k] - b * FWD[k]) / fz;
        float k2 = 0.5f * S / t;
        float dxdc = k2 * dot(ga, dC), dxdr = k2 * dot(ga, dR);
        float dydc = k2 * dot(gb, dC), dydr = k2 * dot(gb, dR);
        float sx = clamp(sqrt(dxdc * dxdc + dxdr * dxdr), 1.0f, P.maxScale);
        float sy = clamp(sqrt(dydc * dydc + dydr * dydr), 1.0f, P.maxScale);
        acc += w * sampleFace(faces, k, x, y, sx, sy, P);
        wsum += w; wK[k] = w;
        if (P.hasDepth) distK[k] = sampleZ(depth, k, x, y, P) * sqrt(1.0f + a * a + b * b);
    }
    uint idx = r * P.W + c;
    float3 col = (wsum > 0) ? acc / wsum : float3(1, 0, 1);
    col = clamp(col, 0.0f, 1.0f);
    float3 s = select(1.055f * pow(col, float3(1.0f / 2.4f)) - 0.055f, col * 12.92f, col <= 0.0031308f);
    outColor[idx] = uchar4(uchar3(rint(clamp(s, 0.0f, 1.0f) * 255.0f)), 255);

    if (P.hasDepth) {
        uint best = 0;
        for (uint k = 1; k < 5; k++) if (wK[k] > wK[best]) best = k;
        float db = distK[best];
        float dist;
        if (isinf(db)) {
            dist = P.skyDist;
        } else {
            float ds = 0, ws = 0;
            for (uint k = 0; k < 5; k++) {
                if (wK[k] <= 0 || isinf(distK[k])) continue;
                if (fabs(distK[k] - db) <= P.distTol * db) { ds += wK[k] * distK[k]; ws += wK[k]; }
            }
            dist = ds / ws;
        }
        outDist[idx] = dist;
    }
}
"""

final class GPUStitcher {
    let device: MTLDevice
    let queue: MTLCommandQueue
    let pipeline: MTLComputePipelineState

    init() throws {
        guard let dev = MTLCreateSystemDefaultDevice() else { throw StitchError("no Metal device") }
        device = dev
        queue = dev.makeCommandQueue()!
        let opts = MTLCompileOptions()
        opts.fastMathEnabled = false
        let lib = try dev.makeLibrary(source: shaderSource, options: opts)
        pipeline = try dev.makeComputePipelineState(function: lib.makeFunction(name: "stitch")!)
    }

    func makeFaceTexture(S: Int) -> MTLTexture {
        let d = MTLTextureDescriptor()
        d.textureType = .type2DArray
        d.pixelFormat = .rgba8Unorm_srgb   // reads return linear light
        d.width = S; d.height = S; d.arrayLength = faceCount
        d.usage = .shaderRead
        d.storageMode = .shared
        return device.makeTexture(descriptor: d)!
    }

    func makeDepthTexture(S: Int) -> MTLTexture {
        let d = MTLTextureDescriptor()
        d.textureType = .type2DArray
        d.pixelFormat = .r32Float
        d.width = S; d.height = S; d.arrayLength = faceCount
        d.usage = .shaderRead
        d.storageMode = .shared
        return device.makeTexture(descriptor: d)!
    }

    /// Runs the stitch kernel in row bands (separate command buffers so other GPU work, e.g. the game, can interleave).
    func run(faces: MTLTexture, depth: MTLTexture?, params: Params, outColor: MTLBuffer, outDist: MTLBuffer?) throws {
        let W = Int(params.W)
        let band = 256
        let dummyDepth = depth ?? makeTinyDepth()
        let dummyDist = outDist ?? device.makeBuffer(length: 16)!
        var last: MTLCommandBuffer?
        var r0 = 0
        while r0 < W {
            var p = params
            p.rowOffset = UInt32(r0)
            let rows = min(band, W - r0)
            let cb = queue.makeCommandBuffer()!
            let enc = cb.makeComputeCommandEncoder()!
            enc.setComputePipelineState(pipeline)
            enc.setTexture(faces, index: 0)
            enc.setTexture(dummyDepth, index: 1)
            enc.setBuffer(outColor, offset: 0, index: 0)
            enc.setBuffer(dummyDist, offset: 0, index: 1)
            enc.setBytes(&p, length: MemoryLayout<Params>.stride, index: 2)
            let tg = MTLSize(width: 16, height: 16, depth: 1)
            enc.dispatchThreads(MTLSize(width: W, height: rows, depth: 1), threadsPerThreadgroup: tg)
            enc.endEncoding()
            cb.commit()
            last = cb
            r0 += rows
        }
        last?.waitUntilCompleted()
        if let e = last?.error { throw StitchError("GPU error: \(e)") }
    }

    private func makeTinyDepth() -> MTLTexture {
        let d = MTLTextureDescriptor()
        d.textureType = .type2DArray
        d.pixelFormat = .r32Float
        d.width = 1; d.height = 1; d.arrayLength = faceCount
        d.usage = .shaderRead
        return device.makeTexture(descriptor: d)!
    }
}
