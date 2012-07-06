/* ----------------------------------------------------------------------
   LIGGGHTS - LAMMPS Improved for General Granular and Granular Heat
   Transfer Simulations

   LIGGGHTS is part of the CFDEMproject
   www.liggghts.com | www.cfdem.com

   Christoph Kloss, christoph.kloss@cfdem.com
   Copyright 2009-2012 JKU Linz
   Copyright 2012-     DCS Computing GmbH, Linz

   LIGGGHTS is based on LAMMPS
   LAMMPS - Large-scale Atomic/Molecular Massively Parallel Simulator
   http://lammps.sandia.gov, Sandia National Laboratories
   Steve Plimpton, sjplimp@sandia.gov

   This software is distributed under the GNU General Public License.

   See the README file in the top-level directory.
------------------------------------------------------------------------- */

/* ----------------------------------------------------------------------
   Contributing authors:
   Christoph Kloss (JKU Linz, DCS Computing GmbH, Linz)
   Philippe Seil (JKU Linz)
   Evan Smuts (U Cape Town, surface velocity rotation)
------------------------------------------------------------------------- */

#include "fix_mesh.h"
#include <stdio.h>
#include <string.h>
#include "error.h"
#include "force.h"
#include "bounding_box.h"
#include "input_mesh_tri.h"
#include "fix_contact_history.h"
#include "fix_neighlist_mesh.h"
#include "multi_node_mesh.h"
#include "modify.h"
#include "comm.h"
#include "math_extra.h"

using namespace LAMMPS_NS;
using namespace FixConst;

#define EPSILON_V 0.00001

FixMesh::FixMesh(LAMMPS *lmp, int narg, char **arg)
: Fix(lmp, narg, arg),
  mesh_(NULL),
  atom_type_mesh_(-1),
  setupFlag_(false),
  pOpFlag_(false)
{
    if(narg < 5)
      error->fix_error(FLERR,this,"not enough arguments - at least keyword 'file' and a filename are required.");

    restart_global = 1;

    iarg_ = 3;

    char mesh_fname[256];
    if(strcmp(arg[iarg_++],"file"))
        error->fix_error(FLERR,this,"expecting keyword 'file'");
    strcpy(mesh_fname,arg[iarg_++]);

    // parse type
    if(strcmp(arg[iarg_],"type") == 0)
    {
        iarg_++;
        atom_type_mesh_ = force->inumeric(arg[iarg_++]);
    }

    // construct a mesh - can be surface or volume mesh
    
    // just create object and return if reading data from restart file
    
    if(modify->have_restart_data(this)) create_mesh_restart();
    else create_mesh(mesh_fname);

    // parse further args

    bool hasargs = true;
    while(iarg_ < narg && hasargs)
    {
      hasargs = false;
      if(strcmp(arg[iarg_],"move") == 0){
          if (narg < iarg_+4) error->fix_error(FLERR,this,"not enough arguments");
          moveMesh(force->numeric(arg[iarg_+1]),force->numeric(arg[iarg_+2]),force->numeric(arg[iarg_+3]));
          iarg_ += 4;
          hasargs = true;
      } else if(strcmp(arg[iarg_],"rotate") == 0){
          if (narg < iarg_+7)
              error->fix_error(FLERR,this,"not enough arguments");
          if(strcmp(arg[iarg_+1],"axis"))
              error->fix_error(FLERR,this,"expecting keyword 'axis' after keyword 'rotate'");
          if(strcmp(arg[iarg_+5],"angle"))
              error->fix_error(FLERR,this,"expecting keyword 'angle' after axis definition");
          rotateMesh(force->numeric(arg[iarg_+2]),force->numeric(arg[iarg_+3]),force->numeric(arg[iarg_+4]),
                   force->numeric(arg[iarg_+6]));
          iarg_ += 7;
          hasargs = true;
      } else if(strcmp(arg[iarg_],"scale") == 0){
          if (narg < iarg_+2) error->fix_error(FLERR,this,"not enough arguments");
          scaleMesh(force->numeric(arg[iarg_+1]));
          iarg_ += 2;
          hasargs = true;
      } else if (strcmp(arg[iarg_],"temperature") == 0) {
          iarg_++;
          double Temp_mesh = atof(arg[iarg_++]);
          mesh_->prop().addGlobalProperty< ScalarContainer<double> >("Temp","comm_none","frame_invariant","restart_yes");
          mesh_->prop().setGlobalProperty< ScalarContainer<double> >("Temp",Temp_mesh);
          mesh_->prop().addGlobalProperty< ScalarContainer<double> >("heatFlux","comm_none","frame_invariant","restart_no");
          mesh_->prop().setGlobalProperty< ScalarContainer<double> >("heatFlux",0.);
          mesh_->prop().addGlobalProperty< ScalarContainer<double> >("heatFluxTotal","comm_none","frame_invariant","restart_yes");
          mesh_->prop().setGlobalProperty< ScalarContainer<double> >("heatFluxTotal",0.);
          
          hasargs = true;
      }
    }
}

/* ---------------------------------------------------------------------- */

FixMesh::~FixMesh()
{
    delete mesh_;
}

/* ---------------------------------------------------------------------- */

void FixMesh::post_create()
{

}

/* ---------------------------------------------------------------------- */

