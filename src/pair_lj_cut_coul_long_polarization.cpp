/* ----------------------------------------------------------------------
   LAMMPS - Large-scale Atomic/Molecular Massively Parallel Simulator
   http://lammps.sandia.gov, Sandia National Laboratories
   Steve Plimpton, sjplimp@sandia.gov

   Copyright (2003) Sandia Corporation.  Under the terms of Contract
   DE-AC04-94AL85000 with Sandia Corporation, the U.S. Government retains
   certain rights in this software.  This software is distributed under 
   the GNU General Public License.

   See the README file in the top-level LAMMPS directory.
------------------------------------------------------------------------- */

/* ----------------------------------------------------------------------
   Contributing author: Paul Crozier (SNL)
                        Adam Hogan (USF)
------------------------------------------------------------------------- */

#include "math.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "pair_lj_cut_coul_long_polarization.h"
#include "atom.h"
#include "comm.h"
#include "force.h"
#include "kspace.h"
#include "update.h"
#include "integrate.h"
#include "respa.h"
#include "neighbor.h"
#include "neigh_list.h"
#include "neigh_request.h"
#include "math_const.h"
#include "memory.h"
#include "error.h"
#include "mpi.h"
#include "float.h"
#include "domain.h"
#include "unistd.h"

using namespace LAMMPS_NS;
using namespace MathConst;

#define EWALD_F   1.12837917
#define EWALD_P   0.3275911
#define A1        0.254829592
#define A2       -0.284496736
#define A3        1.421413741
#define A4       -1.453152027
#define A5        1.061405429

enum{DAMPING_EXPONENTIAL,DAMPING_NONE};

/* ---------------------------------------------------------------------- */

PairLJCutCoulLongPolarization::PairLJCutCoulLongPolarization(LAMMPS *lmp) : Pair(lmp)
{
  /* check for possible errors */
  if (atom->static_polarizability_flag==0) error->all(FLERR,"Pair style lj/cut/coul/long/polarization requires atom attribute polarizability");

  respa_enable = 0;
  ftable = NULL;
  /* set defaults */
  iterations_max = 50;
  damping_type = DAMPING_NONE;
  polar_damp = 2.1304;
  zodid = 0;
  polar_precision = 0.00000000001;
  fixed_iteration = 0;

  polar_gs = 0;
  polar_gs_ranked = 1;
  polar_gamma = 1.03;

  use_previous = 0;

  debug = 0;
  /* end defaults */

  /* create arrays */
  int nlocal = atom->nlocal;
  memory->create(ef_induced,nlocal,3,"pair:ef_induced");
  memory->create(mu_induced_new,nlocal,3,"pair:mu_induced_new");
  memory->create(mu_induced_old,nlocal,3,"pair:mu_induced_old");
  memory->create(dipole_field_matrix,3*nlocal,3*nlocal,"pair:dipole_field_matrix");
  memory->create(ranked_array,nlocal,"pair:ranked_array");
  memory->create(rank_metric,nlocal,"pair:rank_metric");
  nlocal_old = nlocal;
}

/* ---------------------------------------------------------------------- */

PairLJCutCoulLongPolarization::~PairLJCutCoulLongPolarization()
{
  if (allocated) {
    memory->destroy(setflag);
    memory->destroy(cutsq);

    memory->destroy(cut_lj);
    memory->destroy(cut_ljsq);
    memory->destroy(epsilon);
    memory->destroy(sigma);
    memory->destroy(lj1);
    memory->destroy(lj2);
    memory->destroy(lj3);
    memory->destroy(lj4);
    memory->destroy(offset);
  }
  if (ftable) free_tables();
  /* destroy all the arrays! */
  memory->destroy(ef_induced);
  memory->destroy(mu_induced_new);
  memory->destroy(mu_induced_old);
  memory->destroy(dipole_field_matrix);
  memory->destroy(ranked_array);
  memory->destroy(rank_metric);
}

/* ---------------------------------------------------------------------- */

