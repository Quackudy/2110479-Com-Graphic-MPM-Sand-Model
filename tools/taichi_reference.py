#!/usr/bin/env python3
"""Author's affine stage equations adapted to small dense fixtures, plus sand.

Transfer reference: CK-MPM/include/mpm_algorithm.cuh, G2P2G (Apache-2.0).
Sand reference: CK-MPM/include/mpm_material.cuh, Drucker-Prager overload.
This intentionally keeps separate stages for comparison with native Vulkan.
"""
import json
from pathlib import Path
import sys
import numpy as np
import taichi as ti


@ti.data_oriented
class Reference:
    def __init__(self, p, cfg):
        self.cfg = cfg
        self.count = len(p)
        self.n = cfg['grid']
        self.method = 0 if cfg['kernel'] == 'ck' else 1
        self.grids = 2 if self.method == 0 else 1
        self.radius = 2 if self.method == 0 else 3
        self.dt = cfg['dt']
        self.volume = 1e-7*255951/cfg['particles_per_model']
        self.mass = self.volume*cfg['density']
        self.mu = cfg['young_modulus']/(2*(1+cfg['poisson_ratio']))
        self.lam = cfg['young_modulus']*cfg['poisson_ratio']/((1+cfg['poisson_ratio'])*(1-2*cfg['poisson_ratio']))
        s = np.sin(np.deg2rad(cfg['friction_angle']))
        self.ys = np.sqrt(2/3)*2*s/(3-s)
        self.p = ti.field(ti.f32, shape=(len(p), 48))
        self.grid = ti.Vector.field(4, ti.f32, shape=self.grids*self.n**3)
        self.p.from_numpy(p)

    @ti.func
    def matrix(self, p, start):
        F = ti.Matrix.zero(ti.f32, 3, 3)
        for r, c in ti.static(ti.ndrange(3, 3)):
            F[r, c] = self.p[p, start+4*c+r]
        return F

    @ti.func
    def save_matrix(self, p, start, F):
        for r, c in ti.static(ti.ndrange(3, 3)):
            self.p[p, start+4*c+r] = F[r, c]

    @ti.func
    def position(self, p):
        return ti.Vector([self.p[p, 0], self.p[p, 1], self.p[p, 2]])

    @ti.func
    def weights(self, pos, g):
        base = ti.Vector.zero(ti.i32, 3)
        w = ti.Matrix.zero(ti.f32, 3, 3)
        if ti.static(self.method == 0):
            shift = -0.25 if g == 0 else 0.25
            base = ti.cast(ti.floor(pos*self.n-shift), ti.i32)
            d = pos*self.n-(base+shift)
            for axis in ti.static(range(3)):
                w[axis, 0] = 1-d[axis]+ti.sin(2*np.pi*d[axis])/(2*np.pi)
                w[axis, 1] = 1-w[axis, 0]
        else:
            base = ti.cast(ti.floor(pos*self.n-0.5), ti.i32)
            d = pos*self.n-base
            for axis in ti.static(range(3)):
                w[axis, 0] = 0.5*(1.5-d[axis])**2
                w[axis, 1] = 0.75-(d[axis]-1)**2
                w[axis, 2] = 0.5*(d[axis]-0.5)**2
        return base, w

    @ti.func
    def weight(self, x, y, z, w):
        a, b, c = w[0, x], w[1, y], w[2, z]
        return a*b*c

    @ti.func
    def index(self, cell, g):
        return ((g*self.n+cell[2])*self.n+cell[1])*self.n+cell[0]

    @ti.kernel
    def material(self):
        for p in range(self.count):
            F = self.matrix(p, 8)
            U, Sigma, V = ti.svd(F)
            eps = ti.Vector.zero(ti.f32, 3)
            for a in ti.static(range(3)):
                eps[a] = ti.log(ti.max(ti.abs(Sigma[a, a]), 1e-4))
            total = eps.sum()
            trace = total+self.p[p, 32]
            dev = eps-trace/3
            newS = ti.Vector([1.0, 1.0, 1.0])
            if trace >= 0:
                self.p[p, 32] += total
            else:
                self.p[p, 32] = 0
                norm = dev.norm()
                dg = norm+(3*self.lam+2*self.mu)/(2*self.mu)*trace*self.ys
                H = eps
                if dg > 0:
                    H -= dg/ti.max(norm, 1e-20)*dev
                newS = ti.exp(H)
            diagonal = ti.Matrix.zero(ti.f32, 3, 3)
            for a in ti.static(range(3)):
                diagonal[a, a] = newS[a]
            F = U@diagonal@V.transpose()
            logs = ti.log(newS)
            for a in ti.static(range(3)):
                diagonal[a, a] = (2*self.mu*logs[a]+self.lam*logs.sum())/newS[a]
            PF = U@diagonal@V.transpose()@F.transpose()
            self.save_matrix(p, 8, F)
            self.save_matrix(p, 20, PF)

    @ti.kernel
    def clear(self):
        for i in self.grid:
            self.grid[i] = ti.Vector.zero(ti.f32, 4)

    @ti.kernel
    def p2g(self):
        for p in range(self.count):
            velocity = ti.Vector([self.p[p, 4], self.p[p, 5], self.p[p, 6]])
            PF = self.matrix(p, 20)
            C = self.matrix(p, 36)
            D = ti.Matrix.zero(ti.f32, 3, 3)
            for g in ti.static(range(self.grids)):
                base, w = self.weights(self.position(p), g)
                shift = 0.0
                if ti.static(self.method == 0):
                    shift = -0.25 if g == 0 else 0.25
                for x, y, z in ti.ndrange(self.radius, self.radius, self.radius):
                    cell = base+ti.Vector([x, y, z])
                    if (cell >= 0).all() and (cell < self.n).all():
                        weight = self.weight(x, y, z, w)
                        r = (cell+shift)/self.n-self.position(p)
                        D += weight*r.outer_product(r)
            contribution = self.mass*C-self.dt*self.volume*((D/self.grids).inverse()@PF)
            for g in ti.static(range(self.grids)):
                base, w = self.weights(self.position(p), g)
                for x, y, z in ti.ndrange(self.radius, self.radius, self.radius):
                    cell = base+ti.Vector([x, y, z])
                    if (cell >= 0).all() and (cell < self.n).all():
                        weight = self.weight(x, y, z, w)
                        shift = 0.0
                        if ti.static(self.method == 0):
                            shift = -0.25 if g == 0 else 0.25
                        r = (cell+shift)/self.n-self.position(p)
                        momentum = weight*(self.mass*velocity+contribution@r)
                        i = self.index(cell, g)
                        for a in ti.static(range(3)):
                            self.grid[i][a] += momentum[a]
                        self.grid[i][3] += weight*self.mass

    @ti.kernel
    def grid_update(self):
        for i in self.grid:
            velocity = ti.Vector.zero(ti.f32, 3)
            mass = self.grid[i][3]
            if mass > self.mass*1e-8:
                velocity = ti.Vector([self.grid[i][0], self.grid[i][1], self.grid[i][2]])/mass
                velocity[1] += self.dt*self.cfg['gravity']
            c = ti.Vector([i % self.n, (i//self.n) % self.n, (i//(self.n*self.n)) % self.n])
            b = self.cfg['boundary']*self.n
            if (c < b).any() or (c >= self.n-b).any():
                velocity = ti.Vector.zero(ti.f32, 3)
            for a in ti.static(range(3)):
                self.grid[i][a] = velocity[a]

    @ti.kernel
    def g2p(self):
        for p in range(self.count):
            velocity = ti.Vector.zero(ti.f32, 3)
            B, D = ti.Matrix.zero(ti.f32, 3, 3), ti.Matrix.zero(ti.f32, 3, 3)
            for g in ti.static(range(self.grids)):
                base, w = self.weights(self.position(p), g)
                for x, y, z in ti.ndrange(self.radius, self.radius, self.radius):
                    cell = base+ti.Vector([x, y, z])
                    if (cell >= 0).all() and (cell < self.n).all():
                        weight = self.weight(x, y, z, w)
                        i = self.index(cell, g)
                        u = ti.Vector([self.grid[i][0], self.grid[i][1], self.grid[i][2]])
                        velocity += weight*u
                        shift = 0.0
                        if ti.static(self.method == 0):
                            shift = -0.25 if g == 0 else 0.25
                        r = (cell+shift)/self.n-self.position(p)
                        B += weight*u.outer_product(r)
                        D += weight*r.outer_product(r)
            velocity /= self.grids
            C = B@D.inverse()
            F = (ti.Matrix.identity(ti.f32, 3)+self.dt*C)@self.matrix(p, 8)
            for a in ti.static(range(3)):
                self.p[p, a] += self.dt*velocity[a]
                self.p[p, 4+a] = velocity[a]
            self.save_matrix(p, 8, F)
            self.save_matrix(p, 36, C)


def main(directory):
    cfg = json.loads((directory/'config.json').read_text())
    p = np.fromfile(directory/'input.bin', dtype='<f4').reshape(-1, 48)
    ti.init(arch=ti.vulkan, default_fp=ti.f32, fast_math=False, offline_cache=False)
    sim = Reference(p, cfg)
    sim.material()
    sim.p.to_numpy().tofile(directory/'taichi_material.bin')
    sim.clear()
    sim.p2g()
    sim.grid.to_numpy().tofile(directory/'taichi_p2g.bin')
    sim.grid_update()
    sim.grid.to_numpy().tofile(directory/'taichi_grid.bin')
    sim.g2p()
    sim.p.to_numpy().tofile(directory/'taichi_g2p.bin')
    sim.material()
    sim.p.to_numpy().tofile(directory/'taichi_final.bin')
    (directory/'taichi_version.txt').write_text(ti.__version__.__str__())


if __name__ == '__main__':
    main(Path(sys.argv[1]))
