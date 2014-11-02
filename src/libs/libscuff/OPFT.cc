/* Copyright (C) 2005-2011 M. T. Homer Reid
 *
 * This file is part of SCUFF-EM.
 *
 * SCUFF-EM is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * SCUFF-EM is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/*
 * OPFT.cc     -- computation of power, force, and torque using overlap
 *             -- integrals between RWG basis functions
 *
 * homer reid  -- 5/2012
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <ctype.h>

#include <libhrutil.h>

#include "libscuff.h"
#include "libscuffInternals.h"

#include "cmatheval.h"

#define II cdouble(0.0,1.0)

namespace scuff {

/***************************************************************/
/* these constants identify various types of overlap           */
/* integrals; they are only used in this file.                 */
/* Note that these constants are talking about types of        */
/* overlap *integrals*, not to be confused with the various    */
/* types of overlap *matrices,* which are index by different   */
/* constants defined in libscuff.h. (The entries of the overlap*/
/* matrices are linear combinations of various types of overlap*/
/* integrals.)                                                 */
/***************************************************************/
#define OVERLAP_OVERLAP           0
#define OVERLAP_CROSS             1
#define OVERLAP_BULLET_X          2    
#define OVERLAP_NABLANABLA_X      3
#define OVERLAP_TIMESNABLA_X      4
#define OVERLAP_BULLET_Y          5  
#define OVERLAP_NABLANABLA_Y      6
#define OVERLAP_TIMESNABLA_Y      7
#define OVERLAP_BULLET_Z          8  
#define OVERLAP_NABLANABLA_Z      9
#define OVERLAP_TIMESNABLA_Z     10
#define OVERLAP_RXBULLET_X       11    
#define OVERLAP_RXNABLANABLA_X   12
#define OVERLAP_RXTIMESNABLA_X   13
#define OVERLAP_RXBULLET_Y       14  
#define OVERLAP_RXNABLANABLA_Y   15
#define OVERLAP_RXTIMESNABLA_Y   16
#define OVERLAP_RXBULLET_Z       17  
#define OVERLAP_RXNABLANABLA_Z   18
#define OVERLAP_RXTIMESNABLA_Z   19

#define NUMOVERLAPS 20


// Note: the prefactor of (10/3) in the force and torque factors 
// below arises as follows: the force quantity that we would compute
// without it has units of 
// 1 watt / c = (1 joule/s) * (10-8 s/m) / 3
//            = (10/3) nanoNewton
// so we want to multiply the number we would naively 
// compute by 10/3 to get a force in nanonewtons.
// similarly for the torque: multiplying by 10/3 gives the torque
// in nanoNewtons*microns (assuming the incident field was 
// measured in units of volts / microns)
#define TENTHIRDS 3.33333333333333333333333

/***************************************************************/
/* this is a helper function for GetOverlaps that computes the */
/* contributions of a single panel to the overlap integrals.   */
/***************************************************************/
void AddOverlapContributions(RWGSurface *S, RWGPanel *P, int iQa, int iQb, 
                             double Sign, double LL, double *Overlaps)
{
  double *Qa   = S->Vertices + 3*P->VI[ iQa ];
  double *QaP1 = S->Vertices + 3*P->VI[ (iQa+1)%3 ];
  double *QaP2 = S->Vertices + 3*P->VI[ (iQa+2)%3 ];
  double *Qb   = S->Vertices + 3*P->VI[ iQb ];
  double *ZHat = P->ZHat;

  double L1[3], L2[3], DQ[3];
  VecSub(QaP1, Qa, L1);
  VecSub(QaP2, QaP1, L2);
  VecSub(Qa, Qb, DQ);

  double ZxL1[3], ZxL2[3], ZxDQ[3], ZxQa[3], QaxZxL1[3], QaxZxL2[3];
  VecCross(ZHat,   L1,     ZxL1);
  VecCross(ZHat,   L2,     ZxL2);
  VecCross(ZHat,   DQ,     ZxDQ);
  VecCross(ZHat,   Qa,     ZxQa);
  VecCross(Qa,   ZxL1,  QaxZxL1);
  VecCross(Qa,   ZxL2,  QaxZxL2);

  double PreFac = Sign * LL / (2.0*P->Area);

  double L1dL1 = L1[0]*L1[0] + L1[1]*L1[1] + L1[2]*L1[2];
  double L1dL2 = L1[0]*L2[0] + L1[1]*L2[1] + L1[2]*L2[2];
  double L1dDQ = L1[0]*DQ[0] + L1[1]*DQ[1] + L1[2]*DQ[2];
  double L2dL2 = L2[0]*L2[0] + L2[1]*L2[1] + L2[2]*L2[2];
  double L2dDQ = L2[0]*DQ[0] + L2[1]*DQ[1] + L2[2]*DQ[2];

  double TimesFactor = (  (2.0*L1[0]+L2[0])*ZxDQ[0]  
                        + (2.0*L1[1]+L2[1])*ZxDQ[1] 
                        + (2.0*L1[2]+L2[2])*ZxDQ[2]  ) / 6.0;

  double BulletFactor1 =  (L1dL1 + L1dL2)/4.0 + L1dDQ/3.0 + L2dL2/12.0 + L2dDQ/6.0;
  double BulletFactor2 =  (L1dL1 + L1dL2)/5.0 + L1dDQ/4.0 + L2dL2/15.0 + L2dDQ/8.0;
  double BulletFactor3 =  L1dL1/10.0 + 2.0*L1dL2/15.0 + L1dDQ/8.0 + L2dL2/20.0 + L2dDQ/12.0;
  double NablaCrossFactor =  (L1dL1 + L1dL2)/2.0 + L2dL2/6.0;

  Overlaps[0]  += PreFac * BulletFactor1;
  Overlaps[1]  += PreFac * TimesFactor;

  Overlaps[2]  += PreFac * ZHat[0] * BulletFactor1;
  Overlaps[3]  += PreFac * ZHat[0] * 2.0;
  Overlaps[4]  += PreFac * (2.0*ZxL1[0] + ZxL2[0]) / 3.0;

  Overlaps[5]  += PreFac * ZHat[1] * BulletFactor1;
  Overlaps[6]  += PreFac * ZHat[1] * 2.0;
  Overlaps[7]  += PreFac * (2.0*ZxL1[1] + ZxL2[1]) / 3.0;

  Overlaps[8]  += PreFac * ZHat[2] * BulletFactor1;
  Overlaps[9]  += PreFac * ZHat[2] * 2.0;
  Overlaps[10] += PreFac * (2.0*ZxL1[2] + ZxL2[2]) / 3.0;

  Overlaps[11] -= PreFac * (ZxQa[0]*BulletFactor1 + ZxL1[0]*BulletFactor2 + ZxL2[0]*BulletFactor3);
  Overlaps[12] -= PreFac * (2.0*ZxQa[0] + 4.0*ZxL1[0]/3.0 + 2.0*ZxL2[0]/3.0);
  Overlaps[13] += PreFac * (ZHat[0]*NablaCrossFactor + 2.0*QaxZxL1[0]/3.0 + QaxZxL2[0]/3.0);

  Overlaps[14] -= PreFac * (ZxQa[1]*BulletFactor1 + ZxL1[1]*BulletFactor2 + ZxL2[1]*BulletFactor3);
  Overlaps[15] -= PreFac * (2.0*ZxQa[1] + 4.0*ZxL1[1]/3.0 + 2.0*ZxL2[1]/3.0);
  Overlaps[16] += PreFac * (ZHat[1]*NablaCrossFactor + 2.0*QaxZxL1[1]/3.0 + QaxZxL2[1]/3.0);

  Overlaps[17] -= PreFac * (ZxQa[2]*BulletFactor1 + ZxL1[2]*BulletFactor2 + ZxL2[2]*BulletFactor3);
  Overlaps[18] -= PreFac * (2.0*ZxQa[2] + 4.0*ZxL1[2]/3.0 + 2.0*ZxL2[2]/3.0);
  Overlaps[19] += PreFac * (ZHat[2]*NablaCrossFactor + 2.0*QaxZxL1[2]/3.0 + QaxZxL2[2]/3.0);

}

