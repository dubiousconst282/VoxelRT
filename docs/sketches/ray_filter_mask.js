function setup() {
  createCanvas(400, 400);
}

const TileW = 4, TileH = 4;
const S = 80;

function GetSideDist(x, sign) {
  x = fract(x);
  return sign < 0.0 ? x : 1.0 - x;
}

function generateRayMaskDDA(origin, dir) {
  let mask = 0;

  let deltaX = abs(1.0 / dir.x);
  let deltaY = abs(1.0 / dir.y);
  let sideDistX = GetSideDist(origin.x, dir.x) * deltaX;
  let sideDistY = GetSideDist(origin.y, dir.y) * deltaY;
  let vx = floor(origin.x), vy = floor(origin.y);

  for (let i = 0; i < 8; i++) {
    if (((vx | vy) & 15) >= 4) break;
    mask |= 1 << (vx + vy * 4);

    if (sideDistX < sideDistY) {
      sideDistX += deltaX;
      vx += Math.sign(dir.x);
    } else {
      sideDistY += deltaY;
      vy += Math.sign(dir.y);
    }
  }
  return mask;
}
function getFilterMask(origin, dir) {
  let mask = 0;
  for (let ji = 0; ji < 16; ji++) {
    let jx = fract(ji * 0.75487766624669276005 + 0.5);
    let jy = fract(ji * 0.56984029099805326591 + 0.5);

    mask |= generateRayMaskDDA({ x: floor(origin.x) + jx, y: floor(origin.y) + jy }, dir);
  }
  return mask;
}
let start = { x: 60.5, y: 70.5 }, end = { x: 345.5, y: 349.5 };

function draw() {
  background(220);

  let xo = (width - S * TileW) / 2;
  let yo = (height - S * TileH) / 2;

  let origin = {
    x: (start.x - xo) / (S),
    y: (start.y - yo) / (S)
  };
  let dir = { x: end.x - start.x, y: end.y - start.y };
  let dirLen = sqrt(dir.x * dir.x + dir.y * dir.y);
  dir.x /= dirLen;
  dir.y /= dirLen;

  let mask = getFilterMask(origin, dir);

  for (let y = 0; y < TileH; y++) {
    for (let x = 0; x < TileW; x++) {
      let x1 = xo + x * S;
      let y1 = yo + y * S;

      fill(255, 255, 255);
      rect(x1, y1, S, S);

      if (mask >> (x + y * 4) & 1) {
        fill(0, 255, 0, 128);
        rect(x1, y1, S, S);
      }

      fill(0, 0, 0);

      let tx = (x + y * TileW).toString();
      text(tx, x1 + S - textWidth(tx) - 4, y1 + S - 4);
    }
  }

  line(start.x, start.y, end.x, end.y);

  fill(0, 255, 0);
  circle(start.x, start.y, 4);

  fill(255, 0, 0);
  circle(end.x, end.y, 4);
}

let activeDrag = -1;
function mousePressed() {
  activeDrag =
    dist(mouseX, mouseY, start.x, start.y) <
      dist(mouseX, mouseY, end.x, end.y) ? 0 : 1;
}
function mouseDragged() {
  let pt = activeDrag == 0 ? start : end;
  pt.x = mouseX;
  pt.y = mouseY;
}