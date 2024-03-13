const N = 16;
grid = new Array(N * N);
for (let i = 0; i < grid.length; i++) {
  grid[i] = { sx: 0, sy: 0, colored: 0 };
}

function setup() {
  createCanvas(512, 512);
  frameRate(10);

  jumpFlood();
}

function draw() {
  background(220);

  for (let y = 0; y < N; y++) {
    for (let x = 0; x < N; x++) {
      let sx = x * width / N;
      let sy = y * height / N;
      let g = grid[x + y * N];

      let d = dist(g.sx, g.sy, x, y);

      let gr = d / dist(N, N, 0, 0) * 200 + 55;

      fill(gr, gr, gr);
      rect(sx, sy, width / N, height / N);

      let tg = gr < 127 ? 255 : 0;
      fill(tg, tg, tg);
      text(floor(d).toString(), sx + 8, sy + 16);
    }
  }
}
function mousePressed() {
  let gx = min(max(mouseX / width * N | 0, 0), N - 1);
  let gy = min(max(mouseY / height * N | 0, 0), N - 1);
  let g = grid[gx + gy * N];


  for (let i = 0; i < grid.length; i++) {
    grid[i].colored = 0;
  }

  g.colored = 1;
  g.sx = gx;
  g.sy = gy;
  jumpFlood();
}

function jumpFlood() {
  for (let k = N / 2; k >= 1; k /= 2) {
    // Pixels
    for (let y = 0; y < N; y++) {
      for (let x = 0; x < N; x++) {
        let p = grid[x + y * N];
        // Neighbors
        for (let ny = -1; ny <= 1; ny++) {
          for (let nx = -1; nx <= 1; nx++) {
            let qx = x + nx * k, qy = y + ny * k;
            if ((qx | qy) < 0 || (qx | qy) >= N) continue;

            let q = grid[qx + qy * N];

            if (q.colored && (!p.colored || dist(p.sx, p.sy, x, y) > dist(q.sx, q.sy, x, y))) {
              p.sx = q.sx;
              p.sy = q.sy;
              p.colored = 1;
            }
          }
        }
      }
    }
  }
}