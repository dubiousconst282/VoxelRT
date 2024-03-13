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
    getCellSize(x, y) {
        let k = 1;
        for (; k < 8; k++) {
            if (this.get(x >> k, y >> k, k)) break;
        }
        return 1 << (k - 1);
    }
    getCellBounds(x, y, dir, ms = false) {
        let k = 0;
        for (; k < 8; k++) {
            if (this.get(x >> k, y >> k, k)) break;
        }
        k--;
        let mask = (1 << k) - 1;
        x = dir.x < 0 ? (x | mask) : (x & ~mask);
        y = dir.y < 0 ? (y | mask) : (y & ~mask);

        let dx = dir.x < 0 ? -1 : +1;
        let dy = dir.y < 0 ? -1 : +1;

        let cellSize = 1 << k;
        let kx = cellSize, ky = cellSize;

        // 1 2
        // 4 8
        let n = 15;
        for (let i = 1; i < 4; i++) {
            n ^= grid.get((x >> k) + dx * (i & 1), (y >> k) + dy * (i >> 1), k) << i;
        }

        if (Date.now() / 2000 & 1) {
            if (abs(dir.x) > abs(dir.y)) {
                kx += (n & 2) ? cellSize : 0;
                ky += (n & 4) && (!(n & 2) || (n & 8)) ? cellSize : 0;
            } else {
                ky += (n & 4) ? cellSize : 0;
                kx += (n & 2) && (!(n & 4) || (n & 8)) ? cellSize : 0;
            }
        }

        x += kx * dx;
        y += ky * dy;

        return { x, y };
    }
}
const CellSize = 32;
// let start = { x: 1.5, y: 1.5 }, end = { x: 6.9, y: 15.5 };
let start = { x: 13.5, y: 1.5 }, end = { x: 2.9, y: 11.5 };
let grid = new Grid(32, 32);

function setup() {
    createCanvas(512, 512);
    frameRate(30)

    for (let y = 0; y < grid.height; y++) {
        for (let x = 0; x < grid.width; x++) {

            let cx = width / CellSize / 2 - 6;
            let cy = height / CellSize / 2 + 4;

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
        let s = (1 << level) * CellSize;
        if (level == 0 && grid.get(x, y)) fill(128, 128, 128)
        else fill(255, 255, 255)
        rect(x * CellSize, y * CellSize, s, s);
    });

    let gx = mouseX / CellSize | 0;
    let gy = mouseY / CellSize | 0;
    let dir = { x: end.x - start.x, y: end.y - start.y };
    let fs = grid.getCellBounds(gx, gy, dir, true);

    if (dir.x < 0) { gx++; fs.x++; }
    if (dir.y < 0) { gy++; fs.y++; }

    /* fill(0, 255, 0, 80);
     rect(min(gx, fs.x) * CellSize,
         min(gy, fs.y) * CellSize,
         abs(fs.x - gx) * CellSize,
         abs(fs.y - gy) * CellSize);*/


    RayCast(start, end);

    stroke(0, 0, 0);
    line(start.x * CellSize, start.y * CellSize,
        end.x * CellSize, end.y * CellSize);

    fill(0, 255, 0);
    circle(start.x * CellSize, start.y * CellSize, 8);

    fill(255, 0, 0);
    circle(end.x * CellSize, end.y * CellSize, 8);
}

let activeDrag = -1;
function mousePressed() {
    let gx = mouseX / CellSize | 0;
    let gy = mouseY / CellSize | 0;
    activeDrag = dist(gx, gy, start.x, start.y) < dist(gx, gy, end.x, end.y) ? 0 : 1;

    if (keyIsDown(CONTROL)) {
        grid.set(gx, gy, 0, grid.get(gx, gy) ? 0 : 1);
        grid.buildMips();
    }
}
function mouseDragged() {
    let pt = activeDrag == 0 ? start : end;
    pt.x = mouseX / CellSize;
    pt.y = mouseY / CellSize;
}

function signbit(x) { return x < 0 ? 0 : 1; }
function RayCast(origin, target) {
    let dir = { x: target.x - origin.x, y: target.y - origin.y };
    let dirLen = sqrt(dir.x * dir.x + dir.y * dir.y);
    dir.x /= dirLen;
    dir.y /= dirLen;

    let invDx = (1.0 / dir.x)
    let invDy = (1.0 / dir.y)

    let tStartX = (signbit(dir.x) - origin.x - Math.sign(dir.x)) / dir.x;
    let tStartY = (signbit(dir.y) - origin.y - Math.sign(dir.y)) / dir.y;

    let voxelX = floor(origin.x), voxelY = floor(origin.y);

    for (let i = 0; i < 32; i++) {
        if (grid.get(voxelX, voxelY)) {
            fill(255, 32, 32, 255);
            rect(voxelX * CellSize, voxelY * CellSize, CellSize, CellSize);
            break;
        }

        let stepPos = grid.getCellBounds(voxelX, voxelY, dir);
        let tx = tStartX + stepPos.x * invDx;
        let ty = tStartY + stepPos.y * invDy;
        let tmin = min(tx, ty) + 0.001;

        let px = origin.x + tmin * dir.x;
        let py = origin.y + tmin * dir.y;

        fill(32, 128, 256, 128);
        //rect(voxelX * CellSize, voxelY * CellSize, CellSize, CellSize);
        text(i, voxelX * CellSize + 6, voxelY * CellSize + 14);
        //text((stepPos.x - voxelX) + " " + (stepPos.y - voxelY), voxelX * CellSize + 6, voxelY * CellSize + 28);


        if (dir.x < 0) { voxelX++; stepPos.x++; }
        if (dir.y < 0) { voxelY++; stepPos.y++; }

        fill(0, 255, 0, 40);
        rect(min(voxelX, stepPos.x) * CellSize + 2,
            min(voxelY, stepPos.y) * CellSize + 2,
            abs(stepPos.x - voxelX) * CellSize - 4,
            abs(stepPos.y - voxelY) * CellSize - 4,
            6);

        fill(255, 255, 255);
        circle(px * CellSize, py * CellSize, 6);

        voxelX = floor(px), voxelY = floor(py);
    }
}

