// Moto G Power 2025 (XT2515-1, "vegas") remote button-pusher cradle.
//
// Mechanism: one printed plunger per side key (Vol Up, Vol Down, Power).
// Each plunger slides in a closed guide channel perpendicular to the phone's
// right edge, so its tip moves straight in and out and never scrubs the frame.
// A 9 g micro servo (SG90 / MG90S) drives each plunger through a Scotch yoke:
// an M2 screw in the servo horn (radius horn_r) rides in a short slot in the
// plunger's rear plate. A few degrees of horn rotation move the plunger
// horn_r * sin(angle), roughly 0.23 mm per degree at horn_r = 13.
//
// Coordinate system (mm):
//   X = along the phone, 0 at the TOP edge (camera end), + toward USB-C end
//   Y = across the phone, 0 at the RIGHT edge (button side), phone in -Y
//   Z = up, 0 at the table. The phone lies SCREEN UP, back on the floor.
//
// MEASURED vs ESTIMATED (see README "Measure before printing"):
//   phone_l/w/t   : spec sheet (Motorola/GSMArena 167.2 x 76.8 x 8.7)
//   button order  : confirmed from GSMArena review photo (right edge, from the
//                   top: Vol Up, Vol Down, Power/fingerprint)
//   button X/Z,
//   key sizes,
//   camera bump   : ESTIMATED from a single side photo, about +/-5 mm in X.
//                   Print part="fit_gauge" first and fix these numbers.
//   servo numbers : typical SG90 values. Clones vary, so check them with calipers.
//
// Render: openscad -D 'part="cradle"' -o cradle.stl cradle.scad
//         openscad -D 'part="plungers"' -o plungers.stl cradle.scad
//         openscad -D 'part="fit_gauge"' -o fit_gauge.stl cradle.scad

/* [Render] */
part = "assembly"; // [assembly, cradle, plungers, plunger_volup, plunger_voldown, plunger_power, fit_gauge]

/* [Phone body: spec sheet, verify] */
phone_l = 167.2;    // top edge to bottom edge
phone_w = 76.8;     // left edge to right edge
phone_t = 8.7;      // thickness at the frame (excluding camera bump)

/* [Buttons: ESTIMATED, MEASURE with calipers] */
volup_x   = 44.0;   // centre of Vol Up half of the rocker, from TOP edge
voldown_x = 54.0;   // centre of Vol Down half of the rocker, from TOP edge
power_x   = 75.0;   // centre of Power/fingerprint key, from TOP edge
vol_half_len = 9.5; // length of one rocker half along X
power_len = 14.0;   // length of power key along X
key_h     = 2.7;    // key height across the frame (Z direction)
btn_z     = 4.4;    // key centre height above the BACK plane (phone screen-up on a table)
key_proud = 0.5;    // how far the key stands out from the frame

/* [Rear camera bump recess: ESTIMATED, generous] */
cam_x0 = 2;   cam_x1 = 58;    // from top edge
cam_y0 = 2;   cam_y1 = 42;    // from RIGHT edge (camera is behind the right/top corner when screen-up)
cam_depth = 3;

/* [Fit] */
clr       = 0.4;    // pocket clearance around phone
side_gap  = 1.2;    // phone right frame -> guide face (keys stand key_proud into this)
lip_h     = 5.0;    // height of the left/top retaining lips above the floor
wall      = 3.0;
rib_x     = [12, 140];  // locating ribs on the right side, away from keys

/* [Plungers] */
pl_w      = 7.0;    // plunger body width (X)
pl_h      = 5.0;    // plunger body height (Z)
pl_clr    = 0.25;   // sliding clearance per side in guide channel
nose_h    = 2.0;    // contact nose height (Z). Keep < key_h so it never lands on the frame
nose_len  = 3.0;
nose_w    = [5.0, 5.0, 7.0];   // nose width (X) for volup, voldown, power
guide_len = 14.0;
guide_roof = 2.0;
y_extra   = [0, 17, 0];  // extra plunger length so servo bodies don't collide (voldown sits one row back)

