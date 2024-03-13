
function lzcnt(x) {
    if (x == 0) return -1;
    let n = 0
    if ((x & 0xFFFF0000) == 0) { n += 16; x <<= 16; }
    if ((x & 0xFF000000) == 0) { n += 8; x <<= 8; }
    if ((x & 0xF0000000) == 0) { n += 4; x <<= 4; }
    if ((x & 0xC0000000) == 0) { n += 2; x <<= 2; }
    if ((x & 0x80000000) == 0) { n += 1; }
    return n;
}
function tzcnt(x) {
    if (x == 0) return -1;
    return 31 - lzcnt(x & -x);
}

function popcnt_4(x) {
    x -= (x >> 1) & 0x55;
    x = (x & 0x33) + ((x >> 2) & 0x33);
    return x;
}
function tzlz_mask_4(m, dir) {
    m |= (dir ? m >> 1 : m << 1);
    m |= (dir ? m >> 2 : m << 2);
    return ~m & 0xF;
}
function reduce_or_4(m) {
    let r = 0;
    r |= (m >> 0 & 15) ? 1 : 0;
    r |= (m >> 4 & 15) ? 2 : 0;
    r |= (m >> 8 & 15) ? 4 : 0;
    r |= (m >> 12 & 15) ? 8 : 0;
    return r;
}

class Grid {
    constructor(width, height) {
        this.data = new Uint8Array(width * height);
        this.width = width;
        this.height = height;
        this.numLevels = Math.floor(Math.log2(Math.min(width, height)));
    }

    get(x, y, k = 0) {
        if (x < 0 || y < 0 || x >= this.width || y >= this.height) return 0;
        return (this.data[x + y * this.width] >> k) & 1;
    }
    set(x, y, k, value) {
        if (x < 0 || y < 0 || x >= this.width || y >= this.height) return;

        let curr = this.data[x + y * this.width];
        this.data[x + y * this.width] = (curr & ~(1 << k)) | (!!value) << k;
    }

    buildMips() {
        for (let k = 0; k < this.numLevels; k++) {
            for (let y = 0; y < (this.height >> k); y += 2) {
                for (let x = 0; x < (this.width >> k); x += 2) {
                    let s00 = this.get(x + 0, y + 0, k);
                    let s10 = this.get(x + 1, y + 0, k);
                    let s01 = this.get(x + 0, y + 1, k);
                    let s11 = this.get(x + 1, y + 1, k);
                    this.set(x >> 1, y >> 1, k + 1, s00 | s10 | s01 | s11);
                }
            }
        }
    }
    traverse(visitor, x = 0, y = 0, k = undefined) {
        k ??= this.numLevels - 1;
        visitor(x, y, k + 1);

        if (k < 0 || !this.get(x >> (k + 1), y >> (k + 1), k + 1)) return;

        for (let i = 0; i < 4; i++) {
            let cx = x + ((i & 1) << k);
            let cy = y + ((i >> 1) << k);

            this.traverse(visitor, cx, cy, k - 1);
        }
    }
    getS(x, y) {
        let k = 1;
        for (; k < 8; k++) {
            if (this.get(x >> k, y >> k, k)) break;
        }
        return 1 << (k - 1);
    }
    getCellBounds(x, y, dir, ms = false) {
        /*let k = 0;
        for (; k < 8; k++) {
            if (this.get(x >> k, y >> k, k)) break;
        }
        k--;
        let mask = (1 << k) - 1;
        x = dir.x < 0 ? (x & ~mask) : (x | mask);
        y = dir.y < 0 ? (y & ~mask) : (y | mask);*/

        let mask = this.getLevelMask(x & ~3, y & ~3, 0);

        if (dir.x < 0) {
            //let s = 3-(x & 3);
            //mask &= (0xF >> s) * 0x1111;
            mask &= (0x2222 << (x & 3)) - 0x1111;
        } else {
            //mask &= (0xF << (x & 3) & 0xF) * 0x1111;
            let xm = 0x0F0F << (x & 3) & 0x0F0F;
            mask &= xm | (xm << 4);
        }
        if (dir.y > 0) {
            mask &= 0xFFFF << ((y & 3) * 4);
        } else {
            let s = (3 - (y & 3)) * 4;
            mask &= 0xFFFF >> s;
        }
        let xo = x & 3, yo = y & 3;

        let rx = mask >> (yo * 4) & 0xF;
        let mx = tzlz_mask_4(rx, dir.x < 0);

        let ry = reduce_or_4(mask & (mx * 0x1111));
        let my = tzlz_mask_4(ry, dir.y < 0);

        let dx = popcnt_4(mx) - 1;
        let dy = popcnt_4(my) - 1;
        x = dir.x < 0 ? (x | 3) - dx : (x & ~3) + dx;
        y = dir.y < 0 ? (y | 3) - dy : (y & ~3) + dy;

        return { x, y };
    }

