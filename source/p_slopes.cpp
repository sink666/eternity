// Emacs style mode select   -*- C++ -*- 
//-----------------------------------------------------------------------------
//
// Copyright(C) 2013 Stephen McGranahan et al.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see http://www.gnu.org/licenses/
//
//--------------------------------------------------------------------------
//
// DESCRIPTION:
//      Slopes
//      SoM created 05/10/09
//
//-----------------------------------------------------------------------------

#include "z_zone.h"

#include "c_io.h"
#include "doomdef.h"
#include "ev_specials.h"
#include "i_system.h"
#include "m_bbox.h"
#include "p_slopes.h"
#include "p_spec.h"
#include "r_defs.h"
#include "r_state.h"
#include "v_misc.h"

//
// P_MakeSlope
//
// Alocates and fill the contents of a slope structure.
//
static pslope_t *P_MakeSlope(const v3float_t *o, const v2float_t *d, 
                             const float zdelta, bool isceiling)
{
   pslope_t *ret = emalloctag(pslope_t *, sizeof(pslope_t), PU_LEVEL, nullptr);
   memset(ret, 0, sizeof(*ret));

   ret->o.x = M_FloatToFixed(ret->of.x = o->x);
   ret->o.y = M_FloatToFixed(ret->of.y = o->y);
   ret->o.z = M_FloatToFixed(ret->of.z = o->z);

   ret->d.x = M_FloatToFixed(ret->df.x = d->x);
   ret->d.y = M_FloatToFixed(ret->df.y = d->y);

   ret->zdelta = M_FloatToFixed(ret->zdeltaf = zdelta);

   {
      v3float_t v1, v2, v3, d1, d2;
      float len;

      v1.x = o->x;
      v1.y = o->y;
      v1.z = o->z;

      v2.x = v1.x;
      v2.y = v1.y + 10.0f;
      v2.z = P_GetZAtf(ret, v2.x, v2.y);

      v3.x = v1.x + 10.0f;
      v3.y = v1.y;
      v3.z = P_GetZAtf(ret, v3.x, v3.y);

      if(isceiling)
      {
         M_SubVec3f(&d1, &v1, &v3);
         M_SubVec3f(&d2, &v2, &v3);
      }
      else
      {
         M_SubVec3f(&d1, &v1, &v2);
         M_SubVec3f(&d2, &v3, &v2);
      }

      M_CrossProduct3f(&ret->normalf, &d1, &d2);

      len = (float)sqrt(ret->normalf.x * ret->normalf.x +
                        ret->normalf.y * ret->normalf.y + 
                        ret->normalf.z * ret->normalf.z);

      ret->normalf.x /= len;
      ret->normalf.y /= len;
      ret->normalf.z /= len;
   }

   return ret;
}

//
// P_CopySlope
//
// Allocates and returns a copy of the given slope structure.
//
static pslope_t *P_CopySlope(const pslope_t *src)
{
   pslope_t *ret = emalloctag(pslope_t *, sizeof(pslope_t), PU_LEVEL, nullptr);
   memcpy(ret, src, sizeof(*ret));

   return ret;
}

//
// P_MakeLineNormal
//
// Calculates a 2D normal for the given line and stores it in the line
//
void P_MakeLineNormal(line_t *line)
{
   float linedx, linedy, length;

   linedx = line->v2->fx - line->v1->fx;
   linedy = line->v2->fy - line->v1->fy;

   length   = (float)sqrt(linedx * linedx + linedy * linedy);
   line->nx =  linedy / length;
   line->ny = -linedx / length;
}

