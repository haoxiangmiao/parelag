/*
  Copyright (c) 2018, Lawrence Livermore National Security, LLC. Produced at the
  Lawrence Livermore National Laboratory. LLNL-CODE-745557. All Rights reserved.
  See file COPYRIGHT for details.

  This file is part of the ParElag library. For more information and source code
  availability see http://github.com/LLNL/parelag.

  ParElag is free software; you can redistribute it and/or modify it under the
  terms of the GNU Lesser General Public License (as published by the Free
  Software Foundation) version 2.1 dated February 1999.
*/


#include "ParELAG_HybridizationSolverFactory.hpp"

#include "linalg/solver_ops/ParELAG_HybridizationSolver.hpp"
#include "linalg/solver_core/ParELAG_SolverLibrary.hpp"
#include "amge/DeRhamSequence.hpp"
#include "amge/HybridHdivL2.hpp"

#include <numeric>

using std::make_shared;

namespace parelag
{

std::unique_ptr<mfem::Solver> HybridizationSolverFactory::_do_build_block_solver(
    const std::shared_ptr<MfemBlockOperator>& op, SolverState& state) const
{
    PARELAG_ASSERT(op);

    // Build
    auto& params = GetParameters();
    const auto hybrid_strategy =
        params.Get<std::string>("Hybridization strategy");

    if (hybrid_strategy == "Darcy")
    {
    	auto& sequence = state.GetDeRhamSequence();
    	auto sequence_ptr = state.GetDeRhamSequencePtr();
    	auto forms = state.GetForms();

        // Whether the element H(div) dofs have same orientation on shared facet
        bool IsSameOrient = state.GetExtraParameter("IsSameOrient",false);

        // The constant weight in the system [M B^T; B -(W_weight*W)]
        double L2MassWeight = state.GetExtraParameter("L2MassWeight",0.0);

        // Scales of element H(div) mass matrices for the problem being solved
        // If not provided, the scale is treated as 1.0
        auto elemMatrixScaling = state.GetVector("elemMatrixScaling");

    	auto label_ess = state.GetBoundaryLabels(0);
    	mfem::Array<int> ess_HdivDofs;
        if (label_ess.size() > 0)
    	{
    		mfem::Array<int> ess_attr(label_ess.data(),label_ess.size());
    		ess_HdivDofs.SetSize(sequence.GetNumberOfDofs(forms[0]));
    		sequence.GetDofHandler(forms[0])->MarkDofsOnSelectedBndr(
    				ess_attr, ess_HdivDofs);
    	}
        else
        {
            ess_HdivDofs.SetSize(sequence.GetNumberOfDofs(forms[0]));
            ess_HdivDofs = 0;
        }
        std::shared_ptr<HybridHdivL2> hybridization =
                std::make_shared<HybridHdivL2>(sequence_ptr, IsSameOrient,
                                               L2MassWeight, ess_HdivDofs,
                                               elemMatrixScaling);

        mfem::Array<int> Offsets(3);
    	Offsets[0] = 0;
    	Offsets[1] = sequence.GetNumberOfDofs(forms[0]);
    	Offsets[2] = Offsets[1] + sequence.GetNumberOfDofs(forms[1]);

        // Copy the hybridized matrix and eliminate the boundary condition
        // for the matrix. Note that at this stage no rhs is given, so the
        // original hybridized matrix (before elimination) is kept so that
        // later it can be used to finish the elimination process (for rhs)
        mfem::SparseMatrix HB_mat_copy(*hybridization->GetHybridSystem());
        DofHandler* dofhandler = hybridization->GetDofMultiplier();
        SharingMap& mult_dofTrueDof = dofhandler->GetDofTrueDof();

        // Eliminate essential multiplier dofs (1-1 map to natural Hdiv dofs)
        for (int i = 0; i < HB_mat_copy.Size(); i++)
            if (hybridization->GetEssentialMultiplierDofs()[i])
                HB_mat_copy.EliminateRowCol(i);
        auto pHB_mat = Assemble(mult_dofTrueDof, HB_mat_copy, mult_dofTrueDof);

        const int rescale_iter = state.GetExtraParameter("RescaleIteration", -20);
        auto scaling_vector = rescale_iter < 0 ? hybridization->GetRescaling() :
                              _get_scaling_by_smoothing(*pHB_mat, rescale_iter);

        int* scale_i = new int[pHB_mat->Height()+1];
        int* scale_j = new int[pHB_mat->Height()];
        double* scale_data = new double[pHB_mat->Height()];
        std::iota(scale_i, scale_i+pHB_mat->Height()+1, 0);
        std::iota(scale_j, scale_j+pHB_mat->Height(), 0);
        std::copy_n(scaling_vector.GetData(), pHB_mat->Height(), scale_data);
        auto D_Scale = make_shared<mfem::SparseMatrix>(
                    scale_i, scale_j, scale_data,
                    pHB_mat->Height(), pHB_mat->Height());

        const auto facet = AgglomeratedTopology::FACET;
        auto& facet_truefacet = sequence.GetTopology()->EntityTrueEntity(facet);
        const int num_facets = facet_truefacet.GetLocalSize();

        std::shared_ptr<mfem::Solver> solver;

        if (!IsSameOrient || (num_facets == dofhandler->GetNDofs()))
        {
            auto pD_Scale = make_unique<mfem::HypreParMatrix>(
                        pHB_mat->GetComm(), pHB_mat->N(), pHB_mat->RowPart(),
                        D_Scale.get());

            auto Scaled_pHB = ToUnique(RAP(pHB_mat.get(), pD_Scale.get()));

            auto hybrid_state = std::shared_ptr<SolverState>{
                    SolverFact_->GetDefaultState()};
            solver = SolverFact_->BuildSolver(std::move(Scaled_pHB),*hybrid_state);
        }
        else
        {
            mfem::SparseMatrix PV_map(dofhandler->GetNDofs(), num_facets);

            std::vector<mfem::Array<int>> local_dofs(num_facets);
            for (int i = 0; i < num_facets; ++i)
            {
                dofhandler->GetDofs(facet, i, local_dofs[i]);
                PV_map.Add(local_dofs[i][0], i, 1.0);
            }
            PV_map.Finalize();

            auto parallel_PV_map = Assemble(mult_dofTrueDof, PV_map, facet_truefacet);
            parallel_PV_map->ScaleRows(scaling_vector);
            mfem::SparseMatrix scaled_PV_map_local;
            parallel_PV_map->GetDiag(scaled_PV_map_local);

            const auto elem = AgglomeratedTopology::ELEMENT;
            const int num_elems = dofhandler->GetNumberEntities(elem);
            std::vector<mfem::Array<int>> local_dofs2(num_facets);
            for (int i = 0; i < num_elems; ++i)
            {
                hybridization->GetDofMultiplier()->GetDofs(elem, i, local_dofs2[i]);
            }

            solver = make_unique<CGTLAS>(std::move(pHB_mat), local_dofs2, scaled_PV_map_local);
            *D_Scale = 1.0;
        }

        solver->iterative_mode = false;

        std::unique_ptr<mfem::Solver> hybrid_solve =
                make_unique<HybridizationSolver>(std::move(hybridization),
                                                 std::move(solver), Offsets,
                                                 std::move(D_Scale));

        return hybrid_solve;
    }
    else
    {
        const bool invalid_hybridization_strategy = true;

        PARELAG_TEST_FOR_EXCEPTION(
            invalid_hybridization_strategy,
            std::runtime_error,
            "HybridizationSolverFactory::BuildBlockSolver(...):\n"
            "Hybridization strategy \"" << hybrid_strategy <<
            "\" is invalid. Currently, the only option is \"Darcy\"");
    }

}

mfem::Vector HybridizationSolverFactory::_get_scaling_by_smoothing(
     const ParallelCSRMatrix& op, int num_iter) const
{
    mfem::Vector scaling_vector(op.Height());
    scaling_vector = 1.0;

    if (num_iter > 0)
    {
        // Generate a diagonal scaling matrix by smoothing some random vector
        mfem::HypreSmoother smoother(const_cast<ParallelCSRMatrix&>(op));
        mfem::SLISolver sli(op.GetComm());
        sli.SetOperator(op);
        sli.SetPreconditioner(smoother);
        sli.SetMaxIter(num_iter);

        mfem::Vector zeros(op.Height());
        zeros = 0.0;
        sli.Mult(zeros, scaling_vector);
    }

    return scaling_vector;
}

void HybridizationSolverFactory::_do_set_default_parameters()
{
    auto& params = GetParameters();

    // Options: "Assemble then transform" or "transform then assemble"
    params.Get<std::string>("Hybridization strategy","Darcy");

    // May be any solver known to the library
    params.Get<std::string>("Solver","Invalid");
}

void HybridizationSolverFactory::_do_initialize(const ParameterList&)
{
    // Depends on a library
    PARELAG_ASSERT(HasValidSolverLibrary());

    const std::string solver_name =
        GetParameters().Get<std::string>("Solver");

    SolverFact_ = GetSolverLibrary().GetSolverFactory(solver_name);
}

TwoLevelAdditiveSchwarz::TwoLevelAdditiveSchwarz(ParallelCSRMatrix& op,
        const std::vector<mfem::Array<int> >& local_dofs,
        const SerialCSRMatrix& coarse_map)
    : mfem::Solver(op.NumRows(), false),
      op_(op),
      smoother(op_, mfem::HypreSmoother::l1GS),
      local_dofs_(local_dofs.size()),
      coarse_map_(coarse_map),
      local_ops_(local_dofs.size()),
      local_solvers_(local_dofs.size())
{
    // Set up local solvers
    SerialCSRMatrix op_diag;
    op.GetDiag(op_diag);
//    for (int i = 0; i < local_dofs.size(); ++i)
//    {
//        local_ops_[i].SetSize(local_dofs[i].Size());
//        op_diag.GetSubMatrix(local_dofs[i], local_dofs[i], local_ops_[i]);
//        local_solvers_[i].Compute(local_ops_[i]);
//        local_dofs[i].Copy(local_dofs_[i]);
//    }

    // Set up coarse solver
    int num_local_cdofs = coarse_map.NumCols();
    mfem::Array<int> cdof_starts;
    ParPartialSums_AssumedPartitionCheck(op.GetComm(), num_local_cdofs, cdof_starts);
    int num_global_cdofs = cdof_starts.Last();
    SerialCSRMatrix c_map(coarse_map);
    ParallelCSRMatrix parallel_c_map(op.GetComm(), op.N(), num_global_cdofs,
                                     op.ColPart(), cdof_starts, &c_map);

    coarse_op_.reset(RAP(&op, &parallel_c_map));
    coarse_op_->CopyRowStarts();
    coarse_op_->CopyColStarts();
    std::cout<<"Size of coarse_op = "<<coarse_op_->N()<< " "<< op_.N()<<"\n";
    coarse_solver_ = make_unique<mfem::HypreBoomerAMG>(*coarse_op_);
    coarse_solver_->SetPrintLevel(-1);
}

void TwoLevelAdditiveSchwarz::Mult(const mfem::Vector& x, mfem::Vector& y) const
{
    y = 0.0;

    mfem::Vector x_local, y_local, x_coarse, y_coarse;
//    for (int i = 0; i < local_dofs_.size(); ++i)
//    {
//        x.GetSubVector(local_dofs_[i], x_local);
//        y_local.SetSize(x_local.Size());
//        y_local = 0.0;
//        local_solvers_[i].Mult(x_local, y_local);
//        y.AddElementVector(local_dofs_[i], y_local);
//    }

    smoother.Mult(x, y);

    mfem::Vector residual(x);
    op_.Mult(-1.0, y, 1.0, residual);

    x_coarse.SetSize(coarse_map_.NumCols());
    y_coarse.SetSize(coarse_map_.NumCols());
    x_coarse = 0.0;
    y_coarse = 0.0;
    coarse_map_.MultTranspose(residual, x_coarse);
    coarse_solver_->Mult(x_coarse, y_coarse);
    coarse_map_.AddMult(y_coarse, y);

    residual = x;
    op_.Mult(-1.0, y, 1.0, residual);

    mfem::Vector correction(y.Size());
    correction = 0.0;
    smoother.Mult(residual, correction);
//    for (int i = 0; i < local_dofs_.size(); ++i)
//    {
//        residual.GetSubVector(local_dofs_[i], x_local);
//        y_local.SetSize(x_local.Size());
//        y_local = 0.0;
//        local_solvers_[i].Mult(x_local, y_local);
//        correction.AddElementVector(local_dofs_[i], y_local);
//    }

    y += correction;
}

CGTLAS::CGTLAS(std::unique_ptr<ParallelCSRMatrix> op,
               const std::vector<mfem::Array<int> >& local_dofs,
               const SerialCSRMatrix& coarse_map)
    : mfem::Solver(op->NumRows(), false),
      op_(std::move(op)),
      prec_(*op_, local_dofs, coarse_map),
      cg_(op_->GetComm())
{
    cg_.SetPrintLevel(1);
    cg_.SetMaxIter(500);
    cg_.SetRelTol(1e-9);
    cg_.SetAbsTol(1e-12);
    cg_.SetOperator(*op_);
    cg_.SetPreconditioner(prec_);
    cg_.iterative_mode = false;
}



}// namespace parelag
