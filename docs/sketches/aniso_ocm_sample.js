function setup() {
  createCanvas(400, 400);
}

const TileW = 4, TileH = 4;
const S=40;

function lzcnt(x) {
    if (x == 0) return -1;
    let n = 0
    if ((x & 0xFFFF0000) == 0) { n += 16; x <<= 16; }
    if ((x & 0xFF000000) == 0) { n += 8; x <<= 8; }
    if ((x & 0xF0000000) == 0) { n += 4; x <<= 4; }
    if ((x & 0xC0000000) == 0) { n += 2; x <<= 2; }
    if ((x & 0x80000000) == 0) { n += 1; }
    return n;
}
function tzcnt(x) {
    if (x == 0) return -1;
    return 31 - lzcnt(x & -x);
}
function popcnt_4(x) {
  x -= (x >> 1) & 0x55;
  x = (x & 0x33) + ((x >> 2) & 0x33);
  return x;
}
function tzlz_mask_4(m, dir) {
    m |= (dir ? m >> 1 : m << 1);
    m |= (dir ? m >> 2 : m << 2);
    return ~m & 0xF;
}
function reduce_or_4(m, qm) {
  let r = 0;
  r |= (m >> 0 & qm) ? 1 : 0;
  r |= (m >> 4 & qm) ? 2 : 0;
  r |= (m >> 8 & qm) ? 4 : 0;
  r |= (m >> 12 & qm) ? 8 : 0;
  return r;
}
function reduce_or_16(m_0, m_32, t) {
  let r = 0;
  r |= (m_0 >> 0 & t) != 0 ? 1 : 0;
  r |= (m_0 >> 16 & t) != 0 ? 2 : 0;
  r |= (m_32 >> 0 & t) != 0 ? 4 : 0;
  r |= (m_32 >> 16 & t) != 0 ? 8 : 0;
  return r;
}

class Tile { 
  constructor() {
    this.mask_0 = 0;
    this.mask_32 = 0;
  }
  
  get(x, y, z) {
    let i = x+y*4+z*16;
    let m = i >= 32?this.mask_32:this.mask_0;
    return m >> i & 1;
  }
  set(x, y, z, v) {
    let i = x+y*4+z*16;
    if (i >= 32) this.mask_32 = (this.mask_32 & ~(1<<i)) | (!!v)<<i;
    else         this.mask_0 = (this.mask_0 & ~(1<<i)) | (!!v)<<i;
  }
  getCellBounds(x, y, z, dir) {
    let mask = z >= 2 ? this.mask_32 : this.mask_0;
    mask >>= (z&1)*16;

    if (dir.x < 0) {
      //let s = 3-(x & 3);
      //mask &= (0xF >> s) * 0x1111;
      mask &= (0x2222 << (x&3)) - 0x1111;
    } else {
      //mask &= (0xF << (x & 3) & 0xF) * 0x1111;
      let xm = 0x0F0F << (x & 3) & 0x0F0F;
      mask &= xm | (xm<<4);
    }
    if (dir.y > 0) {
      mask &= 0xFFFF << ((y & 3) * 4);
    } else {
      let s = (3-(y&3))*4;
      mask &= 0xFFFF >> s;
    }
    let xo = x & 3, yo = y & 3;

    let rx = mask >> (yo*4) & 0xF;
    let mx = tzlz_mask_4(rx, dir.x<0);

    let ry = reduce_or_4(mask, mx);
    let my = tzlz_mask_4(ry, dir.y<0);
    
    let rz = reduce_or_16(this.mask_0, this.mask_32, mask & (mx * 0x1111));

    text((mask & (mx * 0x1111)).toString(16).padStart(4,'0'), 2, 20);
    text(mx.toString(2).padStart(4,'0'), 2, 30);
    text(my.toString(2).padStart(4,'0'), 2, 40);
    
    let dx = popcnt_4(mx) - 1;
    let dy = popcnt_4(my) - 1;
    x = dir.x < 0 ? (x | 3) - dx : (x & ~3) + dx;
    y = dir.y < 0 ? (y | 3) - dy : (y & ~3) + dy;

    return { x, y };
  }
  