    getCellBounds_V2(x, y, z, dir) {
        let bx = x, by = y;
        let mask = this.getLevelMask(x & ~3, y & ~3, 0);
        x &= 3; y &= 3;

        let idx = x + y * 4 + (z & 1) * 16;


        let dx = dir.x < 0 ? -1 : +1;
        let runMaskX = mask >> (idx & ~3);
        let sx = 3 * dx;
        if (((x + dx * 3) & 15) >= 4 || (runMaskX >> (x + dx * 3) & 1) != 0) sx = 2 * dx;
        if (((x + dx * 2) & 15) >= 4 || (runMaskX >> (x + dx * 2) & 1) != 0) sx = 1 * dx;
        if (((x + dx * 1) & 15) >= 4 || (runMaskX >> (x + dx * 1) & 1) != 0) sx = 0 * dx;

        let filterMaskX = (2 << max(x, x + sx)) - (1 << min(x, x + sx));

        let dy = dir.y < 0 ? -1 : +1;
        let runMaskY = mask;
        let sy = 3 * dy;
        if (((y + dy * 3) & 15) >= 4 || (runMaskY >> ((y + dy * 3) * 4) & filterMaskX) != 0) sy = 2 * dy;
        if (((y + dy * 2) & 15) >= 4 || (runMaskY >> ((y + dy * 2) * 4) & filterMaskX) != 0) sy = 1 * dy;
        if (((y + dy * 1) & 15) >= 4 || (runMaskY >> ((y + dy * 1) * 4) & filterMaskX) != 0) sy = 0 * dy;


        x = bx + sx;
        y = by + sy;

        return { x, y, z };
    }

    getLevelMask(x, y, k) {
        let m = 0;
        for (let i = 0; i < 16; i++) {
            m |= this.get(x + (i & 3), y + (i >> 2), k) << i;
        }
        return m;
    }
    // X row - 0..3 | 4..7 | ...
    calcStepX(mask) {
        if (mask == 0) return 4;
        // tzcnt
        let n = 0;
        if ((mask & 0x3333) == 0) { n += 2; mask >>= 2; }
        if ((mask & 0x1111) == 0) { n += 1; }
        return n;
    }
    calcStepY(mask) {
        if (mask == 0) return 4;
        // tzcnt
        let n = 0;
        if ((mask & 0x00FF) == 0) { n += 2; mask >>= 8; }
        if ((mask & 0x000F) == 0) { n += 1; }
        return n;
    }
}
const S = 32;
let start = { x: 1.5, y: 1.5 }, end = { x: 13.9, y: 15.5 };
//let start = { x: 13.5, y: 1.5 }, end = { x: 2.9, y: 11.5 };
let grid = new Grid(32, 32);

function setup() {
    createCanvas(512, 512);
    frameRate(30)

    for (let y = 0; y < grid.height; y++) {
        for (let x = 0; x < grid.width; x++) {

            let cx = width / S / 2 + 3;
            let cy = height / S / 2 + 4;

            let dx = x - cx, dy = y - cy;
            if (sqrt(dx * dx + dy * dy) < 2.5) {
                grid.set(x, y, 0, 1);
            }
        }
    }
    grid.buildMips();
}

