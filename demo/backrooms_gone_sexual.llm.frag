#version 440

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    float time;
    float amplitude;
    float pad1;
    float pad2;
};

float opSmoothUnion( float d1, float d2, float k ) {
    float h = clamp( 0.5 + 0.5*(d2-d1)/k, 0.0, 1.0 );
    return mix( d2, d1, h ) - k*h*(1.0-h);
}

float map(vec3 p) {
    p.z -= time * 1.5;
    
    vec3 q = p;
    // FIX: This spaces the columns perfectly so X=0 (the camera) is a wide open lane.
    q.x = mod(p.x, 6.0) - 3.0; 
    q.z = mod(p.z, 6.0) - 3.0;
    
    // FIX: Hard-clamp the amplitude so massive bass drops cannot mathematically 
    // cause the geometry to reach the camera lens.
    float safeAmp = clamp(amplitude, 0.0, 1.5);
    
    // Base radius is 0.4. Swells up to 1.9. 
    // Since columns are 3.0 units away, the camera always has 1.1 units of breathing room.
    float radius = 0.4 + safeAmp;
    float columns = length(q.xz) - radius;
    
    // Ceiling and Floor (raised slightly for better framing)
    float floorCeil = 2.5 - abs(p.y);
    
    // Arch Blend
    float archBlend = 1.0 + safeAmp;
    
    float d = opSmoothUnion(columns, floorCeil, archBlend);
    return d * 0.7; 
}

vec3 calcNormal( in vec3 p ) {
    const float h = 1e-4;
    const vec2 k = vec2(1.0, -1.0);
    return normalize( k.xyy*map( p + k.xyy*h ) +
                      k.yyx*map( p + k.yyx*h ) +
                      k.yxy*map( p + k.yxy*h ) +
                      k.xxx*map( p + k.xxx*h ) );
}

void main() {
    vec2 uv = v_uv;
    float aspect = 1.3333;

    vec3 rayOri = vec3(0.0, 0.0, 0.0);
    vec3 rayDir = normalize(vec3((uv - 0.5) * vec2(aspect, 1.0), 1.0));

    float depth = 0.0;
    vec3 p;
    bool hit = false;

    for(int i = 0; i < 80; i++) {
        p = rayOri + rayDir * depth;
        float dist = map(p);
        depth += dist;
        if (dist < 0.005) { hit = true; break; }
    }

    vec3 bgCol = vec3(0.0, 0.0, 0.0);
    vec3 col = bgCol;

    if (hit) {
        vec3 n = calcNormal(p);
        
        float b = max(0.0, dot(n, normalize(vec3(0.5, 0.2, -1.0))));

        // KNOB: Deep teal base color.
        vec3 baseCol = 0.5 + 0.5 * cos(p.z * 0.2 + vec3(0.0, 1.0, 1.5));
        col = baseCol * (b * 0.6 + 0.2);

        // KNOB: Safely scaled rim light.
        float safeAmp = clamp(amplitude, 0.0, 1.5);
        float rim = 1.0 - max(dot(n, -rayDir), 0.0);
        col += vec3(0.0, 0.6, 0.8) * pow(rim, 2.0) * (0.2 + safeAmp);

        float fogFactor = clamp(depth / 18.0, 0.0, 1.0);
        col = mix(col, bgCol, fogFactor * fogFactor); 
    }

    // FIX: HDR Tone Mapping. This formula forces all crazy brightness values back 
    // down smoothly between 0.0 and 1.0. It eliminates blowout completely.
    col = col / (1.0 + col);
    
    // Gamma correction to keep the colors punchy
    col = pow(col, vec3(1.0 / 2.2));

    fragColor = vec4(col, 1.0);
}