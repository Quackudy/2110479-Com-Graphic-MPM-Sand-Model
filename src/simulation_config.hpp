#pragma once
#include <filesystem>
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>
#include <vector>

using Json = nlohmann::json;

// Match Particle in mpm_common.glsl; mat3 columns are explicitly padded.
struct alignas(16) Particle {
  glm::vec4 position{0}, velocity{0};
  glm::vec4 f0{1, 0, 0, 0}, f1{0, 1, 0, 0}, f2{0, 0, 1, 0};
  glm::vec4 s0{0}, s1{0}, s2{0};
  glm::vec4 plastic{0};
  glm::vec4 c0{0}, c1{0}, c2{0}; // Local affine velocity, columns.
};
static_assert(sizeof(Particle) == 192 && offsetof(Particle, plastic) == 128 &&
              offsetof(Particle, c0) == 144);
struct alignas(16) Parameters {
  glm::uvec4 dims;
  glm::vec4 step, material, sand;
};
static_assert(sizeof(Parameters) == 64);

struct SimulationConfig {
  uint32_t grid = 128, particlesPerModel = 32768, fps = 24;
  double seconds = 2;
  uint32_t stepsPerFrame = 3;
  float young = 10000, poisson = 0.4f, density = 2, gravity = -2, friction = 30;
  float cfl = 0.25f, dt = 0, boundary = 0.03125f;
  std::string kernel = "ck";
  std::filesystem::path assets;
  static SimulationConfig load(const std::filesystem::path &path, bool manualLiveDt = false);
  void validate(bool manualLiveDt = false) const;
  float timestep() const;
  Parameters parameters(uint32_t count, float stepDt) const;
  Json json() const;
  std::vector<Particle> loadParticles() const;
};
