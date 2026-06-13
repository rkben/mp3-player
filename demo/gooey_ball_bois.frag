#version 440

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    float time;
    float amplitude;
    float pad1;
    float pad2;
};

float opSmoothUnion( float d1, float d2, float k )
{
    float h = clamp( 0.5 + 0.5*(d2-d1)/k, 0.0, 1.0 );
    return mix( d2, d1, h ) - k*h*(1.0-h);
}

float sdSphere( vec3 p, float s )
{
  return length(p) - s;
}

float map(vec3 p)
{
    float d = 1.0;
    float blendFactor = amplitude * 1.7;
    const float idleSize = 0.01;
    for (int i = 0; i < 10; i++) {
        float fi = float(i);
        float t = time * (fract(fi * 412.531 + 0.513) - 0.5) * 1.5;
        float radius = mix(0.6, 1.0, fract(fi * 412.531 + 0.5124)) * (amplitude + idleSize * 5);

        d = opSmoothUnion(
            sdSphere(p + sin(t + fi * vec3(52.5126, 64.62744, 632.25)) * vec3(2.0, 2.0, 0.8), radius),
            d,
            blendFactor
        );
    }
    return d;
}

vec3 calcNormal( in vec3 p )
{
    const float h = 1e-6;
    const vec2 k = vec2(1.0, -1.0);
    return normalize( k.xyy*map( p + k.xyy*h ) +
                      k.yyx*map( p + k.yyx*h ) +
                      k.yxy*map( p + k.yxy*h ) +
                      k.xxx*map( p + k.xxx*h ) );
}

void main()
{
    vec2 uv = v_uv;
    float aspect = 1.3333;  // KNOB: viewport aspect (4:3). Set to width/height to avoid stretch.

    // KNOB: camera framing. (uv-0.5)*6.0 is the zoom (smaller = closer); the
    //       trailing 3.0 is the camera's Z distance from the blob field.
    vec3 rayOri = vec3((uv - 0.5) * vec2(aspect, 1.0) * 6.0, 3.0);
    vec3 rayDir = vec3(0.0, 0.0, -1.0);  // straight-ahead ortho-ish march.

    float depth = 0.0;
    vec3 p;
    bool hit = false;

    // KNOB: raymarch step budget. More steps = cleaner silhouettes / deeper
    //       reach, at GPU cost. 0.001 is the hit threshold (surface precision).
    for(int i = 0; i < 64; i++) {
        p = rayOri + rayDir * depth;
        float dist = map(p);
        depth += dist;

        if (dist < 0.001) {
            hit = true;
            break;
        }
    }

    vec3 col;
    // KNOB: background color. Black by default; the fog below fades blobs into it.
    vec3 bgCol = vec3(0.0, 0.0, 0.0);

    if (hit) {
        vec3 n = calcNormal(p);
        // KNOB: light direction (normalized 1,1,1). Change to relight the blobs.
        float b = max(0.0, dot(n, vec3(0.577, 0.577, 0.577)));

        // KNOB: base palette. The cos(...) rainbow cycles with time*2.0 (hue
        //       speed) and uv*2.0 (spatial banding); +vec3(0,2,4) sets the R/G/B
        //       phase offset. The (0.85 + b*0.35) term is diffuse shading strength.
        col = (0.5 + 0.5 * cos((b + time * 0.8) + uv.xyx * 2.0 + vec3(0.0, 2.0, 4.0))) * (0.95 + b * 0.85);

        // KNOB: audio-reactive glow. vec3(0,0.3,0.6) is the glow tint (blue); the
        //       *0.2 is its intensity scaling with amplitude.
        col += vec3(0.0, 0.3, 0.6) * amplitude * 0.2;

        depth = min(1.0, depth);
        // KNOB: depth brightening. exp(depth*0.35) lifts nearer/closer hits;
        //       raise 0.35 for more contrast between front and back blobs.
        col *= exp(depth * 0.1);

        // KNOB: fog falloff. Blobs past depth 0.5 fade to bgCol over a range of
        //       2.0 units. Lower the 0.5 to start fog sooner; raise 2.0 to soften it.
        float fogFactor = clamp((depth - 0.3) / 2.0, 0.0, 1.0);
        col = mix(col, bgCol, fogFactor);
    } else {
        col = bgCol;
    }

    // Output a completely solid alpha channel (1.0) so the white QWidget background never leaks
    fragColor = vec4(col, 1.0);
}