  getCellBounds_V2(x, y, z, dir) {
    let mask = z >= 2 ? this.mask_32 : this.mask_0;
    
    //dir.x=(Date.now()/1000)&1?-1:+1;
    dir.x=1;
    dir.y=1;
    
    let dx=dir.x<0?-1:+1;
    let runMaskX = mask >> (y*4 + (z&1)*16);
    let sx = 3*dx;
    if (((x + dx*3)&15) >= 4 || (runMaskX >> (x + dx*3) & 1) != 0) sx = 2*dx;
    if (((x + dx*2)&15) >= 4 || (runMaskX >> (x + dx*2) & 1) != 0) sx = 1*dx;
    if (((x + dx*1)&15) >= 4 || (runMaskX >> (x + dx*1) & 1) != 0) sx = 0*dx;

    let filterMaskX = (2<< max(x,x+sx))-(1<< min(x,x+sx));
    
    let dy=dir.y<0?-1:+1;
    let runMaskY = mask >> ((z&1)*16);
    let sy = 3*dy;
    if (((y + dy*3)&15) >= 4 || (runMaskY >> ((y + dy*3)*4) & filterMaskX) != 0) sy = 2*dy;
    if (((y + dy*2)&15) >= 4 || (runMaskY >> ((y + dy*2)*4) & filterMaskX) != 0) sy = 1*dy;
    if (((y + dy*1)&15) >= 4 || (runMaskY >> ((y + dy*1)*4) & filterMaskX) != 0) sy = 0*dy;
    
    let filterMaskZ = filterMaskX * 0x1111;
    
    filterMaskZ &= (0xFFFF<< min(y,y+sy)*4)&(0xFFFF>> (3-max(y,y+sy))*4);
    
    let dz=dir.z<0?-1:+1;
    let sz = 3*dz;
    if (((z + dz*3)&15) >= 4 || (this.shr64((z + dz*3)*16) & filterMaskZ) != 0) sz = 2*dz;
    if (((z + dz*2)&15) >= 4 || (this.shr64((z + dz*2)*16) & filterMaskZ) != 0) sz = 1*dz;
    if (((z + dz*1)&15) >= 4 || (this.shr64((z + dz*1)*16) & filterMaskZ) != 0) sz = 0*dz;

    text(filterMaskX.toString(2).padStart(4,'0') + " "+x + " "+sx+" "+(x+sx), 2, 10);
    text(filterMaskZ.toString(16).padStart(4,'0') + " "+y + " "+sy+" "+(y+sy), 2, 20);
    x += sx;
    y += sy;
    z += sz;

    return { x, y, z };
  }
  shr64(n) {
    let m = n<32?this.mask_0: this.mask_32;
    return m >> (n&31);
  }
}

let tile = new Tile();
let mouseWasPressed = false;

tile.mask_0|=1<<2;
tile.mask_0|=1<<13;

function draw() {
  background(220);
  
  let mouseWasClicked = mouseWasPressed && !mouseIsPressed;
  mouseWasPressed = mouseIsPressed;
  
  let xo = (width-S*8-16)/2;
  let yo = (height-S*8-16)/2;
  let hoverPos = null;
  
  for (let y = 0; y < 4; y++)
  for (let z = 0; z < 4; z++)
  for (let x = 0; x < 4; x++) {
    let x1 = xo + x*S + (z & 1) * (S*4+16);
    let y1 = yo + y*S + (z >> 1) * (S*4+16);

    if (mouseX >= x1 && mouseY >= y1 && mouseX < x1+S && mouseY < y1+S) {
      hoverPos = {x,y,z};
      if (mouseWasClicked) {
        tile.set(x,y,z,!tile.get(x,y,z));
      }
    }

    if(tile.get(x,y,z)) 
      fill(255,70,70);
    else
      fill(255,255,255);

    rect(x1, y1,S,S);


    let tx=(x+y*4 + z*16).toString();
    
    fill(0,0,0);
    text(tx, x1+S-textWidth(tx) - 4,y1+S-4);
  }
  
  if (hoverPos != null) {
    let bnd = tile.getCellBounds_V2(hoverPos.x,hoverPos.y, hoverPos.z, {x:-1,y:1,z:1});
    
    fill(0,0,255,128);
    
    let z1 = min(hoverPos.z,bnd.z);
    let z2 = max(hoverPos.z,bnd.z);
    
    for(let z=z1;z<=z2;z++) {
      let x1 = xo + min(hoverPos.x,bnd.x)*S + (z & 1) * (S*4+16);
      let y1 = yo + min(hoverPos.y,bnd.y)*S + (z >> 1) * (S*4+16);
      rect(x1, y1, abs(bnd.x-hoverPos.x)*S+S, abs(bnd.y-hoverPos.y)*S+S);
    }
    
    text(bnd.x+" "+bnd.y+" "+bnd.z, 2, 40);
  }
}