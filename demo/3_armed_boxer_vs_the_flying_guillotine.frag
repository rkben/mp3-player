#version 440

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    float time;
    float amplitude;
    vec2 resolution;  // viewport pixel size (x=width, y=height)
};

float opSmoothUnion( float d1, float d2, float k ) {
    float h = clamp( 0.5 + 0.5*(d2-d1)/k, 0.0, 1.0 );
    return mix( d2, d1, h ) - k*h*(1.0-h);
}

mat2 rot(float a) {
    float s = sin(a), c = cos(a);
    return mat2(c, -s, s, c);
}

float map(vec3 p) {
    // KNOB: Core Size. 
    // Barely visible (0.05) when silent, swells massively (up to 1.0+) with volume.
    float coreSize = 0.05 + (amplitude * 1.2);
    float d = length(p) - coreSize;
    
    // KNOB: The Melt Factor. 
    // How intensely the orbiting shapes stretch and goo together.
    float melt = 0.1 + (amplitude * 2.5);
    
    // KNOB: Audio Jitter. Adds a high-frequency boiling texture to the surface on loud beats.
    float jitter = sin(p.x * 15.0 + time * 5.0) * sin(p.y * 15.0) * sin(p.z * 15.0);
    d += jitter * (amplitude * 0.15);

    for(int i = 0; i < 6; i++) {
        float fi = float(i);
        vec3 q = p;
        
        // Twist space so the "tentacles" writhe and orbit
        q.xy *= rot(time * 0.5 + fi * 2.1);
        q.xz *= rot(time * 0.7 + fi * 1.3);
        
        // KNOB: Explosion Reach. 
        // When quiet, they hug the core (0.1). When loud, they shoot outward (3.0+).
        float reach = 0.1 + (amplitude * 3.5);
        vec3 pos = vec3(0.0, reach, 0.0);
        
        // Orbiting sphere radius
        float sphereRadius = 0.1 + (amplitude * 0.4);
        float spike = length(q - pos) - sphereRadius;
        
        // Melt it into the core
        d = opSmoothUnion(d, spike, melt);
    }
    
    return d;
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

    // Using your original straight-ahead orthographic camera
    vec3 rayOri = vec3((uv - 0.5) * vec2(aspect, 1.0) * 8.0, 4.0);
    vec3 rayDir = vec3(0.0, 0.0, -1.0);

    float depth = 0.0;
    vec3 p;
    bool hit = false;

    for(int i = 0; i < 70; i++) {
        p = rayOri + rayDir * depth;
        float dist = map(p);
        depth += dist;
        if (dist < 0.005) { hit = true; break; }
    }

    vec3 bgCol = vec3(0.0, 0.0, 0.02);
    vec3 col = bgCol;

    if (hit) {
        vec3 n = calcNormal(p);
        float b = max(0.0, dot(n, normalize(vec3(0.8, 1.0, 0.5))));

        // KNOB: Base object color.
        vec3 objCol = 0.5 + 0.5 * cos(time * 0.5 + p.yxy * 0.5 + vec3(0.0, 2.0, 4.0));
        col = objCol * (0.2 + b * 0.8);

        // KNOB: High-energy rim lighting. 
        // A harsh green/yellow glow on the edges that only activates on loud bass hits.
        float rim = 1.0 - max(dot(n, -rayDir), 0.0);
        col += vec3(0.5, 1.0, 0.0) * pow(rim, 3.0) * (amplitude * 2.0);

        // KNOB: Emissive core flash.
        col += vec3(1.0, 0.2, 0.5) * (amplitude * amplitude);
        
        float fogFactor = clamp((depth - 2.0) / 4.0, 0.0, 1.0);
        col = mix(col, bgCol, fogFactor);
    }

    fragColor = vec4(col, 1.0);
}