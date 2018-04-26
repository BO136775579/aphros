#pragma once

#include <cmath>
#include <sstream>
#include <stdexcept>
#include <memory>

#include "util/metrics.h"
#include "convdiffv.h"
#include "convdiffi.h"

namespace solver {

template <class Mesh>
class ConvectionDiffusionImplicit : public ConvectionDiffusion<Mesh> {
  Mesh& mesh;
  using Scal = typename Mesh::Scal;
  using Vect = typename Mesh::Vect;
  using Solver = ConvectionDiffusionScalarImplicit<Mesh>;

  static constexpr size_t dim = Mesh::dim;
  using Expr = Expression<Scal, IdxCell, 1 + dim * 2>;
  template <class T>
  using VectGeneric = std::array<T, dim>;
  LayersData<FieldCell<Vect>> fc_velocity_;

  MapFace<std::shared_ptr<ConditionFace>> mf_velocity_cond_;
  MapCell<std::shared_ptr<ConditionCell>> mc_velocity_cond_;
  FieldCell<Vect>* p_fc_force_;

  VectGeneric<MapFace<std::shared_ptr<ConditionFace>>>
  v_mf_velocity_cond_;
  // TODO: Extract scalar CellCondition
  VectGeneric<std::shared_ptr<Solver>>
  v_solver_;
  VectGeneric<FieldCell<Scal>>
  v_fc_force_;

 public:
  using Par = typename Solver::Par;
  std::shared_ptr<Par> par;
  Par* GetPar() { return par.get(); }
  void CopyToVector(Layers layer) {
    fc_velocity_.Get(layer).Reinit(mesh);
    for (size_t n = 0; n < dim; ++n) {
      SetComponent(fc_velocity_.Get(layer), n, v_solver_[n]->GetField(layer));
    }
  }
  ConvectionDiffusionImplicit(
      Mesh& mesh,
      const FieldCell<Vect>& fc_velocity_initial,
      const MapFace<std::shared_ptr<ConditionFace>>&
      mf_velocity_cond,
      const MapCell<std::shared_ptr<ConditionCell>>&
      mc_velocity_cond,
      FieldCell<Scal>* p_fc_density,
      FieldFace<Scal>* p_ff_kinematic_viscosity,
      FieldCell<Vect>* p_fc_force,
      FieldFace<Scal>* p_ff_vol_flux,
      double t, double dt, std::shared_ptr<Par> par)
      : ConvectionDiffusion<Mesh>(t, dt, p_fc_density, 
                                  p_ff_kinematic_viscosity, 
                                  p_fc_force, p_ff_vol_flux)
      , mesh(mesh)
      , mf_velocity_cond_(mf_velocity_cond)
      , mc_velocity_cond_(mc_velocity_cond)
      , p_fc_force_(p_fc_force)
      , par(par)
  {
    for (size_t n = 0; n < dim; ++n) {
      // Boundary conditions for each velocity component
      // (copied from given vector conditions)
      for (auto it = mf_velocity_cond_.cbegin();
          it != mf_velocity_cond_.cend(); ++it) {
        IdxFace idxface = it->GetIdx();
        if (auto cond = dynamic_cast<ConditionFaceValue<Vect>*>(
            mf_velocity_cond_[idxface].get())) {
          v_mf_velocity_cond_[n][idxface] =
              std::make_shared<ConditionFaceValueExtractComponent<Vect>>(
                  cond, n);
        } else {
          throw std::runtime_error("Unknown boudnary condition type");
        }
      }

      // Initialize solver
      v_solver_[n] = std::make_shared<Solver>(
          mesh, 
          GetComponent(fc_velocity_initial, n),
          v_mf_velocity_cond_[n],
          MapCell<std::shared_ptr<ConditionCell>>() /*TODO empty*/,
          p_fc_density, p_ff_kinematic_viscosity,
          &(v_fc_force_[n]), p_ff_vol_flux, t, dt, par);
    }
    CopyToVector(Layers::time_curr);
    CopyToVector(Layers::time_prev);
  }
  void StartStep() override {
    auto sem = mesh.GetSem("convdiffmulti-start");
    for (size_t n = 0; n < dim; ++n) {
      if (sem("dir-init")) {
        v_solver_[n]->SetTimeStep(this->GetTimeStep());
      }
      if (sem.Nested("dir-start")) {
        v_solver_[n]->StartStep();
      }
    }
    if (sem("tovect")) {
      CopyToVector(Layers::iter_curr);
      this->ClearIter();
    }
  }
  void MakeIteration() override {
    auto sem = mesh.GetSem("convdiffmulti-iter");
    for (size_t n = 0; n < dim; ++n) {
      if (sem("dir-get")) {
        v_fc_force_[n] = GetComponent(*p_fc_force_, n);
      }
    }

    for (size_t n = 0; n < dim; ++n) {
      if (sem.Nested("dir-iter")) {
        v_solver_[n]->MakeIteration();
      }
    }

    if (sem("tovect")) {
      CopyToVector(Layers::iter_prev);
      CopyToVector(Layers::iter_curr);
      this->IncIter();
    }
  }
  void FinishStep() override {
    auto sem = mesh.GetSem("convdiffmulti-finish");

    for (size_t n = 0; n < dim; ++n) {
      if (sem.Nested("dir-finish")) {
        v_solver_[n]->FinishStep();
      }
    }
    if (sem("tovect")) {
      CopyToVector(Layers::time_prev);
      CopyToVector(Layers::time_curr);
      this->IncTime();
    }
  }
  double GetError() const override {
    if (this->GetIter() == 0) {
      return 1.;
    }
    return CalcDiff(fc_velocity_.iter_curr, fc_velocity_.iter_prev, mesh);
  }
  const FieldCell<Vect>& GetVelocity() override {
    return fc_velocity_.time_curr;
  }
  const FieldCell<Vect>& GetVelocity(Layers layer) override {
    return fc_velocity_.Get(layer);
  }
  void CorrectVelocity(Layers layer,
                       const FieldCell<Vect>& fc_corr) override {
    auto sem = mesh.GetSem("corr");
    for (size_t n = 0; n < dim; ++n) {
      if (sem.Nested("dir-corr")) {
        v_solver_[n]->CorrectField(layer, GetComponent(fc_corr, n));
      }
    }
    if (sem("tovect")) {
      CopyToVector(layer);
    }
  }
  const FieldCell<Expr>& GetVelocityEquations(size_t comp) override {
    return v_solver_[comp]->GetEquations();
  }
  MapFace<std::shared_ptr<ConditionFace>>&
  GetVelocityCond(size_t comp) {
    return v_mf_velocity_cond_[comp];
  }
};


} // namespace solver

