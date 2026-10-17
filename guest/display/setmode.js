// usage: osascript -l JavaScript setmode.js W H HZ [hidpi] | setmode.js 0x<IODisplayModeID>   (run inside the console GUI session: sudo launchctl asuser 501 sudo -u <user> osascript ...)
function run(argv) {
ObjC.import("CoreGraphics");
ObjC.bindFunction("CGMainDisplayID", ["unsigned int", []]);
ObjC.bindFunction("CGDisplayCopyAllDisplayModes", ["id", ["unsigned int", "id"]]);
ObjC.bindFunction("CGDisplayCopyDisplayMode", ["id", ["unsigned int"]]);
ObjC.bindFunction("CGDisplayModeGetWidth", ["unsigned long", ["id"]]);
ObjC.bindFunction("CGDisplayModeGetHeight", ["unsigned long", ["id"]]);
ObjC.bindFunction("CGDisplayModeGetPixelWidth", ["unsigned long", ["id"]]);
ObjC.bindFunction("CGDisplayModeGetPixelHeight", ["unsigned long", ["id"]]);
ObjC.bindFunction("CGDisplayModeGetRefreshRate", ["double", ["id"]]);
ObjC.bindFunction("CGDisplayModeGetIODisplayModeID", ["int", ["id"]]);
ObjC.bindFunction("CGBeginDisplayConfiguration", ["int", ["void **"]]);
ObjC.bindFunction("CGConfigureDisplayWithDisplayMode", ["int", ["void *", "unsigned int", "id", "id"]]);
ObjC.bindFunction("CGCompleteDisplayConfiguration", ["int", ["void *", "unsigned int"]]);
function desc(m) { return $.CGDisplayModeGetWidth(m) + "x" + $.CGDisplayModeGetHeight(m) + " (px " + $.CGDisplayModeGetPixelWidth(m) + "x" + $.CGDisplayModeGetPixelHeight(m) + ") @" + $.CGDisplayModeGetRefreshRate(m); }
var W = +argv[0], H = +argv[1], HZ = +argv[2], scale = argv[3] == "hidpi" ? 2 : 1;
var d = $.CGMainDisplayID(), out = ["before: " + desc($.CGDisplayCopyDisplayMode(d))];
var opts = $.NSDictionary.dictionaryWithObjectForKey($.NSNumber.numberWithBool(true), $("kCGDisplayShowDuplicateLowResolutionModes"));
var ms = $.CGDisplayCopyAllDisplayModes(d, opts), pick = null;
for (var i = 0; i < ms.count; i++) { var m = ms.objectAtIndex(i);
  if (argv[0].indexOf("0x") == 0 ? ($.CGDisplayModeGetIODisplayModeID(m) >>> 0) == parseInt(argv[0], 16) : $.CGDisplayModeGetWidth(m) == W && $.CGDisplayModeGetHeight(m) == H && $.CGDisplayModeGetPixelWidth(m) == W * scale && Math.round($.CGDisplayModeGetRefreshRate(m)) == HZ) pick = m; }
if (!pick) return out.concat("no such mode").join("\n");
out.push("picked: " + desc(pick));
var ref = Ref();
out.push("begin " + $.CGBeginDisplayConfiguration(ref) + " configure " + $.CGConfigureDisplayWithDisplayMode(ref[0], d, pick, $()) + " complete " + $.CGCompleteDisplayConfiguration(ref[0], 2));
delay(3);
out.push("after: " + desc($.CGDisplayCopyDisplayMode(d)));
return out.join("\n");
}