/***************************************************************/
/* Get the overlap integrals between a single pair of RWG      */
/* basis functions on an RWG surface.                          */
/*                                                             */
/* entries of output array:                                    */
/*                                                             */
/*  Overlaps [0] = O^{\bullet}_{\alpha\beta}                   */
/*   = \int f_a \cdot f_b                                      */
/*                                                             */
/*  Overlaps [1] = O^{\times}_{\alpha\beta}                    */
/*   = \int f_a \cdot (nHat \times f_b)                        */
/*                                                             */
/*  Overlaps [2] = O^{x,\bullet}_{\alpha\beta}                 */
/*   = \int nHat_x f_a \cdot f_b                               */
/*                                                             */
/*  Overlaps [3] = O^{x,\nabla\nabla}_{\alpha\beta}            */
/*   = \int nHat_x (\nabla \cdot f_a) (\nabla \cdot f_b)       */
/*                                                             */
/*  Overlaps [4] = O^{x,\times\nabla}_{\alpha\beta}            */
/*   = \int (nHat \times \cdot f_a)_x (\nabla \cdot f_b)       */
/*                                                             */
/*  [5,6,7]  = like [2,3,4] but with x-->y                     */
/*  [8,9,10] = like [2,3,4] but with x-->z                     */
/*                                                             */
/*  [11-19]: like [2...10] but with an extra factor of         */
/*           (rHat x ) thrown in for torque purposes           */
/*                                                             */
/* note: for now, the origin about which torque is computed    */
/* coincides with the origin of the coordinate system in which */
/* the surface mesh was defined (i.e. the point with coords    */
/* (0,0,0) in the mesh file, as transformed by any             */
/* GTransformations that have been applied since the mesh file */
/* was read in.) if you want to compute the torque about a     */
/* different origin, a quick-and-dirty procedure is to         */
/* apply a GTransformation to the surface, compute the         */
/* overlaps, then undo the GTransformation.                    */
/***************************************************************/
void RWGSurface::GetOverlaps(int neAlpha, int neBeta, double *Overlaps)
{
  RWGEdge *EAlpha = Edges[neAlpha];
  RWGEdge *EBeta  = Edges[neBeta];

  RWGPanel *PAlphaP = Panels[EAlpha->iPPanel];
  RWGPanel *PAlphaM = (EAlpha->iMPanel == -1) ? 0 : Panels[EAlpha->iMPanel];
  int iQPAlpha = EAlpha->PIndex;
  int iQMAlpha = EAlpha->MIndex;
  int iQPBeta  = EBeta->PIndex;
  int iQMBeta  = EBeta->MIndex;

  double LL = EAlpha->Length * EBeta->Length;

  memset(Overlaps,0,NUMOVERLAPS*sizeof(double));

  if ( EAlpha->iPPanel == EBeta->iPPanel )
   AddOverlapContributions(this, PAlphaP, iQPAlpha, iQPBeta,  1.0, LL, Overlaps);
  if ( EAlpha->iPPanel == EBeta->iMPanel )
   AddOverlapContributions(this, PAlphaP, iQPAlpha, iQMBeta, -1.0, LL, Overlaps);
  if ( (EAlpha->iMPanel!=-1) && (EAlpha->iMPanel == EBeta->iPPanel ) )
   AddOverlapContributions(this, PAlphaM, iQMAlpha, iQPBeta, -1.0, LL, Overlaps);
  if ( (EAlpha->iMPanel!=-1) && (EAlpha->iMPanel == EBeta->iMPanel ) )
   AddOverlapContributions(this, PAlphaM, iQMAlpha, iQMBeta,  1.0, LL, Overlaps);
  
}

