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


#include "linalg/factories/ParELAG_Block2x2JacobiSolverFactory.hpp"

#include "linalg/solver_core/ParELAG_SolverLibrary.hpp"
#include "linalg/solver_ops/ParELAG_BlockDiagonalSolver.hpp"

namespace parelag
{

Block2x2JacobiSolverFactory::Block2x2JacobiSolverFactory(
    const ParameterList& params)
    : Block2x2JacobiSolverFactory{nullptr,nullptr,nullptr,params}
{}


Block2x2JacobiSolverFactory::Block2x2JacobiSolverFactory(
    std::shared_ptr<SolverFactory> a00_inverse_factory,
    std::shared_ptr<SolverFactory> a11_inverse_factory,
    std::shared_ptr<SchurComplementFactory> s_factory,
    const ParameterList& params)
    : InvA00_Fact_{std::move(a00_inverse_factory)},
      InvA11_Fact_{std::move(a11_inverse_factory)},
      S_Fact_{std::move(s_factory)}
{ SetParameters(params); }


std::unique_ptr<mfem::Solver>
Block2x2JacobiSolverFactory::_do_build_block_solver(
    const std::shared_ptr<MfemBlockOperator>& blop, SolverState& state) const
{
    PARELAG_ASSERT(blop);
    PARELAG_ASSERT(blop->GetNumBlockRows() == 2);
    PARELAG_ASSERT(blop->GetNumBlockCols() == 2);

    auto my_state = dynamic_cast<NestedSolverState *>(&state);
    PARELAG_ASSERT(my_state);

    // Get the things I need from my state:
    auto A00_state = std::shared_ptr<SolverState>{
        InvA00_Fact_->GetDefaultState()};
    auto A11_state = std::shared_ptr<SolverState>{
        InvA11_Fact_->GetDefaultState()};

    if (my_state->IsSubState("A00"))
        A00_state->MergeState(*my_state->GetSubState("A00"));
    if (my_state->IsSubState("A11"))
        A11_state->MergeState(*my_state->GetSubState("A11"));

    A00_state->MergeState(*my_state);
    A11_state->MergeState(*my_state);
    A11_state->SetForms({A00_state->GetForms()[1]});
    A00_state->SetForms({A00_state->GetForms()[0]});

    std::vector<std::shared_ptr<mfem::Solver>> inv_ops(2);

    // Get A00
    auto A00 = blop->GetBlockPtr(0,0);

    // Set iterative_mode is false so sub-residuals aren't computed.
    inv_ops[0] = std::shared_ptr<mfem::Solver>{
        InvA00_Fact_->BuildSolver(A00,*A00_state)};
    inv_ops[0]->iterative_mode = false;

    std::shared_ptr<mfem::Operator> A11;
    if (not S_Fact_)
        A11 = blop->GetBlockPtr(1,1);
    else
    {
        A11 = S_Fact_->BuildOperator(*blop,*A11_state);
        if (UseNegativeS_)
        {
            auto* s_mat = dynamic_cast<mfem::HypreParMatrix*>(A11.get());
            if (s_mat)
                *s_mat *= -1.0;
        }
    }

    inv_ops[1] = std::shared_ptr<mfem::Solver>{
        InvA11_Fact_->BuildSolver(A11,*A11_state)};
    inv_ops[1]->iterative_mode = false;

    if (S_Fact_)
        return make_unique<BlockDiagonalSolver>(
            blop, inv_ops,
            std::vector<std::shared_ptr<mfem::Operator>>({ A11 }));
    else
        return make_unique<BlockDiagonalSolver>(
            blop, inv_ops);
}


void Block2x2JacobiSolverFactory::_do_set_default_parameters()
{
    auto& params = GetParameters();

    params.Get<bool>("Use Negative S",true);
    params.Get<double>("Alpha",1.0);
    params.Get("S Type","NONE");
}


void Block2x2JacobiSolverFactory::_do_initialize(ParameterList const&)
{
    auto& params = GetParameters();

    UseNegativeS_ = params.Get<bool>("Use Negative S",true);

    // Nested preconditioner, so I assert that the Lib_ member is non-null
    PARELAG_ASSERT(HasValidSolverLibrary());

    // Build the A00_1 solver factory
    std::string invA00_name =
        params.Get("A00 Inverse", "Default Hypre");

    InvA00_Fact_ = GetSolverLibrary().GetSolverFactory(invA00_name);

    // Build the A00_2 solver factory
    std::string invA11_name =
        params.Get("A11 Inverse", "Default Hypre");

    InvA11_Fact_ = GetSolverLibrary().GetSolverFactory(invA11_name);

    // Build the S factory. If NONE, A11 will be used instead.
    std::string S_name = params.Get("S Type", "NONE");

    double alpha = params.Get("Alpha",1.0);

    if (S_name != "NONE")
        S_Fact_ = std::make_shared<SchurComplementFactory>(S_name,alpha);
    else
        S_Fact_ = nullptr;
}

}// namespace parelag