//
// P_GetExtent
//
// Returns the distance to the first line within the sector that
// is intersected by a line parallel to the plane normal with the point (ox, oy)
//
static float P_GetExtent(sector_t *sector, line_t *line, v3float_t *o, v2float_t *d)
{
   float fardist = -1.0f;
   int i;

   // Poll all the lines and find the vertex that is the furthest away from
   // the slope line.
   for(i = 0; i < sector->linecount; i++)
   {
      line_t *li = sector->lines[i];
      float dist;
      
      // Don't compare to the slope line.
      if(li == line)
         continue;
      
      dist = (float)fabs((li->v1->fx - o->x) * d->x + (li->v1->fy - o->y) * d->y);
      if(dist > fardist)
         fardist = dist;

      dist = (float)fabs((li->v2->fx - o->x) * d->x + (li->v2->fy - o->y) * d->y);
      if(dist > fardist)
         fardist = dist;
   }

   return fardist;
}

//
// P_getSlopeProps
//
// haleyjd 02/05/13: Get slope properties for a static init function.
//
static void P_getSlopeProps(int staticFn, bool &frontfloor, bool &backfloor,
                            bool &frontceil, bool &backceil, const int *args)
{
   struct staticslopeprops_t 
   {
      int  staticFn;
      bool frontfloor;
      bool backfloor;
      bool frontceil;
      bool backceil;
   };
   static staticslopeprops_t props[] =
   {
      { EV_STATIC_SLOPE_FSEC_FLOOR,             true,  false, false, false },
      { EV_STATIC_SLOPE_FSEC_CEILING,           false, false, true,  false },
      { EV_STATIC_SLOPE_FSEC_FLOOR_CEILING,     true,  false, true,  false },
      { EV_STATIC_SLOPE_BSEC_FLOOR,             false, true,  false, false },
      { EV_STATIC_SLOPE_BSEC_CEILING,           false, false, false, true  },
      { EV_STATIC_SLOPE_BSEC_FLOOR_CEILING,     false, true,  false, true  },
      { EV_STATIC_SLOPE_BACKFLOOR_FRONTCEILING, false, true,  true,  false },
      { EV_STATIC_SLOPE_FRONTFLOOR_BACKCEILING, true,  false, false, true  },
   };

   // Handle parameterized slope
   if(staticFn == EV_STATIC_SLOPE_PARAM)
   {
      int floor = args[0];
      if(floor < 0 || floor > 3)
         floor = 0;
      int ceiling = args[1];
      if(ceiling < 0 || ceiling > 3)
         ceiling = 0;
      frontfloor = !!(floor & 1);
      backfloor = !!(floor & 2);
      frontceil = !!(ceiling & 1);
      backceil = !!(ceiling & 2);
      return;
   }

   for(size_t i = 0; i < earrlen(props); i++)
   {
      if(staticFn == props[i].staticFn)
      {
         frontfloor = props[i].frontfloor;
         backfloor  = props[i].backfloor;
         frontceil  = props[i].frontceil;
         backceil   = props[i].backceil;
         break;
      }
   }
}

