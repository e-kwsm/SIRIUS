// Copyright (c) 2013-2014 Anton Kozhevnikov, Thomas Schulthess
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

/** \file atom.cpp
 *   
 *  \brief Contains remaining implementation of sirius::Atom class.
 */

#include "atom.h"

namespace sirius {

Atom::Atom(Atom_type* type__, double* position__, double* vector_field__) 
    : type_(type__), 
      symmetry_class_(NULL), 
      offset_aw_(-1), 
      offset_lo_(-1), 
      offset_wf_(-1), 
      apply_uj_correction_(false), 
      uj_correction_l_(-1)
{
    assert(type__);
        
    for (int i = 0; i < 3; i++)
    {
        position_[i] = position__[i];
        vector_field_[i] = vector_field__[i];
    }
}

void Atom::init(int lmax_pot__, int num_mag_dims__, int offset_aw__, int offset_lo__, int offset_wf__)
{
    assert(lmax_pot__ >= -1);
    assert(offset_aw__ >= 0);
    
    offset_aw_ = offset_aw__;
    offset_lo_ = offset_lo__;
    offset_wf_ = offset_wf__;

    lmax_pot_ = lmax_pot__;
    num_mag_dims_ = num_mag_dims__;

    if (type()->esm_type() == full_potential_lapwlo || type()->esm_type() == full_potential_pwlo)
    {
        int lmmax = Utils::lmmax(lmax_pot_);

        h_radial_integrals_.set_dimensions(lmmax, type()->indexr().size(), type()->indexr().size());
        h_radial_integrals_.allocate();
        
        veff_.set_dimensions(lmmax, type()->num_mt_points());
        
        b_radial_integrals_.set_dimensions(lmmax, type()->indexr().size(), type()->indexr().size(), num_mag_dims_);
        b_radial_integrals_.allocate();
        
        for (int j = 0; j < 3; j++) beff_[j].set_dimensions(lmmax, type()->num_mt_points());

        occupation_matrix_.set_dimensions(16, 16, 2, 2);
        occupation_matrix_.allocate();
        
        uj_correction_matrix_.set_dimensions(16, 16, 2, 2);
        uj_correction_matrix_.allocate();
    }

    if (type()->esm_type() == ultrasoft_pseudopotential)
    {
        d_mtrx_.set_dimensions(type()->mt_lo_basis_size(), type()->mt_lo_basis_size());
        d_mtrx_.allocate();
    }
}

void Atom::generate_radial_integrals(MPI_Comm& comm)
{
    Timer t("sirius::Atom::generate_radial_integrals");
    
    int lmmax = Utils::lmmax(lmax_pot_);
    int nmtp = type()->num_mt_points();

    splindex<block> spl_lm(lmmax, Platform::num_mpi_ranks(comm), Platform::mpi_rank(comm));

    std::vector<int> l_by_lm = Utils::l_by_lm(lmax_pot_);

    h_radial_integrals_.zero();
    if (num_mag_dims_) b_radial_integrals_.zero();

    /* interpolate radial functions */
    std::vector< Spline<double> > rf_spline(type()->indexr().size(), Spline<double>(type()->radial_grid()));
    for (int i = 0; i < type()->indexr().size(); i++)
    {
        for (int ir = 0; ir < nmtp; ir++) rf_spline[i][ir] = symmetry_class()->radial_function(ir, i);
        rf_spline[i].interpolate();
    }
    
    #pragma omp parallel default(shared)
    {
        /* potential or magnetic field times a radial function */
        std::vector< Spline<double> > vrf_spline(1 + num_mag_dims_, Spline<double>(type()->radial_grid()));

        for (int lm_loc = 0; lm_loc < (int)spl_lm.local_size(); lm_loc++)
        {
            int lm = (int)spl_lm[lm_loc];
            int l = l_by_lm[lm];

            #pragma omp for
            for (int i2 = 0; i2 < type()->indexr().size(); i2++)
            {
                int l2 = type()->indexr(i2).l;
                
                /* multiply potential by a radial function */
                for (int ir = 0; ir < nmtp; ir++) 
                    vrf_spline[0][ir] = symmetry_class()->radial_function(ir, i2) * veff_(lm, ir);
                vrf_spline[0].interpolate();
                /* multiply magnetic field by a radial function */
                for (int j = 0; j < num_mag_dims_; j++)
                {
                    for (int ir = 0; ir < nmtp; ir++) 
                        vrf_spline[1 + j][ir] = symmetry_class()->radial_function(ir, i2) * beff_[j](lm, ir);
                    vrf_spline[1 + j].interpolate();
                }
                
                for (int i1 = 0; i1 <= i2; i1++)
                {
                    int l1 = type()->indexr(i1).l;
                    if ((l + l1 + l2) % 2 == 0)
                    {
                        if (lm)
                        {
                            h_radial_integrals_(lm, i1, i2) = h_radial_integrals_(lm, i2, i1) = 
                                Spline<double>::integrate(&rf_spline[i1], &vrf_spline[0], 2);
                        }
                        else
                        {
                            h_radial_integrals_(0, i1, i2) = symmetry_class()->h_spherical_integral(i1, i2);
                            h_radial_integrals_(0, i2, i1) = symmetry_class()->h_spherical_integral(i2, i1);
                        }
                        for (int j = 0; j < num_mag_dims_; j++)
                        {
                            b_radial_integrals_(lm, i1, i2, j) = b_radial_integrals_(lm, i2, i1, j) = 
                                Spline<double>::integrate(&rf_spline[i1], &vrf_spline[1 + j], 2);
                        }
                    }
                }
            }
        }
    }

    Platform::reduce(h_radial_integrals_.ptr(), (int)h_radial_integrals_.size(), comm, 0);
    if (num_mag_dims_) Platform::reduce(b_radial_integrals_.ptr(), (int)b_radial_integrals_.size(), comm, 0);
}

}