function draw() {
    background(220);

    grid.traverse((x, y, level) => {
        let s = (1 << level) * S;
        if (level == 0 && grid.get(x, y)) fill(128, 128, 128)
        else fill(255, 255, 255)
        rect(x * S, y * S, s, s);
    });

    RayCast(start, end);

    let gx = mouseX / S | 0;
    let gy = mouseY / S | 0;
    let dir = { x: end.x - start.x, y: end.y - start.y };
    let fs = grid.getCellBounds_V2(gx, gy, 0, dir);

    fill(0, 255, 0, 80);
    rect(min(gx, fs.x) * S,
        min(gy, fs.y) * S,
        (abs(fs.x - gx) + 1) * S,
        (abs(fs.y - gy) + 1) * S);

    circle((fs.x + 0.5) * S, (fs.y + 0.5) * S, 9)

    stroke(0, 0, 0);
    line(start.x * S, start.y * S,
        end.x * S, end.y * S);

    fill(0, 255, 0);
    circle(start.x * S, start.y * S, 8);

    fill(255, 0, 0);
    circle(end.x * S, end.y * S, 8);
}

let activeDrag = -1;
function mousePressed() {
    let gx = mouseX / S | 0;
    let gy = mouseY / S | 0;
    activeDrag = dist(gx, gy, start.x, start.y) < dist(gx, gy, end.x, end.y) ? 0 : 1;

    if (keyIsDown(CONTROL)) {
        grid.set(gx, gy, 0, grid.get(gx, gy) ? 0 : 1);
        grid.buildMips();
    }
}
function mouseDragged() {
    let pt = activeDrag == 0 ? start : end;
    pt.x = mouseX / S;
    pt.y = mouseY / S;
}

function signbit(x) { return x < 0 ? 0 : 1; }
function RayCast(origin, target) {
    let dir = { x: target.x - origin.x, y: target.y - origin.y };
    let dirLen = sqrt(dir.x * dir.x + dir.y * dir.y);
    dir.x /= dirLen;
    dir.y /= dirLen;

    let invDx = (1.0 / dir.x)
    let invDy = (1.0 / dir.y)

    let tStartX = (signbit(dir.x) - origin.x) / dir.x;
    let tStartY = (signbit(dir.y) - origin.y) / dir.y;

    let voxelX = floor(origin.x), voxelY = floor(origin.y);

    for (let i = 0; i < 32; i++) {
        if (grid.get(voxelX, voxelY)) {
            fill(255, 32, 32, 255);
            rect(voxelX * S, voxelY * S, S, S);
            break;
        }

        let stepPos = grid.getCellBounds_V2(voxelX, voxelY, 0, dir);
        //let stepPos = { x: voxelX, y: voxelY };
        let tx = tStartX + stepPos.x * invDx;
        let ty = tStartY + stepPos.y * invDy;
        let tmin = min(tx, ty) + 0.001;

        let px = origin.x + tmin * dir.x;
        let py = origin.y + tmin * dir.y;

        fill(32, 128, 256, 128);
        //rect(voxelX * S, voxelY * S, S, S);
        text(i, voxelX * S + 6, voxelY * S + 14);
        //text((stepPos.x - voxelX) + " " + (stepPos.y - voxelY), voxelX * S + 6, voxelY * S + 28);

        fill(0, 255, 0, 40);
        rect(min(voxelX, stepPos.x) * S + 2,
            min(voxelY, stepPos.y) * S + 2,
            (abs(stepPos.x - voxelX) + 1) * S - 4,
            (abs(stepPos.y - voxelY) + 1) * S - 4,
            6);

        fill(255, 255, 255);
        circle(px * S, py * S, 6);

        voxelX = floor(px), voxelY = floor(py);
    }
}

