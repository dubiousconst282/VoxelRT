//The raycasting code is somewhat based around a 2D raycasting toutorial found here: 
//http://lodev.org/cgtutor/raycasting.html

const bool USE_BRANCHLESS_DDA = true;
const int MAX_RAY_STEPS = 64;

float sdSphere(vec3 p, float d) { return length(p) - d; } 

float sdBox( vec3 p, vec3 b )
{
  vec3 d = abs(p) - b;
  return min(max(d.x,max(d.y,d.z)),0.0) +
         length(max(d,0.0));
}
	
bool getVoxel(ivec3 c) {
	vec3 p = vec3(c) + vec3(0.5);
	float d = min(max(-sdSphere(p, 7.5), sdBox(p, vec3(6.0))), -sdSphere(p, 25.0));
	return d < 0.0;
}

vec2 rotate2d(vec2 v, float a) {
	float sinA = sin(a);
	float cosA = cos(a);
	return vec2(v.x * cosA - v.y * sinA, v.y * cosA + v.x * sinA);	
}

vec4 trace(vec3 rayPos, vec3 rayDir, bool useDDA) {

    vec3 sideDist = vec3(0);
    int i =0;
        
    if (useDDA) {
        ivec3 mapPos = ivec3(floor(rayPos + 0.));

        vec3 deltaDist = abs(vec3(length(rayDir)) / rayDir);
        ivec3 rayStep = ivec3(sign(rayDir));
         sideDist = (sign(rayDir) * (vec3(mapPos) - rayPos) + (sign(rayDir) * 0.5) + 0.5) * deltaDist; 
        bvec3 mask;
        
        for (i = 0; i < MAX_RAY_STEPS; i++) {
            if (getVoxel(mapPos)) break;

            //Thanks kzy for the suggestion!
            mask = lessThanEqual(sideDist.xyz, min(sideDist.yzx, sideDist.zxy));
			sideDist += vec3(mask) * deltaDist;
			mapPos += ivec3(vec3(mask)) * rayStep;
        }
        sideDist -= vec3(mask) * deltaDist;
    } else {
        vec3 invDir = 1.0 / rayDir;
        vec3 tStart = (step(0.0, rayDir) - rayPos)*invDir;
        ivec3 voxelPos = ivec3(floor(rayPos));

        for (i = 0; i < MAX_RAY_STEPS; i++) {
            sideDist = tStart + vec3(voxelPos) * invDir;
            float tmin = min(min(sideDist.x, sideDist.y), sideDist.z);
            
            if (true) {
                // accurate stepping
                vec3 currPos = rayPos + tmin * rayDir;
            
                bvec3 sideMask = equal(sideDist, vec3(tmin));
                voxelPos = ivec3(floor(mix(currPos, currPos + rayDir * 0.01, sideMask)));
            } else {
                // approximate stepping
                vec3 currPos = rayPos + (tmin + 0.001) * rayDir;
                voxelPos = ivec3(floor(currPos));
            }
            if (getVoxel(voxelPos)) break;
        }
     }
   return vec4(sideDist, i);
}

void mainImage( out vec4 fragColor, in vec2 fragCoord )
{
	vec2 screenPos = (fragCoord.xy / iResolution.xy) * 2.0 - 1.0;
	vec3 cameraDir = vec3(0.0, 0.0, 0.8);
	vec3 cameraPlaneU = vec3(1.0, 0.0, 0.0);
	vec3 cameraPlaneV = vec3(0.0, 1.0, 0.0) * iResolution.y / iResolution.x;
	vec3 rayDir = cameraDir + screenPos.x * cameraPlaneU + screenPos.y * cameraPlaneV;
	vec3 rayPos = vec3(0.0, 2.0 * sin(iTime * 2.7), -12.0);
		
	rayPos.xz = rotate2d(rayPos.xz, iTime);
	rayDir.xz = rotate2d(rayDir.xz, iTime);

    bool useDDA = fract(iTime/5.0)<0.5;
	
    vec4 sideDist= vec4(0);
    for(int i = 0; i < 64; i++) {
        sideDist += (i==15?1.0:0.0001)*trace(rayPos+vec3(i)*0.0001,rayDir,useDDA);
    }
	
    bvec3 mask = lessThanEqual(sideDist.xyz, min(sideDist.yzx, sideDist.zxy));
	vec3 color;
	if (mask.x) {
		color = vec3(0.5);
	}
	if (mask.y) {
		color = vec3(1.0);
	}
	if (mask.z) {
		color = vec3(0.75);
	}
    if(useDDA)color.r*=1.5;
	fragColor.rgb = color;
	//fragColor.rgb = vec3(0.1 * noiseDeriv);
}