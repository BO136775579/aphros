// Created by Petr Karnakov on 02.04.2019
// Copyright 2019 ETH Zurich

#include "overlap.h"

// Dummy implementation

double GetSphereOverlap(
    const generic::Vect<double, 3>& x, const generic::Vect<double, 3>& h,
    const generic::Vect<double, 3>& c, double r) {
  double d = (r - x.dist(c)) / h.norm();
  d = (d + 1.) * 0.5;
  d = std::min(d, 1.);
  d = std::max(d, 0.);
  return d;
}
