#version 440
layout(location = 0) in vec2 uv;
layout(location = 1) in vec3 alternateUvAndBlend;
layout(location = 0) out vec4 fragColor;
layout(binding = 1) uniform sampler2D skin;
void main()
{
    vec4 color = texture(skin, uv);
    if (alternateUvAndBlend.z > 0.0)
        color = mix(color, texture(skin, alternateUvAndBlend.xy), alternateUvAndBlend.z);
    if (color.a < 0.5)
        discard;
    fragColor = vec4(color.rgb * color.a, color.a);
}