/* [Servo: SG90/MG90S typical, MEASURE] */
servo_l   = 22.8;   // body length (tab direction)
servo_w   = 12.4;   // body width
servo_tab_span = 32.5;     // tab tip to tab tip
servo_hole_spacing = 27.8; // mounting screw hole spacing
servo_shaft_from_end = 5.9; // output shaft centre from the near end of the body
servo_below_deck = 16.0;   // body bottom -> underside of mounting tabs
horn_above_deck  = 9.0;    // underside of tabs -> underside of horn arm
servo_case_above_deck = 6.8; // underside of tabs -> top of case (for clash checks)
servo_fit = 0.4;
servo_bottom_clear = 1.5;  // servo bottom stays this far above the table
horn_r    = 13.0;   // radius of the horn hole used for the M2 pin
servo_side = [-1, 1, 1];   // -1: servo on the phone-top side of its plunger

/* [Pin and yoke] */
pin_d     = 2.0;    // M2 screw through the horn
nut_gap   = 2.2;    // horn underside -> yoke top (room for the M2 nut under the horn)
yoke_t    = 3.0;
yoke_len  = 7.0;    // along Y
yoke_w    = 8.0;    // along X
slot_len_x = 3.6;   // slot length along X (absorbs horn_r*(1-cos a))

/* [Camera] */
camera_boss = true; // 1/4"-20 nut trap at the top end, for a mini ball head / gooseneck

$fn = 40;
eps = 0.01;

// ------------------------------------------------------------------ derived
names = ["volup", "voldown", "power"];
btn_x = [volup_x, voldown_x, power_x];

pl_top_rel     = btn_z + pl_h/2;           // plunger top, relative to floor
horn_bottom_rel = pl_top_rel + nut_gap;
deck_rel       = horn_bottom_rel - horn_above_deck;
floor_z        = max(6, servo_below_deck + servo_bottom_clear - deck_rel);
deck_z         = floor_z + deck_rel;

guide_y0 = side_gap;
guide_y1 = side_gap + guide_len;
function y_pin(i)    = guide_y1 + 3 + yoke_len/2 + y_extra[i];
function servo_x(i)  = btn_x[i] + servo_side[i]*horn_r;
function servo_cy(i) = y_pin(i) - servo_shaft_from_end + servo_l/2;

base_x0 = -wall;
base_x1 = phone_l + clr + wall;
base_y0 = -phone_w - clr - wall;
base_y1 = max([for (i=[0:2]) servo_cy(i) + servo_tab_span/2]) + wall;
boss_len = 24;

echo(str("floor_z (phone back) = ", floor_z, " mm, deck_z = ", deck_z, " mm"));
echo(str("footprint X ", base_x1 - base_x0 + (camera_boss ? boss_len : 0),
         " x Y ", base_y1 - base_y0, " mm"));
for (i=[0:2]) echo(str(names[i], ": button x=", btn_x[i], " servo shaft at x=",
                       servo_x(i), " y=", y_pin(i)));

// ------------------------------------------------------- clash checks
// Rectangles [x0, y0, x1, y1]
function r_overlap(a, b) = a[0] < b[2] && b[0] < a[2] && a[1] < b[3] && b[1] < a[3];
function servo_rect(i) = [servo_x(i) - servo_w/2 - servo_fit, servo_cy(i) - servo_tab_span/2,
                          servo_x(i) + servo_w/2 + servo_fit, servo_cy(i) + servo_tab_span/2];
function plunger_rect(i) = [btn_x[i] - max(pl_w, yoke_w)/2 - 0.5, guide_y1,
                            btn_x[i] + max(pl_w, yoke_w)/2 + 0.5, y_pin(i) + yoke_len/2 + 2];
for (i=[0:2]) for (j=[0:2]) if (i != j) {
    assert(!r_overlap(servo_rect(i), servo_rect(j)),
           str("servo ", names[i], " collides with servo ", names[j], ": change servo_side / y_extra"));
    assert(!r_overlap(servo_rect(i), plunger_rect(j)),
           str("servo ", names[i], " collides with plunger ", names[j], ": change servo_side / y_extra"));
}
assert(nose_h < key_h, "nose_h must be smaller than key_h");
assert(pl_top_rel - yoke_t > deck_rel + 0.5, "yoke would hit the servo deck");
assert(abs(voldown_x - volup_x) >= pl_w + 2*pl_clr + 1.2, "volume plungers too close; reduce pl_w");

