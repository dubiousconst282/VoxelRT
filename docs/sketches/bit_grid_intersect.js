function setup() {
  createCanvas(400, 400);
}

const TileW = 4, TileH = 4;
const S=64;

class Tile { 
  constructor() {
    this.data = 0;
  }
  
  get(x, y) {
    let i=x+y*4;
    return (this.data>>i)&1;
  }
  set(x, y,  v) {
    let i=x+y*4;
    this.data = (this.data & ~(1<<i)) | (!!v) << i;
  }
  // tzcnt and lzcnt
  // X row - 0..3 | 4..7 | ...
  boundsX() {
    let x1 = 0, x2 = 4;
    
    // tzcnt
    let m = this.data;
    if ((m & 0x3333) == 0) { x1 += 2; m >>= 2; }
    if ((m & 0x1111) == 0) { x1 += 1; }
    
    // lzcnt
    m = this.data;
    if ((m & 0xCCCC) == 0) { x2 -= 2; m <<= 2; }
    if ((m & 0x8888) == 0) { x2 -= 1; }
    
    return [x1,x2];
  }
  // Y row - 0|4|8|12, 1|5|9|12, ...
  boundsY() {
    let x1 = 0, x2 = 4;
    
    // tzcnt
    let m = this.data;
    if ((m & 0x00FF) == 0) { x1 += 2; m >>= 8; }
    if ((m & 0x000F) == 0) { x1 += 1; }
    
    // lzcnt
    m = this.data;
    if ((m & 0xFF00) == 0) { x2 -= 2; m <<= 8; }
    if ((m & 0xF000) == 0) { x2 -= 1; }
    
    return [x1,x2];
  }
  // possible to use something other than incremental traversal for intersecting over small bit grids??
  // - research kd-trees
  intersect(origin, dir) {
    let origMask = this.data;

    let boundX = this.boundsX();
    let boundY = this.boundsY();
    let tx1 = (boundX[0]-origin.x) / dir.x;
    let ty1 = (boundY[0]-origin.y) / dir.y;
    let tx2 = (boundX[1]-origin.x) / dir.x;
    let ty2 = (boundY[1]-origin.y) / dir.y;
    
    let temp = tx1;
    tx1 = min(temp, tx2);
    tx2 = max(temp, tx2);
    temp = ty1;
    ty1 = min(temp, ty2);
    ty2 = max(temp, ty2);
    
    let tmin = Math.max(tx1, ty1);
    let tmax = Math.min(tx2, ty2);

    let x1 = origin.x + dir.x * tmin;
    let y1 = origin.y + dir.y * tmin;
    let x2 = origin.x + dir.x * tmax;
    let y2 = origin.y + dir.y * tmax;
  
    
    let xo = (width-S*TileW)/2;
    let yo = (height-S*TileH)/2;
    circle(xo+x1*S, yo+y1*S, 5);
    circle(xo+x2*S, yo+y2*S, 5);

    let mask = 0xFFFF;
    if (y1 > 0) {
      let s = floor(y1)*4;
      mask &= 0xFFFF << s;
    }
    if (x1 > 0) {
      let s = floor(x1);
      mask &= (0xF << s & 0xF) * 0x1111;
    }
    if (x2 < 4) {
      let s = 3-floor(x2);
      mask &= (0xF >> s) * 0x1111;
    }
    if (y2 < 4) {
      let s = (3-floor(y2))*4;
      mask &= 0xFFFF >> s;
    }
    return mask;
  }
}

let tile = new Tile();
let start = { x: 1.5, y: 40.5 }, end = { x: 345.5, y: 349.5 };
let mouseWasPressed = false;
tile.set(0,0,1);
tile.set(3,3,1);
function draw() {
  background(220);
  
  let mouseWasClicked = mouseWasPressed && !mouseIsPressed;
  mouseWasPressed = mouseIsPressed;
  
  let xo = (width-S*TileW)/2;
  let yo = (height-S*TileH)/2;
  
  let origin = { x: (start.x - xo) / (S), 
                 y: (start.y - yo) / (S) };
  let dir = { x: end.x - start.x, y: end.y - start.y };
  let dirLen = sqrt(dir.x * dir.x + dir.y * dir.y);
  dir.x /= dirLen;
  dir.y /= dirLen;
  let tile2 = new Tile();
  tile2.data = tile.intersect(origin, dir);
  
  for (let y =0; y<TileH; y++) {
    for(let x=0;x< TileW; x++) {
      let x1 = xo + x*S;
      let y1 = yo + y*S;
      
      if (mouseWasClicked && mouseX >= x1 && mouseY >= y1 && mouseX < x1+S && mouseY < y1+S) {
        tile.set(x,y,!tile.get(x,y));
      }
      
      if(tile.get(x,y)) 
        fill(255,70,70);
      else
        fill(255,255,255);
      rect(x1, y1,S,S);
      
      if (tile2.get(x,y)) {
        fill(0,255,0, 128);
        rect(x1, y1,S,S);
      }
      
      fill(0,0,0);
      
      let tx=(x+y*TileW).toString();
      text(tx, x1+S-textWidth(tx) - 4,y1+S-4);
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