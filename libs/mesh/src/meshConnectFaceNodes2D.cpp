/*

The MIT License (MIT)

Copyright (c) 2017 Tim Warburton, Noel Chalmers, Jesse Chan, Ali Karakus

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

*/

#include "mesh.hpp"
#include "mesh2D.hpp"

static int findBestMatch(dfloat x1, dfloat y1,
                   int Np2, int *nodeList, dfloat *x2, dfloat *y2, int *nP){

  int matchIndex = nodeList[0];
  dfloat mindist2 = pow(x1-x2[nodeList[0]],2) + pow(y1-y2[nodeList[0]],2);

  *nP = 0;
  for(int n=1;n<Np2;++n){

    /* next node */
    const int i2 = nodeList[n];

    /* distance between target and next node */
    const dfloat dist2 = pow(x1-x2[i2],2) + pow(y1-y2[i2],2);

    /* if next node is closer to target update match */
    if(dist2<mindist2){
      mindist2 = dist2;
      matchIndex = i2;
      *nP = n;
    }
  }
  if(mindist2>1e-3) {
    stringstream ss;
    ss << "Bad match: x,y = " << x1 << ", " << y1 << "\n";
    LIBP_ABORT(ss.str())
  }
  return matchIndex;
}

// serial face-node to face-node connection
void mesh2D::ConnectFaceNodes(){

  /* volume indices of the interior and exterior face nodes for each element */
  vmapM = (dlong*) calloc(Nfp*Nfaces*Nelements, sizeof(dlong));
  vmapP = (dlong*) calloc(Nfp*Nfaces*Nelements, sizeof(dlong));
  mapP  = (dlong*) calloc(Nfp*Nfaces*Nelements, sizeof(dlong));

  //check if we're connecting a periodic box mesh
  int periodicFlag = 0;
  if (settings.compareSetting("MESH FILE","BOX") &&
      settings.compareSetting("BOX BOUNDARY FLAG","-1"))
    periodicFlag = 1;

  //box dimensions
  dfloat DIMX, DIMY;
  settings.getSetting("BOX DIMX", DIMX);
  settings.getSetting("BOX DIMY", DIMY);

  //box is centered at the origin
  DIMX /= 2.0;
  DIMY /= 2.0;

  /* assume elements already connected */
  for(dlong e=0;e<Nelements;++e){
    for(int f=0;f<Nfaces;++f){
      dlong eP = EToE[e*Nfaces+f];
      int fP = EToF[e*Nfaces+f];
      if(eP<0 || fP<0){ // fake connections for unconnected faces
        eP = e;
        fP = f;
      }

      dfloat offsetX = 0.0;
      dfloat offsetY = 0.0;

      if (periodicFlag) {
        //if the mesh is periodic, this is more complicated.
        // check if this face is on a boundary face
        bool top=true, bottom=true, left=true, right=true;
        for(int n=0;n<NfaceVertices;++n){
          dlong vid = e*Nverts + faceVertices[f*NfaceVertices+n];
          if (fabs(EX[vid]-DIMX)>1e-4) right = false;
          if (fabs(EX[vid]+DIMX)>1e-4) left = false;
          if (fabs(EY[vid]-DIMY)>1e-4) top = false;
          if (fabs(EY[vid]+DIMY)>1e-4) bottom = false;
        }

        if (right)  offsetX = -2.0*DIMX;
        if (left)   offsetX =  2.0*DIMX;
        if (top)    offsetY = -2.0*DIMY;
        if (bottom) offsetY =  2.0*DIMY;
      }

      /* for each node on this face find the neighbor node */
      for(int n=0;n<Nfp;++n){
        dlong idM = faceNodes[f*Nfp+n] + e*Np;
        dfloat xM = x[idM]+offsetX;
        dfloat yM = y[idM]+offsetY;
        dlong  id = Nfaces*Nfp*e + f*Nfp + n;
        int nP;

        int idP = findBestMatch(xM, yM,
                                  Nfp,
                                  faceNodes+fP*Nfp,
                                  x+eP*Np,
                                  y+eP*Np, &nP);

        vmapM[id] = idM;
        vmapP[id] = idP + eP*Np;
        mapP[id] = eP*Nfaces*Nfp + fP*Nfp + nP;

      }
    }
  }
}

//      printf("connecting (%d,%d) to (%d,%d) [ vmapM %d to vmapP %d ]\n",
//             e,f,eP,fP, vmapM[id], vmapP[id]);
