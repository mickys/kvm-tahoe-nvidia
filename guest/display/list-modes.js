// list-modes.js: print every display mode as "<IODisplayModeID hex> WxH pxW @Hz fl IOFlags" (run like setmode.js)
ObjC.import("CoreGraphics");
ObjC.bindFunction("CGMainDisplayID", ["unsigned int", []]);
ObjC.bindFunction("CGDisplayCopyAllDisplayModes", ["id", ["unsigned int", "id"]]);
ObjC.bindFunction("CGDisplayModeGetWidth", ["unsigned long", ["id"]]);
ObjC.bindFunction("CGDisplayModeGetHeight", ["unsigned long", ["id"]]);
ObjC.bindFunction("CGDisplayModeGetPixelWidth", ["unsigned long", ["id"]]);
ObjC.bindFunction("CGDisplayModeGetRefreshRate", ["double", ["id"]]);
ObjC.bindFunction("CGDisplayModeGetIODisplayModeID", ["int", ["id"]]);
ObjC.bindFunction("CGDisplayModeGetIOFlags", ["unsigned int", ["id"]]);
var d = $.CGMainDisplayID(), out = [];
var opts = $.NSDictionary.dictionaryWithObjectForKey($.NSNumber.numberWithBool(true), $("kCGDisplayShowDuplicateLowResolutionModes"));
var ms = $.CGDisplayCopyAllDisplayModes(d, opts);
for (var i = 0; i < ms.count; i++) { var m = ms.objectAtIndex(i);
  out.push(($.CGDisplayModeGetIODisplayModeID(m) >>> 0).toString(16) + " " + $.CGDisplayModeGetWidth(m) + "x" + $.CGDisplayModeGetHeight(m) + " px" + $.CGDisplayModeGetPixelWidth(m) + " @" + $.CGDisplayModeGetRefreshRate(m) + " fl 0x" + ($.CGDisplayModeGetIOFlags(m) >>> 0).toString(16)); }
out.join("\n");
