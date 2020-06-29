#!/usr/bin/env python3

import argparse
import math
import os
import sys
import numpy as np
import copy


def NormalizeType(v):
    if isinstance(v, (float, np.float, np.float32, np.float64)):
        v = float(v)
    elif isinstance(v, (int, np.int, np.int32, np.int64)):
        v = int(v)
    elif isinstance(v, str):
        v = str(v)
    elif isinstance(v, list):
        v = list(map(float, v))
    else:
        assert False, "unknown type of value '{:}'".format(str(v))
    return v


class Config:
    def __init__(self):
        self.var = dict()

    def __getitem__(self, key):
        return self.var[key]

    def __setitem__(self, key, value):
        self.var[key] = value

    def Generate(self):
        t = []
        for k, v in self.var.items():
            v = NormalizeType(v)
            if isinstance(v, float):
                t.append("set double {:} {:}".format(k, v))
            elif isinstance(v, int):
                t.append("set int {:} {:}".format(k, v))
            elif isinstance(v, list):
                v = ' '.join(map(str, v))
                t.append("set vect {:} {:}".format(k, v))
            elif isinstance(v, str):
                if '\n' in v:
                    v = '"' + v + '"'
                t.append("set string {:} {:}".format(k, v))
            else:
                assert False, "unknown type of value '{:}'".format(str(v))
        return '\n'.join(t)

    def GenerateFile(self, path):
        with open(path, 'w') as f:
            f.write(self.Generate())


def VectToStr(v):
    return ' '.join(map(str, v))


class Domain:
    """
    Description of the domain size, the number of blocks and processors.
    All arguments are optional but must provide enough
    information to define th domain size and mesh size.

    lx,ly,lz: `float`
        Domain length.
    nx,ny,nz: `int`
        Number of cells in each direction.
    h: `float`
        Cell length.
    extent: `float`
        Maximum domain length over all directions.
    bsx,bsy,bsz: `int`
        Block size in each direction.
    bx,by,bz: `int`
        Number of blocks in each direction.
    nproc: `int`
        Number of processors.
    """
    def __init__(self,
                 lx=None,
                 ly=None,
                 lz=None,
                 nx=None,
                 ny=None,
                 nz=None,
                 h=None,
                 extent=None,
                 bsx=None,
                 bsy=None,
                 bsz=None,
                 bs=32,
                 bx=None,
                 by=None,
                 bz=None,
                 nproc=None):
        conf = locals()

        def t(line):
            nonlocal conf
            try:
                exec(line, None, conf)
            except:
                pass

        def tt(lines):
            for line in lines:
                t(line)

        # TODO: detect conflicting parameters

        tt([
            "extent = max(lx, ly, lz)",
            "nmax = max(nx, ny, nz)",
            "bsx = bs if bsx is None else bsx",
            "bsy = bs if bsy is None else bsy",
            "bsz = bs if bsz is None else bsz",
            "bs = bsx",
            "h = lz / nz",
            "h = lx / nx",
            "h = ly / ny",
            "h = extent / nmax",
            "lx = float(nx * h)",
            "ly = float(ny * h)",
            "lz = float(nz * h)",
            "nx = int(lx / h + 0.5)",
            "ny = int(ly / h + 0.5)",
            "nz = int(lz / h + 0.5)",
            "bx = (nx + bsx - 1) // bsx",
            "by = (ny + bsy - 1) // bsy",
            "bz = (nz + bsz - 1) // bsz",
            "nx = bx * bsx",
            "ny = by * bsy",
            "nz = bz * bsz",
            "lx = float(nx * h)",
            "ly = float(ny * h)",
            "lz = float(nz * h)",
            "extent = max(lx, ly, lz)",
            "nmax = max(nx, ny, nz)",
        ])

        for k, v in conf.items():
            if k in ['self']:
                continue
            setattr(self, k, v)

    def __str__(self):
        return "Domain: {}".format(str(vars(self)))

    def compare(self, other):
        def isclose(a, b):
            return abs(a - b) < 1e-10

        diff = dict()
        v = vars(self)
        vo = vars(other)
        for k in v:
            if not isclose(v[k], vo[k]):
                diff[k] = (v[k], vo[k])
        return diff

    def printdiff(diff):
        for k, v in diff.items():
            print("{:>8}  {:<7} {:<7}".format(k, *v))


def AdjustedDomain(verbose=True,
                   decrease_nproc=True,
                   increase_size=False,
                   nproc_factor=0.9,
                   size_factor=1.1,
                   **kwargs):
    domain = Domain(**kwargs)
    newdomain = AdjustDomainToProcessors(
        domain,
        decrease_nproc=decrease_nproc,
        increase_size=increase_size,
        nproc_factor=nproc_factor,
        size_factor=size_factor,
    )
    assert newdomain, "Compatible domain not found for \n{:}".format(domain)
    diff = domain.compare(newdomain)
    if diff and verbose:
        print("Domain is adjusted. Changes:")
        Domain.printdiff(diff)
    domain = newdomain
    assert CheckDomain(domain), "Check failed for \n{:}".format(domain)
    return newdomain


