// Created by Petr Karnakov on 27.09.2020
// Copyright 2020 ETH Zurich

#pragma once

#include <array>
#include <exception>
#include <fstream>
#include <limits>
#include <memory>

#include "approx.h"
#include "approx_eb.h"
#include "linear/linear.h"
#include "solver/pois.h"
#include "util/vof.h"

#include "electro.h"

template <class EB_>
struct Electro<EB_>::Imp {
  using Owner = Electro<EB_>;
  using UEB = UEmbed<M>;

  Imp(Owner* owner, M& m, const EB& eb, const MapEmbed<BCond<Scal>>& mebc_pot,
      Scal time, const Conf& conf)
      : owner_(owner)
      , m(m)
      , eb(eb)
      , conf(conf)
      , time_(time)
      , fc_pot_(m, 0)
      , mebc_pot_(mebc_pot) {}
  void Step(
      Scal dt, const FieldCell<Scal>& fc_permit,
      const FieldCell<Scal>& fc_charge, const FieldCell<Scal>& fc_vf) {
    auto sem = m.GetSem("step");
    using Expr = typename M::Expr;
    using ExprFace = typename M::ExprFace;
    struct {
      FieldCell<Scal> fc_rhs;
      FieldCell<Expr> fcl;
    } * ctx(sem);
    auto& t = *ctx;
    if (sem("local")) {
      t.fc_rhs.Reinit(m, 0);
    }
    if (sem.Nested("solve")) {
      const FieldFaceb<Scal> ff_vf =
          UEB::Interpolate(fc_vf, GetBCondZeroGrad<Scal>(mebc_pot_), eb);
      FieldFaceb<Scal> ff_resist_inv(m, 1);
      auto r1 = conf.var.Double["resist1"];
      auto r2 = conf.var.Double["resist2"];
      for (auto f : eb.Faces()) {
        ff_resist_inv[f] = 1 / r2 * ff_vf[f] +  1 / r1 * (1 - ff_vf[f]);
      }
      const auto ffg = UEB::GradientImplicit(fc_pot_, mebc_pot_, m);
      t.fcl.Reinit(m, Expr::GetUnit(0));
      for (auto c : m.Cells()) {
        Expr sum(0);
        m.LoopNci(c, [&](auto q) {
          const auto cf = m.GetFace(c, q);
          const ExprFace flux = ffg[cf] * ff_resist_inv[cf] * m.GetArea(cf);
          m.AppendExpr(sum, flux * m.GetOutwardFactor(c, q), q);
        });
        t.fcl[c] = sum;
      }
      t.fcl.SetName("electro");
    }
    if (sem.Nested("solve")) {
      Solve(t.fcl, &fc_pot_, fc_pot_, M::LS::T::symm, m, "vort");
    }
    if (sem("stat")) {
      time_ += dt;
    }
  }

  Owner* owner_;
  M& m;
  const EB& eb;
  Conf conf;
  Scal time_;
  FieldCell<Scal> fc_pot_;
  const MapEmbed<BCond<Scal>>& mebc_pot_;
};

template <class EB_>
Electro<EB_>::Electro(
    M& m, const EB& eb, const MapEmbed<BCond<Scal>>& mebc_pot, Scal time,
    const Conf& conf)
    : imp(new Imp(this, m, eb, mebc_pot, time, conf)) {}

template <class EB_>
Electro<EB_>::~Electro() = default;

template <class EB_>
auto Electro<EB_>::GetConf() const -> Conf& {
  return imp->conf;
}

template <class EB_>
void Electro<EB_>::Step(
    Scal dt, const FieldCell<Scal>& fc_permit,
    const FieldCell<Scal>& fc_charge, const FieldCell<Scal>& fc_vf) {
  imp->Step(dt, fc_permit, fc_charge, fc_vf);
}

template <class EB_>
auto Electro<EB_>::GetPotential() const -> const FieldCell<Scal>& {
  return imp->fc_pot_;
}

template <class EB_>
auto Electro<EB_>::GetTime() const -> Scal {
  return imp->time_;
}
