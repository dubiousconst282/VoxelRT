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
}
const CellSize = 32;
let start = { x: 1.5, y: 1.5 }, end = { x: 5.5, y: 9.5 };
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
    let fs = grid.getCellSize(gx, gy);

    gx &= ~(fs - 1);
    gy &= ~(fs - 1);
    fill(255, 0, 0, 80);
    rect(gx * CellSize, gy * CellSize, fs * CellSize, fs * CellSize);

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

function GetSideDist(x, sign) {
    x = fract(x);
    return sign < 0.0 ? x : 1.0 - x;
}

function RayCast(origin, target) {
    let dir = { x: target.x - origin.x, y: target.y - origin.y };
    let dirLen = sqrt(dir.x * dir.x + dir.y * dir.y);
    dir.x /= dirLen;
    dir.y /= dirLen;

    let deltaX = abs(1.0 / dir.x);
    let deltaY = abs(1.0 / dir.y);

    fill(255, 255, 255);
    text(origin.x.toFixed(3) + " " + origin.y.toFixed(3) + " -> " + dir.x.toFixed(3) + " " + dir.y.toFixed(3), 180, 12);

    let t = 0;

    for (let i = 0; i < 32; i++) {
        let p = { x: origin.x + dir.x * t, y: origin.y + dir.y * t };
        let posX = floor(p.x), posY = floor(p.y);
        let s = grid.get(posX, posY);

        fill(32, 128, 256, 128);
        if (s > 0) fill(16, 64, 128, 128);
        rect(posX * CellSize, posY * CellSize, CellSize, CellSize);
        text(i, posX * CellSize + 4, posY * CellSize + 18);


        if (s > 0) {
            fill(255, 255, 255);
            circle((origin.x + dir.x * t) * CellSize,
                (origin.y + dir.y * t) * CellSize, 6);

            break;
        }
        let k = grid.getCellSize(posX, posY);
        let dx = GetSideDist(p.x / k, dir.x) * deltaX * k;
        let dy = GetSideDist(p.y / k, dir.y) * deltaY * k;
        t += min(dx, dy) + 0.0001;

        if (s > 1.5) t += s - 0.5;
    }
}
