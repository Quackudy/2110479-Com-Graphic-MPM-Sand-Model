#include "platform.hpp"
#include "renderer.hpp"
#include "scene_cache.hpp"
#include "simulation.hpp"
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <future>
#include <glm/gtc/quaternion.hpp>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>
namespace {
using Clock = std::chrono::steady_clock;
const std::filesystem::path root = PROJECT_DIRECTORY;
const auto output = root / "output";
const auto cachePath = output / "scene.bin";

double elapsed(Clock::time_point begin) {
    return std::chrono::duration<double>(Clock::now() - begin).count();
}
std::string timestamp() {
    const std::tm tm = platform::utcTime(std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
    std::ostringstream s;
    s << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return s.str();
}
void logRun(const Json &log) {
    std::ofstream file(output / "performance.jsonl", std::ios::app);
    file.exceptions(std::ios::badbit | std::ios::failbit);
    file << log.dump() << '\n';
    file.flush();
}
std::filesystem::path calculationLockPath() {
    std::filesystem::create_directories(output);
    return output / "calculate.lock";
}
struct CalculationLock {
    platform::FileLock lock{calculationLockPath()};
    CalculationLock() {
        if (!lock.acquired())
            throw std::runtime_error("another calculation is already running");
    }
};
int evaluate(const std::filesystem::path &executable) {
    std::filesystem::remove(output / "evaluation.json");
    return platform::runProcess({EVALUATION_PYTHON, root / "tools/evaluate.py", executable,
                                 output / "evaluation.json"});
}
void calculate(const std::filesystem::path &executable, const std::filesystem::path &configPath) {
    CalculationLock lock;
    Json log{{"started_utc", timestamp()},
             {"settings_file", std::filesystem::absolute(configPath).string()},
             {"status", "started"},
             {"backend", "Vulkan"},
             {"transfer", "affine"},
             {"scene", "sand_armadillo"}};
    auto begin = Clock::now();
    try {
        const auto config = SimulationConfig::load(configPath);
        log["settings"] = config.json();
        const int evaluationStatus = evaluate(executable);
        log["evaluation"] = {{"status", "failed_to_run"}};
        {
            std::ifstream report(output / "evaluation.json");
            if (report)
                report >> log["evaluation"];
        }
        if (evaluationStatus)
            throw std::runtime_error("reference checks failed; see output/evaluation.json");
        VulkanContext context(nullptr, true);
        log["gpu"] = {{"name", context.properties().deviceName.data()},
                      {"vendor_id", context.properties().vendorID},
                      {"device_id", context.properties().deviceID},
                      {"driver_version", context.properties().driverVersion},
                      {"api_version", context.properties().apiVersion},
                      {"validation", context.validationEnabled()},
                      {"sync_validation", context.syncValidationEnabled()}};
        const auto initial = config.loadParticles();
        Simulation simulation(context, executable.parent_path(), config, initial);
        const float maxDt = config.timestep();
        // Warm pipelines and allocations, then restore exactly the initial state.
        simulation.advance(4, maxDt);
        simulation.reset(initial);
        const uint32_t intervals = static_cast<uint32_t>(std::ceil(config.seconds * config.fps));
        const uint32_t frames = intervals + 1;
        const double duration = double(intervals) / config.fps;
        log["actual_simulated_seconds"] = duration;
        log["saved_frames"] = frames;
        log["particle_count"] = initial.size();
        log["sound_cfl_dt_seconds"] = maxDt;
        Json metadata{{"settings", config.json()},
                      {"created_utc", log["started_utc"]},
                      {"particle_count", initial.size()},
                      {"transfer", "affine"},
                      {"source", "author sand_armadillo assets and Drucker-Prager material"},
                      {"duration", duration}};
        SceneCacheWriter cache(cachePath, static_cast<uint32_t>(initial.size()), frames, config.fps,
                               metadata);
        cache.append(initial);
        double physicsWall = 0, ioWall = 0;
        float maxSpeed = 1, minimumDt = maxDt, maximumDt = 0, maxObservedSpeed = 0;
        Json bounds;
        for (uint32_t frame = 1; frame < frames; ++frame) {
            // Also limit travel to 0.25 cell per substep based on the last saved frame.
            const float limit = std::min(maxDt, 0.25f / config.grid / std::max(maxSpeed, 1e-6f));
            const double requiredSteps = std::ceil((1.0 / config.fps) / limit);
            if (!std::isfinite(requiredSteps) || requiredSteps > 1000000)
                throw std::runtime_error(
                    "timestep requires too many substeps per frame; inspect settings/state");
            const uint32_t steps = static_cast<uint32_t>(requiredSteps);
            const float dt = static_cast<float>(1.0 / (config.fps * double(steps)));
            minimumDt = std::min(minimumDt, dt);
            maximumDt = std::max(maximumDt, dt);
            auto start = Clock::now();
            simulation.advance(steps, dt);
            physicsWall += elapsed(start);
            start = Clock::now();
            auto state = simulation.readParticles();
            glm::vec3 low(1e9f), high(-1e9f), center(0);
            maxSpeed = 0;
            for (const auto &p : state) {
                const float *values = reinterpret_cast<const float *>(&p);
                for (size_t i = 0; i < sizeof(Particle) / sizeof(float); ++i)
                    if (!std::isfinite(values[i]))
                        throw std::runtime_error(
                            "non-finite physics state; previous scene retained");
                for (int a = 0; a < 3; ++a)
                    if (p.position[a] < 1.5f / config.grid ||
                        p.position[a] > 1 - 1.5f / config.grid)
                        throw std::runtime_error("particle escaped safe interpolation domain; "
                                                 "reduce dt or increase resolution");
                low = glm::min(low, glm::vec3(p.position));
                high = glm::max(high, glm::vec3(p.position));
                center += glm::vec3(p.position);
                maxSpeed = std::max(maxSpeed, glm::length(glm::vec3(p.velocity)));
            }
            center /= float(state.size());
            maxObservedSpeed = std::max(maxObservedSpeed, maxSpeed);
            bounds = {{"min", {low.x, low.y, low.z}},
                      {"max", {high.x, high.y, high.z}},
                      {"center_of_mass", {center.x, center.y, center.z}},
                      {"max_particle_speed", maxSpeed}};
            cache.append(state);
            ioWall += elapsed(start);
            std::cout << "Frame " << frame << '/' << intervals
                      << " | t=" << double(frame) / config.fps << " | steps=" << steps
                      << " | max speed=" << maxSpeed << std::endl;
        }
        if (context.validationMessages())
            throw std::runtime_error(
                "Vulkan validation reported warnings/errors; previous scene retained");
        log["substeps"] = simulation.substeps;
        log["dt_range_seconds"] = {minimumDt, maximumDt};
        log["physics_wall_seconds"] = physicsWall;
        log["snapshot_readback_check_write_seconds"] = ioWall;
        double gpuMs = std::accumulate(simulation.gpuMilliseconds.begin(),
                                       simulation.gpuMilliseconds.end(), 0.0);
        log["physics_gpu_seconds"] = gpuMs / 1000;
        log["mean_substep_gpu_ms"] = gpuMs / simulation.substeps;
        for (size_t s = 0; s < 5; ++s)
            log["profiling"][Simulation::stageNames[s]] = {
                {"total_gpu_ms", simulation.gpuMilliseconds[s]},
                {"mean_substep_gpu_ms", simulation.gpuMilliseconds[s] / simulation.substeps}};
        log["profiling_note"] =
            "GPU timestamps include barriers between passes; four warmup substeps excluded; "
            "rendering disabled; validation enabled when available";
        log["scene_checks"] = {{"all_saved_state_finite", true},
                               {"all_saved_particles_in_safe_domain", true},
                               {"validation_messages", context.validationMessages()},
                               {"final", bounds},
                               {"max_saved_particle_speed", maxObservedSpeed}};
        log["status"] = "success";
        log["finished_utc"] = timestamp();
        log["total_wall_seconds"] = elapsed(begin);
        log["cache_bytes"] =
            sizeof(CacheHeader) + metadata.dump().size() + uint64_t(frames) * initial.size() * 12;
        // Log before publishing, so a log write failure cannot overwrite the old scene.
        log["cache_publication"] = "pending_atomic_rename";
        logRun(log);
        cache.commit();
        logRun({{"started_utc", log["started_utc"]},
                {"status", "cache_published"},
                {"cache", cachePath.string()}});
        std::cout << "Saved " << cachePath << " (" << log["cache_bytes"]
                  << " bytes).\nPhysics GPU: " << gpuMs / 1000
                  << " s | physics wall: " << physicsWall
                  << " s\nPerformance log: " << output / "performance.jsonl"
                  << "\nRun ./run.py play to view.\n";
    } catch (const std::exception &e) {
        log["status"] = "failed";
        log["error"] = e.what();
        log["total_wall_seconds"] = elapsed(begin);
        log["finished_utc"] = timestamp();
        logRun(log);
        throw;
    }
}
struct Window {
    GLFWwindow *handle = nullptr;
    ~Window() {
        if (handle)
            glfwDestroyWindow(handle);
        glfwTerminate();
    }
};
struct Controls {
    scene::Camera camera;
    bool captured = false, paused = false, livePhysics = false, restart = false;
    double time = 0, lastX = 0, lastY = 0;
};
void configureControls(GLFWwindow *window, Controls &controls) {
    glfwSetWindowUserPointer(window, &controls);
    glfwSetKeyCallback(window, [](GLFWwindow *w, int key, int, int action, int) {
        if (action != GLFW_PRESS)
            return;
        auto &c = *static_cast<Controls *>(glfwGetWindowUserPointer(w));
        if (key == GLFW_KEY_ESCAPE)
            glfwSetWindowShouldClose(w, true);
        if (key == GLFW_KEY_SPACE)
            c.paused = !c.paused;
        if ((key == GLFW_KEY_R || key == GLFW_KEY_HOME)) {
            c.time = 0;
            c.restart = true;
        }
        if (!c.livePhysics && key == GLFW_KEY_LEFT)
            c.time = std::max(0.0, c.time - 0.5);
        if (!c.livePhysics && key == GLFW_KEY_RIGHT)
            c.time += 0.5;
        if (key == GLFW_KEY_TAB) {
            c.captured = !c.captured;
            glfwSetInputMode(w, GLFW_CURSOR,
                             c.captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
            glfwGetCursorPos(w, &c.lastX, &c.lastY);
        }
    });
    glfwSetCursorPosCallback(window, [](GLFWwindow *w, double x, double y) {
        auto &c = *static_cast<Controls *>(glfwGetWindowUserPointer(w));
        double dx = x - c.lastX, dy = y - c.lastY;
        c.lastX = x;
        c.lastY = y;
        if (!c.captured)
            return;
        auto dir = c.camera.target - c.camera.position;
        auto yaw = glm::angleAxis(float(-dx * 0.002), c.camera.up);
        dir = yaw * dir;
        auto right = glm::normalize(glm::cross(dir, c.camera.up));
        auto rotated = glm::angleAxis(float(-dy * 0.002), right) * dir;
        if (std::abs(glm::dot(glm::normalize(rotated), c.camera.up)) < 0.995f)
            c.camera.target = c.camera.position + rotated;
    });
}
void moveCamera(GLFWwindow *window, Controls &controls, double delta) {
    auto &c = controls.camera;
    glm::vec3 forward = glm::normalize(c.target - c.position),
              right = glm::normalize(glm::cross(forward, c.up)), movement(0);
    if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS)
        movement += forward;
    if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS)
        movement -= forward;
    if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS)
        movement += right;
    if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS)
        movement -= right;
    if (glfwGetKey(window, GLFW_KEY_E) == GLFW_PRESS)
        movement += c.up;
    if (glfwGetKey(window, GLFW_KEY_Q) == GLFW_PRESS)
        movement -= c.up;
    c.position += float(delta) * 0.6f * movement;
    c.target += float(delta) * 0.6f * movement;
}
void play(uint32_t stopAfter) {
    SceneCacheReader cache(cachePath);
    const auto transfer = cache.metadata.value("transfer", std::string("unknown"));
    if (transfer != "affine")
        throw std::runtime_error("saved scene uses obsolete " + transfer +
                                 " transfer; run ./run.py calculate to create an affine scene");
    const float floor = cache.metadata.at("settings").at("boundary");
    if (!std::isfinite(floor) || floor < 0 || floor >= .2f)
        throw std::runtime_error("invalid cached rendering settings");
    Window window;
    if (!glfwInit()) throw std::runtime_error("play requires a desktop display session");
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    window.handle = glfwCreateWindow(1280, 720, "CK-MPM saved affine sand", nullptr, nullptr);
    if (!window.handle) throw std::runtime_error("cannot create playback window");
    Controls controls;
    controls.camera.position = {1.15f, .7f, 1.35f};
    controls.camera.target = {.5f, .22f, .5f};
    controls.camera.nearPlane = .01f;
    controls.camera.farPlane = 10;
    configureControls(window.handle, controls);
    VulkanContext context(window.handle);
    const float radius = .0018f * std::cbrt(65536.f / cache.header.particles);
    Renderer renderer(context, std::filesystem::path(PROJECT_BINARY_DIRECTORY),
                      cache.header.particles, radius, floor);
    uint32_t loaded = UINT32_MAX, rendered = 0;
    std::vector<float> a, b;
    std::future<std::vector<float>> prefetched;
    const double duration = double(cache.header.frames-1) / cache.header.fps;
    auto previous = Clock::now(), start = previous;
    std::cout << "Saved scene: affine transfer | " << cache.header.particles << " particles\n"
                 "Space pause | R/Home replay | Left/Right seek | Tab mouse | WASD/Q/E move | Esc close\n";
    while (!glfwWindowShouldClose(window.handle) && (!stopAfter || rendered < stopAfter)) {
        const double seconds = elapsed(previous);
        previous = Clock::now();
        glfwPollEvents();
        if (!controls.paused) controls.time += seconds;
        controls.time = std::clamp(controls.time, 0.0, duration);
        moveCamera(window.handle, controls, std::min(seconds,.1));
        const float frame = controls.time * cache.header.fps;
        const uint32_t index = std::min(uint32_t(frame), cache.header.frames-1);
        if (!context.beginFrame({.035f,.045f,.065f,1})) continue;
        if (index != loaded) {
            if (loaded != UINT32_MAX && index == loaded+1 && prefetched.valid()) {
                a = std::move(b);
                b = prefetched.get();
            } else {
                if (prefetched.valid()) prefetched.get();
                a = cache.frame(index);
                b = cache.frame(std::min(index+1,cache.header.frames-1));
            }
            renderer.setFrames(a,b);
            loaded = index;
            if (index+2 < cache.header.frames)
                prefetched = std::async(std::launch::async,[&cache,index]{return cache.frame(index+2);});
        }
        context.beginRendering({.035f,.045f,.065f,1});
        renderer.draw(controls.camera,frame-index);
        context.endFrame();
        ++rendered;
        std::ostringstream title;
        title << "CK-MPM saved affine sand | " << std::fixed << std::setprecision(2)
              << controls.time << " / " << duration << " s | "
              << (controls.paused ? "paused" : "playing");
        glfwSetWindowTitle(window.handle,title.str().c_str());
    }
    context.device().waitIdle();
    if (context.validationMessages()) throw std::runtime_error("playback produced Vulkan validation messages");
    logRun({{"status","particle_playback"},{"created_utc",timestamp()},
            {"transfer",transfer},{"rendered_frames",rendered},
            {"mean_render_fps",rendered/std::max(elapsed(start),1e-9)}});
    std::cout << "Processed " << rendered << " frame(s).\n";
}
void live(const std::filesystem::path &executable, uint32_t stopAfter) {
    std::filesystem::create_directories(output);
    Json log{{"status", "live_started"},
             {"started_utc", timestamp()},
             {"backend", "Vulkan"},
             {"transfer", "affine"}};
    auto setup = Clock::now();
    try {
        const auto config = SimulationConfig::load(root / "live.json", true);
        const auto initial = config.loadParticles();
        const float grainRadius = 0.0018f * std::cbrt(65536.0f / initial.size());
        log["settings"] = config.json();
        log["rendering"] = "particle_spheres";
        log["particle_radius"] = grainRadius;
        log["particle_count"] = initial.size();
        Window window;
        if (!glfwInit())
            throw std::runtime_error("live mode requires a desktop display");
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        window.handle = glfwCreateWindow(1024, 640, "CK-MPM live sand", nullptr, nullptr);
        if (!window.handle)
            throw std::runtime_error("cannot create live window");
        Controls controls;
        controls.livePhysics = true;
        controls.camera.position = {1.05f, .65f, 1.25f};
        controls.camera.target = {.5f, .22f, .6f};
        controls.camera.nearPlane = .01f;
        controls.camera.farPlane = 10;
        configureControls(window.handle, controls);
        VulkanContext context(window.handle, true, true);
        Simulation simulation(context, executable.parent_path(), config, initial);
        const float defaultDt = config.timestep();
        log["dt_seconds"] = defaultDt;
        log["simulated_seconds_per_frame"] = double(defaultDt) * config.stepsPerFrame;
        if (!std::isfinite(defaultDt) || defaultDt <= 0)
            throw std::runtime_error("invalid live timestep");
        simulation.advance(1, defaultDt);
        simulation.reset(initial);
        Renderer renderer(context, executable.parent_path(), initial.size(), grainRadius,
                          config.boundary, &simulation.particleBuffer());
        log["gpu"] = {{"name", context.properties().deviceName.data()},
                      {"driver_version", context.properties().driverVersion},
                      {"api_version", context.properties().apiVersion},
                      {"validation", context.validationEnabled()},
                      {"sync_validation", context.syncValidationEnabled()}};
        log["setup_wall_seconds"] = elapsed(setup);
        uint32_t rendered = 0, particleUpdates = 0, resets = 0, maxObservedSteps = 0;
        uint64_t totalSteps = 0;
        double totalSimulated = 0, activeWall = 0, physicsWall = 0, readbackWall = 0;
        std::array<double, 5> gpuMs{};
        double displayFps = 0, displaySpeed = 1, lastReportWall = 0, lastReportSim = 0,
               lastReportActive = 0;
        uint32_t lastReportFrames = 0;
        auto start = Clock::now(), previous = start;
        std::cout << "Live affine physics + particle spheres: " << initial.size() << " particles, "
                  << config.grid
                  << "^3 physics grid, dt=" << defaultDt << " s, "
                  << config.stepsPerFrame << " steps/frame, uncapped rendering.\nSpace pause | R/Home reset | Tab mouse | WASD/Q/E camera | Esc close"
                  << std::endl;
        while (!glfwWindowShouldClose(window.handle) && (!stopAfter || rendered < stopAfter)) {
            auto frameStart = Clock::now();
            const double frameSeconds = elapsed(previous);
            previous = Clock::now();
            glfwPollEvents();
            moveCamera(window.handle, controls, std::min(frameSeconds, .1));
            if (controls.restart) {
                simulation.reset(initial);
                controls.restart = false;
                controls.time = 0;
                ++resets;
            }
            const bool advancing = !controls.paused;
            if (advancing) {
                const uint32_t steps = config.stepsPerFrame;
                const float dt = defaultDt;
                const double advance = double(steps) * dt;
                const auto before = simulation.gpuMilliseconds;
                auto clock = Clock::now();
                simulation.advance(steps, dt);
                const double physicsSeconds = elapsed(clock);
                physicsWall += physicsSeconds;
                maxObservedSteps = std::max(maxObservedSteps, steps);
                for (size_t i = 0; i < 5; ++i)
                    gpuMs[i] += simulation.gpuMilliseconds[i] - before[i];
                totalSteps += steps;
                totalSimulated += advance;
                controls.time += advance;
                clock = Clock::now();
                const auto health = simulation.health();
                if (health.flags || !std::isfinite(health.maxSpeed))
                    throw std::runtime_error("invalid live physics state: GPU flags=" +
                        std::to_string(health.flags) + " particle=" + std::to_string(health.firstBadParticle));
                log["max_particle_speed"] = health.maxSpeed;
                readbackWall += elapsed(clock);
                ++particleUpdates;
            }
            if (!context.beginFrame({.035f, .045f, .065f, 1})) {
                if (advancing) activeWall += elapsed(frameStart);
                continue;
            }
            renderer.prepareFrame();
            context.beginRendering({.035f, .045f, .065f, 1});
            renderer.draw(controls.camera, 0);
            context.endFrame();
            ++rendered;
            if (advancing) activeWall += elapsed(frameStart);
            const double wall = elapsed(start);
            if (wall - lastReportWall >= .5) {
                displayFps = (rendered - lastReportFrames) / (wall - lastReportWall);
                displaySpeed = (totalSimulated - lastReportSim) /
                               std::max(activeWall - lastReportActive, 1e-9);
                std::cout << "dt=" << defaultDt << " s | " << config.stepsPerFrame
                          << " steps/frame | sim/frame=" << double(defaultDt)*config.stepsPerFrame
                          << " s | " << displayFps << " FPS | physics " << displaySpeed << "x"
                          << (controls.paused ? " | paused" : "") << std::endl;
                lastReportWall = wall;
                lastReportFrames = rendered;
                lastReportSim = totalSimulated;
                lastReportActive = activeWall;
            }
            std::ostringstream title;
            title << "CK-MPM LIVE affine | " << std::fixed << std::setprecision(2) << controls.time
                  << " s | " << std::setprecision(1) << displayFps << " FPS | physics "
                  << std::setprecision(2) << displaySpeed << "x | "
                  << (controls.paused ? "paused | " : "")
                  << "Particle spheres";
            glfwSetWindowTitle(window.handle, title.str().c_str());
        }
        context.device().waitIdle();
        log["status"] = "live_complete";
        log["wall_seconds"] = elapsed(start);
        log["active_wall_seconds"] = activeWall;
        log["simulated_seconds"] = totalSimulated;
        log["effective_simulation_speed"] = totalSimulated / std::max(activeWall, 1e-9);
        log["rendered_frames"] = rendered;
        log["mean_render_fps"] = rendered / std::max(elapsed(start), 1e-9);
        log["substeps"] = totalSteps;
        log["particle_updates"] = particleUpdates;
        log["resets"] = resets;
        log["max_observed_substeps_per_frame"] = maxObservedSteps;
        log["physics_wall_seconds"] = physicsWall;
        log["health_check_seconds"] = readbackWall;
        log["particle_readback_bytes"] = 0;
        log["particle_upload_bytes"] = 0;
        log["health_readback_bytes"] = uint64_t(particleUpdates) * sizeof(Simulation::Health);
        log["gpu_resident_particles"] = true;
        for (size_t i = 0; i < 5; ++i)
            log["gpu_profile_ms"][Simulation::stageNames[i]] = gpuMs[i];
        log["raster_frames"] = rendered;
        log["physics_buffer_bytes"] =
            initial.size() * sizeof(Particle) +
            uint64_t(config.parameters(initial.size(), defaultDt).dims.w) * sizeof(glm::vec4);
        log["gpu_particle_buffer_bytes"] = renderer.particleBufferBytes();
        log["scene_cache_bytes_written"] = 0;
        log["validation_messages"] = context.validationMessages();
        log["all_checked_state_finite"] = true;
        if (context.validationMessages())
            throw std::runtime_error("live mode produced Vulkan validation messages");
        logRun(log);
        std::cout << "dt=" << defaultDt << " s | " << config.stepsPerFrame
                  << " steps/frame | sim/frame=" << double(defaultDt)*config.stepsPerFrame
                  << " s\nLive: " << log["mean_render_fps"] << " FPS | physics "
                  << log["effective_simulation_speed"]
                  << "x wall time | cache bytes written: 0\nProcessed " << rendered
                  << " frame(s).\n";
    } catch (const std::exception &e) {
        log["status"] = "live_failed";
        log["error"] = e.what();
        logRun(log);
        throw;
    }
}

