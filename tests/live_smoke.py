#!/usr/bin/env python3
"""Desktop/GPU integration check: live evolution without touching scene caches."""
import json
import os
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
paths = [root/'output/scene.bin', root/'output/surface.bin']
def snapshot():
    return [(p.stat().st_size, p.stat().st_mtime_ns) if p.exists() else None for p in paths]

before = snapshot()
binary = root/'build'/('ck_mpm_starter.exe' if os.name == 'nt' else 'ck_mpm_starter')
result = subprocess.run([str(binary), 'live', '--frames', '180'], cwd=root,
                        capture_output=True, text=True)
print(result.stdout, end='')
print(result.stderr, end='')
assert result.returncode == 0, 'live run failed'
assert '[Vulkan validation]' not in result.stderr, 'Vulkan validation reported errors'
assert before == snapshot(), 'live mode changed a saved particle/surface cache'
report = json.loads((root/'output/performance.jsonl').read_text().splitlines()[-1])
assert report['transfer'] == 'affine'
assert report['status'] == 'live_complete' and report['rendered_frames'] == 180
assert report['scene_cache_bytes_written'] == 0 and report['validation_messages'] == 0
assert report['all_checked_state_finite']
assert report['rendering'] == 'particle_spheres'
assert report['raster_frames'] == 180
assert report['gpu_resident_particles']
assert report['particle_readback_bytes'] == 0 and report['particle_upload_bytes'] == 0
assert report['gpu_particle_buffer_bytes'] == 0
assert report['health_readback_bytes'] == 16*report['particle_updates']
steps = report['settings']['steps_per_frame']
assert report['max_observed_substeps_per_frame'] == steps
assert report['substeps'] == steps*report['particle_updates']
expected_time = report['substeps']*report['dt_seconds']
assert abs(report['simulated_seconds']-expected_time) < 1e-6*max(1, expected_time)
assert report['effective_simulation_speed'] > 0
assert 'steps/frame' in result.stdout and 'FPS' in result.stdout
print(f"Live integration passed: {report['mean_render_fps']:.1f} FPS, "
      f"{report['effective_simulation_speed']:.3f}x simulation speed; scene caches unchanged.")
