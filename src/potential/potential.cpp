/* This file is part of SIRIUS electronic structure library.
 *
 * Copyright (c), ETH Zurich.  All rights reserved.
 *
 * Please, refer to the LICENSE file in the root directory.
 * SPDX-License-Identifier: BSD-3-Clause
 */

/** \file potential.cpp
 *
 *  \brief Generate effective potential.
 */

#include "potential.hpp"
#include "xc_functional.hpp"
#include "lapw/generate_gvec_ylm.hpp"
#include "lapw/generate_sbessel_mt.hpp"
#include "symmetry/symmetrize_field4d.hpp"
#include "dft/energy.hpp"

namespace sirius {

Potential::Potential(Simulation_context& ctx__)
    : Field4D(ctx__, lmax_t(ctx__.lmax_pot()),
              {ctx__.periodic_function_ptr("veff"), ctx__.periodic_function_ptr("bz"),
               ctx__.periodic_function_ptr("bx"), ctx__.periodic_function_ptr("by")})
    , unit_cell_(ctx__.unit_cell())
    , comm_(ctx__.comm())
    , hubbard_potential_(ctx__)
{
    PROFILE("sirius::Potential");

    if (!ctx_.initialized()) {
        RTE_THROW("Simulation_context is not initialized");
    }

    int lmax{-1};

    if (ctx_.full_potential()) {
        lmax = std::max(ctx_.lmax_rho(), ctx_.lmax_pot());
    } else {
        lmax = 2 * ctx_.unit_cell().lmax();
    }
    lmax = std::max(lmax, ctx_.cfg().settings().sht_lmax());

    if (lmax >= 0) {
        sht_ = std::make_unique<SHT>(ctx_.processing_unit(), lmax, ctx_.cfg().settings().sht_coverage());
        if (ctx_.cfg().control().verification() >= 1) {
            sht_->check();
        }
        l_by_lm_ = sf::l_by_lm(lmax);

        /* precompute i^l */
        zil_.resize(lmax + 1);
        for (int l = 0; l <= lmax; l++) {
            zil_[l] = std::pow(std::complex<double>(0, 1), l);
        }

        zilm_.resize(sf::lmmax(lmax));
        for (int l = 0, lm = 0; l <= lmax; l++) {
            for (int m = -l; m <= l; m++, lm++) {
                zilm_[lm] = zil_[l];
            }
        }
    }

    /* create list of XC functionals */
    for (auto& xc_label : ctx_.xc_functionals()) {
        xc_func_.emplace_back(
                XC_functional(ctx_.spfft<double>(), ctx_.unit_cell().lattice_vectors(), xc_label, ctx_.num_spins()));
        if (ctx_.cfg().parameters().xc_dens_tre() > 0) {
            xc_func_.back().set_dens_threshold(ctx_.cfg().parameters().xc_dens_tre());
        }
    }

    using pf  = Periodic_function<double>;
    using spf = Smooth_periodic_function<double>;

    if (ctx_.full_potential()) {
        hartree_potential_ = std::make_unique<pf>(
                ctx_, [&](int ia) { return lmax_t(ctx_.lmax_pot()); }, &ctx_.unit_cell().spl_num_atoms());
        xc_potential_ = std::make_unique<pf>(
                ctx_, [&](int ia) { return lmax_t(ctx_.lmax_pot()); }, &ctx_.unit_cell().spl_num_atoms());
        xc_energy_density_ = std::make_unique<pf>(
                ctx_, [&](int ia) { return lmax_t(ctx_.lmax_pot()); }, &ctx_.unit_cell().spl_num_atoms());
    } else {
        hartree_potential_ = std::make_unique<pf>(ctx_);
        xc_potential_      = std::make_unique<pf>(ctx_);
        xc_energy_density_ = std::make_unique<pf>(ctx_);
    }

    if (this->is_gradient_correction()) {
        int nsigma = (ctx_.num_spins() == 1) ? 1 : 3;
        for (int i = 0; i < nsigma; i++) {
            vsigma_[i] = std::make_unique<spf>(ctx_.spfft<double>(), ctx_.gvec_fft_sptr());
        }
    }

    if (!ctx_.full_potential()) {
        local_potential_ = std::make_unique<spf>(ctx_.spfft<double>(), ctx_.gvec_fft_sptr());
        dveff_           = std::make_unique<spf>(ctx_.spfft<double>(), ctx_.gvec_fft_sptr());
        dveff_->zero();
    }

    vh_el_ = mdarray<double, 1>({unit_cell_.num_atoms()});

    if (ctx_.full_potential()) {
        gvec_ylm_ =
                mdarray<std::complex<double>, 2>({ctx_.lmmax_pot(), ctx_.gvec().count()}, mdarray_label("gvec_ylm_"));

        switch (ctx_.valence_relativity()) {
            case relativity_t::iora: {
                rm2_inv_pw_ = mdarray<std::complex<double>, 1>({ctx_.gvec().num_gvec()});
            }
            case relativity_t::zora: {
                rm_inv_pw_ = mdarray<std::complex<double>, 1>({ctx_.gvec().num_gvec()});
            }
            default: {
                veff_pw_ = mdarray<std::complex<double>, 1>({ctx_.gvec().num_gvec()});
            }
        }
    }

    aux_bf_ = mdarray<double, 2>({3, ctx_.unit_cell().num_atoms()});
    aux_bf_.zero();

    if (ctx_.cfg().parameters().reduce_aux_bf() > 0 && ctx_.cfg().parameters().reduce_aux_bf() < 1) {
        for (int ia = 0; ia < ctx_.unit_cell().num_atoms(); ia++) {
            for (int x : {0, 1, 2}) {
                aux_bf_(x, ia) = 1;
            }
        }
    }

    if (!ctx_.full_potential()) {
        d_mtrx_.resize(unit_cell_.num_atoms());
        for (int ia = 0; ia < unit_cell_.num_atoms(); ia++) {
            int nbf     = unit_cell_.atom(ia).mt_basis_size();
            d_mtrx_[ia] = mdarray<double, 3>({nbf, nbf, ctx_.num_mag_dims() + 1}, mdarray_label("d_mtrx_"));
            d_mtrx_[ia].zero();
        }
    }

    /* in case of PAW */
    init_PAW();

    if (ctx_.hubbard_correction()) {
        U_ = std::make_unique<Hubbard>(ctx_);
    }

    update();
}

void
Potential::update()
{
    PROFILE("sirius::Potential::update");

    if (!ctx_.full_potential()) {
        local_potential_->zero();
        generate_local_potential();
        ewald_energy_ = sirius::ewald_energy(ctx_, ctx_.gvec(), ctx_.unit_cell());
    } else {
        gvec_ylm_ = generate_gvec_ylm(ctx_, ctx_.lmax_pot());

        auto lmax   = std::max(ctx_.lmax_rho(), ctx_.lmax_pot());
        sbessel_mt_ = generate_sbessel_mt(ctx_, lmax + pseudo_density_order_ + 1);

        /* compute moments of spherical Bessel functions
         *
         * In[]:= Integrate[SphericalBesselJ[l,G*x]*x^(2+l),{x,0,R},Assumptions->{R>0,G>0,l>=0}]
         * Out[]= (Sqrt[\[Pi]/2] R^(3/2+l) BesselJ[3/2+l,G R])/G^(3/2)
         *
         * and use relation between Bessel and spherical Bessel functions:
         * Subscript[j, n](z)=Sqrt[\[Pi]/2]/Sqrt[z]Subscript[J, n+1/2](z) */
        sbessel_mom_ = mdarray<double, 3>({ctx_.lmax_rho() + 1, ctx_.gvec().count(), unit_cell_.num_atom_types()},
                                          mdarray_label("sbessel_mom_"));
        sbessel_mom_.zero();
        int ig0{0};
        if (ctx_.comm().rank() == 0) {
            /* for |G| = 0 */
            for (int iat = 0; iat < unit_cell_.num_atom_types(); iat++) {
                sbessel_mom_(0, 0, iat) = std::pow(unit_cell_.atom_type(iat).mt_radius(), 3) / 3.0;
            }
            ig0 = 1;
        }
        for (int iat = 0; iat < unit_cell_.num_atom_types(); iat++) {
            #pragma omp parallel for schedule(static)
            for (int igloc = ig0; igloc < ctx_.gvec().count(); igloc++) {
                auto len = ctx_.gvec().gvec_cart(gvec_index_t::local(igloc)).length();
                for (int l = 0; l <= ctx_.lmax_rho(); l++) {
                    sbessel_mom_(l, igloc, iat) = std::pow(unit_cell_.atom_type(iat).mt_radius(), l + 2) *
                                                  sbessel_mt_(l + 1, igloc, iat) / len;
                }
            }
        }

        /* compute Gamma[5/2 + n + l] / Gamma[3/2 + l] / R^l
         *
         * use Gamma[1/2 + p] = (2p - 1)!!/2^p Sqrt[Pi] */
        gamma_factors_R_ = mdarray<double, 2>({ctx_.lmax_rho() + 1, unit_cell_.num_atom_types()},
                                              mdarray_label("gamma_factors_R_"));
        for (int iat = 0; iat < unit_cell_.num_atom_types(); iat++) {
            for (int l = 0; l <= ctx_.lmax_rho(); l++) {
                long double Rl = std::pow(unit_cell_.atom_type(iat).mt_radius(), l);

                int n_min = (2 * l + 3);
                int n_max = (2 * l + 1) + (2 * pseudo_density_order_ + 2);
                /* split factorial product into two parts to avoid overflow */
                long double f1 = 1.0;
                long double f2 = 1.0;
                for (int n = n_min; n <= n_max; n += 2) {
                    if (f1 < Rl) {
                        f1 *= (n / 2.0);
                    } else {
                        f2 *= (n / 2.0);
                    }
                }
                gamma_factors_R_(l, iat) = static_cast<double>((f1 / Rl) * f2);
            }
        }
    }

    // VDWXC depends on unit cell, which might have changed.
    for (auto& xc : xc_func_) {
        if (xc.is_vdw()) {
            xc.vdw_update_unit_cell(ctx_.spfft<double>(), ctx_.unit_cell().lattice_vectors());
        }
    }
}

bool
Potential::is_gradient_correction() const
{
    bool is_gga{false};
    for (auto& ixc : xc_func_) {
        if (ixc.is_gga() || ixc.is_vdw()) {
            is_gga = true;
        }
    }
    return is_gga;
}

void
Potential::generate(Density const& density__, bool use_symmetry__, bool transform_to_rg__)
{
    PROFILE("sirius::Potential::generate");

    if (!ctx_.full_potential()) {
        /* save current effective potential */
        for (size_t ig = 0; ig < effective_potential().rg().f_pw_local().size(); ig++) {
            dveff_->f_pw_local(ig) = effective_potential().rg().f_pw_local(ig);
        }
    }

    /* zero effective potential and magnetic field */
    zero();

    auto veff_callback = ctx_.veff_callback();
    if (veff_callback) {
        veff_callback();
        // if (!ctx_.full_potential()) {
        //     /* add local ionic potential to the effective potential */
        //     effective_potential().add(local_potential());
        // }
        /* transform to real space */
        // fft_transform(1);
    } else {
        /* solve Poisson equation */
        poisson(density__.rho());

        /* add Hartree potential to the total potential */
        effective_potential() += hartree_potential();

        if (env::print_hash()) {
            auto h = effective_potential().rg().hash_f_rg();
            print_hash("Vha", h, ctx_.out());
        }

        if (ctx_.full_potential()) {
            xc(density__);
        } else {
            /* add local ionic potential to the effective potential */
            effective_potential().rg() += local_potential();
            /* construct XC potentials from rho + rho_core */
            xc<true>(density__);
        }
        /* add XC potential to the effective potential */
        effective_potential() += xc_potential();

        if (env::print_hash()) {
            auto h = effective_potential().rg().hash_f_rg();
            print_hash("Vha+Vxc", h, ctx_.out());
        }

        if (ctx_.full_potential()) {
            effective_potential().mt().sync(ctx_.unit_cell().spl_num_atoms());
            for (int j = 0; j < ctx_.num_mag_dims(); j++) {
                effective_magnetic_field(j).mt().sync(ctx_.unit_cell().spl_num_atoms());
            }
        }

        /* get plane-wave coefficients of effective potential;
         * they will be used in three places:
         *  1) compute D-matrix
         *  2) establish a mapping between fine and coarse FFT grid for the Hloc operator
         *  3) symmetrize effective potential */
        this->fft_transform(-1);
    }

    if (use_symmetry__) {
        /* symmetrize potential and effective magnetic field */
        symmetrize_field4d(*this);
        if (transform_to_rg__) {
            /* transform potential to real space after symmetrization */
            this->fft_transform(1);
        }
    }

    if (!ctx_.full_potential()) {
        /* this is needed later to compute scf correction to forces */
        for (size_t ig = 0; ig < effective_potential().rg().f_pw_local().size(); ig++) {
            dveff_->f_pw_local(ig) = effective_potential().rg().f_pw_local(ig) - dveff_->f_pw_local(ig);
        }
    }

    if (env::print_hash()) {
        auto h = effective_potential().rg().hash_f_pw();
        print_hash("V(G)", h, ctx_.out());
    }

    if (!ctx_.full_potential()) {
        generate_d_mtrx();
        generate_PAW_effective_potential(density__);
        if (ctx_.verbosity() >= 3) {
            rte::ostream out(ctx_.out(), "potential");
            out << "density matrix" << std::endl;
            for (int ia = 0; ia < ctx_.unit_cell().num_atoms(); ia++) {
                auto& atom = ctx_.unit_cell().atom(ia);
                out << "atom : " << ia << std::endl;
                for (int imagn = 0; imagn < ctx_.num_mag_comp(); imagn++) {
                    out << "  imagn : " << imagn << std::endl;
                    for (int ib2 = 0; ib2 < atom.mt_basis_size(); ib2++) {
                        out << "    ";
                        for (int ib1 = 0; ib1 < atom.mt_basis_size(); ib1++) {
                            out << ffmt(8, 3) << density__.density_matrix(ia)(ib1, ib2, imagn);
                        }
                        out << std::endl;
                    }
                }
            }

            out << "D operator matrix" << std::endl;
            for (int ia = 0; ia < ctx_.unit_cell().num_atoms(); ia++) {
                auto& atom = ctx_.unit_cell().atom(ia);
                out << "atom : " << ia << std::endl;
                for (int imagn = 0; imagn < ctx_.num_mag_dims() + 1; imagn++) {
                    out << "  imagn : " << imagn << std::endl;
                    for (int ib2 = 0; ib2 < atom.mt_basis_size(); ib2++) {
                        out << "    ";
                        for (int ib1 = 0; ib1 < atom.mt_basis_size(); ib1++) {
                            out << ffmt(8, 3) << d_mtrx_[ia](ib1, ib2, imagn);
                        }
                        out << std::endl;
                    }
                }
            }
        }
    }

    if (ctx_.hubbard_correction()) {
        ::sirius::generate_potential(density__.occupation_matrix(), this->hubbard_potential());
    }

    if (ctx_.cfg().parameters().reduce_aux_bf() > 0 && ctx_.cfg().parameters().reduce_aux_bf() < 1) {
        for (int ia = 0; ia < ctx_.unit_cell().num_atoms(); ia++) {
            for (int x : {0, 1, 2}) {
                aux_bf_(x, ia) *= ctx_.cfg().parameters().reduce_aux_bf();
            }
        }
    }
}

void
Potential::update_atomic_potential()
{
    for (int ic = 0; ic < unit_cell_.num_atom_symmetry_classes(); ic++) {
        int ia   = unit_cell_.atom_symmetry_class(ic).atom_id(0);
        int nmtp = unit_cell_.atom(ia).num_mt_points();

        std::vector<double> veff(nmtp);

        for (int ir = 0; ir < nmtp; ir++) {
            veff[ir] = y00 * effective_potential().mt()[ia](0, ir);
        }

        unit_cell_.atom_symmetry_class(ic).set_spherical_potential(veff);
    }

    for (int ia = 0; ia < unit_cell_.num_atoms(); ia++) {
        double* veff = &effective_potential().mt()[ia](0, 0);

        double* beff[] = {nullptr, nullptr, nullptr};
        for (int i = 0; i < ctx_.num_mag_dims(); i++) {
            beff[i] = &effective_magnetic_field(i).mt()[ia](0, 0);
        }

        unit_cell_.atom(ia).set_nonspherical_potential(veff, beff);
    }
}

} // namespace sirius
