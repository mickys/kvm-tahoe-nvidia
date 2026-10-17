// cgs-mode.js: list or set display modes through the private CGS API, which (unlike CGDisplayCopyAllDisplayModes)
// includes the HiDPI modes. Run inside the console GUI session:
//   sudo launchctl asuser 501 sudo -u <user> osascript -l JavaScript cgs-mode.js list
//   ... cgs-mode.js set W H HZ SCALE        e.g. set 2048 1080 50 2 (= 4096x2160 HiDPI, the original mode)
function run(argv) {
ObjC.import("CoreGraphics");
ObjC.bindFunction("CGMainDisplayID", ["unsigned int", []]);
ObjC.bindFunction("CGSGetNumberOfDisplayModes", ["int", ["unsigned int", "void *"]]);
ObjC.bindFunction("CGSGetDisplayModeDescriptionOfLength", ["int", ["unsigned int", "int", "void *", "int"]]);
ObjC.bindFunction("CGSGetCurrentDisplayMode", ["int", ["unsigned int", "void *"]]);
ObjC.bindFunction("CGBeginDisplayConfiguration", ["int", ["void **"]]);
ObjC.bindFunction("CGSConfigureDisplayMode", ["int", ["void *", "unsigned int", "int"]]);
ObjC.bindFunction("CGCompleteDisplayConfiguration", ["int", ["void *", "unsigned int"]]);
var d = $.CGMainDisplayID();
function u32(b, o) { return (b[o] | b[o+1] << 8 | b[o+2] << 16 | b[o+3] << 24) >>> 0; }
function bytes(data) { var s = ObjC.unwrap(data.base64EncodedStringWithOptions(0)), r = [];
  var t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/", bits = 0, v = 0;
  for (var i = 0; i < s.length && s[i] != "="; i++) { v = v << 6 | t.indexOf(s[i]); bits += 6; if (bits >= 8) { bits -= 8; r.push(v >> bits & 255); } }
  return r; }
function mode(i) { var m = $.NSMutableData.dataWithLength(0xD4); $.CGSGetDisplayModeDescriptionOfLength(d, i, m.mutableBytes, 0xD4);
  var b = bytes(m), f = new DataView(new Uint8Array(b.slice(208, 212)).buffer).getFloat32(0, true);
  return { i: i, w: u32(b, 8), h: u32(b, 12), depth: u32(b, 16), hz: b[190] | b[191] << 8, scale: f }; }
var n = $.NSMutableData.dataWithLength(4); $.CGSGetNumberOfDisplayModes(d, n.mutableBytes); var count = u32(bytes(n), 0);
var c = $.NSMutableData.dataWithLength(4); $.CGSGetCurrentDisplayMode(d, c.mutableBytes); var cur = u32(bytes(c), 0);
function desc(m) { return m.i + ": " + m.w + "x" + m.h + " @" + m.hz + " scale " + m.scale + " depth " + m.depth + (m.i == cur ? "  <- current" : ""); }
var all = []; for (var i = 0; i < count; i++) all.push(mode(i));
if (argv[0] == "set") {
  var W = +argv[1], H = +argv[2], HZ = +argv[3], S = +argv[4], pick = all.filter(function (m) { return m.w == W && m.h == H && m.hz == HZ && m.scale == S; })[0];
  if (!pick) return "no such mode";
  var ref = Ref(); var r1 = $.CGBeginDisplayConfiguration(ref), r2 = $.CGSConfigureDisplayMode(ref[0], d, pick.i), r3 = $.CGCompleteDisplayConfiguration(ref[0], 2);
  delay(3); var c2 = $.NSMutableData.dataWithLength(4); $.CGSGetCurrentDisplayMode(d, c2.mutableBytes); cur = u32(bytes(c2), 0);
  return "set " + desc(pick) + " -> " + r1 + "/" + r2 + "/" + r3 + "; now " + desc(mode(cur));
}
return all.map(desc).join("\n");
}