//
// P_SpawnSlope_Line
//
// Creates one or more slopes based on the given line type and front/back
// sectors.
//
void P_SpawnSlope_Line(int linenum, int staticFn)
{
   line_t *line = lines + linenum;
   v3float_t origin, point;
   v2float_t direction;
   float dz, extent;

   bool frontfloor = false, backfloor = false, 
        frontceil  = false, backceil  = false;

   P_getSlopeProps(staticFn, frontfloor, backfloor, frontceil, backceil,
                   line->args);
   
   // SoM: We don't need the line to retain its special type
   line->special = 0;

   if(!(frontfloor || backfloor || frontceil || backceil) &&
      staticFn != EV_STATIC_SLOPE_PARAM)  // don't scream on trivial Plane_Align
   {
      C_Printf(FC_ERROR "P_SpawnSlope_Line: called with non-slope line special.");
      return;
   }

   if(!line->backsector)
   {
      C_Printf(FC_ERROR "P_SpawnSlope_Line: used on one-sided line %d.", linenum);
      return;
   }

   origin.x = (line->v2->fx + line->v1->fx) * 0.5f;
   origin.y = (line->v2->fy + line->v1->fy) * 0.5f;

   if(frontfloor || frontceil)
   {
      // Do the front sector
      direction.x = line->nx;
      direction.y = line->ny;

      extent = P_GetExtent(line->frontsector, line, &origin, &direction);

      if(extent < 0.0f)
      {
         C_Printf(FC_ERROR "P_SpawnSlope_Line: no frontsector extent for line %d\n", linenum);
         return;
      }

      // reposition the origin according to the extent
      point.x = origin.x + direction.x * extent;
      point.y = origin.y + direction.y * extent;
      direction.x = -direction.x;
      direction.y = -direction.y;

      if(frontfloor)
      {
         point.z = line->frontsector->srf.floor.heightf;
         dz = (line->backsector->srf.floor.heightf - point.z) / extent;

         line->frontsector->srf.floor.slope = P_MakeSlope(&point, &direction, dz, false);
      }
      if(frontceil)
      {
         point.z = line->frontsector->srf.ceiling.heightf;
         dz = (line->backsector->srf.ceiling.heightf - point.z) / extent;

         line->frontsector->srf.ceiling.slope = P_MakeSlope(&point, &direction, dz, true);
      }
   }

   if(backfloor || backceil)
   {
      // Backsector
      direction.x = -line->nx;
      direction.y = -line->ny;

      extent = P_GetExtent(line->backsector, line, &origin, &direction);

      if(extent < 0.0f)
      {
         C_Printf(FC_ERROR "P_SpawnSlope_Line: no backsector extent for line %d\n", linenum);
         return;
      }

      // reposition the origin according to the extent
      point.x = origin.x + direction.x * extent;
      point.y = origin.y + direction.y * extent;
      direction.x = -direction.x;
      direction.y = -direction.y;

      if(backfloor)
      {
         point.z = line->backsector->srf.floor.heightf;
         dz = (line->frontsector->srf.floor.heightf - point.z) / extent;

         line->backsector->srf.floor.slope = P_MakeSlope(&point, &direction, dz, false);
      }
      if(backceil)
      {
         point.z = line->backsector->srf.ceiling.heightf;
         dz = (line->frontsector->srf.ceiling.heightf - point.z) / extent;

         line->backsector->srf.ceiling.slope = P_MakeSlope(&point, &direction, dz, true);
      }
   }

   /*
   // haleyjd: what's this?? inconsequential.
   if(!line->tag)
      return;
   */
}

//
// Copies a slope plane (ceil/floor) from tagged sector to dest
//
static void P_copyPlane(int tag, sector_t *dest, bool copyCeil)
{
   for(int secnum = -1; (secnum = P_FindSectorFromTag(tag, secnum)) != -1; )
   {
      // Deliberately overwrite the plane_align slope
      const sector_t &srcsec = sectors[secnum];
      if(copyCeil && srcsec.srf.ceiling.slope)
      {
         dest->srf.ceiling.slope = P_CopySlope(srcsec.srf.ceiling.slope);
         return;
      }

      if(!copyCeil && srcsec.srf.floor.slope)
      {
         dest->srf.floor.slope = P_CopySlope(srcsec.srf.floor.slope);
         return;
      }
   }
}