def CheckDomain(domain):
    """
    Reports statistics and performs quality checks of the domain:
        - number of blocks is divisible by `nproc`

    Returns True if checks pass.
    """
    if (domain.bx * domain.by * domain.bz) % domain.nproc == 0:
        return True
    return False


def AdjustDomainToProcessors(domain,
                             decrease_nproc=True,
                             increase_size=False,
                             nproc_factor=0.9,
                             size_factor=1.1,
                             verbose=True):
    """
    Adjusts the domain size and the number of processors
    to make the number of blocks divisible by `domain.nproc`.

    domain: `Domain`
        Domain to start from.
    decrease_nproc: `bool`
        Allow less processors, down to `domain.nproc * nproc_factor`.
    increase_size: `bool`
        Allow larger domain, multiply the number
        of blocks up to `size_factor`.
    nproc_factor: `float`
        Factor for minimal allowed number of processors.

    Returns:
    newdomain: `Domain`
        Adjusted domain or None if not found.
    """
    b_range = lambda b: range(b, int(b * size_factor + 0.5) + 1)
    nproc_range = lambda n: range(n, int(n * nproc_factor + 0.5) - 1, -1)
    for bz in b_range(domain.bz):
        for by in b_range(domain.by):
            for bx in b_range(domain.bx):
                for nproc in nproc_range(domain.nproc):
                    if (bx * by * bz) % nproc == 0:
                        d = Domain(bx=bx,
                                   by=by,
                                   bz=bz,
                                   h=domain.h,
                                   bsx=domain.bsx,
                                   bsy=domain.bsy,
                                   bsz=domain.bsz,
                                   bs=domain.bs,
                                   nproc=nproc)
                        return d
    return None


class Geometry:
    def __init__(self):
        self.lines = []

    def __Append(self, line):
        self.lines.append(line)

    def __Prefix(self, kwargs):
        s = ''
        if kwargs.get("intersect", False):
            s += '&'
        if kwargs.get("invert", False):
            s += '-'
        return s

    def Box(self, center, halfsize, rotation_z=0, **kwargs):
        s = self.__Prefix(kwargs)
        s += "box {:}   {:}   {:}".format(VectToStr(center),
                                          VectToStr(halfsize), rotation_z)
        self.__Append(s)
        return self

    def Sphere(self, center, radii, **kwargs):
        s = self.__Prefix(kwargs)
        s += "sphere {:}   {:}".format(VectToStr(center), VectToStr(radii))
        self.__Append(s)
        return self

    def Cylinder(self, center, axis, radius, axisrange, **kwargs):
        s = self.__Prefix(kwargs)
        s += "cylinder {:}   {:}   {:}   {:}".format(VectToStr(center),
                                                     VectToStr(axis), radius,
                                                     VectToStr(axisrange))
        self.__Append(s)
        return self

    def Generate(self):
        return '\n'.join(self.lines)

    def GenerateFile(self, path):
        with open(path, 'w') as f:
            f.write(self.Generate())


class BoundaryConditions:
    def __init__(self):
        self.lines = []

    def __Indent(self, text):
        lines = text.split('\n')
        return '\n'.join(["  " + line for line in lines])

    def __Append(self, line, geom):
        line = "{:} {{\n{:}\n}}".format(line, self.__Indent(geom.Generate()))
        self.lines.append(line)

    def Wall(self, geom, velocity):
        s = "wall {:}".format(VectToStr(velocity))
        self.__Append(s, geom)

    def WallRotation(self, geom, center, omega):
        s = "wall_rotation {:} {:}".format(VectToStr(center), VectToStr(omega))
        self.__Append(s, geom)

    def WallRotationMagn(self, geom, center, omega):
        s = "wall_rotation_magn {:} {:}".format(VectToStr(center),
                                                VectToStr(omega))
        self.__Append(s, geom)

    def SlipWall(self, geom):
        s = "slipwall"
        self.__Append(s, geom)

    def Inlet(self, geom, velocity):
        s = "inlet {:}".format(VectToStr(velocity))
        self.__Append(s, geom)

    def InletFlux(self, geom, velocity, index):
        s = "inletflux {:} {:}".format(VectToStr(velocity), index)
        self.__Append(s, geom)

    def Outlet(self, geom):
        s = "outlet"
        self.__Append(s, geom)

    def Symm(self, geom):
        s = "symm"
        self.__Append(s, geom)

    def Custom(self, geom, desc):
        self.__Append(desc, geom)

    def Generate(self):
        return '\n'.join(self.lines)

    def GenerateFile(self, path):
        with open(path, 'w') as f:
            f.write(self.Generate())

class Parameters:
    """
    Base class for parameters of a config generator.
    Values of parameters are accessible as attributes:
    >>> par = Parameters()
    >>> par.extent = 1
    """
    def __init__(self, path=None):
        for k in dir(self):
            if not k.startswith('_') and k not in ['exec']:
                setattr(self, k, getattr(self, k))
        if path:
            self.execfile(path)
    def execfile(self, path):
        """
        Executes a Python script and saves the local variables as attributes.
        path: `str`
            Path to script.
        """
        with open(path) as f:
            d = dict()
            exec(f.read(), None, d)
            for k, v in d.items():
                setattr(self, k, v)
        return self

