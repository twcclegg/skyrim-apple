import Foundation
import ImageIO
import CoreGraphics
import UniformTypeIdentifiers

// MARK: - Errors

public struct StitchError: Error, CustomStringConvertible {
    public let description: String
    public init(_ s: String) { description = s }
}

// MARK: - Rig / face geometry

public let rigNames = ["L", "R", "C"]
public let faceCount = 5

/// Face bases in rig coordinates (x right, y up, z forward).
public struct FaceBasis {
    public let fwd: SIMD3<Double>
    public let up: SIMD3<Double>
    public let right: SIMD3<Double>
}

public let faceBases: [FaceBasis] = [
    FaceBasis(fwd: [0, 0, 1], up: [0, 1, 0], right: [1, 0, 0]),   // 0 front
    FaceBasis(fwd: [-1, 0, 0], up: [0, 1, 0], right: [0, 0, 1]),  // 1 left
    FaceBasis(fwd: [1, 0, 0], up: [0, 1, 0], right: [0, 0, -1]),  // 2 right
    FaceBasis(fwd: [0, 1, 0], up: [0, 0, -1], right: [1, 0, 0]),  // 3 up
    FaceBasis(fwd: [0, -1, 0], up: [0, 0, 1], right: [1, 0, 0]),  // 4 down
]

/// Output direction for half-equirect pixel coordinates (continuous; pixel centre = c+0.5).
@inline(__always)
public func eqrDirection(cx: Double, ry: Double, W: Double) -> SIMD3<Double> {
    let az = -Double.pi / 2 + Double.pi * cx / W
    let el = Double.pi / 2 - Double.pi * ry / W
    return SIMD3(cos(el) * sin(az), sin(el), cos(el) * cos(az))
}

// MARK: - File naming

public func blockString(_ b: Int) -> String { String(format: "%05d", b) }

public func shotName(_ b: String, _ rig: String, _ k: Int) -> String { "shot-\(b)-\(rig)\(k).bmp" }
public func depthName(_ b: String, _ k: Int) -> String { "depth-\(b)-C\(k).f32" }
public func sidecarName(_ b: String, _ rig: String, _ k: Int) -> String { "stereo-\(b)-\(rig)\(k).txt" }

public func allInputNames(_ b: String) -> [String] {
    var n: [String] = []
    for rig in rigNames {
        for k in 0..<faceCount {
            n.append(shotName(b, rig, k))
            n.append(sidecarName(b, rig, k))
        }
    }
    for k in 0..<faceCount { n.append(depthName(b, k)) }
    return n
}

// MARK: - Sidecar

public struct Sidecar {
    public var values: [String: [String]] = [:]
    public init() {}

    public static func parse(_ text: String) -> Sidecar {
        var s = Sidecar()
        for rawLine in text.split(whereSeparator: { $0 == "\n" || $0 == "\r" }) {
            let line = rawLine.trimmingCharacters(in: .whitespaces)
            if line.isEmpty || line.hasPrefix("#") { continue }
            let parts = line.split(whereSeparator: { $0 == " " || $0 == "\t" || $0 == "=" }).map(String.init)
            guard let key = parts.first else { continue }
            s.values[key] = Array(parts.dropFirst())
        }
        return s
    }

    public static func load(_ url: URL) throws -> Sidecar {
        guard let data = try? Data(contentsOf: url), let text = String(data: data, encoding: .utf8) else {
            throw StitchError("cannot read sidecar \(url.path)")
        }
        return parse(text)
    }

    public func double(_ key: String) -> Double? {
        guard let v = values[key]?.first else { return nil }
        if v.lowercased() == "inf" || v.lowercased() == "infinity" { return .infinity }
        return Double(v)
    }

    public func int(_ key: String) -> Int? {
        guard let v = values[key]?.first else { return nil }
        return Int(v) ?? Double(v).map { Int($0) }
    }

    /// Image size: accepts `width W` + `height H`, or `size W H`, or a literal `width height`-style line `W H` under key `resolution`.
    public var size: (Int, Int)? {
        if let w = int("width"), let h = int("height") { return (w, h) }
        for key in ["size", "resolution", "dims"] {
            if let v = values[key], v.count >= 2, let w = Int(v[0]), let h = Int(v[1]) { return (w, h) }
        }
        return nil
    }

    public func text() -> String {
        values.keys.sorted().map { k in ([k] + values[k]!).joined(separator: " ") }.joined(separator: "\n") + "\n"
    }
}

// MARK: - Raw file IO

public func readFile(_ url: URL) throws -> Data {
    do { return try Data(contentsOf: url, options: [.uncached]) } catch {
        throw StitchError("cannot read \(url.path): \(error.localizedDescription)")
    }
}

