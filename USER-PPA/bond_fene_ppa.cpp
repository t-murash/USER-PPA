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

/*
  2019/11/07
  by T. Murashima @ Tohoku Univ.
 */

#include "bond_fene_ppa.h"
#include <mpi.h>
#include <cmath>
#include "atom.h"
#include "neighbor.h"
#include "comm.h"
#include "update.h"
#include "force.h"
#include "memory.h"
#include "error.h"

using namespace LAMMPS_NS;

/* ---------------------------------------------------------------------- */

BondFENEPPA::BondFENEPPA(LAMMPS *lmp) : Bond(lmp)
{
  TWO_1_3 = pow(2.0,(1.0/3.0));
}

/* ---------------------------------------------------------------------- */

BondFENEPPA::~BondFENEPPA()
{
  if (allocated && !copymode) {
    memory->destroy(setflag);
    memory->destroy(k);
    memory->destroy(r0);
    memory->destroy(epsilon);
    memory->destroy(sigma);
  }
}

/* ---------------------------------------------------------------------- */

void BondFENEPPA::compute(int eflag, int vflag)
{
  int i1,i2,n,type;
  double delx,dely,delz,ebond,fbond;
  double rsq,r0sq,rlogarg,sr2,sr6;

  ebond = sr6 = 0.0;
  ev_init(eflag,vflag);

  double **x = atom->x;
  double **f = atom->f;
  int **bondlist = neighbor->bondlist;
  int nbondlist = neighbor->nbondlist;
  int nlocal = atom->nlocal;
  int newton_bond = force->newton_bond;

  for (n = 0; n < nbondlist; n++) {
    i1 = bondlist[n][0];
    i2 = bondlist[n][1];
    type = bondlist[n][2];

    delx = x[i1][0] - x[i2][0];
    dely = x[i1][1] - x[i2][1];
    delz = x[i1][2] - x[i2][2];

    // force from log term

    rsq = delx*delx + dely*dely + delz*delz;
    r0sq = r0[type] * r0[type];
    rlogarg = 1.0 - rsq/r0sq;

    // if r -> r0, then rlogarg < 0.0 which is an error
    // issue a warning and reset rlogarg = epsilon
    // if r > 2*r0 something serious is wrong, abort

    if (rlogarg < 0.1) {
      char str[128];
      sprintf(str,"FENE bond too long: " BIGINT_FORMAT " "
              TAGINT_FORMAT " " TAGINT_FORMAT " %g",
              update->ntimestep,atom->tag[i1],atom->tag[i2],sqrt(rsq));
      error->warning(FLERR,str,0);
      if (rlogarg <= -3.0) error->one(FLERR,"Bad FENE bond");
      rlogarg = 0.1;
    }

    fbond = -k[type]/rlogarg;

    // force from LJ term
    // 2019/10/29 by TM
    /* 
    if (rsq < TWO_1_3*sigma[type]*sigma[type]) {
      sr2 = sigma[type]*sigma[type]/rsq;
      sr6 = sr2*sr2*sr2;
      fbond += 48.0*epsilon[type]*sr6*(sr6-0.5)/rsq;
    }
    */

    // energy

    if (eflag) {
      ebond = -0.5 * k[type]*r0sq*log(rlogarg);
      // 2019/10/29 by TM
      /*
      if (rsq < TWO_1_3*sigma[type]*sigma[type])
        ebond += 4.0*epsilon[type]*sr6*(sr6-1.0) + epsilon[type];
      */
    }

    // apply force to each of 2 atoms

    if (newton_bond || i1 < nlocal) {
      f[i1][0] += delx*fbond;
      f[i1][1] += dely*fbond;
      f[i1][2] += delz*fbond;
    }

    if (newton_bond || i2 < nlocal) {
      f[i2][0] -= delx*fbond;
      f[i2][1] -= dely*fbond;
      f[i2][2] -= delz*fbond;
    }

    if (evflag) ev_tally(i1,i2,nlocal,newton_bond,ebond,fbond,delx,dely,delz);
  }
}

/* ---------------------------------------------------------------------- */

void BondFENEPPA::allocate()
{
  allocated = 1;
  int n = atom->nbondtypes;

  memory->create(k,n+1,"bond:k");
  memory->create(r0,n+1,"bond:r0");
  memory->create(epsilon,n+1,"bond:epsilon");
  memory->create(sigma,n+1,"bond:sigma");
  memory->create(setflag,n+1,"bond:setflag");
  for (int i = 1; i <= n; i++) setflag[i] = 0;
}

/* ----------------------------------------------------------------------
   set coeffs for one type
------------------------------------------------------------------------- */