void PairLJCutCoulLongPolarization::compute(int eflag, int vflag)
{
  int nlocal = atom->nlocal;
  int nghost = atom->nghost;
  int ntotal = nlocal + nghost;
  int i;

  /* reallocate arrays if number of atoms grew */
  if (nlocal > nlocal_old)
  {
    memory->destroy(ef_induced);
    memory->create(ef_induced,nlocal,3,"pair:ef_induced");
    memory->destroy(mu_induced_new);
    memory->create(mu_induced_new,nlocal,3,"pair:mu_induced_new");
    memory->destroy(mu_induced_old);
    memory->create(mu_induced_old,nlocal,3,"pair:mu_induced_old");
    memory->destroy(ranked_array);
    memory->create(ranked_array,nlocal,"pair:ranked_array");
    memory->destroy(dipole_field_matrix);
    memory->create(dipole_field_matrix,3*nlocal,3*nlocal,"pair:dipole_field_matrix");
    memory->destroy(rank_metric);
    memory->create(rank_metric,nlocal,"pair:rank_metric");
    nlocal_old = nlocal;
  }
  double **ef_static = atom->ef_static;
  for (i = 0; i < nlocal; i++)
  {
    ef_static[i][0] = 0;
    ef_static[i][1] = 0;
    ef_static[i][2] = 0;
  }
  double ef_temp;

  int ii,j,jj,inum,jnum,itype,jtype,itable;
  double qtmp,xtmp,ytmp,ztmp,delx,dely,delz,evdwl,ecoul,fpair;
  double fraction,table;
  double r,rinv,r2inv,r6inv,forcecoul,forcelj,factor_coul,factor_lj;
  double grij,expm2,prefactor,t,erfc_ewald_stuff;
  int *ilist,*jlist,*numneigh,**firstneigh;
  double rsq;

  evdwl = ecoul = 0.0;
  if (eflag || vflag) ev_setup(eflag,vflag);
  else evflag = vflag_fdotr = 0;

  double **x = atom->x;
  double **f = atom->f;
  double *q = atom->q;
  int *type = atom->type;
  double *special_coul = force->special_coul;
  double *special_lj = force->special_lj;
  int newton_pair = force->newton_pair;
  double qqrd2e = force->qqrd2e;

  inum = list->inum;
  ilist = list->ilist;
  numneigh = list->numneigh;
  firstneigh = list->firstneigh;

  double *static_polarizability = atom->static_polarizability;
  int *molecule = atom->molecule;

  /* sort the dipoles most likey to change if using polar_gs_ranked */
  if (polar_gs_ranked) {
    /* communicate static polarizabilities */
    comm->forward_comm_pair(this);
    MPI_Barrier(world);
    rmin = 1000.0;
    for (i=0;i<nlocal;i++)
    {
      for (j=0;j<ntotal;j++)
      {
        if(i != j) {
          r = sqrt(pow(x[i][0]-x[j][0],2)+pow(x[i][1]-x[j][1],2)+pow(x[i][2]-x[j][2],2));
          if (static_polarizability[i]>0&&static_polarizability[j]>0&&rmin>r&&((molecule[i]!=molecule[j])||molecule[i]==0))
          {
            rmin = r;
          }
        }
      }
    }
    for (i=0;i<nlocal;i++)
    {
      rank_metric[i] = 0;
    }
    for (i=0;i<nlocal;i++)
    {
      for (j=0;j<ntotal;j++)
      {
        if(i != j) {
          r = sqrt(pow(x[i][0]-x[j][0],2)+pow(x[i][1]-x[j][1],2)+pow(x[i][2]-x[j][2],2));
          if (rmin*1.5>r&&((molecule[i]!=molecule[j])||molecule[i]==0))
          {
            rank_metric[i]+=static_polarizability[i]*static_polarizability[j];
          }
        }
      }
    }
  }

  /* loop over neighbors of my atoms */
  for (ii = 0; ii < inum; ii++) {
    i = ilist[ii];
    qtmp = q[i];
    xtmp = x[i][0];
    ytmp = x[i][1];
    ztmp = x[i][2];
    itype = type[i];
    jlist = firstneigh[i];
    jnum = numneigh[i];

    for (jj = 0; jj < jnum; jj++) {
      j = jlist[jj];
      factor_lj = special_lj[sbmask(j)];
      factor_coul = special_coul[sbmask(j)];
      j &= NEIGHMASK;

      delx = xtmp - x[j][0];
      dely = ytmp - x[j][1];
      delz = ztmp - x[j][2];
      rsq = delx*delx + dely*dely + delz*delz;
      jtype = type[j];

      if (rsq < cutsq[itype][jtype]) {
        r2inv = 1.0/rsq;
        if (rsq < cut_coulsq) {
          r = sqrt(rsq);
          if (!ncoultablebits || rsq <= tabinnersq) {
            grij = g_ewald * r;
            expm2 = exp(-grij*grij);
            t = 1.0 / (1.0 + EWALD_P*grij);
            erfc_ewald_stuff = t * (A1+t*(A2+t*(A3+t*(A4+t*A5)))) * expm2;
            prefactor = qqrd2e * qtmp*q[j]/r;
            forcecoul = prefactor * (erfc_ewald_stuff + EWALD_F*grij*expm2);
            if (factor_coul < 1.0) forcecoul -= (1.0-factor_coul)*prefactor;
          } else {
            union_int_float_t rsq_lookup;
            rsq_lookup.f = rsq;
            itable = rsq_lookup.i & ncoulmask;
            itable >>= ncoulshiftbits;
            fraction = (rsq_lookup.f - rtable[itable]) * drtable[itable];
            table = ftable[itable] + fraction*dftable[itable];
            forcecoul = qtmp*q[j] * table;
            if (factor_coul < 1.0) {
              table = ctable[itable] + fraction*dctable[itable];
              prefactor = qtmp*q[j] * table;
              forcecoul -= (1.0-factor_coul)*prefactor;
            }
          }
        } else forcecoul = 0.0;

        if (rsq < cut_ljsq[itype][jtype]) {
          r6inv = r2inv*r2inv*r2inv;
          forcelj = r6inv * (lj1[itype][jtype]*r6inv - lj2[itype][jtype]);
        } else forcelj = 0.0;

        fpair = (forcecoul + factor_lj*forcelj) * r2inv;

        f[i][0] += delx*fpair;
        f[i][1] += dely*fpair;
        f[i][2] += delz*fpair;
        if (newton_pair || j < nlocal) {
          f[j][0] -= delx*fpair;
          f[j][1] -= dely*fpair;
          f[j][2] -= delz*fpair;
        }

        if (eflag) {
          if (rsq < cut_coulsq) {
            if (!ncoultablebits || rsq <= tabinnersq) {
              ecoul = prefactor*erfc_ewald_stuff;
            }
            else {
              table = etable[itable] + fraction*detable[itable];
              ecoul = qtmp*q[j] * table;
            }
            if (factor_coul < 1.0) ecoul -= (1.0-factor_coul)*prefactor;
          } else ecoul = 0.0;

          if (rsq < cut_ljsq[itype][jtype]) {
            evdwl = r6inv*(lj3[itype][jtype]*r6inv-lj4[itype][jtype]) -
              offset[itype][jtype];
            evdwl *= factor_lj;
          } else evdwl = 0.0;
        }
        if (evflag) ev_tally(i,j,nlocal,newton_pair,
                 evdwl,ecoul,fpair,delx,dely,delz);
      }
    }
  }

  double f_shift = -1.0/(cut_coul*cut_coul); 
  double dvdrr;
  double xjimage[3];

  /* calculate static electric field using minimum image */
  for (i = 0; i < nlocal; i++) {
    qtmp = q[i];
    xtmp = x[i][0];
    ytmp = x[i][1];
    ztmp = x[i][2];

    for (j = i+1; j < nlocal; j++) {
      domain->closest_image(x[i],x[j],xjimage);
      delx = xtmp - xjimage[0];
      dely = ytmp - xjimage[1];
      delz = ztmp - xjimage[2];
      rsq = delx*delx + dely*dely + delz*delz;

      if (rsq <= cut_coulsq)
      {
        if ( (molecule[i]!=molecule[j])||molecule[i]==0 )
        {
          r = sqrt(rsq);

          /* Use wolf to calculate the electric field (no damping) */
          dvdrr = 1.0/rsq + f_shift;
          ef_temp = dvdrr*1.0/r;

          ef_static[i][0] += ef_temp*q[j]*delx;
          ef_static[i][1] += ef_temp*q[j]*dely;
          ef_static[i][2] += ef_temp*q[j]*delz;
          ef_static[j][0] -= ef_temp*qtmp*delx;
          ef_static[j][1] -= ef_temp*qtmp*dely;
          ef_static[j][2] -= ef_temp*qtmp*delz;
        }
      }
    }
  }

  double **mu_induced = atom->mu_induced;

  int p,iterations;

  double elementary_charge_to_sqrt_energy_length = sqrt(qqrd2e);

  /* set the static electric field and first guess to alpha*E */
  for (i = 0; i < nlocal; i++) {
    /* it is more convenient to work in gaussian-like units for charges and electric fields */
    ef_static[i][0] = ef_static[i][0]*elementary_charge_to_sqrt_energy_length;
    ef_static[i][1] = ef_static[i][1]*elementary_charge_to_sqrt_energy_length;
    ef_static[i][2] = ef_static[i][2]*elementary_charge_to_sqrt_energy_length;
    /* don't reset the induced dipoles if use_previous is on */
    if (!use_previous)
    {
      /* otherwise set it to alpha*E */
      mu_induced[i][0] = static_polarizability[i]*ef_static[i][0];
      mu_induced[i][1] = static_polarizability[i]*ef_static[i][1];
      mu_induced[i][2] = static_polarizability[i]*ef_static[i][2];
      mu_induced[i][0] *= polar_gamma;
      mu_induced[i][1] *= polar_gamma;
      mu_induced[i][2] *= polar_gamma;
    }
  }

  /* solve for the induced dipoles */
  if (!zodid) iterations = DipoleSolverIterative();
  else iterations = 0;
  if (debug) fprintf(screen,"iterations: %d\n",iterations);

  /* debugging energy calculation - not actually used, should be the same as the polarization energy
     calculated from the pairwise forces */
  double u_polar = 0.0;
  if (debug)
  {
    for (i=0;i<nlocal;i++)
    {
      u_polar += ef_static[i][0]*mu_induced[i][0] + ef_static[i][1]*mu_induced[i][1] + ef_static[i][2]*mu_induced[i][2];
    }
    u_polar *= -0.5;
    printf("u_polar: %.18f\n",u_polar);
  }

  /* variables for dipole forces */
  double forcecoulx,forcecouly,forcecoulz,fx,fy,fz;
  double r3inv,r5inv,r7inv,pdotp,pidotr,pjdotr,pre1,pre2,pre3,pre4,pre5;
  double ef_0,ef_1,ef_2;
  double **mu = mu_induced;
  double xsq,ysq,zsq,common_factor;
  double forcetotalx,forcetotaly,forcetotalz;
  double forcedipolex,forcedipoley,forcedipolez;
  double forceefx,forceefy,forceefz;
  forcetotalx = forcetotaly = forcetotalz = 0.0;
  forcedipolex = forcedipoley = forcedipolez = 0.0;
  forceefx = forceefy = forceefz = 0.0;

  /* dipole forces */
  u_polar = 0.0;
  double u_polar_self = 0.0;
  double u_polar_ef = 0.0;
  double u_polar_dd = 0.0;
  double term_1,term_2,term_3;
  for (i = 0; i < nlocal; i++) {
    qtmp = q[i];
    xtmp = x[i][0];
    ytmp = x[i][1];
    ztmp = x[i][2];

    /* self interaction energy */
    if (eflag&&static_polarizability[i]!=0.0)
      u_polar_self += 0.5 * (mu[i][0]*mu[i][0]+mu[i][1]*mu[i][1]+mu[i][2]*mu[i][2])/static_polarizability[i];

    for (j = i+1; j < nlocal; j++) {
      /* using minimum image again to be consistent */
      domain->closest_image(x[i],x[j],xjimage);
      delx = xtmp - xjimage[0];
      dely = ytmp - xjimage[1];
      delz = ztmp - xjimage[2];
      xsq = delx*delx;
      ysq = dely*dely;
      zsq = delz*delz;
      rsq = xsq + ysq + zsq;

      r2inv = 1.0/rsq;
      rinv = sqrt(r2inv);
      r = 1.0/rinv;
      r3inv = r2inv*rinv;

      forcecoulx = forcecouly = forcecoulz = 0.0;

      if (rsq < cut_coulsq)
      {

        if ( (molecule[i]!=molecule[j])||molecule[i]==0 )
        {
          /* using wolf again */
          dvdrr = 1.0/rsq + f_shift;
          ef_temp = dvdrr*1.0/r*elementary_charge_to_sqrt_energy_length;

          /* dipole on i, charge on j interaction */
          if (static_polarizability[i]!=0.0&&q[j]!=0.0)
          {
            common_factor = q[j]*elementary_charge_to_sqrt_energy_length*r3inv;
            forcecoulx += common_factor * (mu[i][0] * ((-2.0*xsq+ysq+zsq)*r2inv + f_shift*(ysq+zsq)) + \
                                           mu[i][1] * (-3.0*delx*dely*r2inv - f_shift*delx*dely) + \
                                           mu[i][2] * (-3.0*delx*delz*r2inv - f_shift*delx*delz));
            forcecouly += common_factor * (mu[i][0] * (-3.0*delx*dely*r2inv - f_shift*delx*dely) + \
                                           mu[i][1] * ((-2.0*ysq+xsq+zsq)*r2inv + f_shift*(xsq+zsq)) + \
                                           mu[i][2] * (-3.0*dely*delz*r2inv - f_shift*dely*delz));
            forcecoulz += common_factor * (mu[i][0] * (-3.0*delx*delz*r2inv - f_shift*delx*delz) + \
                                           mu[i][1] * (-3.0*dely*delz*r2inv - f_shift*dely*delz) + \
                                           mu[i][2] * ((-2.0*zsq+xsq+ysq)*r2inv + f_shift*(xsq+ysq)));
            if (eflag)
            {
              ef_0 = ef_temp*q[j]*delx;
              ef_1 = ef_temp*q[j]*dely;
              ef_2 = ef_temp*q[j]*delz;

              u_polar_ef -= mu[i][0]*ef_0 + mu[i][1]*ef_1 + mu[i][2]*ef_2;
            }
          }

          /* dipole on j, charge on i interaction */
          if (static_polarizability[j]!=0.0&&qtmp!=0.0)
          {
            common_factor = qtmp*elementary_charge_to_sqrt_energy_length*r3inv;
            forcecoulx -= common_factor * (mu[j][0] * ((-2.0*xsq+ysq+zsq)*r2inv + f_shift*(ysq+zsq)) + \
                                             mu[j][1] * (-3.0*delx*dely*r2inv - f_shift*delx*dely) + \
                                             mu[j][2] * (-3.0*delx*delz*r2inv - f_shift*delx*delz));
            forcecouly -= common_factor * (mu[j][0] * (-3.0*delx*dely*r2inv - f_shift*delx*dely) + \
                                             mu[j][1] * ((-2.0*ysq+xsq+zsq)*r2inv + f_shift*(xsq+zsq)) + \
                                             mu[j][2] * (-3.0*dely*delz*r2inv - f_shift*dely*delz));
            forcecoulz -= common_factor * (mu[j][0] * (-3.0*delx*delz*r2inv - f_shift*delx*delz) + \
                                             mu[j][1] * (-3.0*dely*delz*r2inv - f_shift*dely*delz) + \
                                             mu[j][2] * ((-2.0*zsq+xsq+ysq)*r2inv + f_shift*(xsq+ysq)));
            if (eflag)
            {
              ef_0 = ef_temp*qtmp*delx;
              ef_1 = ef_temp*qtmp*dely;
              ef_2 = ef_temp*qtmp*delz;

              u_polar_ef += mu[j][0]*ef_0 + mu[j][1]*ef_1 + mu[j][2]*ef_2;
            }
          }
        }
      }

      /* dipole on i, dipole on j interaction */
      if (static_polarizability[i]!=0.0 && static_polarizability[j]!=0.0)
      {
        /* exponential dipole-dipole damping */
        if(damping_type == DAMPING_EXPONENTIAL)
        {
          r5inv = r3inv*r2inv;
          r7inv = r5inv*r2inv;

          term_1 = exp(-polar_damp*r);
          term_2 = 1.0+polar_damp*r+0.5*polar_damp*polar_damp*r*r;
          term_3 = 1.0+polar_damp*r+0.5*polar_damp*polar_damp*r*r+1.0/6.0*polar_damp*polar_damp*polar_damp*r*r*r;

          pdotp = mu[i][0]*mu[j][0] + mu[i][1]*mu[j][1] + mu[i][2]*mu[j][2];
          pidotr = mu[i][0]*delx + mu[i][1]*dely + mu[i][2]*delz;
          pjdotr = mu[j][0]*delx + mu[j][1]*dely + mu[j][2]*delz;

          pre1 = 3.0*r5inv*pdotp*(1.0-term_1*term_2) - 15.0*r7inv*pidotr*pjdotr*(1.0-term_1*term_3);
          pre2 = 3.0*r5inv*pjdotr*(1.0-term_1*term_3);
          pre3 = 3.0*r5inv*pidotr*(1.0-term_1*term_3);
          pre4 = -pdotp*r3inv*(-term_1*(polar_damp*rinv+polar_damp*polar_damp) + term_1*polar_damp*term_2*rinv);
          pre5 = 3.0*pidotr*pjdotr*r5inv*(-term_1*(polar_damp*rinv+polar_damp*polar_damp+0.5*r*polar_damp*polar_damp*polar_damp)+term_1*polar_damp*term_3*rinv);

          forcecoulx += pre1*delx + pre2*mu[i][0] + pre3*mu[j][0] + pre4*delx + pre5*delx;
          forcecouly += pre1*dely + pre2*mu[i][1] + pre3*mu[j][1] + pre4*dely + pre5*dely;
          forcecoulz += pre1*delz + pre2*mu[i][2] + pre3*mu[j][2] + pre4*delz + pre5*delz;

          if (eflag)
          {
            u_polar_dd += r3inv*pdotp*(1.0-term_1*term_2) - 3.0*r5inv*pidotr*pjdotr*(1.0-term_1*term_3);
          }

          /* debug information */
          if (debug)
          {
            if (i==0)
            {
              forcedipolex += pre1*delx + pre2*mu[i][0] + pre3*mu[j][0] + pre4*delx + pre5*delx;
              forcedipoley += pre1*dely + pre2*mu[i][1] + pre3*mu[j][1] + pre4*dely + pre5*dely;
              forcedipolez += pre1*delz + pre2*mu[i][2] + pre3*mu[j][2] + pre4*delz + pre5*delz;
            }
            if (j==0)
            {
              forcedipolex -= pre1*delx + pre2*mu[i][0] + pre3*mu[j][0] + pre4*delx + pre5*delx;
              forcedipoley -= pre1*dely + pre2*mu[i][1] + pre3*mu[j][1] + pre4*dely + pre5*dely;
              forcedipolez -= pre1*delz + pre2*mu[i][2] + pre3*mu[j][2] + pre4*delz + pre5*delz;
            }
          }
          /* ---------------- */
        }
        /* no dipole-dipole damping */
        else
        {
          r5inv = r3inv*r2inv;
          r7inv = r5inv*r2inv;

          pdotp = mu[i][0]*mu[j][0] + mu[i][1]*mu[j][1] + mu[i][2]*mu[j][2];
          pidotr = mu[i][0]*delx + mu[i][1]*dely + mu[i][2]*delz;
          pjdotr = mu[j][0]*delx + mu[j][1]*dely + mu[j][2]*delz;

          pre1 = 3.0*r5inv*pdotp - 15.0*r7inv*pidotr*pjdotr;
          pre2 = 3.0*r5inv*pjdotr;
          pre3 = 3.0*r5inv*pidotr;

          forcecoulx += pre1*delx + pre2*mu[i][0] + pre3*mu[j][0];
          forcecouly += pre1*dely + pre2*mu[i][1] + pre3*mu[j][1];
          forcecoulz += pre1*delz + pre2*mu[i][2] + pre3*mu[j][2];

          if (eflag)
          {
            u_polar_dd += r3inv*pdotp - 3.0*r5inv*pidotr*pjdotr;
          }

          /* debug information */
          if (debug)
          {
            if (i==0)
            {
              forcedipolex += pre1*delx + pre2*mu[i][0] + pre3*mu[j][0];
              forcedipoley += pre1*dely + pre2*mu[i][1] + pre3*mu[j][1];
              forcedipolez += pre1*delz + pre2*mu[i][2] + pre3*mu[j][2];
            }
            if (j==0)
            {
              forcedipolex -= pre1*delx + pre2*mu[i][0] + pre3*mu[j][0];
              forcedipoley -= pre1*dely + pre2*mu[i][1] + pre3*mu[j][1];
              forcedipolez -= pre1*delz + pre2*mu[i][2] + pre3*mu[j][2];
            }
          }
          /* ---------------- */
        }
      }

      f[i][0] += forcecoulx;
      f[i][1] += forcecouly;
      f[i][2] += forcecoulz;

      if (newton_pair || j < nlocal)
      {
        f[j][0] -= forcecoulx;
        f[j][1] -= forcecouly;
        f[j][2] -= forcecoulz;
      }

      /* debug information */
      if (i==0)
      {
        forcetotalx += forcecoulx;
        forcetotaly += forcecouly;
        forcetotalz += forcecoulz;
      }
      if (j==0)
      {
        forcetotalx -= forcecoulx;
        forcetotaly -= forcecouly;
        forcetotalz -= forcecoulz;
      }
      /* ---------------- */
      if (evflag) ev_tally_xyz(i,j,nlocal,newton_pair,0.0,0.0,forcecoulx,forcecouly,forcecoulz,delx,dely,delz);
    }
  }
  u_polar = u_polar_self + u_polar_ef + u_polar_dd;
  if (debug)
  {
    printf("self: %.18f\nef: %.18f\ndd: %.18f\n",u_polar_self,u_polar_ef,u_polar_dd);
    printf("u_polar calc: %.18f\n",u_polar);
    printf("polar force on atom 0: %.18f,%.18f,%.18f\n",forcetotalx,forcetotaly,forcetotalz);
    printf("polar dipole force on atom 0: %.18f,%.18f,%.18f\n",forcedipolex,forcedipoley,forcedipolez);
    printf("pos of atom 0: %.5f,%.5f,%.5f\n",x[0][0],x[0][1],x[0][2]);
  }
  force->pair->eng_pol = u_polar;


  /* the fdotr virial is probably off, haven't looked into it deeply */
  if (vflag_fdotr) virial_fdotr_compute();

  /* debugging information, energy is given in kelvins in MPMC so there are conversions to compare between the two programs */
  if (debug)
  {
    FILE *file = NULL;
    int myrank;
    MPI_Comm_rank(MPI_COMM_WORLD, &myrank);
    char file_name[100];
    sprintf(file_name,"tensor%d.csv",myrank);
    file = fopen(file_name, "w");
    for (i=0;i<3*nlocal;i++)
    {
      for (j=0;j<3*nlocal;j++)
      {
        if (j!=0) fprintf(file,",");
        if (screen) fprintf(file,"%f",dipole_field_matrix[i][j]);
      }
    if (screen) fprintf(file,"\n");
    }
    fclose(file);
    double *charge = atom->q;
    sprintf(file_name,"pos%d.xyz",myrank);
    file = fopen(file_name, "w");
    fprintf(file,"%d\n",ntotal);
    fprintf(file,"\n");
    for (i=0;i<ntotal;i++)
    {
      fprintf(file,"H %f %f %f %f\n",x[i][0],x[i][1],x[i][2],charge[i]);
    }
    fclose(file);

    sprintf(file_name,"e_static%d.csv",myrank);
    file = fopen(file_name, "w");
    if (screen) fprintf(file,"-ef_static-\n\n");
    for (i=0;i<nlocal;i++)
    {
      for (j=0;j<3;j++)
      {
        if (j!=0) fprintf(file,",");
        if (screen) fprintf(file,"%f",ef_static[i][j]);
      }
    if (screen) fprintf(file,"\n");
    }
    if (screen) fprintf(file,"\n-force-\n\n");
    for (i=0;i<nlocal;i++)
    {
      for (j=0;j<3;j++)
      {
        if (j!=0) fprintf(file,",");
        if (screen) fprintf(file,"%f",f[i][j]);
      }
    if (screen) fprintf(file,"\n");
    }
    fclose(file);
    double u_polar = 0.0;
    for (i=0;i<nlocal;i++)
    {
      for (j=0;j<3;j++)
      {
        u_polar += ((ef_static[i][j])*22.432653052265)*(mu_induced[i][j]*22.432653052265); //convert to K*A for comparison
      }
    }
    u_polar *= -0.5;
    fprintf(screen,"u_polar (K) %d: %f\n",myrank,u_polar);

    sprintf(file_name,"mu%d.csv",myrank);
    file = fopen(file_name, "w");
    fprintf(file,"u_polar: %f\n\n",u_polar);
    for (i=0;i<nlocal;i++)
    {
      fprintf(file,"pos: %.20f,%.20f,%.20f ef_static: %.10f,%.10f,%.10f mu: ",x[i][0],x[i][1],x[i][2],ef_static[i][0]*22.432653052265,ef_static[i][1]*22.432653052265,ef_static[i][2]*22.432653052265); //convert to sqrt(K) for comparison
      for (j=0;j<3;j++)
      {
        if (j!=0) fprintf(file,",");
        if (screen) fprintf(file,"%.10f",mu_induced[i][j]*22.432653052265); //convert to sqrt(K*A) for comparison
      }
    if (screen) fprintf(file,"\n");
    }
    if (screen) fprintf(file,"\n\n\n");
    fclose(file);

    sprintf(file_name,"e_induced%d.csv",myrank);
    file = fopen(file_name, "w");
    for (i=0;i<nlocal;i++)
    {
      for (j=0;j<3;j++)
      {
        if (j!=0) fprintf(file,",");
        if (screen) fprintf(file,"%f",ef_induced[i][j]);
      }
    if (screen) fprintf(file,"\n");
    }
    if (screen) fprintf(file,"\n\n\n");
    fclose(file);
  }
}

