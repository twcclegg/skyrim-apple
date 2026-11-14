// Lays out labelled comparison sheets for tools/stereo_compare.py (true stereo vs depth reprojection), with
// CoreGraphics only, so no Python imaging packages are needed.
//
//   swiftc -O tools/stereo_compose.swift -o /tmp/stereo_compose
//   /tmp/stereo_compose sheet.json
//
// sheet.json:
//   {"out": "/path/sheet.png", "title": "...", "cell": [960, 540], "nearest": false,
//    "rows": [{"label": "true stereo", "cells": [CELL, ...]}, ...]}
// CELL is {"label": "...", "source": SOURCE}. SOURCE is one of
//   {"path": "/a.bmp", "rect": [x, y, w, h]}       a region of an image file (normalized, top-left origin; the
//                                                  whole image without "rect")
//   {"anaglyph": [SOURCE, SOURCE]}                 red = left's luminance, green and blue = right's
//   {"diff": [SOURCE, SOURCE], "gain": 4}          |a - b| per channel, times gain
// Sources are scaled to the cell (aspect kept, letterboxed in grey); "nearest": true keeps pixels sharp for crops.
import AppKit
import CoreGraphics
import Foundation

func fail(_ text: String) -> Never {
    FileHandle.standardError.write(("stereo_compose: " + text + "\n").data(using: .utf8)!)
    exit(1)
}

/// An RGBA8 picture in memory, rows top to bottom.
struct Picture {
    var width: Int, height: Int
    var pixels: [UInt8]

    init(width: Int, height: Int) {
        self.width = width; self.height = height
        pixels = [UInt8](repeating: 0, count: width * height * 4)
    }

    init(image: CGImage) {
        self.init(width: image.width, height: image.height)
        let w = width, h = height
        pixels.withUnsafeMutableBytes { buffer in
            let context = CGContext(data: buffer.baseAddress, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w * 4,
                                    space: CGColorSpace(name: CGColorSpace.sRGB)!,
                                    bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
            context.draw(image, in: CGRect(x: 0, y: 0, width: w, height: h))
        }
    }

    func image() -> CGImage {
        var copy = pixels
        let provider = CGDataProvider(data: Data(bytes: &copy, count: copy.count) as CFData)!
        return CGImage(width: width, height: height, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: width * 4,
                       space: CGColorSpace(name: CGColorSpace.sRGB)!, bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.premultipliedLast.rawValue),
                       provider: provider, decode: nil, shouldInterpolate: false, intent: .defaultIntent)!
    }
}

var fileCache: [String: Picture] = [:]

func load(_ path: String) -> Picture {
    if let cached = fileCache[path] { return cached }
    guard let image = NSImage(contentsOfFile: path)?.cgImage(forProposedRect: nil, context: nil, hints: nil) else { fail("cannot read \(path)") }
    let picture = Picture(image: image)
    fileCache[path] = picture
    return picture
}

func crop(_ picture: Picture, _ rect: [Double]) -> Picture {
    guard rect.count == 4 else { fail("rect needs [x, y, w, h]") }
    let x0 = max(0, min(picture.width - 1, Int((rect[0] * Double(picture.width)).rounded())))
    let y0 = max(0, min(picture.height - 1, Int((rect[1] * Double(picture.height)).rounded())))
    let w = max(1, min(picture.width - x0, Int((rect[2] * Double(picture.width)).rounded())))
    let h = max(1, min(picture.height - y0, Int((rect[3] * Double(picture.height)).rounded())))
    var out = Picture(width: w, height: h)
    for y in 0..<h {
        let from = ((y0 + y) * picture.width + x0) * 4
        out.pixels.replaceSubrange(y * w * 4..<(y + 1) * w * 4, with: picture.pixels[from..<from + w * 4])
    }
    return out
}

/// b resampled (nearest) to a's size, so pictures of different resolutions can be combined.
func matched(_ b: Picture, to a: Picture) -> Picture {
    if a.width == b.width && a.height == b.height { return b }
    var out = Picture(width: a.width, height: a.height)
    for y in 0..<a.height {
        let sy = min(b.height - 1, y * b.height / a.height)
        for x in 0..<a.width {
            let sx = min(b.width - 1, x * b.width / a.width)
            for c in 0..<4 { out.pixels[(y * a.width + x) * 4 + c] = b.pixels[(sy * b.width + sx) * 4 + c] }
        }
    }
    return out
}

func source(_ spec: [String: Any]) -> Picture {
    if let path = spec["path"] as? String {
        let picture = load(path)
        if let rect = spec["rect"] as? [Double] { return crop(picture, rect) }
        return picture
    }
    if let pair = spec["anaglyph"] as? [[String: Any]], pair.count == 2 {
        let left = source(pair[0]), right = matched(source(pair[1]), to: left)
        var out = Picture(width: left.width, height: left.height)
        for i in 0..<(left.width * left.height) {
            func luma(_ p: Picture) -> UInt8 {
                let r = Double(p.pixels[i * 4]), g = Double(p.pixels[i * 4 + 1]), b = Double(p.pixels[i * 4 + 2])
                return UInt8(min(255, 0.299 * r + 0.587 * g + 0.114 * b))
            }
            let l = luma(left), r = luma(right)
            out.pixels[i * 4] = l; out.pixels[i * 4 + 1] = r; out.pixels[i * 4 + 2] = r; out.pixels[i * 4 + 3] = 255
        }
        return out
    }
    if let pair = spec["diff"] as? [[String: Any]], pair.count == 2 {
        let a = source(pair[0]), b = matched(source(pair[1]), to: a)
        let gain = spec["gain"] as? Double ?? 4
        var out = Picture(width: a.width, height: a.height)
        for i in 0..<(a.width * a.height) {
            for c in 0..<3 {
                let d = abs(Double(a.pixels[i * 4 + c]) - Double(b.pixels[i * 4 + c])) * gain
                out.pixels[i * 4 + c] = UInt8(min(255, d))
            }
            out.pixels[i * 4 + 3] = 255
        }
        return out
    }
    fail("a source needs path, anaglyph or diff")
}

