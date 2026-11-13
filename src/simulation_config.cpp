#include "simulation_config.hpp"
#include <cmath>
#include <algorithm>
#include <fstream>
#include <numbers>
#include <stdexcept>

SimulationConfig SimulationConfig::load(const std::filesystem::path &path, bool manualLiveDt) {
    std::ifstream input(path);
    if (!input)
        throw std::runtime_error("cannot open config: " + path.string());
    Json j;
    input >> j;
    SimulationConfig c;
    for (const auto &[key, value] : j.items()) {
        (void)value;
        if (!c.json().contains(key))
            throw std::runtime_error("unknown simulation setting: " + key);
    }
    auto integer = [&](const char *name, uint32_t fallback) {
        if (!j.contains(name))
            return fallback;
        if (!j[name].is_number_unsigned() &&
            !(j[name].is_number_integer() && j[name].get<int64_t>() >= 0))
            throw std::runtime_error(std::string(name) + " must be a nonnegative integer");
        auto n = j[name].get<uint64_t>();
        if (n > UINT32_MAX)
            throw std::runtime_error("integer setting too large");
        return static_cast<uint32_t>(n);
    };
    c.grid = integer("grid", c.grid);
    c.particlesPerModel = integer("particles_per_model", c.particlesPerModel);
    c.fps = integer("fps", c.fps);
    c.seconds = j.value("seconds", c.seconds);
    c.stepsPerFrame = integer("steps_per_frame", c.stepsPerFrame);
    c.kernel = j.value("kernel", c.kernel);
    c.young = j.value("young_modulus", c.young);
    c.poisson = j.value("poisson_ratio", c.poisson);
    c.density = j.value("density", c.density);
    c.gravity = j.value("gravity", c.gravity);
    c.friction = j.value("friction_angle", c.friction);
    c.cfl = j.value("cfl", c.cfl);
    c.dt = j.value("dt", c.dt);
    c.boundary = j.value("boundary", c.boundary);
    c.assets = j.value("assets", std::string("third_party/CK-MPM/asset/particle_data"));
    if (c.assets.is_relative())
        c.assets = std::filesystem::absolute(path).parent_path() / c.assets;
    c.assets = std::filesystem::weakly_canonical(c.assets);
    c.validate(manualLiveDt);
    return c;
}
void SimulationConfig::validate(bool manualLiveDt) const {
    auto finite = [](double v) { return std::isfinite(v); };
    if (particlesPerModel < 1 || particlesPerModel > 255951)
        throw std::runtime_error("particles_per_model must be 1..255951 (author asset limit); total particles = 2 * particles_per_model");
    if (grid < 16 || grid > 256 || fps < 1 ||
        fps > 240 || !finite(seconds) || seconds <= 0 || seconds > 300 || !finite(young) ||
        young <= 0 || !finite(poisson) || poisson <= -1 || poisson >= 0.5 || !finite(density) ||
        density <= 0 || !finite(gravity) || !finite(friction) || friction <= 0 || friction >= 90 ||
        !finite(cfl) || cfl <= 0 || cfl > 0.5 || !finite(dt) || dt < 0 || !finite(boundary) ||
        boundary < 2.0f / grid || boundary >= 0.2 || (kernel != "ck" && kernel != "quadratic"))
        throw std::runtime_error("invalid simulation settings; see sand.json and README");
    if (stepsPerFrame < 1 || stepsPerFrame > 4096)
        throw std::runtime_error("steps_per_frame must be 1..4096");
    if (!manualLiveDt && dt > 0 &&
        dt > cfl / grid /
                 std::sqrt(young * (1 - poisson) / ((1 + poisson) * (1 - 2 * poisson) * density)))
        throw std::runtime_error("dt exceeds the configured sound-speed CFL limit");
}
float SimulationConfig::timestep() const {
    return dt > 0 ? dt
                  : cfl / grid /
                        std::sqrt(young * (1 - poisson) /
                                  ((1 + poisson) * (1 - 2 * poisson) * density));
}
Parameters SimulationConfig::parameters(uint32_t count, float stepDt) const {
    const float volume = 1e-7f * 255951.0f / particlesPerModel;
    const uint32_t grids = kernel == "ck" ? 2 : 1;
    return {{count, grid, kernel == "ck" ? 0u : 1u, grids * grid * grid * grid},
            {stepDt, 1.0f / grid, volume * density, volume},
            {young / (2 * (1 + poisson)), young * poisson / ((1 + poisson) * (1 - 2 * poisson)),
             gravity, boundary * grid},
            {std::sin(friction * std::numbers::pi_v<float> / 180), volume * density * 1e-8f, 0, 0}};
}
Json SimulationConfig::json() const {
    return {{"grid", grid},
            {"particles_per_model", particlesPerModel},
            {"fps", fps},
            {"seconds", seconds},
            {"steps_per_frame", stepsPerFrame},
            {"kernel", kernel},
            {"young_modulus", young},
            {"poisson_ratio", poisson},
            {"density", density},
            {"gravity", gravity},
            {"friction_angle", friction},
            {"cfl", cfl},
            {"dt", dt},
            {"boundary", boundary},
            {"assets", assets.string()}};
}
std::vector<Particle> SimulationConfig::loadParticles() const {
    std::vector<Particle> result;
    result.reserve(2 * particlesPerModel);
    for (int model = 0; model < 2; ++model) {
        auto path = assets / ("sand_armadillo_" + std::to_string(model) + "_jittered.bin");
        if (!std::filesystem::exists(path) ||
            std::filesystem::file_size(path) != 255951 * 3 * sizeof(float))
            throw std::runtime_error("missing or unhydrated author particle asset: " +
                                     path.string() +
                                     " (run ./run.py, or `git lfs pull` in third_party/CK-MPM)");
        std::ifstream input(path, std::ios::binary);
        std::vector<float> xyz(255951 * 3);
        if (!input.read(reinterpret_cast<char *>(xyz.data()), xyz.size() * sizeof(float)))
            throw std::runtime_error("asset read failed");
        for (uint32_t i = 0; i < particlesPerModel; ++i) {
            size_t k = static_cast<uint64_t>(i) * 255951 / particlesPerModel;
            Particle p;
            p.position = {xyz[3 * k], xyz[3 * k + 1], xyz[3 * k + 2], float(model)};
            p.velocity.z = model == 0 ? -0.5f : 0.5f;
            for (int axis = 0; axis < 3; ++axis)
                if (!std::isfinite(p.position[axis]) || p.position[axis] < boundary + 2.0f / grid ||
                    p.position[axis] > 1 - boundary - 2.0f / grid)
                    throw std::runtime_error("initial particle lies too close to domain boundary");
            result.push_back(p);
        }
    }
    return result;
}