/* ----------------------------------------------------------------------
   allocate all arrays
------------------------------------------------------------------------- */

void PairLJCutCoulLongPolarization::allocate()
{
  allocated = 1;
  int n = atom->ntypes;

  memory->create(setflag,n+1,n+1,"pair:setflag");
  for (int i = 1; i <= n; i++)
    for (int j = i; j <= n; j++)
      setflag[i][j] = 0;

  memory->create(cutsq,n+1,n+1,"pair:cutsq");

  memory->create(cut_lj,n+1,n+1,"pair:cut_lj");
  memory->create(cut_ljsq,n+1,n+1,"pair:cut_ljsq");
  memory->create(epsilon,n+1,n+1,"pair:epsilon");
  memory->create(sigma,n+1,n+1,"pair:sigma");
  memory->create(lj1,n+1,n+1,"pair:lj1");
  memory->create(lj2,n+1,n+1,"pair:lj2");
  memory->create(lj3,n+1,n+1,"pair:lj3");
  memory->create(lj4,n+1,n+1,"pair:lj4");
  memory->create(offset,n+1,n+1,"pair:offset");
}

/* ----------------------------------------------------------------------
   global settings
------------------------------------------------------------------------- */

void PairLJCutCoulLongPolarization::settings(int narg, char **arg)
{
  if (narg < 1) error->all(FLERR,"Illegal pair_style command");

  cut_lj_global = force->numeric(arg[0]);
  if (narg == 1) cut_coul = cut_lj_global;
  else cut_coul = force->numeric(arg[1]);

  int iarg;
  iarg = 2;
  while (iarg < narg)
  {
    if (iarg+2 > narg) error->all(FLERR,"Illegal pair_style command");
    if (strcmp("precision",arg[iarg])==0)
    {
      polar_precision = force->numeric(arg[iarg+1]);
    }
    else if (strcmp("zodid",arg[iarg])==0)
    {
      if (polar_gs||polar_gs_ranked) error->all(FLERR,"Zodid doesn't work with polar_gs or polar_gs_ranked");
      if (strcmp("yes",arg[iarg+1])==0) zodid = 1;
      else if (strcmp("no",arg[iarg+1])==0) zodid = 0;
      else error->all(FLERR,"Illegal pair_style command");
    }
    else if (strcmp("fixed_iteration",arg[iarg])==0)
    {
      if (strcmp("yes",arg[iarg+1])==0) fixed_iteration = 1;
      else if (strcmp("no",arg[iarg+1])==0) fixed_iteration = 0;
      else error->all(FLERR,"Illegal pair_style command");
    }
    else if (strcmp("damp",arg[iarg])==0)
    {
      polar_damp = force->numeric(arg[iarg+1]);
    }
    else if (strcmp("max_iterations",arg[iarg])==0)
    {
      iterations_max = force->inumeric(arg[iarg+1]);
    }
    else if (strcmp("damp_type",arg[iarg])==0)
    {
      if (strcmp("exponential",arg[iarg+1])==0) damping_type = DAMPING_EXPONENTIAL;
      else if (strcmp("none",arg[iarg+1])==0) damping_type = DAMPING_NONE;
      else error->all(FLERR,"Illegal pair_style command");
    }
    else if (strcmp("polar_gs",arg[iarg])==0)
    {
      if (polar_gs_ranked) error->all(FLERR,"polar_gs and polar_gs_ranked are mutually exclusive");
      if (strcmp("yes",arg[iarg+1])==0) polar_gs = 1;
      else if (strcmp("no",arg[iarg+1])==0) polar_gs = 0;
      else error->all(FLERR,"Illegal pair_style command");
    }
    else if (strcmp("polar_gs_ranked",arg[iarg])==0)
    {
      if (polar_gs) error->all(FLERR,"polar_gs and polar_gs_ranked are mutually exclusive");
      if (strcmp("yes",arg[iarg+1])==0) polar_gs_ranked = 1;
      else if (strcmp("no",arg[iarg+1])==0) polar_gs_ranked = 0;
      else error->all(FLERR,"Illegal pair_style command");
    }
    else if (strcmp("polar_gamma",arg[iarg])==0)
    {
      polar_gamma = force->numeric(arg[iarg+1]);
    }
    else if (strcmp("debug",arg[iarg])==0)
    {
      if (strcmp("yes",arg[iarg+1])==0) debug = 1;
      else if (strcmp("no",arg[iarg+1])==0) debug = 0;
      else error->all(FLERR,"Illegal pair_style command");
    }
    else if (strcmp("use_previous",arg[iarg])==0)
    {
      if (strcmp("yes",arg[iarg+1])==0) use_previous = 1;
      else if (strcmp("no",arg[iarg+1])==0) use_previous = 0;
      else error->all(FLERR,"Illegal pair_style command");
    }
    else error->all(FLERR,"Illegal pair_style command");
    iarg+=2;
  }

  // reset cutoffs that have been explicitly set
  if (allocated) {
    int i,j;
    for (i = 1; i <= atom->ntypes; i++)
      for (j = i+1; j <= atom->ntypes; j++)
  if (setflag[i][j]) cut_lj[i][j] = cut_lj_global;
  }
}