func drawLabel(_ text: String, at point: CGPoint, size: CGFloat, in context: CGContext, height: Int) {
    let attributes: [NSAttributedString.Key: Any] = [.font: NSFont.boldSystemFont(ofSize: size), .foregroundColor: NSColor.white]
    let string = NSAttributedString(string: text, attributes: attributes)
    let bounds = string.size()
    // Context is flipped to top-left for layout; text needs the native orientation, so draw via NSGraphicsContext.
    let box = CGRect(x: point.x, y: point.y, width: bounds.width + 12, height: bounds.height + 6)
    context.setFillColor(CGColor(red: 0, green: 0, blue: 0, alpha: 0.7))
    context.fill(CGRect(x: box.minX, y: CGFloat(height) - box.maxY, width: box.width, height: box.height))
    NSGraphicsContext.saveGraphicsState()
    NSGraphicsContext.current = NSGraphicsContext(cgContext: context, flipped: false)
    string.draw(at: CGPoint(x: box.minX + 6, y: CGFloat(height) - box.maxY + 3))
    NSGraphicsContext.restoreGraphicsState()
}

// ---------------------------------------------------------------- sheet

guard CommandLine.arguments.count == 2 else { fail("usage: stereo_compose sheet.json") }
guard let data = FileManager.default.contents(atPath: CommandLine.arguments[1]),
      let sheet = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else { fail("cannot read \(CommandLine.arguments[1])") }
guard let out = sheet["out"] as? String, let rows = sheet["rows"] as? [[String: Any]], !rows.isEmpty else { fail("sheet needs out and rows") }
let cellSize = (sheet["cell"] as? [Int]) ?? [960, 540]
guard cellSize.count == 2, cellSize[0] > 0, cellSize[1] > 0 else { fail("cell needs [width, height]") }
let nearest = sheet["nearest"] as? Bool ?? false
let title = sheet["title"] as? String ?? ""

let gap = 8, rowLabel = 34, titleHeight = title.isEmpty ? 0 : 44
let columns = rows.map { ($0["cells"] as? [[String: Any]])?.count ?? 0 }.max() ?? 0
guard columns > 0 else { fail("rows have no cells") }
let width = gap + columns * (cellSize[0] + gap)
let height = titleHeight + rows.count * (rowLabel + cellSize[1] + gap) + gap

let context = CGContext(data: nil, width: width, height: height, bitsPerComponent: 8, bytesPerRow: 0,
                        space: CGColorSpace(name: CGColorSpace.sRGB)!, bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
context.setFillColor(CGColor(red: 0.12, green: 0.12, blue: 0.12, alpha: 1))
context.fill(CGRect(x: 0, y: 0, width: width, height: height))
context.interpolationQuality = nearest ? .none : .high
if !title.isEmpty { drawLabel(title, at: CGPoint(x: gap, y: 8), size: 20, in: context, height: height) }

for (r, row) in rows.enumerated() {
    let top = titleHeight + r * (rowLabel + cellSize[1] + gap)
    if let label = row["label"] as? String { drawLabel(label, at: CGPoint(x: gap, y: top + 4), size: 16, in: context, height: height) }
    for (c, cell) in ((row["cells"] as? [[String: Any]]) ?? []).enumerated() {
        let x = gap + c * (cellSize[0] + gap), y = top + rowLabel
        context.setFillColor(CGColor(red: 0.3, green: 0.3, blue: 0.3, alpha: 1))
        context.fill(CGRect(x: x, y: height - y - cellSize[1], width: cellSize[0], height: cellSize[1]))
        guard let spec = cell["source"] as? [String: Any] else { continue }
        let picture = source(spec)
        let scale = min(Double(cellSize[0]) / Double(picture.width), Double(cellSize[1]) / Double(picture.height))
        let w = Double(picture.width) * scale, h = Double(picture.height) * scale
        let fx = Double(x) + (Double(cellSize[0]) - w) / 2, fy = Double(y) + (Double(cellSize[1]) - h) / 2
        context.draw(picture.image(), in: CGRect(x: fx, y: Double(height) - fy - h, width: w, height: h))
        if let label = cell["label"] as? String { drawLabel(label, at: CGPoint(x: x + 4, y: y + 4), size: 13, in: context, height: height) }
    }
}

guard let png = NSBitmapImageRep(cgImage: context.makeImage()!).representation(using: .png, properties: [:]) else { fail("cannot encode PNG") }
do { try png.write(to: URL(fileURLWithPath: out)) } catch { fail("cannot write \(out): \(error)") }
print(out)
