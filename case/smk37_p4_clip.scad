// Clip: holds the Guition JC1060P470C (ESP32-P4, 7 inch, in its case) on the rear edge of the M-Vave SMK-37.
// How to print and mount it: see README.md in this folder.
//
// Each clip is screwed to 2 of the 4 brass M2 posts on the back of the screen (one low, one high).
// Print 2 clips: clip A (side = 0) and clip B (side = 1), one for each pair of posts. The holes are near
// one side edge, so A and B are mirror parts. The test slice (part = 1) is a thin copy to check the fit
// on the keyboard; its holes are in the middle, so it does not show the side offset.
//
// The part is a 2D profile, extruded along Z (the clip width), then drilled.
//   X = depth: x = 0 is the rear face of the keyboard, +X goes to the player, -X goes behind the keyboard.
//   Y = up:    y = 0 is the top deck of the keyboard.
// So the STL already lies on its side: the layers follow the profile. No supports.
//
// STATUS (2026-11-23): clip A and clip B were printed with these values and the fit is good.
// "PRINT" in a comment = the value was tuned on a real print. "MEASURED" = measured on the real part.
// The other values are design choices; they worked, but nobody measured the part they stand for.
//
// Web sizes used at the start (not needed to print):
//   Screen plate 181.80 x 113.72 mm: manuals.plus/ae/1005010748515638 (seller manual).
//   SMK-37 about 480 x 210 x 72 mm overall, knobs included: equipboard.com (not from M-Vave).

part = 0;            // 0 = full clip, 1 = thin test slice
side = 0;            // full clip only: 0 = clip A, 1 = clip B (the mirror of A). Print one of each.
hole_edge = 6;       // PRINT: the screw holes are this far from one side edge of the clip

// ---- keyboard (SMK-37): the jaw ----
// The rear of the keyboard is flat: there is no step to hold. The grip is the pinch and the hook.
kb_rear_h  = 34.0;   // PRINT: jaw opening, top tongue to bottom tongue. The body is 34.5 high (MEASURED).
kb_top_reach = 10;   // PRINT: top tongue length. The deck is free for only 3 mm (MEASURED), then the knobs
                     // start, so put each clip between two knobs.
kb_lip_len = 60;     // bottom tongue length
lip_t      = 2.6;    // PRINT: bottom tongue thickness
pinch      = 0.7;    // PRINT: degrees. The bottom tongue points up, so the jaw is a spring clamp.
                     // 0 = no grip. 2 was too tight: the back plate bowed.
hook_x     = 36;     // PRINT: steep face of the hook, from the rear edge. It falls into the recess under the keyboard.
hook_h     = 2.5;    // PRINT: hook height. 0 = no hook
hook_len   = 5;      // hook length: 1 mm flat top, then a ramp to the tip side (so the clip slides on)

// ---- screen (JC1060P470C): the arm and the posts on its back ----
screw_d    = 2.4;    // hole for an M2 screw (MEASURED: the thread is M2)
post_gap_min = 93;   // The low post to the high post, centre to centre, along the arm.
post_gap_max = 101;  // PRINT: the top hole is a slot from min to max. The gap was not measured.
arm_top    = 5;      // PRINT: arm length above the end of the slot
lift       = 30;     // PRINT: low post above the deck, along the arm (the screen must clear the knobs)
tilt       = 45;     // PRINT: the screen leans back this many degrees from vertical
arm_t      = 4;      // arm thickness
arm_x      = 3;      // front face of the arm at the deck, from the rear edge to the player

// ---- foot behind the keyboard (the jaw alone is not sturdy enough for the screen) ----
strut_v    = 97;     // where the strut meets the arm, along the arm from the deck (must stay below the slot)
strut_t    = 4;      // strut and foot thickness
foot_drop  = 0;      // foot bottom, below the bottom of the tongue. 0 = same level as the tongue.
                     // Make it bigger if the foot does not touch the table.
access_d   = 6;      // hole through the strut for the screwdriver (low screw)

// ---- print ----
wall    = 3;         // thickness of the back plate and the top tongue
clip_w  = 25;        // width of one clip
test_w  = 10;        // width of the test slice
fillet  = 1.5;       // inside corner radius (stops cracks at the jaw)
round_r = 0.6;       // outside corner radius

$fn = 48;
arm_len = lift + post_gap_max + arm_top;
w = part == 0 ? clip_w : test_w;

module jaw() {
    bottom = -kb_rear_h - lip_t;
    translate([-wall, bottom]) square([wall, wall - bottom]);              // back plate
    translate([-wall, 0]) square([kb_top_reach + wall, wall]);             // top tongue (rests on the deck)
    translate([0, -kb_rear_h]) rotate(pinch)                               // bottom tongue, tip points up
        translate([-wall, -lip_t]) square([kb_lip_len + wall, lip_t]);
}

module arm_frame() { translate([arm_x, wall]) rotate(tilt) children(); }   // u = out of the arm face, v = up the arm

module arm() {
    intersection() {                                                       // never below the deck plane
        arm_frame() translate([-arm_t, -10]) square([arm_t, arm_len + 10]);
        translate([-500, 0]) square(1000);
    }
    difference() {
        hull() {                                                           // brace behind the arm, down the back plate
            arm_frame() translate([-arm_t, 0]) square([arm_t, lift - 6]); // stops below the low screw head
            translate([-wall, -15]) square([wall, 15 + wall]);
        }
        translate([0, -500]) square(500);                                  // the brace must not enter the jaw (was a bug)
    }
}

module bar(p, q, t) { hull() { translate(p) circle(d = t); translate(q) circle(d = t); } }

module foot() {                                     // a foot on the table, and a strut from it up to the arm
    px = arm_x + (-arm_t / 2) * cos(tilt) - strut_v * sin(tilt);            // the point on the arm
    py = wall  + (-arm_t / 2) * sin(tilt) + strut_v * cos(tilt);
    fy = -kb_rear_h - lip_t - foot_drop + strut_t / 2;                      // centre line of the foot
    bar([-wall / 2, fy], [px, fy], strut_t);                                // foot, from the back plate to the rear
    bar([px, fy], [px, py], strut_t);                                       // strut, straight up to the arm
    if (foot_drop > 0) bar([-wall / 2, fy], [-wall / 2, -kb_rear_h], wall); // join a lower foot to the back plate
}

module profile() {
    offset(r = round_r) offset(r = -round_r)        // round the outside corners
    offset(r = -fillet) offset(delta = fillet)      // fill the inside corners
    union() { jaw(); arm(); foot(); }
    if (hook_h > 0)                                // hook: after the fillets, so its rear face stays steep
        translate([0, -kb_rear_h]) rotate(pinch)
            polygon([[hook_x, -1], [hook_x, hook_h], [hook_x + 1, hook_h], [hook_x + hook_len, 0], [hook_x + hook_len, -1]]);
}

module hole(v) {                                    // screw hole through the arm, at height v along the arm
    translate([0, 0, part == 1 ? w / 2 : (side == 0 ? hole_edge : w - hole_edge)]) arm_frame() translate([0, v, 0])
        rotate([0, 90, 0]) cylinder(d = screw_d, h = 60, center = true);
}

difference() {
    linear_extrude(height = w) profile();
    hole(lift);                                                            // low post
    translate([0, 0, part == 1 ? w / 2 : (side == 0 ? hole_edge : w - hole_edge)])   // screwdriver access
        arm_frame() translate([-arm_t - 10, lift, 0]) rotate([0, -90, 0]) cylinder(d = access_d, h = 300);
    hull() { hole(lift + post_gap_min); hole(lift + post_gap_max); }       // high post (slot)
}