/* ----------------------------------------------------------------------
   set coeffs for one or more type pairs
------------------------------------------------------------------------- */

void PairLJCutCoulLongPolarization::coeff(int narg, char **arg)
{
  if (narg < 4 || narg > 5) error->all(FLERR,"Incorrect args for pair coefficients");
  if (!allocated) allocate();

  int ilo,ihi,jlo,jhi;
  force->bounds(arg[0],atom->ntypes,ilo,ihi);
  force->bounds(arg[1],atom->ntypes,jlo,jhi);

  double epsilon_one = force->numeric(arg[2]);
  double sigma_one = force->numeric(arg[3]);

  double cut_lj_one = cut_lj_global;
  if (narg == 5) cut_lj_one = force->numeric(arg[4]);

  int count = 0;
  for (int i = ilo; i <= ihi; i++) {
    for (int j = MAX(jlo,i); j <= jhi; j++) {
      epsilon[i][j] = epsilon_one;
      sigma[i][j] = sigma_one;
      cut_lj[i][j] = cut_lj_one;
      setflag[i][j] = 1;
      count++;
    }
  }

  if (count == 0) error->all(FLERR,"Incorrect args for pair coefficients");
}

/* ----------------------------------------------------------------------
   init specific to this pair style
------------------------------------------------------------------------- */