//
// Implements Plane_Copy(frontfloortag, frontceiltag, backfloortag, backceiltag, shareslope)
// shareslope copies w/ flags:
// 1: Front floor copied to back,   2: Back floor copied to front
// 4: Front ceil copied to back,    8: Back ceil copied to front
//
// * ExtraData: 493
// * Hexen:     118
//
static void P_copySectorSlopeParam(line_t *line)
{
   // If there's no backsector, only consider args[0] and args[1]
   for(int i = 0; i <= (line->backsector ? 3 : 1); i++)
   {
      if(line->args[i])
      {
         // If i is 2 or 3, backsector is dest, else frontsec is. If i is odd then copy to ceil
         P_copyPlane(line->args[i], i & 2 ? line->backsector : line->frontsector, i & 1);
      }
   }

   if(line->backsector)
   {
      if((line->args[4] & 3) == 1)
         line->backsector->srf.floor.slope = P_CopySlope(line->frontsector->srf.floor.slope);
      else if((line->args[4] & 3) == 2)
         line->frontsector->srf.floor.slope = P_CopySlope(line->backsector->srf.floor.slope);
      else if((line->args[4] & 3) == 3)
      {
         C_Printf(FC_ERROR "P_CopySectorSlopeParam: Plane_Copy[4] flags 1 and 2 are mutually"
            " exclusive.\n");
      }

      if((line->args[4] & 12) == 4)
         line->backsector->srf.ceiling.slope = P_CopySlope(line->frontsector->srf.ceiling.slope);
      else if((line->args[4] & 12) == 8)
         line->frontsector->srf.ceiling.slope = P_CopySlope(line->backsector->srf.ceiling.slope);
      else if((line->args[4] & 12) == 12)
      {
         C_Printf(FC_ERROR "P_CopySectorSlopeParam: Plane_Copy[4] flags 4 and 8 are mutually"
            " exclusive.\n");
      }
   }

   line->special = 0;
}

//
// P_CopySectorSlope
//
// Searches through tagged sectors and copies
//
void P_CopySectorSlope(line_t *line, int staticFn)
{
   sector_t *fsec = line->frontsector;
   int i;
   bool copyFloor   = false;
   bool copyCeiling = false;

   switch(staticFn)
   {
   case EV_STATIC_SLOPE_FRONTFLOOR_TAG:
      copyFloor = true;
      break;
   case EV_STATIC_SLOPE_FRONTCEILING_TAG:
      copyCeiling = true;
      break;
   case EV_STATIC_SLOPE_FRONTFLOORCEILING_TAG:
      copyFloor = copyCeiling = true;
      break;
   case EV_STATIC_SLOPE_PARAM_TAG:
      P_copySectorSlopeParam(line);
      return;
   default:
      I_Error("P_CopySectorSlope: unknown static init %d\n", staticFn);
   }

   // Check for copy linedefs
   for(i = -1; (i = P_FindSectorFromLineArg0(line, i)) >= 0;)
   {
      const sector_t *srcsec = &sectors[i];

      if(copyFloor && !fsec->srf.floor.slope && srcsec->srf.floor.slope)
         fsec->srf.floor.slope = P_CopySlope(srcsec->srf.floor.slope);

      if(copyCeiling && !fsec->srf.ceiling.slope && srcsec->srf.ceiling.slope)
         fsec->srf.ceiling.slope = P_CopySlope(srcsec->srf.ceiling.slope);
   }

   line->special = 0;
}


// ============================================================================
//
// Various utilities related to slopes
//

//
// P_GetZAt
//
// Returns the height of the sloped plane at (x, y) as a fixed_t
//
fixed_t P_GetZAt(pslope_t *slope, fixed_t x, fixed_t y)
{
   fixed_t dist = FixedMul(x - slope->o.x, slope->d.x) +
                  FixedMul(y - slope->o.y, slope->d.y);

   return slope->o.z + FixedMul(dist, slope->zdelta);
}

//
// P_GetZAtf
//
// Returns the height of the sloped plane at (x, y) as a float
//
float P_GetZAtf(pslope_t *slope, float x, float y)
{
   float dist = (x - slope->of.x) * slope->df.x + (y - slope->of.y) * slope->df.y;
   return slope->of.z + (dist * slope->zdeltaf);
}

//
// P_DistFromPlanef
//
float P_DistFromPlanef(const v3float_t *point, const v3float_t *pori, 
                       const v3float_t *pnormal)
{
   return (point->x - pori->x) * pnormal->x + 
          (point->y - pori->y) * pnormal->y +
          (point->z - pori->z) * pnormal->z;
}

// EOF

