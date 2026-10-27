#version 440

layout(location = 0) in vec2 aPosition;
layout(location = 1) in vec2 aLocalPosition;
layout(location = 0) out vec2 localPosition;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    float radius;
    float halfWidth;
    vec4 color;
};

void main()
{
    localPosition = aLocalPosition;
    gl_Position = qt_Matrix * vec4(aPosition, 0.0, 1.0);
}