void PairLJCutCoulLongPolarization::init_style()
{
  if (!atom->q_flag)
    error->all(FLERR,"Pair style lj/cut/coul/long requires atom attribute q");

  // request regular neighbor lists

  int irequest;

  irequest = neighbor->request(this);

  cut_coulsq = cut_coul * cut_coul;

  cut_respa = NULL;

  // ensure use of KSpace long-range solver, set g_ewald

  if (force->kspace == NULL)
    error->all(FLERR,"Pair style is incompatible with KSpace style");
  else
    g_ewald = force->kspace->g_ewald;

  // setup force tables

  if (ncoultablebits) init_tables();
}

/* ----------------------------------------------------------------------
   neighbor callback to inform pair style of neighbor list to use
   regular or rRESPA
------------------------------------------------------------------------- */

void PairLJCutCoulLongPolarization::init_list(int id, NeighList *ptr)
{
  if (id == 0) list = ptr;
}

/* ----------------------------------------------------------------------
   init for one type pair i,j and corresponding j,i
------------------------------------------------------------------------- */

double PairLJCutCoulLongPolarization::init_one(int i, int j)
{
  if (setflag[i][j] == 0) {
    epsilon[i][j] = mix_energy(epsilon[i][i],epsilon[j][j],
             sigma[i][i],sigma[j][j]);
    sigma[i][j] = mix_distance(sigma[i][i],sigma[j][j]);
    cut_lj[i][j] = mix_distance(cut_lj[i][i],cut_lj[j][j]);
  }

  double cut = MAX(cut_lj[i][j],cut_coul);
  cut_ljsq[i][j] = cut_lj[i][j] * cut_lj[i][j];

  lj1[i][j] = 48.0 * epsilon[i][j] * pow(sigma[i][j],12.0);
  lj2[i][j] = 24.0 * epsilon[i][j] * pow(sigma[i][j],6.0);
  lj3[i][j] = 4.0 * epsilon[i][j] * pow(sigma[i][j],12.0);
  lj4[i][j] = 4.0 * epsilon[i][j] * pow(sigma[i][j],6.0);

  if (offset_flag) {
    double ratio = sigma[i][j] / cut_lj[i][j];
    offset[i][j] = 4.0 * epsilon[i][j] * (pow(ratio,12.0) - pow(ratio,6.0));
  } else offset[i][j] = 0.0;

  cut_ljsq[j][i] = cut_ljsq[i][j];
  lj1[j][i] = lj1[i][j];
  lj2[j][i] = lj2[i][j];
  lj3[j][i] = lj3[i][j];
  lj4[j][i] = lj4[i][j];
  offset[j][i] = offset[i][j];

  // check interior rRESPA cutoff

  if (cut_respa && MIN(cut_lj[i][j],cut_coul) < cut_respa[3])
    error->all(FLERR,"Pair cutoff < Respa interior cutoff");

  // compute I,J contribution to long-range tail correction
  // count total # of atoms of type I and J via Allreduce

  if (tail_flag) {
    int *type = atom->type;
    int nlocal = atom->nlocal;

    double count[2],all[2];
    count[0] = count[1] = 0.0;
    for (int k = 0; k < nlocal; k++) {
      if (type[k] == i) count[0] += 1.0;
      if (type[k] == j) count[1] += 1.0;
    }
    MPI_Allreduce(count,all,2,MPI_DOUBLE,MPI_SUM,world);
        
    double sig2 = sigma[i][j]*sigma[i][j];
    double sig6 = sig2*sig2*sig2;
    double rc3 = cut_lj[i][j]*cut_lj[i][j]*cut_lj[i][j];
    double rc6 = rc3*rc3;
    double rc9 = rc3*rc6;
    etail_ij = 8.0*MY_PI*all[0]*all[1]*epsilon[i][j] * 
      sig6 * (sig6 - 3.0*rc6) / (9.0*rc9); 
    ptail_ij = 16.0*MY_PI*all[0]*all[1]*epsilon[i][j] * 
      sig6 * (2.0*sig6 - 3.0*rc6) / (9.0*rc9); 
  } 

  return cut;
}

/* ----------------------------------------------------------------------
   setup force tables used in compute routines
------------------------------------------------------------------------- */

