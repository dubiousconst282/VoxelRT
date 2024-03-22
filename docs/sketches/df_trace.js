class Grid {
    constructor(width, height) {
        this.data = new Uint8Array(width * height);
        this.df = new Uint32Array(width * height);
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
    getDist(x, y, axis) {
        let idx = x + y * this.width;
        let shift = axis * 5;
        return (this.df[idx] >> shift) & 31;
    }
    setDist(x, y, axis, value) {
        value = Math.min(value, 31);

        let idx = x + y * this.width;
        let shift = axis * 5;
        this.df[idx] = (this.df[idx] & ~(31 << shift)) | (value << shift);
    }

    updateDist() {
        // Splat occupancy
        for (let y = 0; y < this.height; y++) {
            for (let x = 0; x < this.width; x++) {
                let v = this.get(x, y) ? 0 : (31 * 0x2108421);
                this.df[x + y * this.width] = v;
            }
        }

        // Bi-directional X
        for (let y = 0; y < this.height; y++) {
            for (let x = 1; x < this.width; x++) {
                let d = this.getDist(x, y, 0);
                d = min(d, this.getDist(x - 1, y, 0) + 1);
                this.setDist(x, y, 0, d);
            }
            for (let x = this.width - 2; x >= 0; x--) {
                let d = this.getDist(x, y, 0);
                d = min(d, this.getDist(x + 1, y, 0) + 1);
                this.setDist(x, y, 0, d);
            }
        }

        // Bi-directional Y
        for (let x = 0; x < this.width; x++) {
            for (let y = 1; y < this.height; y++) {
                let d = this.getDist(x, y, 0);
                d = min(d, this.getDist(x, y - 1, 0) + 1);
                this.setDist(x, y, 0, d);
            }
            for (let y = this.height - 2; y >= 0; y--) {
                let d = this.getDist(x, y, 0);
                d = min(d, this.getDist(x, y + 1, 0) + 1);
                this.setDist(x, y, 0, d);
            }
        }
    }

    getCellBounds(x, y, dir, ms = false) {
        let d = this.getDist(x, y, 0);
        let Cx = dir.x / (abs(dir.x) + abs(dir.y));
        let Cy = dir.y / (abs(dir.x) + abs(dir.y));
        if (d > 0) {
            x += ceil(d * Cx - max(0, Math.sign(dir.x)));
            y += ceil(d * Cy - max(0, Math.sign(dir.y)));
            //x += floor(d * Cx + max(0, -Math.sign(dir.x)));
            //y += floor(d * Cy + max(0, -Math.sign(dir.y)));
        }
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
    grid.updateDist();
}

function draw() {
    background(220);

    for (let y = 0; y < grid.height; y++) {
        for (let x = 0; x < grid.width; x++) {
            let d = grid.getDist(x, y, 0);
            let g = 64 + d / 15 * (255 - 64);

            if (d == 0) fill(80, 80, 200)
            else {
                fill(g, g, g);
            }

            rect(x * CellSize, y * CellSize, CellSize, CellSize);

            if (d > 0) {
                fill(255, 255, 255);
                text(d, x * CellSize + (CellSize - textWidth(d)) / 2, y * CellSize + 20);
            }
        }
    }

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
        grid.updateDist();
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

    let tStartX = (signbit(dir.x) - origin.x) / dir.x;
    let tStartY = (signbit(dir.y) - origin.y) / dir.y;

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
        //text(i, voxelX * CellSize + 6, voxelY * CellSize + 14);
        //text((stepPos.x - voxelX) + " " + (stepPos.y - voxelY), voxelX * CellSize + 6, voxelY * CellSize + 28);

        fill(0, 255, 0, 40);
        rect(min(voxelX, stepPos.x) * CellSize + 2,
            min(voxelY, stepPos.y) * CellSize + 2,
            (abs(stepPos.x - voxelX) + 1) * CellSize - 4,
            (abs(stepPos.y - voxelY) + 1) * CellSize - 4,
            6);

        fill(255, 255, 255);
        circle(px * CellSize, py * CellSize, 6);

        voxelX = floor(px), voxelY = floor(py);
    }
}

