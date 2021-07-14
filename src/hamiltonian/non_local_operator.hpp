// Copyright (c) 2013-2017 Anton Kozhevnikov, Thomas Schulthess
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without modification, are permitted provided that
// the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this list of conditions and the
//    following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions
//    and the following disclaimer in the documentation and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED
// WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
// PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
// ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
// OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

/** \file non_local_operator.hpp
 *
 *  \brief Contains declaration of sirius::Non_local_operator class.
 */

#ifndef __NON_LOCAL_OPERATOR_HPP__
#define __NON_LOCAL_OPERATOR_HPP__

#include "SDDK/memory.hpp"
#include "SDDK/type_definition.hpp"
#include "context/simulation_context.hpp"
#include "hubbard/hubbard_matrix.hpp"

namespace sddk {
template <typename T>
class Wave_functions;
class spin_range;
};

namespace sirius {
/* forward declaration */
class Beta_projectors;
class Beta_projectors_base;

/// Non-local part of the Hamiltonian and S-operator in the pseudopotential method.
class Non_local_operator
{
  protected:
    Simulation_context const& ctx_;

    sddk::device_t pu_;

    int packed_mtrx_size_;

    sddk::mdarray<int, 1> packed_mtrx_offset_;

    /// Non-local operator matrix.
    sddk::mdarray<double, 3> op_;

    bool is_null_{false};

    /// True if the operator is diagonal in spin.
    bool is_diag_{true};

    /* copy assignment operrator is forbidden */
    Non_local_operator& operator=(Non_local_operator const& src) = delete;
    /* copy constructor is forbidden */
    Non_local_operator(Non_local_operator const& src) = delete;

  public:
    /// Constructor.
    Non_local_operator(Simulation_context const& ctx__);

    /// Apply chunk of beta-projectors to all wave functions.
    template <typename T>
    void apply(int chunk__, int ispn_block__, sddk::Wave_functions<real_type<T>>& op_phi__, int idx0__, int n__,
               Beta_projectors_base& beta__, sddk::matrix<T>& beta_phi__);

    /// Apply beta projectors from one atom in a chunk of beta projectors to all wave-functions.
    template <typename T>
    void apply(int chunk__, int ia__, int ispn_block__, sddk::Wave_functions<real_type<T>>& op_phi__, int idx0__, int n__,
               Beta_projectors_base& beta__, sddk::matrix<T>& beta_phi__);

    template <typename T>
    inline T value(int xi1__, int xi2__, int ia__)
    {
        return this->value<T>(xi1__, xi2__, 0, ia__);
    }

    template <typename T>
    T value(int xi1__, int xi2__, int ispn__, int ia__);

    inline bool is_diag() const
    {
        return is_diag_;
    }
};

class D_operator : public Non_local_operator
{
  private:
    void initialize();

  public:
    D_operator(Simulation_context const& ctx_);
};

class Q_operator : public Non_local_operator
{
  private:
    void initialize();

  public:
    Q_operator(Simulation_context const& ctx__);
};

template <typename T>
class U_operator
{
  private:
    Simulation_context const& ctx_;
    sddk::mdarray<std::complex<T>, 3> um_;
    std::vector<int> offset_;
    int nhwf_;
  public:

    U_operator(Simulation_context const& ctx__, Hubbard_matrix const& um1__, std::array<double, 3> vk__)
        : ctx_(ctx__)
    {
        /* a pair of "total number, offests" for the Hubbard orbitals idexing */
        auto r = ctx_.unit_cell().num_hubbard_wf();
        this->nhwf_ = r.first;
        this->offset_ = r.second;
        um_ = sddk::mdarray<std::complex<T>, 3>(this->nhwf_, this->nhwf_, ctx_.num_mag_dims() + 1);
        um_.zero();

        /* copy only local blocks */
        // TODO: implement Fourier-transfomation of the T-dependent occupancy matrix
        // to get the generic k-dependent matrix
        for (int ia = 0; ia < ctx_.unit_cell().num_atoms(); ia++) {
            if (ctx_.unit_cell().atom(ia).type().hubbard_correction()) {
                int nb = ctx_.unit_cell().atom(ia).type().indexb_hub().size();
                for (int j = 0; j < ctx_.num_mag_dims() + 1; j++) {
                    for (int m1 = 0; m1 < nb; m1++) {
                        for (int m2 = 0; m2 < nb; m2++) {
                            um_(this->offset_[ia] + m1, this->offset_[ia] + m2, j) = um1__.local(ia)(m1, m2, j);
                        }
                    }
                }
            }
        }
    }

