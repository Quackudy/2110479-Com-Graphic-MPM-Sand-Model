// std430 layout: every particle member occupies exactly 16 bytes.
struct Particle {
    vec4 position;
    vec4 velocity;
    vec4 f0; vec4 f1; vec4 f2;
    vec4 s0; vec4 s1; vec4 s2;
    vec4 plastic;
    vec4 c0; vec4 c1; vec4 c2;
};
layout(std430, set=0, binding=0) buffer Particles { Particle particles[]; };
layout(std430, set=0, binding=1) buffer Grid { vec4 nodes[]; };
layout(push_constant) uniform Parameters {
    uvec4 dims; // particle count, grid resolution, kernel (0 CK / 1 quadratic), node count
    vec4 step; // dt, dx, particle mass, particle volume
    vec4 material; // mu, lambda, gravity, boundary thickness in grid cells
    vec4 sand; // sin(friction angle), mass cutoff, unused, unused
} params;
mat3 deformation(Particle p) { return mat3(p.f0.xyz, p.f1.xyz, p.f2.xyz); }
mat3 stress(Particle p) { return mat3(p.s0.xyz, p.s1.xyz, p.s2.xyz); }
mat3 affine(Particle p) { return mat3(p.c0.xyz, p.c1.xyz, p.c2.xyz); }
bool validNode(ivec3 c) { return all(greaterThanEqual(c, ivec3(0))) && all(lessThan(c, ivec3(params.dims.y))); }
uint nodeIndex(ivec3 c, uint g) {
    uint n = params.dims.y;
    return ((g*n + uint(c.z))*n + uint(c.y))*n + uint(c.x);
}