// ------------------------------------------------------------------ parts
module rounded_box(p0, p1, r=1.5) {
    s = [p1[0]-p0[0], p1[1]-p0[1], p1[2]-p0[2]];
    translate(p0) hull() for (x=[r, s[0]-r]) for (y=[r, s[1]-r])
        translate([x, y, 0]) cylinder(r=r, h=s[2]);
}

module box(p0, p1) translate(p0) cube([p1[0]-p0[0], p1[1]-p0[1], p1[2]-p0[2]]);

module cradle() {
    difference() {
        union() {
            // phone platform + servo deck
            rounded_box([base_x0, base_y0, 0], [base_x1, guide_y0 + eps, floor_z]);
            rounded_box([base_x0, guide_y0 - eps, 0], [base_x1, base_y1, deck_z]);
            // top stop
            box([base_x0, base_y0, 0], [-clr, guide_y0, floor_z + lip_h]);
            // left lip
            box([base_x0, base_y0, 0], [base_x1, -phone_w - clr, floor_z + lip_h]);
            // bottom corner posts (USB-C, mic and 3.5 mm jack stay open)
            box([phone_l + clr, base_y0, 0], [base_x1, base_y0 + wall + 10, floor_z + lip_h]);
            box([phone_l + clr, -10, 0], [base_x1, guide_y0, floor_z + lip_h]);
            // right locating ribs
            for (rx = rib_x) box([rx - 3, clr, 0], [rx + 3, guide_y0 + 1, floor_z + lip_h]);
            // plunger guide blocks
            for (i=[0:2]) box([btn_x[i] - pl_w/2 - pl_clr - wall, guide_y0, 0],
                              [btn_x[i] + pl_w/2 + pl_clr + wall, guide_y1,
                               floor_z + pl_top_rel + pl_clr + guide_roof]);
            if (camera_boss)
                rounded_box([base_x0 - boss_len, -phone_w/2 - 14, 0], [base_x0 + 1, -phone_w/2 + 14, floor_z]);
        }
        // rear camera bump recess
        box([cam_x0, -cam_y1, floor_z - cam_depth], [cam_x1, -cam_y0, floor_z + 1]);
        // finger notch on the left side (lift the phone out from here)
        translate([phone_l*0.62, -phone_w - clr, floor_z + 3])
            rotate([0, 90, 0]) cylinder(r=9, h=34, center=true);
        box([phone_l*0.62 - 17, base_y0 - 1, floor_z], [phone_l*0.62 + 17, -phone_w - clr + eps, floor_z + lip_h + 1]);
        for (i=[0:2]) {
            // guide channel
            box([btn_x[i] - pl_w/2 - pl_clr, guide_y0 - 2, floor_z + btn_z - pl_h/2 - pl_clr],
                [btn_x[i] + pl_w/2 + pl_clr, guide_y1 + 1, floor_z + btn_z + pl_h/2 + pl_clr]);
            // trench under the yoke for the pin tip
            box([btn_x[i] - yoke_w/2 - 1.5, guide_y1 + 0.5, deck_z - 3],
                [btn_x[i] + yoke_w/2 + 1.5, y_pin(i) + yoke_len/2 + 2.5, deck_z + 1]);
            // servo pocket (through) + screw pilots + wire channel
            box([servo_x(i) - (servo_w + servo_fit)/2, servo_cy(i) - (servo_l + servo_fit)/2, -1],
                [servo_x(i) + (servo_w + servo_fit)/2, servo_cy(i) + (servo_l + servo_fit)/2, deck_z + 1]);
            for (s=[-1, 1]) translate([servo_x(i), servo_cy(i) + s*servo_hole_spacing/2, deck_z - 8])
                cylinder(d=1.7, h=9);
            box([servo_x(i) - 3, servo_cy(i), -1], [servo_x(i) + 3, base_y1 + 1, 6]);
        }
        if (camera_boss) translate([base_x0 - boss_len/2, -phone_w/2, 0]) {
            translate([0, 0, -1]) cylinder(d=6.7, h=floor_z + 2);                // 1/4"-20 clearance
            translate([0, 0, -eps]) cylinder(d=11.4/cos(30), h=6, $fn=6);      // 7/16" hex nut trap
        }
    }
}