public func fileSize(_ url: URL) -> Int? {
    var st = stat()
    guard stat(url.path, &st) == 0 else { return nil }
    return Int(st.st_size)
}

public func fileMTime(_ url: URL) -> Double? {
    var st = stat()
    guard stat(url.path, &st) == 0 else { return nil }
    return Double(st.st_mtimespec.tv_sec) + Double(st.st_mtimespec.tv_nsec) * 1e-9
}

/// Write data to `url` atomically (temp file in same dir + fsync + rename).
public func writeAtomically(_ data: UnsafeRawBufferPointer, to url: URL) throws {
    try writeAtomically([data], to: url)
}

/// Write the concatenation of `chunks` to `url` atomically.
public func writeAtomically(_ chunks: [UnsafeRawBufferPointer], to url: URL) throws {
    let tmp = url.deletingLastPathComponent().appendingPathComponent(".\(url.lastPathComponent).tmp-\(getpid())")
    let fd = open(tmp.path, O_WRONLY | O_CREAT | O_TRUNC, 0o644)
    guard fd >= 0 else { throw StitchError("cannot create \(tmp.path)") }
    for data in chunks {
        var off = 0
        while off < data.count {
            let n = write(fd, data.baseAddress! + off, min(data.count - off, 1 << 26))
            if n <= 0 { close(fd); unlink(tmp.path); throw StitchError("write failed \(tmp.path) errno \(errno)") }
            off += n
        }
    }
    fsync(fd)
    close(fd)
    guard rename(tmp.path, url.path) == 0 else { unlink(tmp.path); throw StitchError("rename failed \(url.path)") }
}

// MARK: - BMP

public struct BMPInfo {
    public var width: Int
    public var height: Int
    public var topDown: Bool
    public var bpp: Int
    public var dataOffset: Int
    public var stride: Int
    public var masks: (UInt32, UInt32, UInt32)?   // R,G,B for 32-bit bitfields
    public var expectedFileSize: Int { dataOffset + stride * height }
}

@inline(__always) func le32(_ p: UnsafeRawBufferPointer, _ o: Int) -> UInt32 { p.loadUnaligned(fromByteOffset: o, as: UInt32.self).littleEndian }
@inline(__always) func le16(_ p: UnsafeRawBufferPointer, _ o: Int) -> UInt16 { p.loadUnaligned(fromByteOffset: o, as: UInt16.self).littleEndian }

public func parseBMPHeader(_ p: UnsafeRawBufferPointer) throws -> BMPInfo {
    guard p.count >= 54, p[0] == 0x42, p[1] == 0x4D else { throw StitchError("not a BMP") }
    let off = Int(le32(p, 10))
    let hdr = Int(le32(p, 14))
    let w = Int(Int32(bitPattern: le32(p, 18)))
    let hRaw = Int(Int32(bitPattern: le32(p, 22)))
    let bpp = Int(le16(p, 28))
    let comp = le32(p, 30)
    guard w > 0, hRaw != 0 else { throw StitchError("bad BMP size") }
    guard bpp == 24 || bpp == 32 else { throw StitchError("unsupported BMP bpp \(bpp)") }
    var masks: (UInt32, UInt32, UInt32)? = nil
    if comp == 3 || comp == 6 {
        guard bpp == 32 else { throw StitchError("bitfields only supported for 32bpp") }
        let mo = hdr >= 52 ? 14 + 40 : 14 + hdr
        guard p.count >= mo + 12 else { throw StitchError("truncated BMP masks") }
        masks = (le32(p, mo), le32(p, mo + 4), le32(p, mo + 8))
    } else if comp != 0 {
        throw StitchError("unsupported BMP compression \(comp)")
    }
    let stride = ((w * bpp + 31) / 32) * 4
    return BMPInfo(width: w, height: abs(hRaw), topDown: hRaw < 0, bpp: bpp, dataOffset: off, stride: stride, masks: masks)
}

@inline(__always) func maskShift(_ m: UInt32) -> UInt32 { m == 0 ? 0 : UInt32(m.trailingZeroBitCount) }