void FixMesh::create_mesh(char *mesh_fname)
{
    
    if(strcmp(style,"mesh/surface") == 0)
    {
        mesh_ = new TriMesh(lmp);
        static_cast<TriMesh*>(mesh_)->setMeshID(id);

        // read file
        // can be from STL file or VTK file
        InputMeshTri *mesh_input = new InputMeshTri(lmp,0,NULL);
        mesh_input->meshtrifile(mesh_fname,static_cast<TriMesh*>(mesh_));
        delete mesh_input;
    }
    else error->one(FLERR,"Illegal implementation of create_mesh();");
}

/* ---------------------------------------------------------------------- */

void FixMesh::create_mesh_restart()
{
    
    if(strcmp(style,"mesh/surface") == 0)
    {
        mesh_ = new TriMesh(lmp);
        static_cast<TriMesh*>(mesh_)->setMeshID(id);
    }
    else error->one(FLERR,"Illegal implementation of create_mesh();");
}

/* ---------------------------------------------------------------------- */

void FixMesh::pre_delete(bool unfixflag)
{
    // error if moving mesh is operating on a mesh to be deleted
    
    if(unfixflag)
    {
        if(mesh_->isMoving())
            error->fix_error(FLERR,this,
                    "illegal unfix command, may not unfix a mesh while a fix move is applied."
                    "Unfix the fix move/mesh first");
    }
}

/* ---------------------------------------------------------------------- */

int FixMesh::setmask()
{
    int mask = 0;
    mask |= PRE_EXCHANGE;
    mask |= PRE_FORCE;
    return mask;
}

/* ---------------------------------------------------------------------- */

void FixMesh::setup_pre_force(int vflag)
{
    // first-time set-up
    
    if(!setupFlag_)
    {
        initialSetup();
        setupFlag_ = true;
    }
    // if mesh already set-up and parallelized
    
    else
    {
        mesh_->pbcExchangeBorders(1);
    }

    pOpFlag_ = false;

}

/* ---------------------------------------------------------------------- */

void FixMesh::initialSetup()
{
    
    mesh_->initalSetup();

    // warn if there are elements that extend outside box
    if(!mesh_->allNodesInsideSimulationBox())
       error->warning(FLERR,"Not all nodes of fix mesh inside simulation box, "
                            "elements will be deleted or wrapped around periodic boundary conditions");

    if(comm->me == 0)
       fprintf(screen,"Import and parallelization of mesh %s containing %d triangle(s) successful\n",
               id,mesh_->sizeGlobal());
}

/* ----------------------------------------------------------------------
   invoke parallelism
------------------------------------------------------------------------- */

void FixMesh::pre_exchange()
{
    // flag parallel operations on this step
    
    pOpFlag_ = true;
}

/* ----------------------------------------------------------------------
   forward comm for mesh, currently no reverse comm invoked
------------------------------------------------------------------------- */

void FixMesh::pre_force(int vflag)
{
    
    // case re-neigh step
    if(pOpFlag_)
    {
        mesh_->pbcExchangeBorders(0);
        pOpFlag_ = false;
    }
    // case regular step
    else
        mesh_->forwardComm();
}

/* ---------------------------------------------------------------------- */

int FixMesh::min_type()
{
    if(atom_type_mesh_ == -1) return 1;
    return atom_type_mesh_;
}

/* ---------------------------------------------------------------------- */

int FixMesh::max_type()
{
    if(atom_type_mesh_ == -1) return 1;
    return atom_type_mesh_;
}

/* ----------------------------------------------------------------------
   moves the mesh by a vector - (dx dy dz) is the displacement vector
------------------------------------------------------------------------- */

void FixMesh::moveMesh(double const dx, double const dy, double const dz)
{
    if (comm->me == 0)
    {
      //fprintf(screen,"moving mesh by ");
      //fprintf(screen,"%f %f %f\n", dx,dy,dz);
    }

    double arg[3] = {dx,dy,dz};
    mesh_->move(arg);
}

/* ----------------------------------------------------------------------
   rotates the mesh around an axis through the origin
   phi the rotation angle
   (axisX axisY axisZ) vector in direction of the axis
------------------------------------------------------------------------- */

void FixMesh::rotateMesh(double const axisX, double const axisY, double const axisZ, double const phi)
{
    double axis[3] = {axisX,axisY,axisZ}, p[3] = {0.,0.,0.};

    if (comm->me == 0)
    {
      //fprintf(screen,"rotate ");
      //fprintf(screen,"%f %f %f %f\n", phi, axisX, axisY, axisZ);
    }

    mesh_->rotate(phi*3.14159265/180.0,axis,p);
}

/* ----------------------------------------------------------------------
   scales the mesh by a factor in x, y and z direction
   can also be used to distort a mesh
   (factorX factorY factorZ) vector contains the factors to scale the
   mesh in the xyz direction
------------------------------------------------------------------------- */

void FixMesh::scaleMesh(double const factor)
{
    if (comm->me == 0){
      //fprintf(screen,"scale ");
      //fprintf(screen,"%f \n", factor);
    }
    mesh_->scale(factor);
}

/* ----------------------------------------------------------------------
   pack entire state of Fix into one write
------------------------------------------------------------------------- */

void FixMesh::write_restart(FILE *fp)
{
    mesh_->writeRestart(fp);
}

/* ----------------------------------------------------------------------
   use state info from restart file to restart the Fix
------------------------------------------------------------------------- */

void FixMesh::restart(char *buf)
{
    double *list = (double *) buf;
    mesh_->restart(list);
}
