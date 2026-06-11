// MyTides Enclosure — Landscape orientation
// Board: Elegoo ESP32-32E (88.9mm x 50.8mm)
// Battery: EEMB 2000mAh (34.5mm x 56mm x 10.6mm)
// Magnets: 4x 8mm dia x 3mm thick, one per corner
// Orientation: Landscape (USB-C on bottom edge)

// ── Parameters ────────────────────────────────────────────────────────────────

// Board dimensions
board_w        = 88.9;   // landscape width
board_h        = 50.8;   // landscape height
board_thickness = 1.6;

// Tall components on board back (connectors, chips)
board_back_clearance = 3.5;

// Battery
bat_w  = 56.0;
bat_h  = 34.5;
bat_d  = 10.6;

// Wall thickness
wall = 2.0;

// Enclosure outer dimensions
enc_w = board_w + wall * 2 + 2;   // 94.9mm
enc_h = board_h + wall * 2 + 2;   // 56.8mm

// Front shell (holds screen/board)
front_depth = board_thickness + board_back_clearance + wall;  // ~7.1mm
screen_opening_w = 73.0;   // visible LCD area width
screen_opening_h = 53.0;   // visible LCD area height — trimmed by bezel
bezel_w = (enc_w - screen_opening_w) / 2;
bezel_h_top = 6.0;
bezel_h_bot = enc_h - screen_opening_h - bezel_h_top;

// Back shell (holds battery)
back_depth = bat_d + 4.0 + wall;   // ~16.6mm → round to 17mm
back_inner_d = back_depth - wall;

// Magnet pockets
mag_dia    = 8.0;
mag_depth  = 3.2;   // slightly deeper than magnet so it seats fully
mag_margin = 4.0;   // distance from corner to magnet center

// USB-C cutout (bottom edge of landscape enclosure)
usbc_w     = 9.5;
usbc_h     = 3.5;
usbc_x_offset = 0;   // centered on board, which is centered in enclosure

// RESET button hole (left of USB-C on board bottom edge)
reset_x_offset = -22.0;   // approx from board center
reset_dia = 4.0;

// BOOT button hole (right of USB-C on board bottom edge)
boot_x_offset = 18.0;
boot_dia = 4.0;

// Snap fit clip dimensions
clip_w   = 8.0;
clip_d   = 1.2;
clip_h   = 4.0;
clip_gap = 0.25;   // clearance between front/back shells

// Board mounting standoffs
standoff_h   = board_back_clearance;
standoff_dia = 5.0;
standoff_hole = 2.5;

// Board mounting hole positions (approx from board center, landscape)
// CYD mounting holes are near the 4 corners
board_hole_x = 40.0;
board_hole_y = 21.0;

// ── Modules ───────────────────────────────────────────────────────────────────

module rounded_box(w, h, d, r = 3) {
    hull() {
        for (x = [-w/2 + r, w/2 - r])
        for (y = [-h/2 + r, h/2 - r])
            translate([x, y, 0]) cylinder(r = r, h = d, $fn = 32);
    }
}

module magnet_pocket() {
    cylinder(d = mag_dia + 0.3, h = mag_depth + 0.1, $fn = 32);
}

module standoff(h) {
    difference() {
        cylinder(d = standoff_dia, h = h, $fn = 24);
        cylinder(d = standoff_hole, h = h + 0.1, $fn = 24);
    }
}

// ── Front Shell ───────────────────────────────────────────────────────────────

module front_shell() {
    difference() {
        // Outer body
        rounded_box(enc_w, enc_h, front_depth);

        // Screen opening
        translate([0, (enc_h/2 - bezel_h_top - screen_opening_h/2), -0.1])
            cube([screen_opening_w, screen_opening_h, front_depth + 0.2], center = true);

        // Interior cavity for board
        translate([0, 0, wall])
            rounded_box(board_w + 0.4, board_h + 0.4, front_depth);

        // Snap clip receiver slots (sides)
        for (side = [-1, 1])
            translate([side * (enc_w/2 - wall/2), 0, front_depth - clip_h])
                cube([wall + 0.1, clip_w + clip_gap*2, clip_h + 0.1], center = true);
    }

    // Board mounting standoffs inside front shell
    for (x = [-board_hole_x/2, board_hole_x/2])
    for (y = [-board_hole_y/2, board_hole_y/2])
        translate([x, y, wall])
            standoff(standoff_h);
}

// ── Back Shell ────────────────────────────────────────────────────────────────

module back_shell() {
    difference() {
        // Outer body
        rounded_box(enc_w, enc_h, back_depth);

        // Interior cavity
        translate([0, 0, wall])
            rounded_box(board_w + 0.4, board_h + 0.4, back_inner_d + 0.1);

        // Battery recess centered in cavity
        translate([0, (board_h/2 - bat_h/2 - 2), wall + 0.5])
            cube([bat_w + 0.6, bat_h + 0.6, bat_d + 1.0], center = true);

        // Magnet pockets — 4 corners
        corner_x = enc_w/2 - mag_margin;
        corner_y = enc_h/2 - mag_margin;
        for (x = [-corner_x, corner_x])
        for (y = [-corner_y, corner_y])
            translate([x, y, back_depth - mag_depth])
                magnet_pocket();

        // USB-C cutout — bottom edge (landscape = short edge)
        translate([usbc_x_offset, -enc_h/2 - 0.1, back_depth/2 - back_depth/4])
            cube([usbc_w, wall + 0.3, usbc_h], center = true);

        // RESET hole
        translate([reset_x_offset, -enc_h/2 - 0.1, back_depth/2 - back_depth/4])
            rotate([-90, 0, 0])
                cylinder(d = reset_dia, h = wall + 0.3, $fn = 20);

        // BOOT hole
        translate([boot_x_offset, -enc_h/2 - 0.1, back_depth/2 - back_depth/4])
            rotate([-90, 0, 0])
                cylinder(d = boot_dia, h = wall + 0.3, $fn = 20);

        // Snap clip cutouts (sides)
        for (side = [-1, 1])
            translate([side * (enc_w/2 - wall/2), 0, clip_h/2 + 1])
                cube([wall + 0.1, clip_w, clip_h + 0.1], center = true);
    }

    // Snap clips
    for (side = [-1, 1])
        translate([side * (enc_w/2 - clip_d/2 - clip_gap), 0, clip_h/2 + 1])
            cube([clip_d, clip_w - clip_gap*2, clip_h], center = true);
}

// ── Wall Mount Plate ──────────────────────────────────────────────────────────

module wall_mount_plate() {
    plate_d = 4.0;
    difference() {
        rounded_box(enc_w, enc_h, plate_d);

        // Magnet pockets (mirror of back shell)
        corner_x = enc_w/2 - mag_margin;
        corner_y = enc_h/2 - mag_margin;
        for (x = [-corner_x, corner_x])
        for (y = [-corner_y, corner_y])
            translate([x, y, plate_d - mag_depth])
                magnet_pocket();

        // Wall screw holes
        for (x = [-enc_w/2 + 8, enc_w/2 - 8])
        for (y = [-enc_h/2 + 8, enc_h/2 - 8])
            translate([x, y, -0.1])
                cylinder(d = 4.5, h = plate_d + 0.2, $fn = 20);
    }
}

// ── Render ────────────────────────────────────────────────────────────────────
// Comment/uncomment the part you want to export

// Front shell
translate([0, 0, 0])
    front_shell();

// Back shell — offset for print plate preview
translate([enc_w + 10, 0, 0])
    back_shell();

// Wall mount plate
translate([-(enc_w + 10), 0, 0])
    wall_mount_plate();
