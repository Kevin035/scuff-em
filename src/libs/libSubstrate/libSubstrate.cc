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
 * libSubstrate.cc -- implicit handling of multilayered material substrates
 *                 -- this file: stuff common to static and full-wave cases
 *
 * homer reid   -- 3/2017
 *
 */
#include <stdio.h>
#include <math.h>
#include <stdarg.h>
#include <fenv.h>

#include "libhrutil.h"
#include "libMDInterp.h"
#include "libMatProp.h"
#include "libSGJC.h"
#include "libSubstrate.h"

/***************************************************************/
/* if the ErrMsg field of the class instance is nonzero on     */
/* return, something went wrong                                */
/***************************************************************/
LayeredSubstrate::LayeredSubstrate(const char *FileName)
{
  char *Dir=0;
  FILE *f=fopenPath(getenv("SCUFF_SUBSTRATE_PATH"), FileName,"r",&Dir);
  if (!f)
   { ErrMsg=vstrdup("could not open file %s",FileName);
     return; 
   };
  Log("Reading substrate definition from %s/%s.",Dir ? Dir : ".",FileName);

  NumInterfaces=0;
  MPLayer=(MatProp **)mallocEC(1*sizeof(MatProp *));
  MPLayer[0]=new MatProp("VACUUM");
  zInterface=0;
  zGP=HUGE_VAL;

#define MAXSTR 1000
  char Line[MAXSTR];
  int LineNum=0;
  while( fgets(Line,MAXSTR,f) )
   { 
     /*--------------------------------------------------------------*/
     /*- skip blank lines and constants -----------------------------*/
     /*--------------------------------------------------------------*/
     LineNum++;
     int NumTokens;
     char *Tokens[2];
     int Length=strlen(Line);
     if (Length==0) continue;
     Line[(Length-1)]=0; // remove trailing carriage return
     NumTokens=Tokenize(Line, Tokens, 2);
     if ( NumTokens==0 || Tokens[0][0]=='#' )
      continue; 

     /*--------------------------------------------------------------*/
     /*- all lines must be of the form   ----------------------------*/
     /*-   zValue  MaterialName          ----------------------------*/
     /*- or                              ----------------------------*/
     /*-   MEDIUM  MaterialName          ----------------------------*/
     /*- or                              ----------------------------*/
     /*-   zValue  GROUNDPLANE           ----------------------------*/
     /*--------------------------------------------------------------*/
     if ( NumTokens!=2 )
      { ErrMsg=vstrdup("%s:%i syntax error",FileName,LineNum);
        return;
      };

     if ( !strcasecmp(Tokens[0],"MEDIUM") )
      { MPLayer[0] = new MatProp(Tokens[1]);
        if (MPLayer[0]->ErrMsg)
          { ErrMsg=vstrdup("%s:%i: %s",FileName,LineNum,MPLayer[0]->ErrMsg);
            return;
          }
        Log("Setting upper half-space medium to %s.",MPLayer[0]->Name);
        continue;
      };

     double z;
     if ( 1!=sscanf(Tokens[0],"%le",&z) )
      { ErrMsg=vstrdup("%s:%i bad z-value %s",FileName,LineNum,Tokens[0]);
        return;
      };

     if ( !strcasecmp(Tokens[1],"GROUNDPLANE") )
      { zGP = z;
        Log(" Ground plane at z=%e.",zGP);
      }
     else
      { 
        if (NumInterfaces>0 && z>zInterface[NumInterfaces-1])
         { ErrMsg=vstrdup("%s:%i: z coordinate lies above previous layer");
           return;
         };

        MatProp *MP = new MatProp(Tokens[1]);
        if (MP->ErrMsg)
         { ErrMsg=vstrdup("%s:%i: %s",FileName,LineNum,MP->ErrMsg);
           return;
         };
        NumInterfaces++;
        MPLayer=(MatProp **)reallocEC(MPLayer,(NumInterfaces+1)*sizeof(MatProp *));
        zInterface=(double  *)reallocEC(zInterface, NumInterfaces*sizeof(double));
        MPLayer[NumInterfaces]=MP;
         zInterface[NumInterfaces-1]=z;
        Log(" Layer #%i: %s at z=%e.",NumInterfaces,MP->Name,z);
      };
   };
  fclose(f);

  /*--------------------------------------------------------------*/
  /*- sanity check                                               -*/
  /*--------------------------------------------------------------*/
  if (zGP!=HUGE_VAL && zGP>zInterface[NumInterfaces-1])
   { ErrMsg=vstrdup("%s: ground plane must lie below all dielectric layers",FileName);
     return;
   };

  /*--------------------------------------------------------------*/
  /*--------------------------------------------------------------*/
  /*--------------------------------------------------------------*/
  EpsLayer   = (cdouble *)mallocEC((NumInterfaces+1)*sizeof(cdouble));
  MuLayer    = (cdouble *)mallocEC((NumInterfaces+1)*sizeof(cdouble));
  OmegaCache = -1.0;

  qMaxEval  = 10000;
  qAbsTol   = 1.0e-12;
  qRelTol   = 1.0e-6;
  PPIOrder  = 9;
  PhiEOrder = 9;
  char *s;
  if ((s=getenv("SCUFF_SUBSTRATE_QMAXEVAL")))
   sscanf(s,"%i",&qMaxEval);
  if ((s=getenv("SCUFF_SUBSTRATE_QABSTOL")))
   sscanf(s,"%le",&qAbsTol);
  if ((s=getenv("SCUFF_SUBSTRATE_QRELTOL")))
   sscanf(s,"%le",&qRelTol);
  if ((s=getenv("SCUFF_SUBSTRATE_PPIORDER")))
   sscanf(s,"%i",&PPIOrder);
  if ((s=getenv("SCUFF_SUBSTRATE_PHIEORDER")))
   sscanf(s,"%i",&PhiEOrder);
  I1D=0;
  I1DRhoMin=HUGE_VAL;
  I1DRhoMax=0;
  ErrMsg=0;
}

/***************************************************************/
/***************************************************************/
/***************************************************************/
LayeredSubstrate::~LayeredSubstrate()
{
  for(int n=0; n<=NumInterfaces; n++)
   delete MPLayer[n];
  free(MPLayer);
  free(EpsLayer);
  free(MuLayer);
  free(zInterface);
  if (I1D)
   delete I1D;
}

/***************************************************************/
/***************************************************************/
/***************************************************************/
void LayeredSubstrate::UpdateCachedEpsMu(cdouble Omega)
{
  if ( EqualFloat(OmegaCache, Omega) )
   return;
  OmegaCache=Omega;
  for(int n=0; n<=NumInterfaces; n++)
   MPLayer[n]->GetEpsMu(Omega, EpsLayer+n, MuLayer+n);
}

/***************************************************************/
/***************************************************************/
/***************************************************************/
int LayeredSubstrate::GetRegionIndex(double z)
{ for(int ni=0; ni<NumInterfaces; ni++)
   if (z>zInterface[ni]) return ni;
  return NumInterfaces;
}
