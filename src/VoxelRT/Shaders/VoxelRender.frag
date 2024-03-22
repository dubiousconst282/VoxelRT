in vec2 v_FragCoord;
out vec4 o_FragColor;

uniform mat4 u_InvProjMat;
uniform int u_DebugView;
uniform int u_FrameNo;

#include "VoxelMap.glsl"
#include "VoxelTraversal.glsl"

#include "RandomGen.glsl"

void getPrimaryRay(vec2 uv, out vec3 rayPos, out vec3 rayDir) {
    vec4 near = u_InvProjMat * vec4(uv * 2.0 - 1.0, 0.0, 1.0);
    vec4 far = near + u_InvProjMat[2];
    rayPos = vec3(near * (1.0 / near.w));
    rayDir = normalize(vec3(far * (1.0 / far.w)));
}

void main() {
    random_init_uv(v_FragCoord, u_FrameNo);

    vec3 rayDir, rayPos;
    getPrimaryRay(v_FragCoord, rayPos, rayDir);
    
    //HitInfo hit;
    //rayTrace(rayPos, rayDir, hit);
    //o_FragColor = vec4(hit.norm *0.5+0.5, 1.0);
    //o_FragColor = vec4(getMaterialColor(hit.mat), 1.0);
    //o_FragColor.rgb *= smoothstep(1.0,0.95, max(abs(hit.uv.x*2-1), abs(hit.uv.y*2-1)));
    
    //if (rayTrace(hit.pos + hit.norm*0.0001, normalize(vec3(-0.3,1,0.3)), hit)) {
    //    o_FragColor.rgb*=0.3;
    //}

    if (u_DebugView != 0) {
        HitInfo hit;
        rayTrace(rayPos, rayDir, hit);
        vec3 color = u_DebugView == 1 ? getMaterialColor(hit.mat) : 
                     u_DebugView == 2 ? hit.norm * 0.5 + 0.5 :
                     u_DebugView == 3 ? vec3(hit.iters)/96.0 : vec3(1, 0, 1);
        o_FragColor = vec4(color, 1.0);
        return;
    }

    vec3 color = vec3(0.0);
    vec3 throughput = vec3(1.0);

    for (int i = 0; i < 3; i++) {
        HitInfo hit;
        if (!rayTrace(rayPos, rayDir, hit)) {
            vec3 skyColor = vec3(0.8, 0.9, 0.96) * 1.5;
            color += throughput * skyColor;
            break;
        }
        
        throughput *= getMaterialColor(hit.mat);
        color += throughput * getMaterialEmission(hit.mat);

        rayPos = hit.pos + hit.norm * 0.01;
        rayDir = normalize(hit.norm + random_dir());
    }
    o_FragColor = vec4(color, 1.0);
}