/*****************************************************************/
/* this is a simpler interface to the above routine that returns */
/* the simple overlap integral and sets *pOTimes = crossed       */
/* overlap integral if it is non-NULL                            */
/*****************************************************************/
double RWGSurface::GetOverlap(int neAlpha, int neBeta, double *pOTimes)
{
  double Overlaps[NUMOVERLAPS];
  GetOverlaps(neAlpha, neBeta, Overlaps);
  if (pOTimes) *pOTimes=Overlaps[1];
  return Overlaps[0];
}

/***************************************************************/
/***************************************************************/
/***************************************************************/
int GetOverlappingEdgeIndices(RWGSurface *S, int nea, int nebArray[5])
{
  nebArray[0] = nea;

  RWGEdge *E   = S->Edges[nea];
  RWGPanel *PP = S->Panels[ E->iPPanel ]; 
  int      iQP = E->PIndex;
  nebArray[1] = PP->EI[ (iQP+1)%3 ];
  nebArray[2] = PP->EI[ (iQP+2)%3 ];

  if ( E->iMPanel == -1 )
   return 3;

  RWGPanel *PM = S->Panels[ E->iMPanel ];
  int      iQM = E->MIndex;
  nebArray[3] = PM->EI[ (iQM+1)%3 ];
  nebArray[4] = PM->EI[ (iQM+2)%3 ];
   
  return 5;
}

