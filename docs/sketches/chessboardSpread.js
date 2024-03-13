// "Fast Ray-tracing of Rectilinear Volume Data Using Distance Transforms"

const N = 16;
grid = new Array(N * N);
for (let i = 0; i < grid.length; i++) {
  grid[i] = 15;
}

function setup() {
  createCanvas(512, 512);
  frameRate(10);

  spread();
}

function draw() {
  background(220);

  for (let y = 0; y < N; y++) {
    for (let x = 0; x < N; x++) {
      let sx = x * width / N;
      let sy = y * height / N;
      let d = grid[x + y * N];

      let gr = d / 15 * 200 + 55;


      fill(gr, gr, gr);
      rect(sx, sy, width / N, height / N);

      let tg = gr < 127 ? 255 : 0;
      fill(tg, tg, tg);
      text(d.toFixed(0), sx + 8, sy + 16);
    }
  }
}
function mousePressed() {
  let gx = min(max(mouseX / width * N | 0, 0), N - 1);
  let gy = min(max(mouseY / height * N | 0, 0), N - 1);
  grid[gx + gy * N] = 0;

  spread();
}

function spread() {
  spreadForward();
  spreadBackward();
}

function spreadForward() {
  for (let y = 0; y < N; y++) {
    for (let x = 0; x < N; x++) {
      let r = grid[x+y*N];
      for (let sy=-1;sy<=0;sy++)
      for (let sx=-1;sx<=1;sx++) {
        if((x+sx)<0||(x+sx)>=N)continue;
        if((y+sy)<0||(y+sy)>=N)continue;
        if(sx==0&&sy==0)continue;
        r=min(r, grid[(x+sx)+(y+sy)*N] + 1);
      }
      grid[x+y*N] = r;
    }
  }
}
function spreadBackward() {
  for (let y = N-1; y >= 0; y--) {
    for (let x = N-1; x >= 0; x--) {
      let r = grid[x+y*N];
      for (let sy=-1;sy<=1;sy++)
      for (let sx=-1;sx<=1;sx++) {
        if((x+sx)<0||(x+sx)>=N)continue;
        if((y+sy)<0||(y+sy)>=N)continue;
        if(sx==0&&sy==0)continue;
        r=min(r, grid[(x+sx)+(y+sy)*N] + 1);
      }
      grid[x+y*N] = r;
    }
  }
}