/// Decode a BMP into tightly packed RGBA8, row 0 = top of image.
public func decodeBMP(_ data: Data, into out: UnsafeMutablePointer<UInt8>) throws -> BMPInfo {
    try data.withUnsafeBytes { (p: UnsafeRawBufferPointer) -> BMPInfo in
        let info = try parseBMPHeader(p)
        guard p.count >= info.expectedFileSize else { throw StitchError("truncated BMP (\(p.count) < \(info.expectedFileSize))") }
        let w = info.width, h = info.height
        let base = p.baseAddress!.assumingMemoryBound(to: UInt8.self)
        DispatchQueue.concurrentPerform(iterations: 8) { chunk in
            let r0 = chunk * h / 8, r1 = (chunk + 1) * h / 8
            for row in r0..<r1 {
                let srcRow = info.topDown ? row : (h - 1 - row)
                let src = base + info.dataOffset + srcRow * info.stride
                let dst = out + row * w * 4
                if info.bpp == 24 {
                    for x in 0..<w {
                        dst[4 * x] = src[3 * x + 2]
                        dst[4 * x + 1] = src[3 * x + 1]
                        dst[4 * x + 2] = src[3 * x]
                        dst[4 * x + 3] = 255
                    }
                } else if let m = info.masks {
                    let s32 = UnsafeRawPointer(src)
                    let sr = maskShift(m.0), sg = maskShift(m.1), sb = maskShift(m.2)
                    for x in 0..<w {
                        let v = s32.loadUnaligned(fromByteOffset: 4 * x, as: UInt32.self).littleEndian
                        dst[4 * x] = UInt8(truncatingIfNeeded: (v & m.0) >> sr)
                        dst[4 * x + 1] = UInt8(truncatingIfNeeded: (v & m.1) >> sg)
                        dst[4 * x + 2] = UInt8(truncatingIfNeeded: (v & m.2) >> sb)
                        dst[4 * x + 3] = 255
                    }
                } else {
                    for x in 0..<w {   // BGRX
                        dst[4 * x] = src[4 * x + 2]
                        dst[4 * x + 1] = src[4 * x + 1]
                        dst[4 * x + 2] = src[4 * x]
                        dst[4 * x + 3] = 255
                    }
                }
            }
        }
        return info
    }
}

/// Write a 24-bit bottom-up BMP from RGBA8 (row 0 = top).
public func encodeBMP24(rgba: UnsafePointer<UInt8>, width w: Int, height h: Int) -> Data {
    let stride = ((w * 24 + 31) / 32) * 4
    let size = 54 + stride * h
    var d = Data(count: size)
    d.withUnsafeMutableBytes { (p: UnsafeMutableRawBufferPointer) in
        func put32(_ o: Int, _ v: UInt32) { p.storeBytes(of: v.littleEndian, toByteOffset: o, as: UInt32.self) }
        func put16(_ o: Int, _ v: UInt16) { p.storeBytes(of: v.littleEndian, toByteOffset: o, as: UInt16.self) }
        p[0] = 0x42; p[1] = 0x4D
        put32(2, UInt32(size)); put32(10, 54); put32(14, 40)
        put32(18, UInt32(w)); put32(22, UInt32(h)); put16(26, 1); put16(28, 24)
        put32(30, 0); put32(34, UInt32(stride * h)); put32(38, 2835); put32(42, 2835)
        let base = p.baseAddress!.assumingMemoryBound(to: UInt8.self)
        for row in 0..<h {
            let dst = base + 54 + (h - 1 - row) * stride
            let src = rgba + row * w * 4
            for x in 0..<w {
                dst[3 * x] = src[4 * x + 2]
                dst[3 * x + 1] = src[4 * x + 1]
                dst[3 * x + 2] = src[4 * x]
            }
        }
    }
    return d
}

// MARK: - f32 grids (uint32 w, uint32 h, float32 rows)

public func f32Header(_ data: Data) -> (Int, Int)? {
    guard data.count >= 8 else { return nil }
    return data.withUnsafeBytes { p in (Int(le32(p, 0)), Int(le32(p, 4))) }
}

public func f32HeaderOfFile(_ url: URL) -> (Int, Int)? {
    guard let fh = FileHandle(forReadingAtPath: url.path) else { return nil }
    defer { try? fh.close() }
    guard let d = try? fh.read(upToCount: 8) else { return nil }
    return f32Header(d)
}

public func encodeF32(_ values: UnsafePointer<Float>, width w: Int, height h: Int) -> Data {
    var d = Data(count: 8 + w * h * 4)
    d.withUnsafeMutableBytes { p in
        p.storeBytes(of: UInt32(w).littleEndian, toByteOffset: 0, as: UInt32.self)
        p.storeBytes(of: UInt32(h).littleEndian, toByteOffset: 4, as: UInt32.self)
        memcpy(p.baseAddress! + 8, values, w * h * 4)
    }
    return d
}

// MARK: - PNG via ImageIO

public let sRGBSpace = CGColorSpace(name: CGColorSpace.sRGB)!