/***************************************************************/
/***************************************************************/
/***************************************************************/
void RWGGeometry::GetOPFT(int SurfaceIndex, cdouble Omega,
                          HVector *KNVector, HVector *RHS,
                          HMatrix *SigmaMatrix,
                          double PFT[7], 
                          double *PTot, double **ByEdge)
{
  if (SurfaceIndex<0 || SurfaceIndex>=NumSurfaces)
   { memset(PFT,0,7*sizeof(double));
     Warn("GetOPFTTrace called for unknown surface #i",SurfaceIndex);
     return;
   };

  RWGSurface *S=Surfaces[SurfaceIndex];
  int Offset = BFIndexOffset[SurfaceIndex];
  int NE=S->NumEdges;

  /*--------------------------------------------------------------*/
  /*- get material parameters of exterior medium -----------------*/
  /*--------------------------------------------------------------*/
  cdouble ZZ=ZVAC, k2=Omega*Omega;
  cdouble Eps, Mu;
  RegionMPs[S->RegionIndices[0]]->GetEpsMu(Omega, &Eps, &Mu);
  k2 *= Eps*Mu;
  ZZ *= sqrt(Mu/Eps);

  /*--------------------------------------------------------------*/
  /*- initialize edge-by-edge contributions to zero --------------*/
  /*--------------------------------------------------------------*/
  if (ByEdge)
   { for(int nq=0; nq<NUMPFT; nq++)
      if (ByEdge[nq])
       memset(ByEdge[nq],0,NE*sizeof(double));
   };

  /***************************************************************/
  /***************************************************************/
  /***************************************************************/
  double PAbs=0.0, Fx=0.0, Fy=0.0, Fz=0.0, Taux=0.0, Tauy=0.0, Tauz=0.0;
  for(int nea=0; nea<NE; nea++)
   { 
     /*--------------------------------------------------------------*/
     /* populate an array whose indices are the 3 or 5 edges         */
     /* that have nonzero overlaps with edge #nea, then loop over    */
     /* those edges                                                  */
     /*--------------------------------------------------------------*/
     int nebArray[5];
     int nebCount= GetOverlappingEdgeIndices(S, nea, nebArray);
     for(int nneb=0; nneb<nebCount; nneb++)
      { 
        int neb=nebArray[nneb];
        double Overlaps[20];
        S->GetOverlaps(nea, neb, Overlaps);

        /*--------------------------------------------------------------*/
        /*--------------------------------------------------------------*/
        /*--------------------------------------------------------------*/
        cdouble KK, KN, NK, NN;
        if (KNVector)
         { 
           cdouble kAlpha =       KNVector->GetEntry(Offset + 2*nea + 0);
           cdouble nAlpha = -ZVAC*KNVector->GetEntry(Offset + 2*nea + 1);
           cdouble kBeta  =       KNVector->GetEntry(Offset + 2*neb + 0);
           cdouble nBeta  = -ZVAC*KNVector->GetEntry(Offset + 2*neb + 1);

           KK = conj(kAlpha) * kBeta;
           KN = conj(kAlpha) * nBeta;
           NK = conj(nAlpha) * kBeta;
           NN = conj(nAlpha) * nBeta;
         }
        else
         {
           KK = SigmaMatrix->GetEntry(Offset+2*neb+0, Offset+2*nea+0);
           KN = SigmaMatrix->GetEntry(Offset+2*neb+1, Offset+2*nea+0);
           NK = SigmaMatrix->GetEntry(Offset+2*neb+0, Offset+2*nea+1);
           NN = SigmaMatrix->GetEntry(Offset+2*neb+1, Offset+2*nea+1);
         };

       /*--------------------------------------------------------------*/
       /*--------------------------------------------------------------*/
       /*--------------------------------------------------------------*/
       // power
       double dPAbs = 0.25*real( (KN-NK) * Overlaps[OVERLAP_CROSS] );

       // force, torque
       double dF[3], dTau[3];
       dF[0] = 0.25*TENTHIRDS*
               real( -(KK*ZZ + NN/ZZ)*(Overlaps[OVERLAP_BULLET_X] - Overlaps[OVERLAP_NABLANABLA_X]/k2)
                     +(NK-KN)*2.0*Overlaps[OVERLAP_TIMESNABLA_X] / (II*Omega)
                   );

       dF[1] = 0.25*TENTHIRDS*
               real( -(KK*ZZ + NN/ZZ)*(Overlaps[OVERLAP_BULLET_Y] - Overlaps[OVERLAP_NABLANABLA_Y]/k2)
                     +(NK-KN)*2.0*Overlaps[OVERLAP_TIMESNABLA_Y] / (II*Omega)
                   );

       dF[2] = 0.25*TENTHIRDS*
               real( -(KK*ZZ + NN/ZZ)*(Overlaps[OVERLAP_BULLET_Z] - Overlaps[OVERLAP_NABLANABLA_Z]/k2)
                     +(NK-KN)*2.0*Overlaps[OVERLAP_TIMESNABLA_Z] / (II*Omega)
                   );

       dTau[0] = 0.25*TENTHIRDS*
                 real( -(KK*ZZ + NN/ZZ)*(Overlaps[OVERLAP_RXBULLET_X] - Overlaps[OVERLAP_RXNABLANABLA_X]/k2)
                       +(NK-KN)*2.0*Overlaps[OVERLAP_RXTIMESNABLA_X] / (II*Omega)
                     );

       dTau[1] = 0.25*TENTHIRDS*
                 real( -(KK*ZZ + NN/ZZ)*(Overlaps[OVERLAP_RXBULLET_Y] - Overlaps[OVERLAP_RXNABLANABLA_Y]/k2)
                       +(NK-KN)*2.0*Overlaps[OVERLAP_RXTIMESNABLA_Y] / (II*Omega)
                     );

       dTau[2] = 0.25*TENTHIRDS*
                 real( -(KK*ZZ + NN/ZZ)*(Overlaps[OVERLAP_RXBULLET_Z] - Overlaps[OVERLAP_RXNABLANABLA_Z]/k2)
                       +(NK-KN)*2.0*Overlaps[OVERLAP_RXTIMESNABLA_Z] / (II*Omega)
                     );

       /*--------------------------------------------------------------*/
       /*- accumulate contributions to full sums ----------------------*/
       /*--------------------------------------------------------------*/
       PAbs += dPAbs;
       Fx   += dF[0];
       Fy   += dF[1];
       Fz   += dF[2];
       Taux += dTau[0];
       Tauy += dTau[1];
       Tauz += dTau[2];

       /*--------------------------------------------------------------*/
       /*- accumulate contributions to by-edge sums -------------------*/
       /*--------------------------------------------------------------*/
       if (ByEdge) 
        {  
          if (ByEdge[0]) ByEdge[0][nea] += dPAbs;
          if (ByEdge[1]) ByEdge[1][nea] += dF[0];
          if (ByEdge[2]) ByEdge[2][nea] += dF[1];
          if (ByEdge[3]) ByEdge[3][nea] += dF[2];
          if (ByEdge[4]) ByEdge[4][nea] += dTau[0];
          if (ByEdge[5]) ByEdge[5][nea] += dTau[1];
          if (ByEdge[6]) ByEdge[6][nea] += dTau[2];
        };

      } // for (int nneb=... 

   }; // for(int nea=0; nea<S->NE; nea++)

  /*--------------------------------------------------------------*/
  /*--------------------------------------------------------------*/
  /*--------------------------------------------------------------*/
  PFT[0] = PAbs;
  PFT[1] = Fx;
  PFT[2] = Fy;
  PFT[3] = Fz;
  PFT[4] = Taux;
  PFT[5] = Tauy;
  PFT[6] = Tauz;

  /*--------------------------------------------------------------*/
  /*- get extinction ---------------------------------------------*/
  /*--------------------------------------------------------------*/
  if (PTot && KNVector && RHS)
   { double Extinction=0.0;
     for (int ne=0, nbf=0; ne<NE; ne++)
      { 
        cdouble kAlpha =   KNVector->GetEntry(Offset + nbf);
        cdouble vEAlpha = -ZVAC*RHS->GetEntry(Offset + nbf);
        nbf++;
        Extinction += 0.5*real( conj(kAlpha)*vEAlpha );
        if (S->IsPEC) continue;

        cdouble nAlpha  = -ZVAC*KNVector->GetEntry(Offset + nbf);
        cdouble vHAlpha =       -1.0*RHS->GetEntry(Offset + nbf);
        nbf++;
        Extinction += 0.5*real( conj(nAlpha)*vHAlpha );
      };
     *PTot = Extinction;
   };

} // GetOPFT

