#version 440

layout(location = 0) in vec2 vTexCoord;
layout(location = 1) in float vOpacity;
layout(location = 2) in float vWave;
layout(location = 3) in float vAbsWave;
layout(location = 4) in float vEffect;

layout(location = 0) out vec4 fragColor;

layout(binding = 1) uniform sampler2D source;

// Footprint-aware 4-sample supersample. The QSG export path can't carry
// a CPU-built mip chain (createTextureFromImage gives back a level-0
// QSGTexture and the runtime mip request was reverted in beta20 due to
// the Intel UMD GenerateMips crash), so heavy minification — taps spawn
// from ~1 px and grow, against a 122 px source — would alias badly with
// a single bilinear tap. fwidth(uv) gives the screen-space derivative
// of the UV, which scales naturally with minification: a rotated-grid
// 4-sample average across that footprint averages more source detail
// the smaller the sprite is drawn, and collapses to (effectively) a
// single sample when the sprite is at or above source resolution.
vec4 sampleFootprintAverage(sampler2D tex, vec2 uv)
{
    vec2 r = fwidth(uv) * 0.25;
    vec4 c0 = texture(tex, uv + vec2( r.x,  r.y));
    vec4 c1 = texture(tex, uv + vec2(-r.x,  r.y));
    vec4 c2 = texture(tex, uv + vec2( r.x, -r.y));
    vec4 c3 = texture(tex, uv + vec2(-r.x, -r.y));
    return (c0 + c1 + c2 + c3) * 0.25;
}

float selectionAlpha(vec2 uv)
{
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return 0.0;
    return texture(source, uv).a;
}

void main()
{
    if (vOpacity <= 0.0) {
        fragColor = vec4(0.0);
        return;
    }

    if (vEffect > 2.5) {
        // Screen-space alpha dilation minus the original silhouette. Interiors
        // are transparent, retaining original break/each/EX/mine skin colors.
        vec2 dx = dFdx(vTexCoord) * 2.0;
        vec2 dy = dFdy(vTexCoord) * 2.0;
        float around = 0.0;
        for (int x = -1; x <= 1; ++x)
            for (int y = -1; y <= 1; ++y)
                around = max(around, selectionAlpha(vTexCoord + float(x) * dx + float(y) * dy));
        float alpha = max(0.0, around - selectionAlpha(vTexCoord)) * vOpacity;
        fragColor = vec4(vec3(1.0, 0.84, 0.0) * alpha, alpha);
        return;
    }
    vec4 texel = sampleFootprintAverage(source, vTexCoord);
    if (texel.a <= 0.0) {
        fragColor = vec4(0.0);
        return;
    }

    vec3 color = texel.rgb / texel.a;

    if (vEffect > 1.5) {
        float brightness = 0.95 + max(vWave * 0.65, 0.0);
        float contrast = 1.0 + min(vWave * -0.55, 0.0);
        color *= brightness;
        color = vec3(0.5) + (color - vec3(0.5)) * contrast;
    } else if (vEffect > 0.5) {
        float brightness = 0.95 + vAbsWave * 0.5;
        color *= brightness;
    }

    color = clamp(color, 0.0, 1.0);
    fragColor = vec4(color * texel.a, texel.a) * vOpacity;
}