/// Encode RGBA8 (alpha ignored) as an 8-bit RGB sRGB PNG.
public func encodePNG(rgba: UnsafeRawPointer, width w: Int, height h: Int) throws -> Data {
    let cfdata = CFDataCreateWithBytesNoCopy(nil, rgba.assumingMemoryBound(to: UInt8.self), w * h * 4, kCFAllocatorNull)!
    guard let provider = CGDataProvider(data: cfdata),
          let img = CGImage(width: w, height: h, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: w * 4,
                            space: sRGBSpace, bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.noneSkipLast.rawValue),
                            provider: provider, decode: nil, shouldInterpolate: false, intent: .defaultIntent)
    else { throw StitchError("CGImage creation failed") }
    let out = NSMutableData()
    guard let dest = CGImageDestinationCreateWithData(out as CFMutableData, UTType.png.identifier as CFString, 1, nil) else {
        throw StitchError("PNG destination failed")
    }
    CGImageDestinationAddImage(dest, img, nil)
    guard CGImageDestinationFinalize(dest) else { throw StitchError("PNG encode failed") }
    return out as Data
}

/// Decode an image file and count pixels whose RGB differs from `ref` (tightly packed RGBA8).
/// Returns nil if the size differs.
public func countImageMismatches(_ url: URL, ref: UnsafePointer<UInt8>, width W: Int, height H: Int) throws -> Int? {
    guard let src = CGImageSourceCreateWithURL(url as CFURL, [kCGImageSourceShouldCache: false] as CFDictionary),
          let img = CGImageSourceCreateImageAtIndex(src, 0, [kCGImageSourceShouldCache: false] as CFDictionary),
          let data = img.dataProvider?.data else { throw StitchError("cannot decode \(url.path)") }
    guard img.width == W, img.height == H else { return nil }
    let bpr = img.bytesPerRow, bpp = img.bitsPerPixel / 8
    guard img.bitsPerComponent == 8, bpp == 3 || bpp == 4 else { throw StitchError("unexpected decoded format") }
    let off: Int = {
        switch img.alphaInfo {
        case .first, .premultipliedFirst, .noneSkipFirst: return 1
        default: return 0
        }
    }()
    let p = CFDataGetBytePtr(data)!
    var bad = 0
    for y in 0..<H {
        let row = p + y * bpr + off
        let r = ref + y * W * 4
        for x in 0..<W where row[x * bpp] != r[4 * x] || row[x * bpp + 1] != r[4 * x + 1] || row[x * bpp + 2] != r[4 * x + 2] {
            bad += 1
        }
    }
    return bad
}

public struct DecodedImage {
    public var width: Int
    public var height: Int
    public var rgba: [UInt8]   // tightly packed RGBA8
}

/// Decode an image file to RGBA8 without colour management (raw stored values).
public func decodeImageFile(_ url: URL) throws -> DecodedImage {
    guard let src = CGImageSourceCreateWithURL(url as CFURL, [kCGImageSourceShouldCache: false] as CFDictionary),
          let img = CGImageSourceCreateImageAtIndex(src, 0, [kCGImageSourceShouldCache: false] as CFDictionary),
          let data = img.dataProvider?.data else { throw StitchError("cannot decode \(url.path)") }
    let w = img.width, h = img.height, bpr = img.bytesPerRow, bpp = img.bitsPerPixel / 8
    guard img.bitsPerComponent == 8, bpp == 3 || bpp == 4 else {
        throw StitchError("unexpected decoded format bpc=\(img.bitsPerComponent) bpp=\(bpp)")
    }
    let alphaFirst: Bool = {
        switch img.alphaInfo {
        case .first, .premultipliedFirst, .noneSkipFirst: return true
        default: return false
        }
    }()
    var out = [UInt8](repeating: 255, count: w * h * 4)
    let p = CFDataGetBytePtr(data)!
    out.withUnsafeMutableBufferPointer { o in
        for y in 0..<h {
            let row = p + y * bpr
            for x in 0..<w {
                let s = row + x * bpp + ((bpp == 4 && alphaFirst) ? 1 : 0)
                o[(y * w + x) * 4] = s[0]
                o[(y * w + x) * 4 + 1] = s[1]
                o[(y * w + x) * 4 + 2] = s[2]
            }
        }
    }
    return DecodedImage(width: w, height: h, rgba: out)
}

// MARK: - Colour

@inline(__always) public func srgbToLinear(_ c: Double) -> Double {
    c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4)
}
@inline(__always) public func linearToSrgb(_ c: Double) -> Double {
    let x = min(max(c, 0), 1)
    return x <= 0.0031308 ? x * 12.92 : 1.055 * pow(x, 1 / 2.4) - 0.055
}

public func now() -> Double { Date().timeIntervalSince1970 }
public func log(_ s: String) {
    FileHandle.standardError.write((s + "\n").data(using: .utf8)!)
}
