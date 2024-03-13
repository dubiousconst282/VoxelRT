const CellSize = 32;
let start = { x: 1.5, y: 1.5 }, end = { x: 5.5, y: 9.5 };

function setup() {
    createCanvas(512, 512);
}

function draw() {
    background(220);

    for (let y = 0; y < floor(height / CellSize); y++) {
        for (let x = 0; x < floor(width / CellSize); x++) {
            let s = sample(x, y);
            if (s < 0) fill(128, 128, 128);
            else fill(255, 255, 255);
            rect(x * CellSize, y * CellSize, CellSize, CellSize);
        }
    }
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
    activeDrag =
        dist(mouseX / CellSize, mouseY / CellSize, start.x, start.y) <
            dist(mouseX / CellSize, mouseY / CellSize, end.x, end.y)
            ? 0 : 1;
}
function mouseDragged() {
    let pt = activeDrag == 0 ? start : end;
    pt.x = mouseX / CellSize;
    pt.y = mouseY / CellSize;
}

function sign2i(x) { return x < 0 ? -1 : +1; }

function GetSideDist(x, sign) {
    x = fract(x);
    return sign < 0.0 ? x : 1.0 - x;
}

function RayCast(origin, target) {
    // ivec3 mapPos = ivec3(floor(origin));
    // vec3 deltaDist = abs(1.0 / dir);
    // vec3 rayStep = sign(dir);
    // vec3 sideDist = (rayStep * (floor(origin) - origin) + rayStep * 0.5 + 0.5) * deltaDist;

    let dir = { x: target.x - origin.x, y: target.y - origin.y };
    let dirLen = sqrt(dir.x * dir.x + dir.y * dir.y);
    dir.x /= dirLen;
    dir.y /= dirLen;

    let posX = floor(origin.x), stepX = sign2i(dir.x);
    let posY = floor(origin.y), stepY = sign2i(dir.y);

    let deltaX = abs(1.0 / dir.x), distX = GetSideDist(origin.x, dir.x) * deltaX;
    let deltaY = abs(1.0 / dir.y), distY = GetSideDist(origin.y, dir.y) * deltaY;

    fill(255, 255, 255);
    text(origin.x.toFixed(3) + " " + origin.y.toFixed(3) + " -> " + target.x.toFixed(3) + " " + target.y.toFixed(3), 80, 12);
    text("Dx: " + deltaX.toFixed(3) + " Sx: " + distX.toFixed(3), 2, 12);
    text("Dy: " + deltaY.toFixed(3) + " Sy: " + distY.toFixed(3), 2, 24);
    let lineY = 0;

    for (let i = 0; i < 32; i++) {
        let s = sample(posX, posY);

        fill(32, 128, 256);
        if (s < 0) fill(16, 64, 128);
        rect(posX * CellSize, posY * CellSize, CellSize, CellSize);

        if (s < 0) break;

        let hitDist = min(distX, distY);
        fill(255, 255, 255);
        circle((origin.x + dir.x * hitDist) * CellSize,
            (origin.y + dir.y * hitDist) * CellSize, 6);



        if (s >= 2) {
            let targetDist = (hitDist + s - 1);

            text((targetDist - hitDist).toFixed(2), posX * CellSize + 4, posY * CellSize + 16);

            let advX = origin.x + dir.x * targetDist;
            let advY = origin.y + dir.y * targetDist;

            let deltaDist = dist(advX, advY, origin.x, origin.y);
            distX = GetSideDist(advX, dir.x) * deltaX + deltaDist;
            distY = GetSideDist(advY, dir.y) * deltaY + deltaDist;

            posX = floor(advX);
            posY = floor(advY);
            continue;
        }


        if (distX < distY) {
            distX += deltaX;
            posX += stepX;
        } else {
            distY += deltaY;
            posY += stepY;
        }
    }
}

function sample(x, y) {
    let cx = width / CellSize / 2 - 6;
    let cy = height / CellSize / 2 + 4;
    let r = 2.5;

    let dx = x - cx, dy = y - cy;
    return floor(sqrt(dx * dx + dy * dy) - r);
}