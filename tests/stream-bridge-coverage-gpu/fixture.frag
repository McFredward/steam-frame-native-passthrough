#version 450
layout(set=1,binding=0) uniform sampler2D coverage;
layout(location=0) out float result;
void main(){result=texture(coverage,vec2(gl_FragCoord.x/16.,.5)).r;}
