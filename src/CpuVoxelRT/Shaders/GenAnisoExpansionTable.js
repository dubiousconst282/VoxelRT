function at(mask, x, y, z) {
    // 0 1   8 16   x+
    // 2 4  32 64   y+
    //        z+
    let i = x | (y << 1) | (z << 2);
    return (mask >> (i - 1)) & 1;
}
function expandX(mask, size) {
    for (let y = 0; y <= size.y; y++) {
        for (let z = 0; z <= size.z; z++) {
            if (at(mask, size.x + 1, y, z)) return;
        }
    }
    size.x++;
}
function expandY(mask, size) {
    for (let x = 0; x <= size.x; x++) {
        for (let z = 0; z <= size.z; z++) {
            if (at(mask, x, size.y + 1, z)) return;
        }
    }
    size.y++;
}
function expandZ(mask, size) {
    for (let x = 0; x <= size.x; x++) {
        for (let y = 0; y <= size.y; y++) {
            if (at(mask, x, y, size.z + 1)) return;
        }
    }
    size.z++;
}
function sortperm3(v) {
    let C = (a, b) => {
        if (v[a] > v[b]) {
            let tmp = v[a];
            v[a] = v[b];
            v[b] = tmp;
            return 1;
        }
        return 0;
    };
    let m = 0;
    m |= C(0, 2) << 0;
    m |= C(0, 1) << 1;
    m |= C(1, 2) << 2;
    return m;
}

let axisPerms = [[0, 1, 2], [0, 2, 1], [1, 0, 2], [1, 2, 0], [2, 0, 1], [2, 1, 0]];
axisPerms.sort((a, b) => sortperm3([...a]) - sortperm3([...b]));

let table = new Uint32Array(Math.ceil((6 * 128 * 4) / 32));

for (let p = 0; p < 6; p++) {
    for (let mask = 0; mask < 128; mask++) {
        let size = { x: 0, y: 0, z: 0 };

        for (let axis of axisPerms[p]) {
            if (axis == 0) expandX(mask, size);
            if (axis == 1) expandY(mask, size);
            if (axis == 2) expandZ(mask, size);
        }
        // console.log("%d %d %o", p, mask, size);
        let expandMask = size.x | (size.y << 1) | (size.z << 2);
        let entryIndex = (mask + p * 128) * 4;
        table[entryIndex >> 5] |= expandMask << (entryIndex & 31);
    }
}

let tableStr = "";
for (let i = 0; i < table.length; i++)
    tableStr += "0x" + table[i].toString(16).padStart(8, '0') + (i % 8 == 7 ? ",\n" : ", ");

console.log(tableStr);