void PairLJCutCoulLongPolarization::init_tables()
{
  int masklo,maskhi;
  double r,grij,expm2,derfc,rsw;
  double qqrd2e = force->qqrd2e;

  tabinnersq = tabinner*tabinner;
  init_bitmap(tabinner,cut_coul,ncoultablebits,
        masklo,maskhi,ncoulmask,ncoulshiftbits);
  
  int ntable = 1;
  for (int i = 0; i < ncoultablebits; i++) ntable *= 2;
  
  // linear lookup tables of length N = 2^ncoultablebits
  // stored value = value at lower edge of bin
  // d values = delta from lower edge to upper edge of bin

  if (ftable) free_tables();
  
  memory->create(rtable,ntable,"pair:rtable");
  memory->create(ftable,ntable,"pair:ftable");
  memory->create(ctable,ntable,"pair:ctable");
  memory->create(etable,ntable,"pair:etable");
  memory->create(drtable,ntable,"pair:drtable");
  memory->create(dftable,ntable,"pair:dftable");
  memory->create(dctable,ntable,"pair:dctable");
  memory->create(detable,ntable,"pair:detable");

  if (cut_respa == NULL) {
    vtable = ptable = dvtable = dptable = NULL;
  } else {
    memory->create(vtable,ntable*sizeof(double),"pair:vtable");
    memory->create(ptable,ntable*sizeof(double),"pair:ptable");
    memory->create(dvtable,ntable*sizeof(double),"pair:dvtable");
    memory->create(dptable,ntable*sizeof(double),"pair:dptable");
  }

  union_int_float_t rsq_lookup;
  union_int_float_t minrsq_lookup;
  int itablemin;
  minrsq_lookup.i = 0 << ncoulshiftbits;
  minrsq_lookup.i |= maskhi;
    
  for (int i = 0; i < ntable; i++) {
    rsq_lookup.i = i << ncoulshiftbits;
    rsq_lookup.i |= masklo;
    if (rsq_lookup.f < tabinnersq) {
      rsq_lookup.i = i << ncoulshiftbits;
      rsq_lookup.i |= maskhi;
    }
    r = sqrtf(rsq_lookup.f);
    grij = g_ewald * r;
    expm2 = exp(-grij*grij);
    derfc = erfc(grij);
    if (cut_respa == NULL) {
      rtable[i] = rsq_lookup.f;
      ftable[i] = qqrd2e/r * (derfc + EWALD_F*grij*expm2);
      ctable[i] = qqrd2e/r;
      etable[i] = qqrd2e/r * derfc;
    } else {
      rtable[i] = rsq_lookup.f;
      ftable[i] = qqrd2e/r * (derfc + EWALD_F*grij*expm2 - 1.0);
      ctable[i] = 0.0;
      etable[i] = qqrd2e/r * derfc;
      ptable[i] = qqrd2e/r;
      vtable[i] = qqrd2e/r * (derfc + EWALD_F*grij*expm2);
      if (rsq_lookup.f > cut_respa[2]*cut_respa[2]) {
  if (rsq_lookup.f < cut_respa[3]*cut_respa[3]) {
    rsw = (r - cut_respa[2])/(cut_respa[3] - cut_respa[2]); 
    ftable[i] += qqrd2e/r * rsw*rsw*(3.0 - 2.0*rsw);
    ctable[i] = qqrd2e/r * rsw*rsw*(3.0 - 2.0*rsw);
  } else {
    ftable[i] = qqrd2e/r * (derfc + EWALD_F*grij*expm2);
    ctable[i] = qqrd2e/r;
  }
      }
    }
    minrsq_lookup.f = MIN(minrsq_lookup.f,rsq_lookup.f);
  }

  tabinnersq = minrsq_lookup.f;
  
  int ntablem1 = ntable - 1;
  
  for (int i = 0; i < ntablem1; i++) {
    drtable[i] = 1.0/(rtable[i+1] - rtable[i]);
    dftable[i] = ftable[i+1] - ftable[i];
    dctable[i] = ctable[i+1] - ctable[i];
    detable[i] = etable[i+1] - etable[i];
  }

  if (cut_respa) {
    for (int i = 0; i < ntablem1; i++) {
      dvtable[i] = vtable[i+1] - vtable[i];
      dptable[i] = ptable[i+1] - ptable[i];
    }
  }
  
  // get the delta values for the last table entries 
  // tables are connected periodically between 0 and ntablem1
    
  drtable[ntablem1] = 1.0/(rtable[0] - rtable[ntablem1]);
  dftable[ntablem1] = ftable[0] - ftable[ntablem1];
  dctable[ntablem1] = ctable[0] - ctable[ntablem1];
  detable[ntablem1] = etable[0] - etable[ntablem1];
  if (cut_respa) {
    dvtable[ntablem1] = vtable[0] - vtable[ntablem1];
    dptable[ntablem1] = ptable[0] - ptable[ntablem1];
  }

  // get the correct delta values at itablemax    
  // smallest r is in bin itablemin
  // largest r is in bin itablemax, which is itablemin-1,
  //   or ntablem1 if itablemin=0
  // deltas at itablemax only needed if corresponding rsq < cut*cut
  // if so, compute deltas between rsq and cut*cut 

  double f_tmp,c_tmp,e_tmp,p_tmp,v_tmp;
  itablemin = minrsq_lookup.i & ncoulmask;
  itablemin >>= ncoulshiftbits;  
  int itablemax = itablemin - 1; 
  if (itablemin == 0) itablemax = ntablem1;     
  rsq_lookup.i = itablemax << ncoulshiftbits;
  rsq_lookup.i |= maskhi;

  if (rsq_lookup.f < cut_coulsq) {
    rsq_lookup.f = cut_coulsq;  
    r = sqrtf(rsq_lookup.f);
    grij = g_ewald * r;
    expm2 = exp(-grij*grij);
    derfc = erfc(grij);

    if (cut_respa == NULL) {
      f_tmp = qqrd2e/r * (derfc + EWALD_F*grij*expm2);
      c_tmp = qqrd2e/r;
      e_tmp = qqrd2e/r * derfc;
    } else {
      f_tmp = qqrd2e/r * (derfc + EWALD_F*grij*expm2 - 1.0);
      c_tmp = 0.0;
      e_tmp = qqrd2e/r * derfc;
      p_tmp = qqrd2e/r;
      v_tmp = qqrd2e/r * (derfc + EWALD_F*grij*expm2);
      if (rsq_lookup.f > cut_respa[2]*cut_respa[2]) {
        if (rsq_lookup.f < cut_respa[3]*cut_respa[3]) {
          rsw = (r - cut_respa[2])/(cut_respa[3] - cut_respa[2]); 
          f_tmp += qqrd2e/r * rsw*rsw*(3.0 - 2.0*rsw);
          c_tmp = qqrd2e/r * rsw*rsw*(3.0 - 2.0*rsw);
        } else {
          f_tmp = qqrd2e/r * (derfc + EWALD_F*grij*expm2);
          c_tmp = qqrd2e/r;
        }
      }
    }

    drtable[itablemax] = 1.0/(rsq_lookup.f - rtable[itablemax]);   
    dftable[itablemax] = f_tmp - ftable[itablemax];
    dctable[itablemax] = c_tmp - ctable[itablemax];
    detable[itablemax] = e_tmp - etable[itablemax];
    if (cut_respa) {
      dvtable[itablemax] = v_tmp - vtable[itablemax];
      dptable[itablemax] = p_tmp - ptable[itablemax];
    }   
  }
}

/* ----------------------------------------------------------------------
  proc 0 writes to restart file
------------------------------------------------------------------------- */

void PairLJCutCoulLongPolarization::write_restart(FILE *fp)
{
  write_restart_settings(fp);

  int i,j;
  for (i = 1; i <= atom->ntypes; i++)
  {
    for (j = i; j <= atom->ntypes; j++) {
      fwrite(&setflag[i][j],sizeof(int),1,fp);
      if (setflag[i][j]) {
        fwrite(&epsilon[i][j],sizeof(double),1,fp);
        fwrite(&sigma[i][j],sizeof(double),1,fp);
        fwrite(&cut_lj[i][j],sizeof(double),1,fp);
      }
    }
  }
}

