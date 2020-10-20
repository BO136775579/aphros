// Created by Petr Karnakov on 03.10.2020
// Copyright 2020 ETH Zurich

#pragma once

#include "hypre.h"
#include "linear.h"

namespace linear {

template <class M>
struct SolverHypre<M>::Imp {
  using Owner = SolverHypre<M>;

  Imp(Owner* owner, const Extra& extra_)
      : owner_(owner), conf(owner_->conf), extra(extra_) {}
  Info Solve(
      const FieldCell<Expr>& fc_system, const FieldCell<Scal>* fc_init,
      FieldCell<Scal>& fc_sol, M& m) {
    using MIdx = typename M::MIdx;
    auto sem = m.GetSem(__func__);
    struct {
      // local on all blocks
      std::vector<MIdx> stencil;
      std::vector<Scal> data_a;
      std::vector<Scal> data_b;
      std::vector<Scal> data_x;
      Info info;

      // reduced to lead block
      std::vector<MIdx> origin;
      std::vector<MIdx> size;
      std::vector<std::vector<Scal>*> ptr_a;
      std::vector<std::vector<Scal>*> ptr_b;
      std::vector<std::vector<Scal>*> ptr_x;
      std::vector<Info*> ptr_info;
      std::unique_ptr<Hypre> hypre;
    } * ctx(sem);
    auto& t = *ctx;
    if (m.flags.check_symmetry &&
        (extra.solver == "pcg" || extra.solver == "pcg+smg")) {
      if (sem.Nested()) {
        UDebug<M>::CheckSymmetry(fc_system, m);
      }
    }
    if (sem("solve")) {
      const auto bic = m.GetInBlockCells();
      t.origin.push_back(bic.GetBegin());
      t.size.push_back(bic.GetSize());

      // copy data from l to block-local buffer
      t.stencil = {
          MIdx{0, 0, 0}, MIdx{-1, 0, 0}, MIdx{1, 0, 0}, MIdx{0, -1, 0},
          MIdx{0, 1, 0}, MIdx{0, 0, -1}, MIdx{0, 0, 1},
      };

      t.data_a.resize(t.stencil.size() * bic.size());
      t.data_b.resize(bic.size());
      t.data_x.resize(bic.size(), 0);

      { // matrix coeffs
        size_t i = 0;
        for (auto c : m.Cells()) {
          for (size_t k = 0; k < 7; ++k) {
            t.data_a[i++] = fc_system[c][k];
          }
        }
      }

      { // rhs
        size_t i = 0;
        for (auto c : m.Cells()) {
          t.data_b[i++] = -fc_system[c].back();
        }
      }

      if (fc_init) { // initial guess
        size_t i = 0;
        for (auto c : m.Cells()) {
          t.data_x[i++] = (*fc_init)[c];
        }
      }

      // pass pointers to block-local data to the lead block
      t.ptr_a.push_back(&t.data_a);
      t.ptr_b.push_back(&t.data_b);
      t.ptr_x.push_back(&t.data_x);
      t.ptr_info.push_back(&t.info);

      m.GatherToLead(&t.origin);
      m.GatherToLead(&t.size);
      m.GatherToLead(&t.ptr_a);
      m.GatherToLead(&t.ptr_b);
      m.GatherToLead(&t.ptr_x);
      m.GatherToLead(&t.ptr_info);
    }
    if (sem("hypre") && m.IsLead()) {
      using HypreBlock = typename Hypre::Block;
      using HypreMIdx = typename Hypre::MIdx;

      const size_t nblocks = t.origin.size();
      fassert_equal(t.size.size(), nblocks);
      fassert_equal(t.ptr_a.size(), nblocks);
      fassert_equal(t.ptr_b.size(), nblocks);
      fassert_equal(t.ptr_x.size(), nblocks);

      std::vector<HypreBlock> blocks(nblocks);
      for (size_t i = 0; i < nblocks; ++i) {
        HypreBlock& block = blocks[i];
        block.l = t.origin[i];
        block.u = t.origin[i] + t.size[i] - MIdx(1);
        for (auto w : t.stencil) {
          block.st.push_back(w);
        }
        block.a = t.ptr_a[i];
        block.r = t.ptr_b[i];
        block.x = t.ptr_x[i];
      }

      const HypreMIdx per = MIdx(m.flags.is_periodic);
      t.hypre = std::make_unique<Hypre>(
          m.GetMpiComm(), blocks, m.GetGlobalSize(), per);

      t.hypre->Solve(conf.tol, extra.print, extra.solver, conf.maxiter);

      // fill local info and update by pointers on other blocks
      t.info.residual = t.hypre->GetResidual();
      t.info.iter = t.hypre->GetIter();
      for (size_t i = 0; i < nblocks; ++i) {
        t.ptr_info[i] = &t.info;
      }
    }
    if (sem()) {
      // copy solution from t.data_x to field
      fc_sol.Reinit(m);
      size_t i = 0;
      for (auto c : m.Cells()) {
        fc_sol[c] = t.data_x[i++];
      }
      m.Comm(&fc_sol);
      if (m.flags.linreport && m.IsRoot()) {
        std::cout << std::scientific;
        std::cout << "linear(hypre) '" + fc_system.GetName() + "':"
                  << " res=" << t.info.residual << " iter=" << t.info.iter
                  << std::endl;
      }
    }
    if (sem()) {
    }
    return t.info;
  }

