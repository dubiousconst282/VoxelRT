const CellSize = 64;
let start = { x: 1.5, y: 1.5 }, end = { x: 5.5, y: 9.5 };

function setup() {
    createCanvas(512, 512);
}

function draw() {
    background(220);

    strokeWeight(0.25);
    for (let y = 0; y < floor(height / CellSize); y++) {
        for (let x = 0; x < floor(width / CellSize); x++) {
            fill(255, 255, 255);
            rect(x * CellSize, y * CellSize, CellSize, CellSize);
        }
    }
    strokeWeight(1);
    RayCast(start, end);

    stroke(0, 0, 0);
    line(start.x * CellSize, start.y * CellSize,
        end.x * CellSize, end.y * CellSize);

    fill(0, 255, 0);
    circle(start.x * CellSize, start.y * CellSize, 10);

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

function GetSideDist(x, sign) {
    x = fract(x);
    return sign < 0.0 ? x : 1.0 - x;
}

function RayCast(origin, target) {
    let dir = { x: target.x - origin.x, y: target.y - origin.y };
    let dirLen = sqrt(dir.x * dir.x + dir.y * dir.y);
    dir.x /= dirLen;
    dir.y /= dirLen;

    let posX = floor(origin.x);
    let posY = floor(origin.y);

    let deltaX = abs(1.0 / dir.x), distX = GetSideDist(origin.x, dir.x) * deltaX;
    let deltaY = abs(1.0 / dir.y), distY = GetSideDist(origin.y, dir.y) * deltaY;

    let numSteps = Math.floor(Date.now() / 1500) % 11;

    for (let i = 0; i < numSteps; i++) {
        fill(32, 128, 255);
        rect(posX * CellSize, posY * CellSize, CellSize, CellSize);

        let hitDist = min(distX, distY);

        if (i != numSteps - 1) {
            fill(255, 255, 255);
            circle((origin.x + dir.x * hitDist) * CellSize,
                (origin.y + dir.y * hitDist) * CellSize, 8);

            if (distX < distY) {
                distX += deltaX;
                posX += dir.x < 0 ? -1 : +1;
            } else {
                distY += deltaY;
                posY += dir.y < 0 ? -1 : +1;
            }
        } else {
            textSize(15);
            textFont("Consolas")

            strokeWeight(5);
            stroke(250, 250, 250)
            fill(0, 0, 0);

            text("dx: " + distX.toFixed(2),
                (origin.x + dir.x * distX) * CellSize + 8,
                (origin.y + dir.y * distX) * CellSize + 3);

            text("dy: " + distY.toFixed(2),
                (origin.x + dir.x * distY) * CellSize + 8,
                (origin.y + dir.y * distY) * CellSize + 3);

            strokeWeight(2);
            textSize(12);

            let st = distX < distY ? "dx < dy" : "dy < dx";
            text(st,
                posX * CellSize + (CellSize - textWidth(st)) / 2,
                posY * CellSize + CellSize / 2 - 3);

            st = distX < distY ? "step X" : "step Y";
            text(st,
                posX * CellSize + (CellSize - textWidth(st)) / 2,
                posY * CellSize + CellSize / 2 - 3 + 12);

            strokeWeight(1);
            stroke(0, 0, 0);
            fill(255, 0, 0);
            circle((origin.x + dir.x * distX) * CellSize,
                (origin.y + dir.y * distX) * CellSize, 6);

            circle((origin.x + dir.x * distY) * CellSize,
                (origin.y + dir.y * distY) * CellSize, 6);
        }
    }
}