/*****************************************************************/
/* this is a legacy routine for explicitly forming the (sparse)  */
/* overlap matrices that are sandwiched between                  */
/* surface-current vectors to get power, force, and torque       */
/*                                                               */
/* on entry, NeedMatrix is an array of 8 boolean flags, with     */
/* NeedMatrix[n] = 1 if the user wants overlap matrix #n.        */
/* (here 8 = SCUFF_NUM_OMATRICES).                               */
/*                                                               */
/* If NeedMatrix[n] = 1, then SArray must have at least n slots. */
/*                                                               */
/* If SArray[n] = 0 on entry, then a new SMatrix of the correct  */
/* size is allocated for that slot. If SArray[n] is nonzero but  */
/* points to an SMatrix of the incorrect size, then a new        */
/* SMatrix of the correct size is allocated, and SArray[n] is    */
/* overwritten with a pointer to this new SMatrix; the memory    */
/* allocated for the old SMatrix is not deallocated.             */
/*                                                               */
/* Omega is only referenced for the computation of force/torque  */
/* overlap matrices.                                             */
/*                                                               */
/* ExteriorMP is only referenced for the computation of force/   */
/* torque overlap matrices, and then only if it non-null; if     */ 
/* ExteriorMP==0 then the exterior medium is taken to be vacuum. */
/*****************************************************************/
#if 0
#define SCUFF_OMATRIX_OVERLAP    0
#define SCUFF_OMATRIX_POWER      1
#define SCUFF_OMATRIX_XFORCE     2
#define SCUFF_OMATRIX_YFORCE     3
#define SCUFF_OMATRIX_ZFORCE     4
#define SCUFF_OMATRIX_XTORQUE    5
#define SCUFF_OMATRIX_YTORQUE    6
#define SCUFF_OMATRIX_ZTORQUE    7
#define SCUFF_NUM_OMATRICES      8
void RWGSurface::GetOverlapMatrices(const bool NeedMatrix[SCUFF_NUM_OMATRICES],
                                    SMatrix *SArray[SCUFF_NUM_OMATRICES],
                                    cdouble Omega,
                                    MatProp *ExteriorMP)
{
  int NR  = NumBFs;

  /*--------------------------------------------------------------*/
  /*- the number of nonzero entries per row of the overlap matrix */
  /*- is fixed by the definition of the RWG basis function; each  */
  /*- RWG function overlaps with at most 5 RWG functions          */
  /*- (including itself), which gives 10 if we have both electric */
  /*- and magnetic currents.                                      */
  /*--------------------------------------------------------------*/
  int nnz = IsPEC ? 5 : 10;

  /*--------------------------------------------------------------*/
  /*- make sure each necessary slot of SArray points to an SMatrix*/  
  /*- of the appropriate size                                     */  
  /*--------------------------------------------------------------*/
  for(int n=0; n<SCUFF_NUM_OMATRICES; n++)
   { 
     if ( NeedMatrix[n] ) 
      { 
        if ( SArray[n] && ( (SArray[n]->NR != NR) || (SArray[n]->NC != NR) ) )
         { Warn("wrong-sized matrix passed to GetOverlapMatrices (reallocating)...");
           SArray[n]=0;
         };

        if (SArray[n]==0)
         SArray[n]=new SMatrix(NR, NR, LHM_COMPLEX);

	// TODO: avoid reallocation if shape is okay?
        SArray[n]->BeginAssembly(nnz*NR);

      };   
   };

  /*--------------------------------------------------------------*/
  /*--------------------------------------------------------------*/
  /*--------------------------------------------------------------*/
  cdouble ZZ=ZVAC, K2=Omega*Omega;
  if (ExteriorMP)
   { 
     cdouble Eps, Mu;
     ExteriorMP->GetEpsMu(Omega, &Eps, &Mu);
     K2 *= Eps*Mu;
     ZZ *= sqrt(Mu/Eps);
   };

  /*--------------------------------------------------------------*/
  /* TOS = 'two over sigma' where sigma = surface conductivity    */
  /*--------------------------------------------------------------*/
  cdouble TOS=0.0;
  if (SurfaceSigmaMP)
   { cdouble Sigma = SurfaceSigmaMP->GetEps(Omega);
     TOS = 2.0/Sigma;
   };

  char *SSParmNames[4]={ const_cast<char *>("w"), const_cast<char *>("x"), 
                         const_cast<char *>("y"), const_cast<char *>("z") };
  cdouble SSParmValues[4];
  SSParmValues[0]=Omega*(MatProp::FreqUnit);
 
  /*--------------------------------------------------------------*/
  /*- assemble the overlap matrices one row at a time ------------*/
  /*--------------------------------------------------------------*/
  for(int neAlpha=0; neAlpha<NumEdges; neAlpha++)
   { 
      /*--------------------------------------------------------------*/
      /*- if we have a spatially-varying surface conductivity, get its*/
      /*- value at the centroid of basis function #neAlpha            */
      /*--------------------------------------------------------------*/
      if (SurfaceSigmaMP)
       { SSParmValues[1] = Edges[neAlpha]->Centroid[0];
         SSParmValues[2] = Edges[neAlpha]->Centroid[1];
         SSParmValues[3] = Edges[neAlpha]->Centroid[2];
         cdouble Sigma=cevaluator_evaluate(SurfaceSigma, 4, SSParmNames, SSParmValues);
         TOS = 2.0/Sigma;
       };

      /*--------------------------------------------------------------*/
      /*- populate an array whose entries are the indices of the edges*/
      /*- whose RWG basis functions have nonzero overlap with neAlpha.*/
      /*--------------------------------------------------------------*/
      RWGEdge *E = Edges[neAlpha];
      int OverlappingEdgeIndices[5];
      int NumOverlappingEdges;

      RWGPanel *P = Panels[E->iPPanel];
      int iQ      = E->PIndex;

      OverlappingEdgeIndices[0] = P->EI[  iQ      ]; // =neAlpha
      NumOverlappingEdges=1;
      if ( P->EI[ (iQ+1)%3 ] >= 0 )
       OverlappingEdgeIndices[NumOverlappingEdges++] = P->EI[ (iQ+1)%3 ];
      if ( P->EI[ (iQ+2)%3 ] >= 0 )
       OverlappingEdgeIndices[NumOverlappingEdges++] = P->EI[ (iQ+2)%3 ];

      if (E->iMPanel!=-1)
       { 
         P  = Panels[E->iMPanel];
         iQ = E->MIndex;
         if ( P->EI[ (iQ+1)%3 ] >= 0 )
          OverlappingEdgeIndices[NumOverlappingEdges++] = P->EI[ (iQ+1)%3 ];
         if ( P->EI[ (iQ+2)%3 ] >= 0 )
          OverlappingEdgeIndices[NumOverlappingEdges++] = P->EI[ (iQ+2)%3 ];
       };

      /*--------------------------------------------------------------*/
      /*--------------------------------------------------------------*/
      /*--------------------------------------------------------------*/
      for(int noei=0; noei<NumOverlappingEdges; noei++)
       { 
         int neBeta = OverlappingEdgeIndices[noei];
         double Overlaps[NUMOVERLAPS];
         GetOverlaps(neAlpha, neBeta, Overlaps);

         cdouble XForce1 = 
TENTHIRDS*(Overlaps[OVERLAP_BULLET_X] - Overlaps[OVERLAP_NABLANABLA_X]/K2);
         cdouble XForce2 = TENTHIRDS*2.0*Overlaps[OVERLAP_TIMESNABLA_X] / (II*Omega);

         cdouble YForce1 = TENTHIRDS*(Overlaps[OVERLAP_BULLET_Y] - Overlaps[OVERLAP_NABLANABLA_Y]/K2);
         cdouble YForce2 = TENTHIRDS*2.0*Overlaps[OVERLAP_TIMESNABLA_Y] / (II*Omega);

         cdouble ZForce1 = TENTHIRDS*(Overlaps[OVERLAP_BULLET_Z] - Overlaps[OVERLAP_NABLANABLA_Z]/K2);
         cdouble ZForce2 = TENTHIRDS*2.0*Overlaps[OVERLAP_TIMESNABLA_Z] / (II*Omega);

         cdouble XTorque1 = TENTHIRDS*(Overlaps[OVERLAP_RXBULLET_X] - Overlaps[OVERLAP_RXNABLANABLA_X]/K2);
         cdouble XTorque2 = TENTHIRDS*2.0*Overlaps[OVERLAP_RXTIMESNABLA_X] / (II*Omega);

         cdouble YTorque1 = TENTHIRDS*(Overlaps[OVERLAP_RXBULLET_Y] - Overlaps[OVERLAP_RXNABLANABLA_Y]/K2);
         cdouble YTorque2 = TENTHIRDS*2.0*Overlaps[OVERLAP_RXTIMESNABLA_Y] / (II*Omega);

         cdouble ZTorque1 = TENTHIRDS*(Overlaps[OVERLAP_RXBULLET_Z] - Overlaps[OVERLAP_RXNABLANABLA_Z]/K2);
         cdouble ZTorque2 = TENTHIRDS*2.0*Overlaps[OVERLAP_RXTIMESNABLA_Z] / (II*Omega);

         if (IsPEC)
          { 
            if ( NeedMatrix[SCUFF_OMATRIX_OVERLAP] )
             SArray[SCUFF_OMATRIX_OVERLAP]->SetEntry(neAlpha, neBeta, Overlaps[OVERLAP_OVERLAP]);

            if ( NeedMatrix[SCUFF_OMATRIX_POWER] )
             SArray[SCUFF_OMATRIX_POWER]->SetEntry(neAlpha, neBeta, TOS*Overlaps[OVERLAP_OVERLAP]);

            if ( NeedMatrix[SCUFF_OMATRIX_XFORCE] )
             SArray[SCUFF_OMATRIX_XFORCE]->SetEntry(neAlpha, neBeta, ZZ*XForce1);
            if ( NeedMatrix[SCUFF_OMATRIX_YFORCE] )
             SArray[SCUFF_OMATRIX_YFORCE]->SetEntry(neAlpha, neBeta, ZZ*YForce1);
            if ( NeedMatrix[SCUFF_OMATRIX_ZFORCE] )
             SArray[SCUFF_OMATRIX_ZFORCE]->SetEntry(neAlpha, neBeta, ZZ*ZForce1);

            if ( NeedMatrix[SCUFF_OMATRIX_XTORQUE] )
             SArray[SCUFF_OMATRIX_XTORQUE]->SetEntry(neAlpha, neBeta, ZZ*XTorque1);
            if ( NeedMatrix[SCUFF_OMATRIX_YTORQUE] )
             SArray[SCUFF_OMATRIX_YTORQUE]->SetEntry(neAlpha, neBeta, ZZ*YTorque1);
            if ( NeedMatrix[SCUFF_OMATRIX_ZTORQUE] )
             SArray[SCUFF_OMATRIX_ZTORQUE]->SetEntry(neAlpha, neBeta, ZZ*ZTorque1);
          }
         else
          {
            if ( NeedMatrix[SCUFF_OMATRIX_OVERLAP] )
             { SArray[SCUFF_OMATRIX_OVERLAP]->SetEntry(2*neAlpha+0, 2*neBeta+0, Overlaps[OVERLAP_OVERLAP]);
               SArray[SCUFF_OMATRIX_OVERLAP]->SetEntry(2*neAlpha+1, 2*neBeta+1, Overlaps[OVERLAP_OVERLAP]);
             };

            if ( NeedMatrix[SCUFF_OMATRIX_POWER] )
             { 
               SArray[SCUFF_OMATRIX_POWER]->SetEntry(2*neAlpha+0, 2*neBeta+0, TOS*Overlaps[OVERLAP_OVERLAP]);
               SArray[SCUFF_OMATRIX_POWER]->SetEntry(2*neAlpha+0, 2*neBeta+1, Overlaps[OVERLAP_CROSS]);
               SArray[SCUFF_OMATRIX_POWER]->SetEntry(2*neAlpha+1, 2*neBeta+0, -1.0*Overlaps[OVERLAP_CROSS]);
             };

            if ( NeedMatrix[SCUFF_OMATRIX_XFORCE] )
             { SArray[SCUFF_OMATRIX_XFORCE]->SetEntry(2*neAlpha+0, 2*neBeta+0, -1.0*ZZ*XForce1);
               SArray[SCUFF_OMATRIX_XFORCE]->SetEntry(2*neAlpha+0, 2*neBeta+1, -1.0*XForce2);
               SArray[SCUFF_OMATRIX_XFORCE]->SetEntry(2*neAlpha+1, 2*neBeta+0,      XForce2);
               SArray[SCUFF_OMATRIX_XFORCE]->SetEntry(2*neAlpha+1, 2*neBeta+1, -1.0*XForce1/ZZ);
             };

            if ( NeedMatrix[SCUFF_OMATRIX_YFORCE] )
             { SArray[SCUFF_OMATRIX_YFORCE]->SetEntry(2*neAlpha+0, 2*neBeta+0, -1.0*ZZ*YForce1);
               SArray[SCUFF_OMATRIX_YFORCE]->SetEntry(2*neAlpha+0, 2*neBeta+1, -1.0*YForce2);
               SArray[SCUFF_OMATRIX_YFORCE]->SetEntry(2*neAlpha+1, 2*neBeta+0,      YForce2);
               SArray[SCUFF_OMATRIX_YFORCE]->SetEntry(2*neAlpha+1, 2*neBeta+1, -1.0*YForce1/ZZ);
             };

            if ( NeedMatrix[SCUFF_OMATRIX_ZFORCE] )
             { SArray[SCUFF_OMATRIX_ZFORCE]->SetEntry(2*neAlpha+0, 2*neBeta+0, -1.0*ZZ*ZForce1);
               SArray[SCUFF_OMATRIX_ZFORCE]->SetEntry(2*neAlpha+0, 2*neBeta+1, -1.0*ZForce2);
               SArray[SCUFF_OMATRIX_ZFORCE]->SetEntry(2*neAlpha+1, 2*neBeta+0,      ZForce2);
               SArray[SCUFF_OMATRIX_ZFORCE]->SetEntry(2*neAlpha+1, 2*neBeta+1, -1.0*ZForce1/ZZ);
             };

            if ( NeedMatrix[SCUFF_OMATRIX_XTORQUE] )
             { SArray[SCUFF_OMATRIX_XTORQUE]->SetEntry(2*neAlpha+0, 2*neBeta+0, -1.0*ZZ*XTorque1);
               SArray[SCUFF_OMATRIX_XTORQUE]->SetEntry(2*neAlpha+0, 2*neBeta+1, -1.0*XTorque2);
               SArray[SCUFF_OMATRIX_XTORQUE]->SetEntry(2*neAlpha+1, 2*neBeta+0,      XTorque2);
               SArray[SCUFF_OMATRIX_XTORQUE]->SetEntry(2*neAlpha+1, 2*neBeta+1, -1.0*XTorque1/ZZ);
             };

            if ( NeedMatrix[SCUFF_OMATRIX_YTORQUE] )
             { SArray[SCUFF_OMATRIX_YTORQUE]->SetEntry(2*neAlpha+0, 2*neBeta+0, -1.0*ZZ*YTorque1);
               SArray[SCUFF_OMATRIX_YTORQUE]->SetEntry(2*neAlpha+0, 2*neBeta+1, -1.0*YTorque2);
               SArray[SCUFF_OMATRIX_YTORQUE]->SetEntry(2*neAlpha+1, 2*neBeta+0,      YTorque2);
               SArray[SCUFF_OMATRIX_YTORQUE]->SetEntry(2*neAlpha+1, 2*neBeta+1, -1.0*YTorque1/ZZ);
             };

            if ( NeedMatrix[SCUFF_OMATRIX_ZTORQUE] )
             { SArray[SCUFF_OMATRIX_ZTORQUE]->SetEntry(2*neAlpha+0, 2*neBeta+0, -1.0*ZZ*ZTorque1);
               SArray[SCUFF_OMATRIX_ZTORQUE]->SetEntry(2*neAlpha+0, 2*neBeta+1, -1.0*ZTorque2);
               SArray[SCUFF_OMATRIX_ZTORQUE]->SetEntry(2*neAlpha+1, 2*neBeta+0,      ZTorque2);
               SArray[SCUFF_OMATRIX_ZTORQUE]->SetEntry(2*neAlpha+1, 2*neBeta+1, -1.0*ZTorque1/ZZ);
             };

          }; // if (IsPEC) ... else ... 

       }; // for(int noei=0;...
         
   }  // for(neAlpha...) ... for (neBeta...)

  for(int n=0; n<SCUFF_NUM_OMATRICES; n++)
   if ( NeedMatrix[n] )  
    SArray[n]->EndAssembly();

}
#endif