// plunger i in world coordinates at neutral (horn pointing along X)
module plunger_local(i) {
    tip_y = side_gap;
    body_y0 = tip_y + nose_len;
    yp = y_pin(i);
    // nose with chamfered tip
    hull() {
        translate([-(nose_w[i] - 1)/2, tip_y, -(nose_h - 0.6)/2]) cube([nose_w[i] - 1, 0.5, nose_h - 0.6]);
        translate([-nose_w[i]/2, tip_y + 0.8, -nose_h/2]) cube([nose_w[i], nose_len - 0.8 + eps, nose_h]);
    }
    // body
    translate([-pl_w/2, body_y0, -pl_h/2]) cube([pl_w, yp - yoke_len/2 - body_y0 + eps, pl_h]);
    // yoke plate (top flush with body top so the part prints upside down, flat)
    difference() {
        translate([-yoke_w/2, yp - yoke_len/2, pl_h/2 - yoke_t]) cube([yoke_w, yoke_len, yoke_t]);
        hull() for (s=[-1, 1]) translate([s*(slot_len_x - pin_d)/2, yp, -pl_h])
            cylinder(d=pin_d + 0.35, h=2*pl_h);
    }
}

module plunger_world(i) translate([btn_x[i], 0, floor_z + btn_z]) plunger_local(i);

module plunger_print(i) rotate([180, 0, 0]) translate([0, -side_gap, -pl_h/2]) plunger_local(i);

module plungers_print() for (i=[0:2]) translate([i*14, 0, 0]) plunger_print(i);

// Hold against the phone's right side (phone screen-up on the table, gauge
// standing on its long edge). The windows must frame the keys.
module fit_gauge() {
    t = 1.6;
    difference() {
        cube([phone_l, phone_t + 4, t]);
        // rocker window
        translate([volup_x - vol_half_len/2, btn_z - key_h/2, -1])
            cube([voldown_x - volup_x + vol_half_len, key_h, t + 2]);
        // power window
        translate([power_x - power_len/2, btn_z - key_h/2, -1]) cube([power_len, key_h, t + 2]);
        // nose-position ticks on the top edge: 1 = volup, 2 = voldown, 3 = power
        for (i=[0:2]) for (k=[0:i]) translate([btn_x[i] - 0.4 + (k - i/2)*1.6, phone_t + 2.5, -1])
            cube([0.8, 2, t + 2]);
    }
}

// --------------------------------------------------------------- assembly
module servo_dummy(i) {
    color("SteelBlue", 0.8) {
        translate([servo_x(i) - servo_w/2, servo_cy(i) - servo_l/2, deck_z - servo_below_deck])
            cube([servo_w, servo_l, servo_below_deck + servo_case_above_deck]);
        translate([servo_x(i) - servo_w/2, servo_cy(i) - servo_tab_span/2, deck_z])
            cube([servo_w, servo_tab_span, 2.5]);
    }
    color("White") translate([servo_x(i), y_pin(i), floor_z + horn_bottom_rel]) {
        hull() { cylinder(d=6, h=1.5); translate([-servo_side[i]*horn_r, 0, 0]) cylinder(d=4, h=1.5); }
        translate([-servo_side[i]*horn_r, 0, -6]) cylinder(d=pin_d, h=9);
    }
}

if (part == "cradle") cradle();
else if (part == "plungers") plungers_print();
else if (part == "plunger_volup") plunger_print(0);
else if (part == "plunger_voldown") plunger_print(1);
else if (part == "plunger_power") plunger_print(2);
else if (part == "fit_gauge") fit_gauge();
else if (part == "none") { }  // for include <cradle.scad>
else {
    color("Gainsboro") cradle();
    for (i=[0:2]) { color("OrangeRed") plunger_world(i); servo_dummy(i); }
    %translate([0, -phone_w, floor_z]) cube([phone_l, phone_w, phone_t]);
}
