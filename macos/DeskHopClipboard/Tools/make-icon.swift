// Native vector drawing, offline. No image-generation service or clipboard.
import AppKit
let directory = URL(fileURLWithPath: CommandLine.arguments[1])
try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
for size in [16, 32, 128, 256, 512] {
    for scale in [1, 2] {
        let pixels = size * scale
        let bitmap = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: pixels, pixelsHigh: pixels,
            bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB,
            bytesPerRow: 0, bitsPerPixel: 0)!
        NSGraphicsContext.saveGraphicsState()
        NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: bitmap)
        let p = CGFloat(pixels), inset = p * 0.07
        NSColor(calibratedRed: 0.10, green: 0.32, blue: 0.62, alpha: 1).setFill()
        NSBezierPath(roundedRect: NSRect(x: inset, y: inset, width: p - 2 * inset, height: p - 2 * inset), xRadius: p * 0.18, yRadius: p * 0.18).fill()
        // Keyboard silhouette with an arrow indicating one-way text delivery.
        NSColor.white.setStroke()
        let board = NSBezierPath(roundedRect: NSRect(x: p * 0.21, y: p * 0.25, width: p * 0.58, height: p * 0.32), xRadius: p * 0.035, yRadius: p * 0.035)
        board.lineWidth = max(1, p * 0.025); board.stroke()
        NSColor.white.setFill()
        for row in 0..<2 { for col in 0..<6 {
            NSBezierPath(roundedRect: NSRect(x: p * (0.255 + CGFloat(col) * 0.084), y: p * (0.42 - CGFloat(row) * 0.07), width: p * 0.045, height: p * 0.032), xRadius: 1, yRadius: 1).fill()
        } }
        NSBezierPath(roundedRect: NSRect(x: p * 0.37, y: p * 0.29, width: p * 0.26, height: p * 0.03), xRadius: 1, yRadius: 1).fill()
        let arrow = NSBezierPath(); arrow.move(to: NSPoint(x: p * 0.3, y: p * 0.7)); arrow.line(to: NSPoint(x: p * 0.7, y: p * 0.7))
        arrow.move(to: NSPoint(x: p * 0.59, y: p * 0.81)); arrow.line(to: NSPoint(x: p * 0.7, y: p * 0.7)); arrow.line(to: NSPoint(x: p * 0.59, y: p * 0.59))
        arrow.lineWidth = max(1, p * 0.04); arrow.lineCapStyle = .round; arrow.lineJoinStyle = .round; arrow.stroke()
        NSGraphicsContext.restoreGraphicsState()
        let name = "icon_\(size)x\(size)\(scale == 2 ? "@2x" : "").png"
        try bitmap.representation(using: .png, properties: [:])!.write(to: directory.appendingPathComponent(name))
    }
}