/***************************************************************/
/***************************************************************/
/***************************************************************/
#if 0
// older legacy version of GetOPFT that formed the overlap
// matrices explicitly
void RWGGeometry::GetOPFT(HVector *KN, HVector *RHS, cdouble Omega,
                          int SurfaceIndex, double OPFT[8])
{

  if (SurfaceIndex<0 || SurfaceIndex>=NumSurfaces)
   { memset(OPFT,0,8*sizeof(double));
     Warn("GetOPFT called for unknown surface #i",SurfaceIndex);
     return;  
   };
  RWGSurface *S=Surfaces[SurfaceIndex];

  /***************************************************************/
  /* compute overlap matrices.                                   */
  /* TODO: allow caller to pass caller-allocated storage for     */
  /* these matrices and for the KNTS vector below to avoid       */
  /* allocating on the fly                                       */
  /***************************************************************/
  bool NeedMatrix[SCUFF_NUM_OMATRICES];
  SMatrix *OMatrices[SCUFF_NUM_OMATRICES];
  for(int nm=0; nm<SCUFF_NUM_OMATRICES; nm++)
   { NeedMatrix[nm]=true;
     OMatrices[nm] = 0; 
   };
  S->GetOverlapMatrices(NeedMatrix, OMatrices, Omega, RegionMPs[0]);

  /***************************************************************/
  /* extract the chunk of the KN vector that is relevant for this*/
  /* surface and in the process (1) undo the SCUFF normalization */
  /* of the magnetic currents and (2) compute the total power    */
  /***************************************************************/
  int N = S->NumBFs;
  int Offset = BFIndexOffset[SurfaceIndex];
  HVector *KNTS=new HVector(N,LHM_COMPLEX); // 'KN, this surface'
  cdouble KAlpha, NAlpha, vEAlpha, vHAlpha;
  OPFT[1]=0.0;
  if (S->IsPEC)
   { for(int ne=0; ne<S->NumEdges; ne++)
      { 
        KAlpha  = KN->GetEntry(Offset + ne);
        vEAlpha = RHS ? -ZVAC*RHS->GetEntry( Offset + ne ) : 0.0;

        OPFT[1] += real( conj(KAlpha)*vEAlpha );

        KNTS->SetEntry(ne,  KAlpha );
      }
   }
  else //non-PEC
   { for(int ne=0; ne<S->NumEdges; ne++)
      { 
        KAlpha =       KN->GetEntry(Offset + 2*ne + 0);
        NAlpha = -ZVAC*KN->GetEntry(Offset + 2*ne + 1);

        vEAlpha = RHS ? -ZVAC*RHS->GetEntry( Offset + 2*ne + 0 ) : 0.0;
        vHAlpha = RHS ?  -1.0*RHS->GetEntry( Offset + 2*ne + 1 ) : 0.0;

        KNTS->SetEntry( 2*ne + 0,  KAlpha );
        KNTS->SetEntry( 2*ne + 1,  NAlpha );

        OPFT[1] += real( conj(KAlpha)*vEAlpha + conj(NAlpha)*vHAlpha );
      };
   };
  OPFT[1] *= 0.5;

  OPFT[0] = 0.25*OMatrices[SCUFF_OMATRIX_POWER]->BilinearProductD(KNTS,KNTS);

  OPFT[2] = 0.25*OMatrices[SCUFF_OMATRIX_XFORCE]->BilinearProductD(KNTS,KNTS);
  OPFT[3] = 0.25*OMatrices[SCUFF_OMATRIX_YFORCE]->BilinearProductD(KNTS,KNTS);
  OPFT[4] = 0.25*OMatrices[SCUFF_OMATRIX_ZFORCE]->BilinearProductD(KNTS,KNTS);

  OPFT[5] = 0.25*OMatrices[SCUFF_OMATRIX_XTORQUE]->BilinearProductD(KNTS,KNTS);
  OPFT[6] = 0.25*OMatrices[SCUFF_OMATRIX_YTORQUE]->BilinearProductD(KNTS,KNTS);
  OPFT[7] = 0.25*OMatrices[SCUFF_OMATRIX_ZTORQUE]->BilinearProductD(KNTS,KNTS);

  /***************************************************************/
  /***************************************************************/
  /***************************************************************/
  delete KNTS;
  for(int nm=0; nm<SCUFF_NUM_OMATRICES; nm++)
   delete OMatrices[nm];

}
#endif

/***************************************************************/
/* alternative interface to GetOPFT in which the caller        */
/* specifies the label of the surface instead of the index     */
/***************************************************************/
#if 0
void RWGGeometry::GetOPFT(HVector *KN, HVector *RHS, cdouble Omega,
                          char *SurfaceLabel, double OPFT[8])
{
  /*--------------------------------------------------------------*/
  /*- find the surface in question -------------------------------*/
  /*--------------------------------------------------------------*/
  if (RWGSurface *S=GetSurfaceByLabel(SurfaceLabel))
   { 
     GetOPFT(KN, RHS, Omega, S->Index, OPFT);
   }
  else
   { Warn("unknown surface label %s passed to GetOPFT",SurfaceLabel);
     memset(OPFT, 0, 8*sizeof(double));
   };
}
#endif

}// namespace scuff