template <class T> void dump(const std::filesystem::path &file, const std::vector<T> &values) {
    std::ofstream out(file, std::ios::binary);
    out.exceptions(std::ios::badbit | std::ios::failbit);
    out.write(reinterpret_cast<const char *>(values.data()), values.size() * sizeof(T));
}
void fixtures(const std::filesystem::path &directory, const std::filesystem::path &shaders, bool healthOnly = false) {
    auto config = SimulationConfig::load(directory / "config.json");
    auto path = directory / "input.bin";
    auto bytes = std::filesystem::file_size(path);
    if (bytes == 0 || bytes % sizeof(Particle) || bytes > 4096 * sizeof(Particle))
        throw std::runtime_error("invalid fixture input");
    std::vector<Particle> input(bytes / sizeof(Particle));
    std::ifstream in(path, std::ios::binary);
    in.read(reinterpret_cast<char *>(input.data()), bytes);
    if (!in)
        throw std::runtime_error("fixture read failed");
    VulkanContext context(nullptr, true);
    Simulation simulation(context, shaders, config, input);
    auto health = simulation.health();
    Json healthResult{{"max_speed", health.maxSpeed}, {"flags", health.flags},
                      {"first_bad_particle", health.firstBadParticle},
                      {"validation_messages", context.validationMessages()}};
    std::ofstream(directory / "health.json") << healthResult.dump(2);
    if (healthOnly) {
        if (context.validationMessages()) throw std::runtime_error("health fixture validation errors");
        return;
    }
    simulation.stage(Simulation::Material, config.timestep());
    dump(directory / "material.bin", simulation.readParticles());
    simulation.stage(Simulation::Clear, config.timestep());
    simulation.stage(Simulation::P2G, config.timestep());
    dump(directory / "p2g.bin", simulation.readGrid());
    simulation.stage(Simulation::GridUpdate, config.timestep());
    dump(directory / "grid.bin", simulation.readGrid());
    simulation.stage(Simulation::G2P, config.timestep());
    dump(directory / "g2p.bin", simulation.readParticles());
    simulation.stage(Simulation::Material, config.timestep());
    dump(directory / "final.bin", simulation.readParticles());
    simulation.stage(Simulation::Clear, config.timestep());
    simulation.stage(Simulation::P2G, config.timestep());
    dump(directory / "second_p2g.bin", simulation.readGrid());
    simulation.stage(Simulation::GridUpdate, config.timestep());
    dump(directory / "second_grid.bin", simulation.readGrid());
    simulation.stage(Simulation::G2P, config.timestep());
    simulation.stage(Simulation::Material, config.timestep());
    dump(directory / "second_final.bin", simulation.readParticles());
    Json metadata{{"gpu", context.properties().deviceName.data()},
                  {"validation", context.validationEnabled()},
                  {"sync_validation", context.syncValidationEnabled()},
                  {"validation_messages", context.validationMessages()}};
    std::ofstream(directory / "device.json") << metadata.dump(2);
    if (context.validationMessages())
        throw std::runtime_error("fixture validation errors");
}
} // namespace
int main(int argc, char **argv) {
    try {
        const auto exe = platform::executablePath();
        const std::string command = argc >= 2 ? argv[1] : "help";
        if (command == "calculate" && argc <= 3)
            calculate(exe, argc == 3 ? std::filesystem::path(argv[2]) : root / "sand.json");
        else if ((command == "play" || command == "live") &&
                 (argc == 2 || (argc == 4 && std::string(argv[2]) == "--frames"))) {
            uint32_t n = 0;
            if (argc == 4) {
                std::string value = argv[3];
                size_t end = 0;
                auto parsed = std::stoull(value, &end);
                if (value.empty() || value[0] == '-' || end != value.size() || parsed > UINT32_MAX)
                    throw std::runtime_error("invalid frame count");
                n = static_cast<uint32_t>(parsed);
            }
            if (command == "live")
                live(exe, n);
            else
                play(n);
        } else if ((command == "_fixtures" || command == "_health") && argc == 3)
            fixtures(argv[2], exe.parent_path(), command == "_health");
        else if (command == "evaluate" && argc == 2) {
            CalculationLock lock;
            return evaluate(exe);
        } else if (command == "help" || command == "--help")
            std::cout
                << "Usage:\n"
                   "  ./run.py calculate [settings.json]   Calculate and replace the saved scene\n"
                   "  ./run.py live [--frames N]           Live reduced-detail physics and particle spheres\n"
                   "  ./run.py play [--frames N]           Play saved affine particles\n"
                   "  ./run.py evaluate                    Run numerical reference checks\n"
                   "  ./run.py test                        Build and run the ctest suite (run.py only)\n";
        else
            throw std::runtime_error("unknown command; use --help");
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "Error: " << e.what() << '\n';
        return 1;
    }
}