 private:
  Owner* owner_;
  Conf& conf;
  Extra extra;
};

template <class M>
SolverHypre<M>::SolverHypre(const Conf& conf_, const Extra& extra)
    : Base(conf_), imp(new Imp(this, extra)) {}

template <class M>
SolverHypre<M>::~SolverHypre() = default;

template <class M>
auto SolverHypre<M>::Solve(
    const FieldCell<Expr>& fc_system, const FieldCell<Scal>* fc_init,
    FieldCell<Scal>& fc_sol, M& m) -> Info {
  return imp->Solve(fc_system, fc_init, fc_sol, m);
}

template <class M>
struct SolverConjugate<M>::Imp {
  using Owner = SolverConjugate<M>;

  Imp(Owner* owner, const Extra& extra_)
      : owner_(owner), conf(owner_->conf), extra(extra_) {}
  // TODO: use fc_init
  Info Solve(
      const FieldCell<Expr>& fc_system, const FieldCell<Scal>* fc_init,
      FieldCell<Scal>& fc_sol, M& m) {
    auto sem = m.GetSem(__func__);
    struct {
      FieldCell<Scal> fcu;
      FieldCell<Scal> fcr;
      FieldCell<Scal> fcp;
      FieldCell<Scal> fc_opp; // linear fc_system operator applied to p
      Scal dot_p_opp;
      Scal dot_r;
      Scal dot_r_prev;

      Scal maxdiff;
      int iter = 0;
      Info info;
    } * ctx(sem);
    auto& t = *ctx;
    if (sem("init")) {
      if (fc_init) {
        t.fcu = *fc_init;
      } else {
        t.fcu.Reinit(m, 0);
      }
      t.fcr.Reinit(m);
      for (auto c : m.Cells()) {
        const auto& e = fc_system[c];
        Scal u = t.fcu[c] * e[0] + e.back();
        for (auto q : m.Nci(c)) {
          u += t.fcu[m.GetCell(c, q)] * e[1 + q];
        }
        t.fcr[c] = -u;
      }
      m.Comm(&t.fcr);
    }
    if (sem("init")) {
      t.fcp = t.fcr;
      t.fc_opp.Reinit(m);
    }
    sem.LoopBegin();
    /*
    if (sem("iter")) {
      for (auto c : m.Cells()) {
        const auto& e = fc_system[c];
        Scal p = t.fcp[c] * e[0];
        for (auto q : m.Nci(c)) {
          p += t.fcp[m.GetCell(c, q)] * e[1 + q];
        }
        t.fc_opp[c] = p;
      }

      t.dot_r_prev = 0;
      t.dot_p_opp = 0;
      for (auto c : m.Cells()) {
        t.dot_r_prev += sqr(t.fcr[c]);
        t.dot_p_opp += t.fcp[c] * t.fc_opp[c];
      }
      m.Reduce(&t.dot_r_prev, Reduction::sum);
      m.Reduce(&t.dot_p_opp, Reduction::sum);
    }
    if (sem("iter2")) {
      t.maxdiff = 0;
      const Scal alpha = t.dot_r_prev / t.dot_p_opp;
      t.dot_r = 0;
      for (auto c : m.Cells()) {
        t.fcu[c] += alpha * t.fcp[c];
        t.fcr[c] -= alpha * t.fc_opp[c];
        t.dot_r += sqr(t.fcr[c]);
        t.maxdiff = std::max(t.maxdiff, std::abs(t.fcp[c] * alpha));
      }
      m.Reduce(&t.dot_r, Reduction::sum);
      m.Reduce(&t.maxdiff, Reduction::max);
    }
    if (sem("iter3")) {
      for (auto c : m.Cells()) {
        t.fcp[c] = t.fcr[c] + (t.dot_r / t.dot_r_prev) * t.fcp[c];
      }
      m.Comm(&t.fcp);
    }
    */
    for (auto i : GRange<int>(10)) {
      if (sem()){}
    }
    if (sem("check")) {
      t.info.residual = t.maxdiff;
      t.info.iter = t.iter;
      if (t.iter++ > conf.maxiter || t.maxdiff < conf.tol) {
        sem.LoopBreak();
      }
    }
    sem.LoopEnd();
    if (sem("result")) {
      fc_sol = t.fcu;
      m.Comm(&fc_sol);
      if (m.flags.linreport && m.IsRoot()) {
        std::cout << std::scientific;
        std::cout << "linear(conjugate) '" + fc_system.GetName() + "':"
                  << " res=" << t.info.residual << " iter=" << t.info.iter
                  << std::endl;
      }
    }
    if (sem()) {
    }
    return t.info;
  }

