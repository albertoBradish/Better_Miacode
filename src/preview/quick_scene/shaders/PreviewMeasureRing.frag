#version 440

layout(location = 0) in vec2 localPosition;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    float radius;
    float halfWidth;
    vec4 color;
};

void main()
{
    float distanceFromCircle = length(localPosition) - radius;
    // Derivatives follow the actual framebuffer scale, including high-DPI
    // preview and export. Integrating both edges keeps subpixel strokes thin.
    float pixelWidth = max(fwidth(distanceFromCircle), 0.0001);
    float outerCoverage = clamp(0.5 - (distanceFromCircle - halfWidth) / pixelWidth, 0.0, 1.0);
    float innerCoverage = clamp(0.5 - (distanceFromCircle + halfWidth) / pixelWidth, 0.0, 1.0);
    fragColor = color * (outerCoverage - innerCoverage) * qt_Opacity;
}