void BondFENEPPA::coeff(int narg, char **arg)
{
  if (narg != 5) error->all(FLERR,"Incorrect args for bond coefficients");
  if (!allocated) allocate();

  int ilo,ihi;
  utils::bounds(FLERR,arg[0],1,atom->nbondtypes,ilo,ihi,error);

  double k_one = utils::numeric(FLERR,arg[1],false,lmp);
  double r0_one = utils::numeric(FLERR,arg[2],false,lmp);
  double epsilon_one = utils::numeric(FLERR,arg[3],false,lmp);
  double sigma_one = utils::numeric(FLERR,arg[4],false,lmp);

  int count = 0;
  for (int i = ilo; i <= ihi; i++) {
    k[i] = k_one;
    r0[i] = r0_one;
    epsilon[i] = epsilon_one;
    sigma[i] = sigma_one;
    setflag[i] = 1;
    count++;
  }

  if (count == 0) error->all(FLERR,"Incorrect args for bond coefficients");
}

/* ----------------------------------------------------------------------
   check if special_bond settings are valid
------------------------------------------------------------------------- */

void BondFENEPPA::init_style()
{
  // special bonds should be 0 1 1

  if (force->special_lj[1] != 0.0 || force->special_lj[2] != 1.0 ||
      force->special_lj[3] != 1.0) {
    if (comm->me == 0)
      error->warning(FLERR,"Use special bonds = 0,1,1 with bond style fene");
  }
}

/* ---------------------------------------------------------------------- */

double BondFENEPPA::equilibrium_distance(int i)
{
  return 0.97*sigma[i];
}

/* ----------------------------------------------------------------------
   proc 0 writes to restart file
------------------------------------------------------------------------- */

void BondFENEPPA::write_restart(FILE *fp)
{
  fwrite(&k[1],sizeof(double),atom->nbondtypes,fp);
  fwrite(&r0[1],sizeof(double),atom->nbondtypes,fp);
  fwrite(&epsilon[1],sizeof(double),atom->nbondtypes,fp);
  fwrite(&sigma[1],sizeof(double),atom->nbondtypes,fp);
}

/* ----------------------------------------------------------------------
   proc 0 reads from restart file, bcasts
------------------------------------------------------------------------- */

void BondFENEPPA::read_restart(FILE *fp)
{
  allocate();

  if (comm->me == 0) {
    fread(&k[1],sizeof(double),atom->nbondtypes,fp);
    fread(&r0[1],sizeof(double),atom->nbondtypes,fp);
    fread(&epsilon[1],sizeof(double),atom->nbondtypes,fp);
    fread(&sigma[1],sizeof(double),atom->nbondtypes,fp);
  }
  MPI_Bcast(&k[1],atom->nbondtypes,MPI_DOUBLE,0,world);
  MPI_Bcast(&r0[1],atom->nbondtypes,MPI_DOUBLE,0,world);
  MPI_Bcast(&epsilon[1],atom->nbondtypes,MPI_DOUBLE,0,world);
  MPI_Bcast(&sigma[1],atom->nbondtypes,MPI_DOUBLE,0,world);

  for (int i = 1; i <= atom->nbondtypes; i++) setflag[i] = 1;
}

/* ----------------------------------------------------------------------
   proc 0 writes to data file
------------------------------------------------------------------------- */

void BondFENEPPA::write_data(FILE *fp)
{
  for (int i = 1; i <= atom->nbondtypes; i++)
    fprintf(fp,"%d %g %g %g %g\n",i,k[i],r0[i],epsilon[i],sigma[i]);
}

/* ---------------------------------------------------------------------- */

double BondFENEPPA::single(int type, double rsq, int /*i*/, int /*j*/,
                        double &fforce)
{
  double r0sq = r0[type] * r0[type];
  double rlogarg = 1.0 - rsq/r0sq;

  // if r -> r0, then rlogarg < 0.0 which is an error
  // issue a warning and reset rlogarg = epsilon
  // if r > 2*r0 something serious is wrong, abort

  if (rlogarg < 0.1) {
    char str[128];
    sprintf(str,"FENE bond too long: " BIGINT_FORMAT " %g",
            update->ntimestep,sqrt(rsq));
    error->warning(FLERR,str,0);
    if (rlogarg <= -3.0) error->one(FLERR,"Bad FENE bond");
    rlogarg = 0.1;
  }

  double eng = -0.5 * k[type]*r0sq*log(rlogarg);
  fforce = -k[type]/rlogarg;

  // 2019/10/29 by TM
  /*
  if (rsq < TWO_1_3*sigma[type]*sigma[type]) {
    double sr2,sr6;
    sr2 = sigma[type]*sigma[type]/rsq;
    sr6 = sr2*sr2*sr2;
    eng += 4.0*epsilon[type]*sr6*(sr6-1.0) + epsilon[type];
    fforce += 48.0*epsilon[type]*sr6*(sr6-0.5)/rsq;
  }
  */

  return eng;
}
