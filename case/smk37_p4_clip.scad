// Clip: holds the Guition JC1060P470C (ESP32-P4, 7 inch, in its case) on the rear edge of the M-Vave SMK-37.
// Each clip is screwed to 2 of the 4 brass threaded posts on the back of the screen (one low, one high).
// Print 2 clips: clip A (side = 0) and clip B (side = 1), one for each pair of posts. The holes are 6 mm from
// one side edge, so A and B are mirror parts. Print the test slice first (part = 1).
// The part is a 2D profile (X = depth, +X to the player; Y = up), extruded along Z, then drilled.
// So the STL already lies on its side: the layers follow the C shape. No supports.
//
// STATUS (2026-11-23): the user printed clip A and clip B with these values and the fit is good
// (keyboard jaw, hook, screw holes, 45 degree tilt, foot). Values marked GUESS below are now confirmed by that print.
//
// SIZES AND SOURCES (2026-11-22), from before the test prints:
//   Screen plate 181.80 x 113.72 mm: manuals.plus/ae/1005010748515638 (seller manual).
//   The 4 posts are near the corners of that plate (the user's photo). Their spacing and thread are GUESSES.
//   SMK-37 about 480 x 210 x 72 mm overall (knobs included): equipboard.com (not from M-Vave).
//   No source gives the SMK-37 rear edge shape. Values marked GUESS must be checked with the test slice.

part = 0;            // 0 = full clip, 1 = thin test slice
side = 0;            // full clip only: 0 = clip A, 1 = clip B (the mirror of A). Print one of each.
hole_edge = 6;       // the screw holes are this far from one side edge of the clip (the user asked for 6)

// ---- keyboard (SMK-37) ----
kb_rear_h  = 34.0;   // jaw opening. The user measured 34.5 (2026-11-22), then asked for 0.5 less after a test print.
kb_top_reach = 10;   // top tongue length (15 was 5 too long: the user's test print). The deck is free for only 3 mm (measured), then the knobs start:
                     // put each clip where no knob is in the way
kb_lip_len = 60;     // bottom tongue length (the user asked: long below, shorter on top)
lip_t      = 2.6;    // bottom tongue thickness (3 was 0.4 too thick: the user's test print)
pinch      = 0.7;    // degrees: the bottom tongue points up, so the jaw is a spring clamp (0 = no grip)
                     // 2 degrees (tip 2.1 mm high) was too tight in the user's test print: the back plate bowed.
                     // The rear of the keyboard is flat (user), so the grip is the pinch and the hook.
hook_x     = 36;     // steep face of the hook, from the rear edge. 31 was 5 mm too near (the user's test print)
hook_h     = 2.5;    // hook height (the user asked for 2.5). 0 = no hook
hook_len   = 5;      // GUESS: hook length (1 mm flat top, then a ramp to the tip side)

// ---- screen (JC1060P470C): the posts on its back ----
screw_d    = 2.4;    // hole for an M2 screw (the user checked the thread: M2)
post_gap_min = 93;   // GUESS: the low post to the high post is about 102 mm, centre to centre.
post_gap_max = 101;  //        The top hole is a slot from min to max, so 93 to 101 mm fits.
arm_top    = 5;      // arm length above the end of the slot. The arm end is now 6 mm lower than in the last
                     // print (the user: "top is still 6 mm too long"). The low hole is confirmed good.

// ---- foot behind the keyboard (the user: the clip alone is not sturdy enough for the screen) ----
strut_v    = 97;     // where the strut meets the arm, along the arm from the deck (must stay below the slot)
strut_t    = 4;      // strut and foot thickness
foot_drop  = 0;      // GUESS: foot bottom, below the bottom of the tongue. 0 = same level as the tongue.
                     // Make it bigger if the foot does not touch the table.
access_d   = 6;      // hole through the strut for the screwdriver (low screw)
lift       = 30;     // GUESS: low post height above the deck, along the arm (screen must clear the knobs)
tilt       = 45;     // screen leans back this many degrees from vertical (the user asked for 45)
arm_t      = 4;      // arm thickness
arm_x      = 3;      // front face of the arm at the deck, measured from the rear edge to the player

// ---- print ----
wall    = 3;         // wall thickness of the jaw
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