/* ----------------------------------------------------------------------
  proc 0 reads from restart file, bcasts
------------------------------------------------------------------------- */

void PairLJCutCoulLongPolarization::read_restart(FILE *fp)
{
  read_restart_settings(fp);

  allocate();

  int i,j;
  int me = comm->me;
  for (i = 1; i <= atom->ntypes; i++)
  {
    for (j = i; j <= atom->ntypes; j++)
    {
      if (me == 0) fread(&setflag[i][j],sizeof(int),1,fp);
      MPI_Bcast(&setflag[i][j],1,MPI_INT,0,world);
      if (setflag[i][j]) {
        if (me == 0) {
          fread(&epsilon[i][j],sizeof(double),1,fp);
          fread(&sigma[i][j],sizeof(double),1,fp);
          fread(&cut_lj[i][j],sizeof(double),1,fp);
        }
        MPI_Bcast(&epsilon[i][j],1,MPI_DOUBLE,0,world);
        MPI_Bcast(&sigma[i][j],1,MPI_DOUBLE,0,world);
        MPI_Bcast(&cut_lj[i][j],1,MPI_DOUBLE,0,world);
      }
    }
  }
}

/* ----------------------------------------------------------------------
  proc 0 writes to restart file
------------------------------------------------------------------------- */

void PairLJCutCoulLongPolarization::write_restart_settings(FILE *fp)
{
  fwrite(&cut_lj_global,sizeof(double),1,fp);
  fwrite(&cut_coul,sizeof(double),1,fp);
  fwrite(&offset_flag,sizeof(int),1,fp);
  fwrite(&mix_flag,sizeof(int),1,fp);

  fwrite(&iterations_max,sizeof(int),1,fp);
  fwrite(&damping_type,sizeof(int),1,fp);
  fwrite(&polar_damp,sizeof(double),1,fp);
  fwrite(&zodid,sizeof(int),1,fp);
  fwrite(&polar_precision,sizeof(double),1,fp);
  fwrite(&fixed_iteration,sizeof(int),1,fp);
  fwrite(&polar_gs,sizeof(int),1,fp);
  fwrite(&polar_gs_ranked,sizeof(int),1,fp);
  fwrite(&polar_gamma,sizeof(double),1,fp);
  fwrite(&debug,sizeof(int),1,fp);
}

/* ----------------------------------------------------------------------
  proc 0 reads from restart file, bcasts
------------------------------------------------------------------------- */

void PairLJCutCoulLongPolarization::read_restart_settings(FILE *fp)
{
  if (comm->me == 0) {
    fread(&cut_lj_global,sizeof(double),1,fp);
    fread(&cut_coul,sizeof(double),1,fp);
    fread(&offset_flag,sizeof(int),1,fp);
    fread(&mix_flag,sizeof(int),1,fp);

    fread(&iterations_max,sizeof(int),1,fp);
    fread(&damping_type,sizeof(int),1,fp);
    fread(&polar_damp,sizeof(double),1,fp);
    fread(&zodid,sizeof(int),1,fp);
    fread(&polar_precision,sizeof(double),1,fp);
    fread(&fixed_iteration,sizeof(int),1,fp);
    fread(&polar_gs,sizeof(int),1,fp);
    fread(&polar_gs_ranked,sizeof(int),1,fp);
    fread(&polar_gamma,sizeof(double),1,fp);
    fread(&debug,sizeof(int),1,fp);
  }
  MPI_Bcast(&cut_lj_global,1,MPI_DOUBLE,0,world);
  MPI_Bcast(&cut_coul,1,MPI_DOUBLE,0,world);
  MPI_Bcast(&offset_flag,1,MPI_INT,0,world);
  MPI_Bcast(&mix_flag,1,MPI_INT,0,world);

  MPI_Bcast(&iterations_max,1,MPI_INT,0,world);
  MPI_Bcast(&damping_type,1,MPI_INT,0,world);
  MPI_Bcast(&polar_damp,1,MPI_DOUBLE,0,world);
  MPI_Bcast(&zodid,1,MPI_INT,0,world);
  MPI_Bcast(&polar_precision,1,MPI_DOUBLE,0,world);
  MPI_Bcast(&fixed_iteration,1,MPI_INT,0,world);
  MPI_Bcast(&polar_gs,1,MPI_INT,0,world);
  MPI_Bcast(&polar_gs_ranked,1,MPI_INT,0,world);
  MPI_Bcast(&polar_gamma,1,MPI_DOUBLE,0,world);
  MPI_Bcast(&debug,1,MPI_INT,0,world);
}

/* ----------------------------------------------------------------------
   free memory for tables used in pair computations
------------------------------------------------------------------------- */

void PairLJCutCoulLongPolarization::free_tables()
{
  memory->destroy(rtable);
  memory->destroy(drtable);
  memory->destroy(ftable);
  memory->destroy(dftable);
  memory->destroy(ctable);
  memory->destroy(dctable);
  memory->destroy(etable);
  memory->destroy(detable);
  memory->destroy(vtable);
  memory->destroy(dvtable);
  memory->destroy(ptable);
  memory->destroy(dptable);
}

/* ---------------------------------------------------------------------- */

double PairLJCutCoulLongPolarization::single(int i, int j, int itype, int jtype,
         double rsq,
         double factor_coul, double factor_lj,
         double &fforce)
{
  double r2inv,r6inv,r,grij,expm2,t,erfc,prefactor;
  double fraction,table,forcecoul,forcelj,phicoul,philj;
  int itable;

  r2inv = 1.0/rsq;
  if (rsq < cut_coulsq) {
    if (!ncoultablebits || rsq <= tabinnersq) {
      r = sqrt(rsq);
      grij = g_ewald * r;
      expm2 = exp(-grij*grij);
      t = 1.0 / (1.0 + EWALD_P*grij);
      erfc = t * (A1+t*(A2+t*(A3+t*(A4+t*A5)))) * expm2;
      prefactor = force->qqrd2e * atom->q[i]*atom->q[j]/r;
      forcecoul = prefactor * (erfc + EWALD_F*grij*expm2);
      if (factor_coul < 1.0) forcecoul -= (1.0-factor_coul)*prefactor;
    } else {
      union_int_float_t rsq_lookup_single;
      rsq_lookup_single.f = rsq;
      itable = rsq_lookup_single.i & ncoulmask;
      itable >>= ncoulshiftbits;
      fraction = (rsq_lookup_single.f - rtable[itable]) * drtable[itable];
      table = ftable[itable] + fraction*dftable[itable];
      forcecoul = atom->q[i]*atom->q[j] * table;
      if (factor_coul < 1.0) {
        table = ctable[itable] + fraction*dctable[itable];
        prefactor = atom->q[i]*atom->q[j] * table;
        forcecoul -= (1.0-factor_coul)*prefactor;
      }
    }
  } else forcecoul = 0.0;

  if (rsq < cut_ljsq[itype][jtype]) {
    r6inv = r2inv*r2inv*r2inv;
    forcelj = r6inv * (lj1[itype][jtype]*r6inv - lj2[itype][jtype]);
  } else forcelj = 0.0;

  fforce = (forcecoul + factor_lj*forcelj) * r2inv;

  double eng = 0.0;
  if (rsq < cut_coulsq) {
    if (!ncoultablebits || rsq <= tabinnersq)
      phicoul = prefactor*erfc;
    else {
      table = etable[itable] + fraction*detable[itable];
      phicoul = atom->q[i]*atom->q[j] * table;
    }
    if (factor_coul < 1.0) phicoul -= (1.0-factor_coul)*prefactor;
    eng += phicoul;
  }

  if (rsq < cut_ljsq[itype][jtype]) {
    philj = r6inv*(lj3[itype][jtype]*r6inv-lj4[itype][jtype]) -
      offset[itype][jtype];
    eng += factor_lj*philj;
  }

  return eng;
}

/* ---------------------------------------------------------------------- */

void *PairLJCutCoulLongPolarization::extract(const char *str, int &dim)
{
  dim = 0;
  if (strcmp(str,"cut_coul") == 0) return (void *) &cut_coul;
  return NULL;
}

/* ---------------------------------------------------------------------- */

