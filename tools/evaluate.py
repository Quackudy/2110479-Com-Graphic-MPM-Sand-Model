#!/usr/bin/env python3
"""Small deterministic stage checks; no cached scene is read or replaced."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from datetime import datetime, timezone
import numpy as np


def matrices(p, start):
    return p[:, start:start+12].reshape(-1, 3, 4)[:, :, :3].transpose(0, 2, 1).astype(np.float64)


def store(p, start, a):
    p[:, start:start+12].reshape(-1, 3, 4)[:, :, :3] = a.transpose(0, 2, 1)


def material(p, cfg):
    out = p.copy()
    F = matrices(p, 8)
    U, S, Vh = np.linalg.svd(F)
    eps = np.log(np.maximum(np.abs(S), 1e-4))
    total = eps.sum(axis=1)
    trace = total + p[:, 32]
    dev = eps-trace[:, None]/3
    norm = np.linalg.norm(dev, axis=1)
    mu = cfg['young_modulus']/(2*(1+cfg['poisson_ratio']))
    lam = cfg['young_modulus']*cfg['poisson_ratio']/((1+cfg['poisson_ratio'])*(1-2*cfg['poisson_ratio']))
    sin_phi = np.sin(np.deg2rad(cfg['friction_angle']))
    ys = np.sqrt(2/3)*2*sin_phi/(3-sin_phi)
    dg = norm+(3*lam+2*mu)/(2*mu)*trace*ys
    H = eps-np.maximum(dg, 0)[:, None]*dev/np.maximum(norm[:, None], 1e-20)
    newS = np.exp(H)
    positive = trace >= 0
    newS[positive] = 1
    out[:, 32] = np.where(positive, p[:, 32]+total, 0)
    newF = (U*newS[:, None, :])@Vh
    logs = np.log(newS)
    phat = (2*mu*logs+lam*logs.sum(axis=1)[:, None])/newS
    PF = ((U*phat[:, None, :])@Vh)@newF.transpose(0, 2, 1)
    store(out, 8, newF)
    store(out, 20, PF)
    return out


def neighbors(position, cfg):
    n = cfg['grid']
    for g in range(2 if cfg['kernel'] == 'ck' else 1):
        xp = np.asarray(position, dtype=np.float64)*n
        if cfg['kernel'] == 'ck':
            shift = -0.25 if g == 0 else 0.25
            base = np.floor(xp-shift).astype(int)
            d = xp-(base+shift)
            w0 = 1-d+np.sin(2*np.pi*d)/(2*np.pi)
            w = np.array([w0, 1-w0])
        else:
            base = np.floor(xp-0.5).astype(int)
            d = xp-base
            w = np.array([0.5*(1.5-d)**2, 0.75-(d-1)**2, 0.5*(d-0.5)**2])
        for z in range(len(w)):
            for y in range(len(w)):
                for x in range(len(w)):
                    c = base+[x, y, z]
                    if (c < 0).any() or (c >= n).any():
                        continue
                    a = np.array([w[x, 0], w[y, 1], w[z, 2]])
                    weight = a.prod()
                    offset = (c+(shift if cfg['kernel'] == 'ck' else 0))/n-position
                    yield ((g*n+c[2])*n+c[1])*n+c[0], weight, offset


def p2g(p, cfg):
    n = cfg['grid']
    grid = np.zeros(((2 if cfg['kernel'] == 'ck' else 1)*n**3, 4))
    volume = 1e-7*255951/cfg['particles_per_model']
    mass = volume*cfg['density']
    pf = matrices(p, 20)
    C = matrices(p, 36)
    for k, particle in enumerate(p):
        support = list(neighbors(particle[:3], cfg))
        grids = 2 if cfg['kernel'] == 'ck' else 1
        D = sum(w*np.outer(r, r) for _, w, r in support)/grids
        contribution = mass*C[k]-cfg['dt']*volume*(np.linalg.inv(D)@pf[k])
        for i, w, r in support:
            grid[i, :3] += w*(mass*particle[4:7]+contribution@r)
            grid[i, 3] += w*mass
    return grid


def grid_update(grid, cfg):
    out = grid.copy()
    cutoff = 1e-7*255951/cfg['particles_per_model']*cfg['density']*1e-8
    active = out[:, 3] > cutoff
    out[:, :3] = 0
    out[active, :3] = grid[active, :3]/grid[active, 3, None]
    out[active, 1] += cfg['dt']*cfg['gravity']
    n = cfg['grid']
    indices = np.arange(len(out))
    coords = np.array([indices % n, (indices//n) % n, (indices//(n*n)) % n]).T
    b = cfg['boundary']*n
    boundary = ((coords < b) | (coords >= n-b)).any(axis=1)
    out[boundary, :3] = 0
    return out


def g2p(p, grid, cfg):
    out = p.copy()
    F = matrices(p, 8)
    average = 0.5 if cfg['kernel'] == 'ck' else 1
    C = np.zeros((len(p), 3, 3))
    for k, particle in enumerate(p):
        velocity = np.zeros(3)
        B, D = np.zeros((3, 3)), np.zeros((3, 3))
        for i, w, r in neighbors(particle[:3], cfg):
            velocity += w*grid[i, :3]
            B += w*np.outer(grid[i, :3], r)
            D += w*np.outer(r, r)
        velocity *= average
        C[k] = B@np.linalg.inv(D)
        out[k, :3] = particle[:3]+cfg['dt']*velocity
        out[k, 4:7] = velocity
        F[k] = (np.eye(3)+cfg['dt']*C[k])@F[k]
    store(out, 8, F)
    store(out, 36, C)
    return out


def fixture():
    rng = np.random.default_rng(20261103)
    p = np.zeros((64, 48), dtype='<f4')
    p[:, :3] = rng.uniform(0.3, 0.7, (64, 3))
    p[:6, :3] = [[0.12, .5, .5], [.88, .5, .5], [.5, .12, .5], [.5, .88, .5], [.5, .5, .12], [.5, .5, .88]]
    p[:, 4:7] = rng.normal(0, 0.1, (64, 3))
    p[:, 3] = np.arange(64) >= 32
    F = np.eye(3)[None, :, :]+rng.normal(0, 0.015, (64, 3, 3))
    F[0] = np.eye(3)
    F[1] = np.eye(3)*.8
    F[2] = np.eye(3)*1.2
    F[3] = [[1, .4, 0], [0, 1, 0], [0, 0, 1]]
    F[4] = np.diag([.7, .9, 1.1])
    F[5] = [[0, -1, 0], [1, 0, 0], [0, 0, 1]]
    # Repeated and unequal stretches, rotations, and both plastic volume branches.
    F[6] = np.diag([.05, .9, 1.2])
    p[7:12, 32] = [-.1, .1, -.02, .02, .2]
    store(p, 8, F)
    store(p, 36, rng.normal(0, .5, (len(p), 3, 3)))
    return p


def error(a, b, scale=1):
    if a.shape != b.shape or not np.isfinite(a).all() or not np.isfinite(b).all():
        raise AssertionError('shape mismatch or non-finite reference result')
    delta = np.asarray(a, dtype=float)-b
    return {'max_abs': float(np.abs(delta).max()), 'rms': float(np.sqrt(np.mean(delta**2))),
            'relative_l2': float(np.linalg.norm(delta)/max(np.linalg.norm(b), 1e-30)),
            'max_normalized': float(np.abs(delta).max()/scale), 'normalization_scale': float(scale)}


def run(executable, report_path):
    root = Path(__file__).resolve().parents[1]
    report = {'status': 'running', 'created_utc': datetime.now(timezone.utc).isoformat(),
              'numpy_version': np.__version__, 'fixture_seed': 20261103,
              'reference': 'float64 NumPy affine stages following CUDA G2P2G, plus original CUDA sand material when available',
              'taichi_comparison': 'not_run; create .venv-reference with tools/requirements-reference.txt',
              'scope': '64 deterministic particles, CK and quadratic, two steps, material/boundary cases plus analytic translation/rotation/shear',
              'tolerance_note': 'Engineering tolerances for float32 shaders versus float64 CPU / original approximate CUDA SVD; not paper accuracy claims',
              'shader_sha256': {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in executable.parent.glob('*.spv')},
              'checks': {}}
    report['reference_source_sha256'] = {
        str(p.relative_to(root.parent)): hashlib.sha256(p.read_bytes()).hexdigest()
        for p in [root/'tools/taichi_reference.py', root/'tools/evaluate.py',
                  root.parent/'CK-MPM/include/mpm_material.cuh',
                  root.parent/'CK-MPM/include/mpm_math.cuh',
                  root.parent/'CK-MPM/include/mpm_algorithm.cuh'] if p.exists()}
    report_path.parent.mkdir(parents=True, exist_ok=True)
    try:
        with tempfile.TemporaryDirectory(prefix='ck-mpm-check-') as tmp:
            directory = Path(tmp)
            cfg = {'assets': str(root.parent/'CK-MPM/asset/particle_data'), 'grid': 32, 'particles_per_model': 32,
                   'fps': 24, 'seconds': 1, 'young_modulus': 10000, 'poisson_ratio': .4, 'density': 2,
                   'gravity': -2, 'friction_angle': 30, 'cfl': .25, 'dt': 1e-6, 'boundary': .125}
            p = fixture()
            p.tofile(directory/'input.bin')
            expected_material = material(p, cfg)
            reference = executable.parent/'cuda_material_reference'
            cuda_material = None
            if reference.exists():
                mu = cfg['young_modulus']/(2*(1+cfg['poisson_ratio']))
                lam = cfg['young_modulus']*cfg['poisson_ratio']/((1+cfg['poisson_ratio'])*(1-2*cfg['poisson_ratio']))
                subprocess.run([str(reference), str(directory/'input.bin'), str(directory/'cuda.bin'), str(mu), str(lam)], check=True)
                cuda_material = np.fromfile(directory/'cuda.bin', dtype='<f4').reshape(-1, 48)
                report['cuda_material_reference'] = 'original unmodified CK-MPM ComputeStress and svd, executed on CUDA GPU'
            else:
                report['cuda_material_reference'] = 'not_run; build optional cuda_reference target with CUDA toolkit and author repo'
            taichi_python = root/'.venv-reference/bin/python'
            for kernel in ['ck', 'quadratic']:
                p.tofile(directory/'input.bin')
                cfg['kernel'] = kernel
                (directory/'config.json').write_text(json.dumps(cfg))
                subprocess.run([str(executable), '_fixtures', str(directory)], check=True)
                report['device'] = json.loads((directory/'device.json').read_text())
                def read(name, columns):
                    return np.fromfile(directory/f'{name}.bin', dtype='<f4').reshape(-1, columns)
                checks = report['checks'][kernel] = {}
                health = json.loads((directory/'health.json').read_text())
                if health['flags'] or health['validation_messages']:
                    raise AssertionError(f'health check rejected valid fixture: {health}')
                actual_material = read('material', 48)
                def compare(name, actual, expected, scale, tolerance):
                    m = error(actual, expected, scale)
                    m['tolerance_max_normalized'] = tolerance
                    m['passed'] = m['max_normalized'] <= tolerance
                    checks[name] = m
                    if not m['passed']:
                        raise AssertionError(f'{kernel}/{name}: {m}')
                compare('health_max_speed', np.array([health['max_speed']]),
                        np.array([np.linalg.norm(p[:, 4:7], axis=1).max()]), 1, 2e-6)
                compare('material_F_numpy', matrices(actual_material, 8), matrices(expected_material, 8), 1, 2e-5)
                compare('material_logJp_numpy', actual_material[:, 32], expected_material[:, 32], 1, 2e-5)
                compare('material_stress_numpy', matrices(actual_material, 20), matrices(expected_material, 20), cfg['young_modulus'], 1e-4)
                if cuda_material is not None:
                    compare('material_F_cuda', matrices(actual_material, 8), matrices(cuda_material, 8), 1, 2e-4)
                    compare('material_logJp_cuda', actual_material[:, 32], cuda_material[:, 32], 1, 2e-4)
                    compare('material_stress_cuda', matrices(actual_material, 20), matrices(cuda_material, 20), cfg['young_modulus'], 5e-4)
                # Feed the GPU material output to CPU P2G to isolate transfer errors.
                expected_p2g = p2g(actual_material, cfg)
                actual_p2g = read('p2g', 4)
                mass = 1e-7*255951/cfg['particles_per_model']*cfg['density']
                compare('p2g_mass', actual_p2g[:, 3], expected_p2g[:, 3], mass, 2e-5)
                compare('p2g_momentum', actual_p2g[:, :3], expected_p2g[:, :3], mass, 2e-4)
                grids = 2 if kernel == 'ck' else 1
                compare('total_grid_mass', np.array([actual_p2g[:, 3].sum()]), np.array([len(p)*mass*grids]), len(p)*mass*grids, 2e-6)
                expected_grid = grid_update(actual_p2g, cfg)
                actual_grid = read('grid', 4)
                compare('grid_velocity', actual_grid[:, :3], expected_grid[:, :3], 1, 2e-5)
                actual_g2p = read('g2p', 48)
                expected_g2p = g2p(actual_material, actual_grid, cfg)
                compare('g2p_position_cells', actual_g2p[:, :3], expected_g2p[:, :3], 1/cfg['grid'], 1e-5)
                compare('g2p_velocity', actual_g2p[:, 4:7], expected_g2p[:, 4:7], 1, 2e-5)
                compare('g2p_affine', matrices(actual_g2p, 36), matrices(expected_g2p, 36), cfg['grid'], 3e-5)
                compare('g2p_F', matrices(actual_g2p, 8), matrices(expected_g2p, 8), 1, 2e-5)
                final = read('final', 48)
                expected_final = material(expected_g2p, cfg)
                compare('final_F', matrices(final, 8), matrices(expected_final, 8), 1, 3e-5)
                compare('final_stress', matrices(final, 20), matrices(expected_final, 20), cfg['young_modulus'], 2e-4)
                # A second step catches lost affine state and using the old-position moment in P2G.
                second_p2g = read('second_p2g', 4)
                compare('second_p2g_momentum', second_p2g[:, :3], p2g(final, cfg)[:, :3], mass, 2e-4)
                second_grid = read('second_grid', 4)
                second_final = read('second_final', 48)
                second_expected = material(g2p(final, second_grid, cfg), cfg)
                compare('second_affine', matrices(second_final, 36), matrices(second_expected, 36), cfg['grid'], 3e-5)
                compare('second_F', matrices(second_final, 8), matrices(second_expected, 8), 1, 3e-5)
                if taichi_python.exists():
                    subprocess.run([str(taichi_python), str(root/'tools/taichi_reference.py'), str(directory)], check=True)
                    report['taichi_comparison'] = 'Taichi Vulkan affine, CUDA G2P2G equations adapted to dense fixtures plus checked sand material'
                    report['taichi_version'] = (directory/'taichi_version.txt').read_text()
                    tm = read('taichi_material', 48)
                    tp = read('taichi_p2g', 4)
                    tg = read('taichi_grid', 4)
                    tv = read('taichi_g2p', 48)
                    tf = read('taichi_final', 48)
                    compare('material_F_taichi', matrices(actual_material, 8), matrices(tm, 8), 1, 2e-4)
                    compare('material_stress_taichi', matrices(actual_material, 20), matrices(tm, 20), cfg['young_modulus'], 5e-4)
                    compare('p2g_mass_taichi', actual_p2g[:, 3], tp[:, 3], mass, 2e-5)
                    compare('p2g_momentum_taichi', actual_p2g[:, :3], tp[:, :3], mass, 5e-4)
                    compare('grid_velocity_taichi', actual_grid[:, :3], tg[:, :3], 1, 1e-3)
                    compare('g2p_position_cells_taichi', actual_g2p[:, :3], tv[:, :3], 1/cfg['grid'], 2e-5)
                    compare('g2p_velocity_taichi', actual_g2p[:, 4:7], tv[:, 4:7], 1, 5e-4)
                    compare('g2p_affine_taichi', matrices(actual_g2p, 36), matrices(tv, 36), cfg['grid'], 1e-3)
                    compare('g2p_F_taichi', matrices(actual_g2p, 8), matrices(tv, 8), 1, 2e-4)
                    compare('final_F_taichi', matrices(final, 8), matrices(tf, 8), 1, 3e-4)
                    compare('final_stress_taichi', matrices(final, 20), matrices(tf, 20), cfg['young_modulus'], 5e-4)
                # Analytic fields catch PIC averaging even if both numerical references agree.
                affine_cfg = dict(cfg, gravity=0)
                (directory/'config.json').write_text(json.dumps(affine_cfg))
                rng = np.random.default_rng(712)
                for name, A in [('translation', np.zeros((3, 3))),
                                ('rotation', np.array([[0., -1, 0], [1, 0, 0], [0, 0, 0]])),
                                ('shear', np.array([[0., .75, 0], [0, 0, 0], [0, 0, 0]]))]:
                    field = np.zeros((64, 48), dtype='<f4')
                    field[:, :3] = rng.uniform(.35, .65, (64, 3))
                    field[:, 4:7] = field[:, :3]@A.T+np.array([.2, -.1, .3])
                    store(field, 8, np.broadcast_to(np.eye(3), (64, 3, 3)))
                    store(field, 36, np.broadcast_to(A, (64, 3, 3)))
                    field.tofile(directory/'input.bin')
                    subprocess.run([str(executable), '_fixtures', str(directory)], check=True)
                    result = read('g2p', 48)
                    compare(f'analytic_{name}_velocity', result[:, 4:7], field[:, 4:7], 1, 2e-5)
                    compare(f'analytic_{name}_affine', matrices(result, 36),
                            np.broadcast_to(A, (64, 3, 3)), 1, 5e-5)
                    compare(f'analytic_{name}_F', matrices(result, 8),
                            np.broadcast_to(np.eye(3)+cfg['dt']*A, (64, 3, 3)), 1, 2e-5)
                # Exercise multiple reduction groups, a partial final group, and every error flag.
                healthy = np.repeat(field[:1], 257, axis=0)
                healthy[:, 4:7] = 0
                healthy[:, 4] = np.arange(257)*.01
                for name, index, column, value, flag in [
                        ('valid', 0, 0, None, 0), ('nan_F', 129, 8, np.nan, 1),
                        ('inf_affine', 256, 36, np.inf, 1), ('domain', 17, 0, -.1, 2),
                        ('stencil', 128, 0, .001, 4)]:
                    health_input = healthy.copy()
                    if value is not None:
                        health_input[index, column] = value
                    health_input.tofile(directory/'input.bin')
                    subprocess.run([str(executable), '_health', str(directory)], check=True)
                    summary = json.loads((directory/'health.json').read_text())
                    passed = (summary['flags'] == flag and summary['validation_messages'] == 0
                              and summary['first_bad_particle'] == (index if flag else 4294967295))
                    checks[f'health_{name}'] = {'passed': passed, 'summary': summary}
                    if not passed:
                        raise AssertionError(f'{kernel}/health_{name}: {summary}')
                    compare(f'health_{name}_speed', np.array([summary['max_speed']]),
                            np.array([2.56]), 1, 2e-6)
            report['status'] = 'pass' if cuda_material is not None and taichi_python.exists() else 'pass_available_checks'
    except Exception as exc:
        report['status'] = 'fail'
        report['error'] = str(exc)
    report_path.write_text(json.dumps(report, indent=2)+'\n')
    print(f"Evaluation: {report['status']} — {report_path}", flush=True)
    if report['status'] == 'fail':
        print(report['error'], file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    if len(sys.argv) != 3:
        raise SystemExit('usage: evaluate.py executable report.json')
    raise SystemExit(run(Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()))