 private:
  Owner* owner_;
  Conf& conf;
  Extra extra;
};

template <class M>
SolverConjugate<M>::SolverConjugate(const Conf& conf_, const Extra& extra)
    : Base(conf_), imp(new Imp(this, extra)) {}

template <class M>
SolverConjugate<M>::~SolverConjugate() = default;

template <class M>
auto SolverConjugate<M>::Solve(
    const FieldCell<Expr>& fc_system, const FieldCell<Scal>* fc_init,
    FieldCell<Scal>& fc_sol, M& m) -> Info {
  return imp->Solve(fc_system, fc_init, fc_sol, m);
}

template <class M>
struct SolverJacobi<M>::Imp {
  using Owner = SolverJacobi<M>;

  Imp(Owner* owner, const Extra& extra_)
      : owner_(owner), conf(owner_->conf), extra(extra_) {}
  Info Solve(
      const FieldCell<Expr>& fc_system, const FieldCell<Scal>* fc_init,
      FieldCell<Scal>& fc_sol, M& m) {
    auto sem = m.GetSem(__func__);
    struct {
      FieldCell<Scal> fcu;
      FieldCell<Scal> fcu_new;
      Scal maxdiff;
      int iter = 0;
      Info info;
    } * ctx(sem);
    auto& t = *ctx;
    if (sem("init")) {
      if (fc_init) {
        t.fcu = *fc_init;
      } else {
        t.fcu.Reinit(m, 0);
      }
      t.fcu_new.Reinit(m, 0);
    }
    sem.LoopBegin();
    if (sem("iter")) {
      t.maxdiff = 0;
      for (auto c : m.Cells()) {
        const auto& e = fc_system[c];
        Scal nondiag = e.back();
        for (auto q : m.Nci(c)) {
          nondiag += t.fcu[m.GetCell(c, q)] * e[1 + q];
        }
        t.fcu_new[c] = -nondiag / e[0];
        t.maxdiff = std::max(t.maxdiff, std::abs(t.fcu_new[c] - t.fcu[c]));
      }
      t.fcu.swap(t.fcu_new);
      m.Comm(&t.fcu);
      m.Reduce(&t.maxdiff, Reduction::max);
    }
    if (sem("check")) {
      t.info.residual = t.maxdiff;
      t.info.iter = t.iter;
      if (t.iter++ > conf.maxiter || t.maxdiff < conf.tol) {
        sem.LoopBreak();
      }
    }
    sem.LoopEnd();
    if (sem("result")) {
      fc_sol = t.fcu;
      m.Comm(&fc_sol);
      if (m.flags.linreport && m.IsRoot()) {
        std::cout << std::scientific;
        std::cout << "linear(jacobi) '" + fc_system.GetName() + "':"
                  << " res=" << t.info.residual << " iter=" << t.info.iter
                  << std::endl;
      }
    }
    if (sem("")) {
    }
    return t.info;
  }

 private:
  Owner* owner_;
  Conf& conf;
  Extra extra;
};

template <class M>
SolverJacobi<M>::SolverJacobi(const Conf& conf_, const Extra& extra)
    : Base(conf_), imp(new Imp(this, extra)) {}

template <class M>
SolverJacobi<M>::~SolverJacobi() = default;

template <class M>
auto SolverJacobi<M>::Solve(
    const FieldCell<Expr>& fc_system, const FieldCell<Scal>* fc_init,
    FieldCell<Scal>& fc_sol, M& m) -> Info {
  return imp->Solve(fc_system, fc_init, fc_sol, m);
}

} // namespace linear