int PairLJCutCoulLongPolarization::DipoleSolverIterative()
{
  double **ef_static = atom->ef_static;
  double *static_polarizability = atom->static_polarizability;
  double **mu_induced = atom->mu_induced;
  int nlocal = atom->nlocal;
  int i,ii,j,jj,p,q,iterations,keep_iterating,index;
  double change;

  /* build dipole interaction tensor */
  build_dipole_field_matrix();

  keep_iterating = 1;
  iterations = 0;
  for(i = 0; i < nlocal; i++) ranked_array[i] = i;

  /* rank the dipoles by bubble sort */
  if(polar_gs_ranked) {
    int tmp,sorted;
    for(i = 0; i < nlocal; i++) {
      for(j = 0, sorted = 1; j < (nlocal-1); j++) {
        if(rank_metric[ranked_array[j]] < rank_metric[ranked_array[j+1]]) {
          sorted = 0;
          tmp = ranked_array[j];
          ranked_array[j] = ranked_array[j+1];
          ranked_array[j+1] = tmp;
        }
      }
      if(sorted) break;
    }
  }

  while (keep_iterating)
  {
    /* save old dipoles and clear the induced field */
    for(i = 0; i < nlocal; i++)
    {
      for(p = 0; p < 3; p++)
      {
        mu_induced_old[i][p] = mu_induced[i][p];
        ef_induced[i][p] = 0;
      }
    }

    /* contract the dipoles with the field tensor */
    for(i = 0; i < nlocal; i++) {
      index = ranked_array[i];
      ii = index*3;
      for(j = 0; j < nlocal; j++) {
        jj = j*3;
        if(index != j) {
          for(p = 0; p < 3; p++)
            for(q = 0; q < 3; q++)
              ef_induced[index][p] -= dipole_field_matrix[ii+p][jj+q]*mu_induced[j][q];
        }
      } /* end j */


      /* dipole is the sum of the static and induced parts */
      for(p = 0; p < 3; p++) {
        mu_induced_new[index][p] = static_polarizability[index]*(ef_static[index][p] + ef_induced[index][p]);

        /* Gauss-Seidel */
        if(polar_gs || polar_gs_ranked)
          mu_induced[index][p] = mu_induced_new[index][p];
      }

    } /* end i */

    double u_polar = 0.0;
    if (debug)
    {
      for (i=0;i<nlocal;i++)
      {
        u_polar += ef_static[i][0]*mu_induced[i][0] + ef_static[i][1]*mu_induced[i][1] + ef_static[i][2]*mu_induced[i][2];
      }
      u_polar *= -0.5;
      printf("u_polar (K) %d: %.18f\n",iterations,u_polar*22.432653052265*22.432653052265);
    }

    /* determine if we are done by precision */
    if (fixed_iteration==0)
    {
      keep_iterating = 0;
      change = 0;
      for(i = 0; i < nlocal; i++)
      {
        for(p = 0; p < 3; p++)
        {
          change += (mu_induced_new[i][p] - mu_induced_old[i][p])*(mu_induced_new[i][p] - mu_induced_old[i][p]);
        }
      }
      change /= (double)(nlocal)*3.0;
      if (change > polar_precision*polar_precision)
      {
        keep_iterating = 1;
      }
    }
    else
    {
      /* or by fixed iteration */
      if(iterations >= iterations_max) return iterations;
    }

    /* save the dipoles for the next pass */
    for(i = 0; i < nlocal; i++) {
      for(p = 0; p < 3; p++) {
          mu_induced[i][p] = mu_induced_new[i][p];
      }
    }

    iterations++;
    /* divergence detection */
    /* if we fail to converge, then set dipoles to alpha*E */
    if(iterations > iterations_max) {

      for(i = 0; i < nlocal; i++)
        for(p = 0; p < 3; p++)
          mu_induced[i][p] = static_polarizability[i]*ef_static[i][p];

      error->warning(FLERR,"Number of iterations exceeding max_iterations, setting dipoles to alpha*E");
      return iterations;
    }
  }
  return iterations;
}

/* ---------------------------------------------------------------------- */

void PairLJCutCoulLongPolarization::build_dipole_field_matrix()
{
  int N = atom->nlocal;
  double **x = atom->x;
  double *static_polarizability = atom->static_polarizability;
  int i,j,ii,jj,p,q;
  double r,r2,r3,r5,s,v,damping_term1=1.0,damping_term2=1.0;
  double xjimage[3] = {0.0,0.0,0.0};

  /* zero out the matrix */
  for (i=0;i<3*N;i++)
  {
    for (j=0;j<3*N;j++)
    {
      dipole_field_matrix[i][j] = 0;
    }
  }

  /* set the diagonal blocks */
  for(i = 0; i < N; i++) {
    ii = i*3;
    for(p = 0; p < 3; p++) {
      if(static_polarizability[i] != 0.0)
        dipole_field_matrix[ii+p][ii+p] = 1.0/static_polarizability[i];
      else
        dipole_field_matrix[ii+p][ii+p] = DBL_MAX;
    }
  }

    /* calculate each Tij tensor component for each dipole pair */
  for(i = 0; i < (N - 1); i++) {
    ii = i*3;
    for(j = (i + 1); j < N; j++) {
      jj = j*3;

      /* inverse displacements */
      double xi[3] = {x[i][0],x[i][1],x[i][2]};
      double xj[3] = {x[j][0],x[j][1],x[j][2]};
      domain->closest_image(xi,xj,xjimage);
      r2 = pow(xi[0]-xjimage[0],2)+pow(xi[1]-xjimage[1],2)+pow(xi[2]-xjimage[2],2);

      r = sqrt(r2);
      if(r == 0.0)
        r3 = r5 = DBL_MAX;
      else {
        r3 = 1.0/(r*r*r);
        r5 = 1.0/(r*r*r*r*r);
      }

      /* set the damping function */
      if(damping_type == DAMPING_EXPONENTIAL) {
        damping_term1 = 1.0 - exp(-polar_damp*r)*(0.5*polar_damp*polar_damp*r2 + polar_damp*r + 1.0);
        damping_term2 = 1.0 - exp(-polar_damp*r)*(polar_damp*polar_damp*polar_damp*r2*r/6.0 + 0.5*polar_damp*polar_damp*r2 + polar_damp*r + 1.0);
      }

      /* build the tensor */
      for(p = 0; p < 3; p++) {
        for(q = 0; q < 3; q++) {
          dipole_field_matrix[ii+p][jj+q] = -3.0*(x[i][p]-xjimage[p])*(x[i][q]-xjimage[q])*damping_term2*r5;
          /* additional diagonal term */
          if(p == q)
            dipole_field_matrix[ii+p][jj+q] += damping_term1*r3;
        }
      }

      /* set the lower half of the tensor component */
      for(p = 0; p < 3; p++)
        for(q = 0; q < 3; q++)
          dipole_field_matrix[jj+p][ii+q] = dipole_field_matrix[ii+p][jj+q];
    }
  }

  return;
}

/* ---------------------------------------------------------------------- */

int PairLJCutCoulLongPolarization::pack_comm(int n, int *list, double *buf,
           int pbc_flag, int *pbc)
{
  int i,j,m;
  double *static_polarizability = atom->static_polarizability;
  double **ef_static = atom->ef_static;
  double **mu_induced = atom->mu_induced;

  m = 0;
  for (i = 0; i < n; i++) {
    j = list[i];
    buf[m++] = static_polarizability[j];
    buf[m++] = ef_static[j][0];
    buf[m++] = ef_static[j][1];
    buf[m++] = ef_static[j][2];
    buf[m++] = mu_induced[j][0];
    buf[m++] = mu_induced[j][1];
    buf[m++] = mu_induced[j][2];
  }
  return 7;
}

/* ---------------------------------------------------------------------- */

void PairLJCutCoulLongPolarization::unpack_comm(int n, int first, double *buf)
{
  int i,m,last;
  double *static_polarizability = atom->static_polarizability;
  double **ef_static = atom->ef_static;
  double **mu_induced = atom->mu_induced;

  m = 0;
  last = first + n;
  for (i = first; i < last; i++) {
    static_polarizability[i] = buf[m++];
    ef_static[i][0] = buf[m++];
    ef_static[i][1] = buf[m++];
    ef_static[i][2] = buf[m++];
    mu_induced[i][0] = buf[m++];
    mu_induced[i][1] = buf[m++];
    mu_induced[i][2] = buf[m++];
  }
}
