import Foundation
import CoreGraphics
let pid = Int(CommandLine.arguments[1])!
let list = CGWindowListCopyWindowInfo([.optionOnScreenOnly, .excludeDesktopElements], kCGNullWindowID) as? [[String: Any]] ?? []
let windows = list.filter { ($0[kCGWindowOwnerPID as String] as? Int) == pid }.map { ["name": $0[kCGWindowName as String] ?? "", "bounds": $0[kCGWindowBounds as String] ?? [:], "layer": $0[kCGWindowLayer as String] ?? 0] }
let data = try JSONSerialization.data(withJSONObject: windows)
print(String(data: data, encoding: .utf8)!)