    ~U_operator()
    {
    }

    inline auto nhwf() const
    {
        return nhwf_;
    }

    inline auto offset(int ia__) const
    {
        return offset_[ia__];
    }

    std::complex<T> operator()(int m1, int m2, int j)
    {
        return um_(m1, m2, j);
    }
};

//template <typename T>
//class P_operator : public Non_local_operator<T>
//{
//  public:
//    P_operator(Simulation_context const& ctx_, mdarray<double_complex, 3>& p_mtrx__)
//        : Non_local_operator<T>(ctx_)
//    {
//        /* Q-operator is independent of spin */
//        this->op_ = mdarray<T, 2>(this->packed_mtrx_size_, 1);
//        this->op_.zero();
//
//        auto& uc = ctx_.unit_cell();
//        for (int ia = 0; ia < uc.num_atoms(); ia++) {
//            int iat = uc.atom(ia).type().id();
//            if (!uc.atom_type(iat).augment()) {
//                continue;
//            }
//            int nbf = uc.atom(ia).mt_basis_size();
//            for (int xi2 = 0; xi2 < nbf; xi2++) {
//                for (int xi1 = 0; xi1 < nbf; xi1++) {
//                    this->op_(this->packed_mtrx_offset_(ia) + xi2 * nbf + xi1, 0) = -p_mtrx__(xi1, xi2, iat).real();
//                }
//            }
//        }
//        if (this->pu_ == device_t::GPU) {
//            this->op_.allocate(memory_t::device);
//            this->op_.copy_to(memory_t::device);
//        }
//    }
//};

/// Apply non-local part of the Hamiltonian and S operators.
/** This operations must be combined because of the expensive inner product between wave-functions and beta
 *  projectors, which is computed only once.
 *
 *  \param [in]  spins   Range of the spin index.
 *  \param [in]  N       Starting index of the wave-functions.
 *  \param [in]  n       Number of wave-functions to which D and Q are applied.
 *  \param [in]  beta    Beta-projectors.
 *  \param [in]  phi     Wave-functions.
 *  \param [in]  d_op    Pointer to D-operator.
 *  \param [out] hphi    Resulting |beta>D<beta|phi>
 *  \param [in]  q_op    Pointer to Q-operator.
 *  \param [out] sphi    Resulting |beta>Q<beta|phi>
 */
template <typename T>
void
apply_non_local_d_q(sddk::spin_range spins__, int N__, int n__, Beta_projectors& beta__,
                    sddk::Wave_functions<real_type<T>>& phi__, D_operator* d_op__, sddk::Wave_functions<real_type<T>>* hphi__, Q_operator* q_op__,
                    sddk::Wave_functions<real_type<T>>* sphi__);

template <typename T>
void
apply_S_operator(sddk::device_t pu__, sddk::spin_range spins__, int N__, int n__, Beta_projectors& beta__,
                 sddk::Wave_functions<real_type<T>>& phi__, Q_operator* q_op__, sddk::Wave_functions<real_type<T>>& sphi__);

template <typename T>
void
apply_U_operator(Simulation_context& ctx__, spin_range spins__, int N__, int n__, Wave_functions<T>& hub_wf__,
    Wave_functions<T>& phi__, U_operator<T>& um__, Wave_functions<T>& hphi__);

} // namespace sirius

#endif
