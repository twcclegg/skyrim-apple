// Writes an MV-HEVC spatial video (Apple Vision Pro) from pairs of left/right images.
//
//   spatial_video LIST.txt OUT.mov --fps 24 --baseline-mm 64 --hfov 80.7 [--disparity 0] [--bitrate 40]
//
// LIST.txt: one frame per line, "left_path<TAB>right_path", or "sbs_path" (left half | right half).
import AVFoundation
import AppKit
import CoreMedia
import VideoToolbox

func fail(_ s: String) -> Never { FileHandle.standardError.write((s + "\n").data(using: .utf8)!); exit(1) }

var positional: [String] = []
var opts: [String: String] = [:]
var i = 1
let argv = CommandLine.arguments
while i < argv.count {
    if argv[i].hasPrefix("--") { opts[String(argv[i].dropFirst(2))] = argv[i + 1]; i += 2 } else { positional.append(argv[i]); i += 1 }
}
guard positional.count == 2 else { fail("usage: spatial_video LIST.txt OUT.mov [--fps 24] [--baseline-mm 64] [--hfov 80.7]") }
let fps = Int32(opts["fps"] ?? "24")!
let baselineMM = Double(opts["baseline-mm"] ?? "64")!
let hfov = Double(opts["hfov"] ?? "80.7")!
let disparity = Int32(opts["disparity"] ?? "0")!
let bitrate = Int(Double(opts["bitrate"] ?? "40")! * 1_000_000)
let halfEq = (opts["projection"] ?? "rect") == "halfeq"

guard VTIsStereoMVHEVCEncodeSupported() else { fail("this Mac can't encode MV-HEVC") }

func cgImage(_ path: String) -> CGImage {
    guard let img = NSImage(contentsOfFile: path), let cg = img.cgImage(forProposedRect: nil, context: nil, hints: nil)
    else { fail("can't read \(path)") }
    return cg
}

let lines = try! String(contentsOfFile: positional[0], encoding: .utf8).split(separator: "\n").map(String.init)
var frames: [(CGImage, CGImage)] = []
for line in lines where !line.isEmpty {
    let parts = line.split(separator: "\t").map(String.init)
    if parts.count == 2 {
        frames.append((cgImage(parts[0]), cgImage(parts[1])))
    } else {
        let sbs = cgImage(parts[0])
        let w = sbs.width / 2
        frames.append((sbs.cropping(to: CGRect(x: 0, y: 0, width: w, height: sbs.height))!,
                       sbs.cropping(to: CGRect(x: w, y: 0, width: w, height: sbs.height))!))
    }
}
guard let first = frames.first else { fail("no frames") }
let width = first.0.width, height = first.0.height

let outURL = URL(fileURLWithPath: positional[1])
try? FileManager.default.removeItem(at: outURL)
let writer = try! AVAssetWriter(outputURL: outURL, fileType: .mov)
let compression: [String: Any] = [
    kVTCompressionPropertyKey_MVHEVCVideoLayerIDs as String: [0, 1],
    kVTCompressionPropertyKey_MVHEVCViewIDs as String: [0, 1],
    kVTCompressionPropertyKey_MVHEVCLeftAndRightViewIDs as String: [0, 1],
    kVTCompressionPropertyKey_HasLeftStereoEyeView as String: true,
    kVTCompressionPropertyKey_HasRightStereoEyeView as String: true,
    kVTCompressionPropertyKey_HeroEye as String: kVTHeroEye_Left,
    kVTCompressionPropertyKey_StereoCameraBaseline as String: UInt32(baselineMM * 1000),       // micrometres
    kVTCompressionPropertyKey_HorizontalFieldOfView as String: UInt32((halfEq ? 180 : hfov) * 1000),           // millidegrees
    kVTCompressionPropertyKey_HorizontalDisparityAdjustment as String: disparity,             // 1/10000 of width
    kVTCompressionPropertyKey_ProjectionKind as String: halfEq ? kCMFormatDescriptionProjectionKind_HalfEquirectangular : kCMFormatDescriptionProjectionKind_Rectilinear,
    AVVideoAverageBitRateKey: bitrate,
    AVVideoExpectedSourceFrameRateKey: fps,
]
let input = AVAssetWriterInput(mediaType: .video, outputSettings: [
    AVVideoCodecKey: AVVideoCodecType.hevc, AVVideoWidthKey: width, AVVideoHeightKey: height,
    AVVideoCompressionPropertiesKey: compression,
    AVVideoColorPropertiesKey: [AVVideoColorPrimariesKey: AVVideoColorPrimaries_ITU_R_709_2,
                                AVVideoTransferFunctionKey: AVVideoTransferFunction_ITU_R_709_2,
                                AVVideoYCbCrMatrixKey: AVVideoYCbCrMatrix_ITU_R_709_2],
])
input.expectsMediaDataInRealTime = false
let adaptor = AVAssetWriterInputTaggedPixelBufferGroupAdaptor(assetWriterInput: input, sourcePixelBufferAttributes: [
    kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA,
    kCVPixelBufferWidthKey as String: width, kCVPixelBufferHeightKey as String: height,
])
guard writer.canAdd(input) else { fail("writer refuses the MV-HEVC input") }
writer.add(input)
guard writer.startWriting() else { fail("startWriting: \(String(describing: writer.error))") }
writer.startSession(atSourceTime: .zero)

func pixelBuffer(_ img: CGImage) -> CVPixelBuffer {
    var pb: CVPixelBuffer?
    guard let pool = adaptor.pixelBufferPool, CVPixelBufferPoolCreatePixelBuffer(nil, pool, &pb) == kCVReturnSuccess,
          let buf = pb else { fail("no pixel buffer") }
    CVPixelBufferLockBaseAddress(buf, [])
    let ctx = CGContext(data: CVPixelBufferGetBaseAddress(buf), width: width, height: height, bitsPerComponent: 8,
                        bytesPerRow: CVPixelBufferGetBytesPerRow(buf), space: CGColorSpace(name: CGColorSpace.sRGB)!,
                        bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue)!
    ctx.draw(img, in: CGRect(x: 0, y: 0, width: width, height: height))
    CVPixelBufferUnlockBaseAddress(buf, [])
    return buf
}

for (n, (l, r)) in frames.enumerated() {
    while !input.isReadyForMoreMediaData { usleep(2000) }
    let lt = CMTaggedBuffer(tags: [.stereoView(.leftEye), .videoLayerID(0)], pixelBuffer: pixelBuffer(l))
    let rt = CMTaggedBuffer(tags: [.stereoView(.rightEye), .videoLayerID(1)], pixelBuffer: pixelBuffer(r))
    guard adaptor.appendTaggedBuffers([lt, rt], withPresentationTime: CMTime(value: CMTimeValue(n), timescale: fps)) else {
        fail("append frame \(n): \(String(describing: writer.error))")
    }
}
input.markAsFinished()
let done = DispatchSemaphore(value: 0)
writer.finishWriting { done.signal() }
done.wait()
if writer.status != .completed { fail("finish: \(String(describing: writer.error))") }
print("spatial_video: \(frames.count) frames \(width)x\(height) at \(fps) fps -> \(outURL.path)")
