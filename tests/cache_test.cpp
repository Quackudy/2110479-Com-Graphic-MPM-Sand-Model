#include "scene_cache.hpp"
#include <iostream>
#include <fstream>
#include <limits>
#include <unistd.h>

void require(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
template <class F> void rejects(F function) {
    try {
        function();
    } catch (const std::exception &) {
        return;
    }
    throw std::runtime_error("invalid cache accepted");
}
int main() {
    auto directory =
        std::filesystem::temp_directory_path() / ("ck-cache-test-" + std::to_string(getpid()));
    std::filesystem::create_directories(directory);
    try {
        auto path = directory / "scene.bin";
        std::vector<Particle> p(2);
        p[0].position = {.1, .2, .3, 0};
        p[1].position = {.4, .5, .6, 1};
        {
            SceneCacheWriter writer(path, 2, 2, 24, {{"test", true}});
            writer.append(p);
            p[0].position.x = .2;
            writer.append(p);
            writer.commit();
        }
        {
            SceneCacheReader reader(path);
            require(reader.header.frames == 2, "frame count");
            require(reader.metadata["test"] == true, "metadata");
            require(reader.frame(1)[0] == .2f, "random access frame");
            require(reader.frame(0)[0] == .1f, "rewind");
            rejects([&] { reader.frame(2); });
        }
        {
            SceneCacheWriter writer(path, 2, 3, 24, {});
            writer.append(p);
            rejects([&] { writer.commit(); });
        }
        {
            SceneCacheReader reader(path);
            require(reader.header.frames == 2, "incomplete calculation replaced previous scene");
        }
        {
            SceneCacheWriter writer(path, 2, 1, 24, {{"new", true}});
            writer.append(p);
            writer.commit();
        }
        {
            SceneCacheReader reader(path);
            require(reader.header.frames == 1 && reader.metadata["new"] == true,
                    "replacement failed");
        }
        require(!std::filesystem::exists(path.string() + ".tmp"), "temporary file leaked");
        std::filesystem::resize_file(path, std::filesystem::file_size(path) - 1);
        rejects([&] { SceneCacheReader reader(path); });
        rejects([&] {
            SimulationConfig c;
            c.grid = 0;
            c.validate();
        });
        rejects([&] {
            SimulationConfig c;
            c.poisson = .5;
            c.validate();
        });
        rejects([&] {
            SimulationConfig c;
            c.dt = 1;
            c.validate();
        });
        try {
            SimulationConfig c;
            c.particlesPerModel = 500000;
            c.validate();
            throw std::runtime_error("oversized particle setting accepted");
        } catch (const std::runtime_error &e) {
            require(std::string(e.what()).find("particles_per_model must be 1..255951") != std::string::npos,
                    "particle count error must identify the field and limit");
        }
        SimulationConfig config;
        auto settings = config.json();
        settings["dt"] = .005;
        settings["steps_per_frame"] = 3;
        auto configPath = directory / "live.json";
        std::ofstream(configPath) << settings.dump();
        rejects([&] { SimulationConfig::load(configPath); });
        config = SimulationConfig::load(configPath, true);
        require(config.dt == .005f && config.stepsPerFrame == 3, "manual live settings round trip");
        for (uint32_t invalid : {0u, 4097u})
            rejects([&] { auto c = config; c.stepsPerFrame = invalid; c.validate(true); });
        for (float invalid : {-1.f, std::numeric_limits<float>::infinity()})
            rejects([&] { auto c = config; c.dt = invalid; c.validate(true); });
        std::filesystem::remove_all(directory);
        std::cout << "Cache replacement, rollback, random access, corruption, and config checks "
                     "passed.\n";
    } catch (...) {
        std::filesystem::remove_all(directory);
        throw;
    }
}
