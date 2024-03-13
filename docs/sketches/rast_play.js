const PIXEL_SIZE = 8;

function setup() {
  createCanvas(512, 512);
  frameRate(20);

  for (let i = 0; i < 3; i++) {
    pts.push({ x: round(random() * width / PIXEL_SIZE), y: round(random() * height / PIXEL_SIZE) });
  }
  // JSON.stringify(pts)
  pts = [{ "x": 51, "y": 45 }, { "x": 11, "y": 54 }, { "x": 21, "y": 15 }];
}

pts = [];
dragPt = null;

function draw() {
  background(255);
  translate(-0.5, -0.5);

  // pixel grid
  strokeWeight(0.4)
  stroke(0, 0, 0);
  for (let x = 0; x < width; x += PIXEL_SIZE) {
    line(x, 0, x, height);
  }

  for (let y = 0; y < height; y += PIXEL_SIZE) {
    line(0, y, width, y);
  }

  rasterize(pts[0], pts[1], pts[2]);

  noFill();
  triangle(
    (pts[0].x + 0.5) * PIXEL_SIZE, (pts[0].y + 0.5) * PIXEL_SIZE,
    (pts[1].x + 0.5) * PIXEL_SIZE, (pts[1].y + 0.5) * PIXEL_SIZE,
    (pts[2].x + 0.5) * PIXEL_SIZE, (pts[2].y + 0.5) * PIXEL_SIZE);

  // vertices
  if (dragPt) {
    dragPt.x = round(mouseX / PIXEL_SIZE);
    dragPt.y = round(mouseY / PIXEL_SIZE);
  }

  for (let i = 0; i < 3; i++) {
    let px = (pts[i].x + 0.5) * PIXEL_SIZE;
    let py = (pts[i].y + 0.5) * PIXEL_SIZE;

    noFill();
    circle(px, py, 12);

    fill(0, 0, 0);
    text(i + "", px - 4, py + 5);
  }
}

function mousePressed() {
  for (let pt of pts) {
    let dx = mouseX - pt.x * PIXEL_SIZE, dy = mouseY - pt.y * PIXEL_SIZE;
    if (dx * dx + dy * dy < 32 * 32) {
      dragPt = pt;
      break;
    }
  }
}
function mouseReleased() {
  dragPt = null;
}

function rasterize(v0, v1, v2) {
  let x0 = round(v0.x), x1 = round(v1.x), x2 = round(v2.x);
  let y0 = round(v0.y), y1 = round(v1.y), y2 = round(v2.y);

  let minX = min(x0, min(x1, x2)), minY = min(y0, min(y1, y2));
  let maxX = max(x0, max(x1, x2)), maxY = max(y0, max(y1, y2));

  let A01 = y0 - y1, B01 = x1 - x0;
  let A12 = y1 - y2, B12 = x2 - x1;
  let A20 = y2 - y0, B20 = x0 - x2;

  const q = 4, qm = q - 1;

  minX = minX & ~qm, minY = minY & ~qm;
  maxX = (maxX + qm) & ~qm, maxY = (maxY + qm) & ~qm;

  let w0 = A12 * (minX - x1) + B12 * (minY - y1);
  let w1 = A20 * (minX - x2) + B20 * (minY - y2);
  let w2 = A01 * (minX - x0) + B01 * (minY - y0);

  if (floor(Date.now() / 1000) % 2) {
    w0 += (A12 > 0 || (A12 == 0 && B12 > 0)) ? 0 : -1;
    w1 += (A20 > 0 || (A20 == 0 && B20 > 0)) ? 0 : -1;
    w2 += (A01 > 0 || (A01 == 0 && B01 > 0)) ? 0 : -1;
  }
  let rc0 = (max(B12, 0) + max(A12, 0)) * -qm;
  let rc1 = (max(B20, 0) + max(A20, 0)) * -qm;
  let rc2 = (max(B01, 0) + max(A01, 0)) * -qm;
  let ac0 = (min(B12, 0) + min(A12, 0)) * -qm;
  let ac1 = (min(B20, 0) + min(A20, 0)) * -qm;
  let ac2 = (min(B01, 0) + min(A01, 0)) * -qm;

  A01 *= q, A12 *= q, A20 *= q;
  B01 *= q, B12 *= q, B20 *= q;

  for (let y = minY; y <= maxY; y += q) {
    let rw0 = w0, rw1 = w1, rw2 = w2;

    for (let x = minX; x <= maxX; x += q) {

      if (rw0 < rc0 || rw1 < rc1 || rw2 < rc2) {
        // Trivial reject
        fill(255, 80, 80);
        rect(x * PIXEL_SIZE, y * PIXEL_SIZE, PIXEL_SIZE * q, PIXEL_SIZE * q);
      }
      else if (rw0 >= ac0 && rw1 >= ac1 && rw2 >= ac2) {
        // Trivial accept
        fill(80, 255, 80);
        rect(x * PIXEL_SIZE, y * PIXEL_SIZE, PIXEL_SIZE * q, PIXEL_SIZE * q);
      }
      else {
        // Partial
        for (let sy = 0; sy < q; sy++) {
          for (let sx = 0; sx < q; sx++) {
            if (((rw0 + A12 / q * sx + B12 / q * sy) | (rw1 + A20 / q * sx + B20 / q * sy) | (rw2 + A01 / q * sx + B01 / q * sy)) >= 0) {
              fill(255, 255, 80);
            } else {
              fill(255, 80, 80);
            }
            rect((x + sx) * PIXEL_SIZE, (y + sy) * PIXEL_SIZE, PIXEL_SIZE, PIXEL_SIZE);
          }
        }
      }
      rw0 += A12, rw1 += A20, rw2 += A01;
    }
    w0 += B12, w1 += B20, w2 += B01;
  }
}