#include "ellipticQuad2D.h"

typedef struct{

  iint row;
  iint col;
  iint ownerRank;
  dfloat val;

}nonZero_t;

// compare on global indices 
int parallelCompareRowColumn(const void *a, const void *b){

  nonZero_t *fa = (nonZero_t*) a;
  nonZero_t *fb = (nonZero_t*) b;
  
  if(fa->row < fb->row) return -1;
  if(fa->row > fb->row) return +1;

  if(fa->col < fb->col) return -1;
  if(fa->col > fb->col) return +1;

  return 0;

}


void ellipticCoarsePreconditionerSetupQuad2D(mesh_t *mesh, precon_t *precon, ogs_t *ogs, dfloat lambda, const char *options){

  int size, rank;
  MPI_Comm_size(MPI_COMM_WORLD, &size);
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  
  // ------------------------------------------------------------------------------------
  // 1. Create a contiguous numbering system, starting from the element-vertex connectivity
  iint Nnum = mesh->Nverts*mesh->Nelements;
  
  iint *globalNumbering = (iint*) calloc(Nnum, sizeof(iint));

  iint *globalOwners = (iint*) calloc(Nnum, sizeof(iint));
  iint *globalStarts = (iint*) calloc(size+1, sizeof(iint));
  
  iint *sendCounts  = (iint*) calloc(size, sizeof(iint));
  iint *sendOffsets = (iint*) calloc(size+1, sizeof(iint));
  iint *recvCounts  = (iint*) calloc(size, sizeof(iint));
  iint *recvOffsets = (iint*) calloc(size+1, sizeof(iint));

  iint *sendSortId = (iint *) calloc(Nnum,sizeof(iint));
  iint *globalSortId;
  iint *compressId;

  // use original vertex numbering
  memcpy(globalNumbering, mesh->EToV, Nnum*sizeof(iint));

  // squeeze numbering
  meshParallelConsecutiveGlobalNumbering(Nnum, globalNumbering, globalOwners, globalStarts,
                                         sendSortId, &globalSortId, &compressId,
                                         sendCounts, sendOffsets, recvCounts, recvOffsets);
  
  // build gs
  void *gsh = gsParallelGatherScatterSetup(Nnum, globalNumbering);

  dfloat *degree = (dfloat*) calloc(Nnum, sizeof(dfloat));
  for(iint n=0;n<Nnum;++n)
    degree[n] = 1;
  
  gsParallelGatherScatter(gsh, degree, dfloatString, "add");
  
  dfloat *invDegree = (dfloat*) calloc(Nnum, sizeof(dfloat));
  for(iint n=0;n<Nnum;++n)
    invDegree[n] = 1./degree[n];

  precon->o_coarseInvDegree = mesh->device.malloc(Nnum*sizeof(dfloat), invDegree);

  // clean up
  gsParallelGatherScatterDestroy(gsh);

  // temporary
  precon->o_ztmp = mesh->device.malloc(mesh->Np*mesh->Nelements*sizeof(dfloat));
  
  // ------------------------------------------------------------------------------------
  // 2. Build coarse grid element basis functions

  dfloat *V1  = (dfloat*) calloc(mesh->Np*mesh->Nverts, sizeof(dfloat));
  dfloat *Vr1 = (dfloat*) calloc(mesh->Np*mesh->Nverts, sizeof(dfloat));
  dfloat *Vs1 = (dfloat*) calloc(mesh->Np*mesh->Nverts, sizeof(dfloat));

  for(iint j=0;j<mesh->Nq;++j){
    for(iint i=0;i<mesh->Nq;++i){
      iint n = i+j*mesh->Nq;
      /*
	dfloat rn = mesh->r[n];
	dfloat sn = mesh->s[n];
      */
      dfloat rn = mesh->gllz[i];
      dfloat sn = mesh->gllz[j];
      V1[0*mesh->Np+n] = 0.25*(1-rn)*(1-sn);
      V1[1*mesh->Np+n] = 0.25*(1+rn)*(1-sn);
      V1[2*mesh->Np+n] = 0.25*(1+rn)*(1+sn);
      V1[3*mesh->Np+n] = 0.25*(1-rn)*(1+sn);

      Vr1[0*mesh->Np+n] = 0.25*(-1)*(1-sn);
      Vr1[1*mesh->Np+n] = 0.25*(+1)*(1-sn);
      Vr1[2*mesh->Np+n] = 0.25*(+1)*(1+sn);
      Vr1[3*mesh->Np+n] = 0.25*(-1)*(1+sn);
      
      Vs1[0*mesh->Np+n] = 0.25*(1-rn)*(-1);
      Vs1[1*mesh->Np+n] = 0.25*(1+rn)*(-1);
      Vs1[2*mesh->Np+n] = 0.25*(1+rn)*(+1);
      Vs1[3*mesh->Np+n] = 0.25*(1-rn)*(+1);
    }
  }
  precon->o_V1  = mesh->device.malloc(mesh->Nverts*mesh->Np*sizeof(dfloat), V1);
  precon->o_Vr1 = mesh->device.malloc(mesh->Nverts*mesh->Np*sizeof(dfloat), Vr1);
  precon->o_Vs1 = mesh->device.malloc(mesh->Nverts*mesh->Np*sizeof(dfloat), Vs1);

  // ------------------------------------------------------------------------------------
  // 3. Build non-zeros of stiffness matrix (unassembled)
  iint nnz = mesh->Nverts*mesh->Nverts*mesh->Nelements;
  iint   *rowsA = (iint*) calloc(nnz, sizeof(iint));
  iint   *colsA = (iint*) calloc(nnz, sizeof(iint));
  dfloat *valsA = (dfloat*) calloc(nnz, sizeof(dfloat));

  nonZero_t *sendNonZeros = (nonZero_t*) calloc(nnz, sizeof(nonZero_t));
  iint *AsendCounts  = (iint*) calloc(size, sizeof(iint));
  iint *ArecvCounts  = (iint*) calloc(size, sizeof(iint));
  iint *AsendOffsets = (iint*) calloc(size+1, sizeof(iint));
  iint *ArecvOffsets = (iint*) calloc(size+1, sizeof(iint));
  
  iint cnt = 0;

  printf("Building coarse matrix system\n");
  for(iint e=0;e<mesh->Nelements;++e){
    for(iint n=0;n<mesh->Nverts;++n){
      for(iint m=0;m<mesh->Nverts;++m){
	dfloat Snm = 0;
 
	// use GLL nodes for integration
	// (since Jacobian is high order tensor-product polynomial)
	for(iint j=0;j<mesh->Nq;++j){
	  for(iint i=0;i<mesh->Nq;++i){
	    iint id = i+j*mesh->Nq;
	    
	    dfloat Vr1ni = Vr1[n*mesh->Np+id];
	    dfloat Vs1ni = Vs1[n*mesh->Np+id];
	    dfloat V1ni  = V1[n*mesh->Np+id];
	    
	    dfloat Vr1mi = Vr1[m*mesh->Np+id];
	    dfloat Vs1mi = Vs1[m*mesh->Np+id];
	    dfloat V1mi  = V1[m*mesh->Np+id];

	    dfloat rx = mesh->vgeo[e*mesh->Np*mesh->Nvgeo + id + RXID*mesh->Np];
	    dfloat sx = mesh->vgeo[e*mesh->Np*mesh->Nvgeo + id + SXID*mesh->Np];
	    dfloat ry = mesh->vgeo[e*mesh->Np*mesh->Nvgeo + id + RYID*mesh->Np];
	    dfloat sy = mesh->vgeo[e*mesh->Np*mesh->Nvgeo + id + SYID*mesh->Np];
	    dfloat JW = mesh->vgeo[e*mesh->Np*mesh->Nvgeo + id + JWID*mesh->Np];

	    dfloat Vx1ni = rx*Vr1ni+sx*Vs1ni;
	    dfloat Vy1ni = ry*Vr1ni+sy*Vs1ni;
	    dfloat Vx1mi = rx*Vr1mi+sx*Vs1mi;
	    dfloat Vy1mi = ry*Vr1mi+sy*Vs1mi;
	    
	    Snm += (Vx1ni*Vx1mi+Vy1ni*Vy1mi)*JW;
	    Snm += (lambda*V1ni*V1mi)*JW;
	  }
	}
	//	Snm = (n==m) ? 1: 0;

	valsA[cnt] = Snm;
	rowsA[cnt] = e*mesh->Nverts+n;
	colsA[cnt] = e*mesh->Nverts+m;

	// pack non-zero
	sendNonZeros[cnt].val = Snm;
	sendNonZeros[cnt].row = globalNumbering[e*mesh->Nverts+n];
	sendNonZeros[cnt].col = globalNumbering[e*mesh->Nverts+m];
	sendNonZeros[cnt].ownerRank = globalOwners[e*mesh->Nverts+n];
	
	++cnt;
      }
    }
  }

  // count how many non-zeros to send to each process
  for(iint n=0;n<cnt;++n)
    AsendCounts[sendNonZeros[n].ownerRank] += sizeof(nonZero_t);

  // sort by row ordering
  qsort(sendNonZeros, cnt, sizeof(nonZero_t), parallelCompareRowColumn);
  
  // find how many nodes to expect (should use sparse version)
  MPI_Alltoall(AsendCounts, 1, MPI_IINT, ArecvCounts, 1, MPI_IINT, MPI_COMM_WORLD);

  // find send and recv offsets for gather
  iint recvNtotal = 0;
  for(iint r=0;r<size;++r){
    AsendOffsets[r+1] = AsendOffsets[r] + AsendCounts[r];
    ArecvOffsets[r+1] = ArecvOffsets[r] + ArecvCounts[r];
    recvNtotal += ArecvCounts[r]/sizeof(nonZero_t);
  }

  nonZero_t *recvNonZeros = (nonZero_t*) calloc(recvNtotal, sizeof(nonZero_t));
  
  // determine number to receive
  MPI_Alltoallv(sendNonZeros, AsendCounts, AsendOffsets, MPI_CHAR,
		recvNonZeros, ArecvCounts, ArecvOffsets, MPI_CHAR,
		MPI_COMM_WORLD);

  // sort received non-zero entries by row block (may need to switch compareRowColumn tests)
  qsort(recvNonZeros, recvNtotal, sizeof(nonZero_t), parallelCompareRowColumn);

  // compress duplicates
  cnt = 0;
  for(iint n=1;n<recvNtotal;++n){
    if(recvNonZeros[n].row == recvNonZeros[cnt].row &&
       recvNonZeros[n].col == recvNonZeros[cnt].col){
      recvNonZeros[cnt].val += recvNonZeros[n].val;
    }
    else{
      ++cnt;
      recvNonZeros[cnt] = recvNonZeros[n];
    }
  }
  recvNtotal = cnt+1;

  iint *recvRows = (iint *) calloc(recvNtotal,sizeof(iint));
  iint *recvCols = (iint *) calloc(recvNtotal,sizeof(iint));
  dfloat *recvVals = (dfloat *) calloc(recvNtotal,sizeof(dfloat));
  
  for (iint n=0;n<recvNtotal;n++) {
    recvRows[n] = recvNonZeros[n].row;
    recvCols[n] = recvNonZeros[n].col;
    recvVals[n] = recvNonZeros[n].val;
  }


#if 0
  for(iint r=0;r<size;++r){
    if(r==rank){
      for(iint n=0;n<recvNtotal;++n){
	printf("rank %d non zero %d has row %d, col %d, val %g, own %d\n",
	       rank, n,
	       recvNonZeros[n].row,
	       recvNonZeros[n].col,
	       recvNonZeros[n].val,
	       recvNonZeros[n].ownerRank);
      }
      fflush(stdout);
    }
    MPI_Barrier(MPI_COMM_WORLD);
  }
#endif
  
  if(strstr(options, "XXT")){

    precon->xxt = xxtSetup(Nnum,
			   globalNumbering,
			   nnz,
			   rowsA,
			   colsA,
			   valsA,
			   0,
			   iintString,
			   dfloatString); // 0 if no null space
    
  }

  if(strstr(options, "ALMOND")){

    // TW: need to set up local numbering here (replace globalNumbering with localNumbering)

    // TW: to here.
    
    precon->almond = almondSetup(Nnum, 
				 globalStarts, // TW: need to replace this
				 globalNumbering,
				 recvNtotal,      // TW: number of nonzeros
				 recvRows,        // TW: need to use local numbering
				 recvCols,        // TW: need to use local numbering 
				 recvVals,
				 sendSortId, 
				 globalSortId, 
				 compressId,
				 sendCounts, 
				 sendOffsets, 
				 recvCounts, 
				 recvOffsets,    
				 0,
				 iintString,
				 dfloatString); // 0 if no null space
    
  }
  
  if(strstr(options ,"AMG2013")){
    
    precon->amg = amg2013Setup(Nnum,
			       globalStarts, //global partitioning
			       recvNtotal,                                
			       recvRows,
			       recvCols,
			       recvVals,
			       sendSortId, 
			       globalSortId, 
			       compressId,
			       sendCounts, 
			       sendOffsets, 
			       recvCounts, 
			       recvOffsets,
			       iintString,
			       dfloatString);

  }

  precon->o_r1 = mesh->device.malloc(Nnum*sizeof(dfloat));
  precon->o_z1 = mesh->device.malloc(Nnum*sizeof(dfloat));
  precon->r1 = (dfloat*) malloc(Nnum*sizeof(dfloat));
  precon->z1 = (dfloat*) malloc(Nnum*sizeof(dfloat));
  

  free(AsendCounts);
  free(ArecvCounts);
  free(AsendOffsets);
  free(ArecvOffsets);

  if(strstr(options, "UBERGRID")){
    /* pseudo code for building (really) coarse system */
    iint coarseN = mesh->N;
    iint coarseNp = (coarseN+1)*(coarseN+1);
    iint *globalNumbering2 = (iint*) calloc(size*coarseNp, sizeof(iint)); // hack 
  
    iint Ntotal = mesh->Np*(mesh->Nelements + mesh->totalHaloPairs);
    dfloat *zero = (dfloat*) calloc(Ntotal, sizeof(dfloat));

    precon->coarseNp = coarseNp;
    precon->B    = (dfloat*) calloc(coarseNp*Ntotal, sizeof(dfloat));

    /* populate b here */
    cnt = 0;
    for(iint j=0;j<coarseN+1;++j){
      for(iint i=0;i<coarseN+1;++i){
      
	for(iint m=0;m<mesh->Np*mesh->Nelements;++m){
	  precon->B[cnt*Ntotal+m] = pow(mesh->x[m],i)*pow(mesh->y[m],j); // need to rescale and shift
	}
	++cnt;
      }
    }

    // hack
    for(iint n=0;n<size*coarseNp;++n){
      globalNumbering2[n] = n + 1;
    }

    precon->o_B  = (occa::memory*) calloc(coarseNp, sizeof(occa::memory));
    for(iint n=0;n<coarseNp;++n)
      precon->o_B[n] = mesh->device.malloc(Ntotal*sizeof(dfloat), precon->B+n*Ntotal);

    precon->o_tmp2 = mesh->device.malloc(Ntotal*sizeof(dfloat)); // sloppy
    precon->tmp2 = (dfloat*) calloc(Ntotal,sizeof(dfloat));
  
    // storage for A*b operation
    dfloat *sendBuffer = (dfloat*) calloc(mesh->totalHaloPairs*mesh->Np, sizeof(dfloat));
    dfloat *recvBuffer = (dfloat*) calloc(mesh->totalHaloPairs*mesh->Np, sizeof(dfloat));

    occa::memory o_b = mesh->device.malloc(Ntotal*sizeof(dfloat));
    occa::memory o_gradb = mesh->device.malloc(4*Ntotal*sizeof(dfloat));
    occa::memory o_Ab = mesh->device.malloc(Ntotal*sizeof(dfloat));

    dfloat *Ab = (dfloat*) calloc(Ntotal, sizeof(dfloat));
    dfloat cutoff = (sizeof(dfloat)==4) ? 1e-6:1e-15;

    // maximum fixed size coarseNp x coarseNp from every rank
    iint *rowsA2 = (iint*) calloc(coarseNp*coarseNp*size, sizeof(iint));
    iint *colsA2 = (iint*) calloc(coarseNp*coarseNp*size, sizeof(iint));
    dfloat *valsA2 = (dfloat*) calloc(coarseNp*coarseNp*size, sizeof(dfloat));  

    // assume coarseNp on every process (can generalize to variable number of dofs per process)
    iint nnz2 = 0;
    for(iint r=0;r<size;++r){

      for(iint n=0;n<coarseNp;++n){

	if(r==rank) // each rank takes turn populating b
	  o_b.copyFrom(precon->B+n*Ntotal);
	else
	  o_b.copyFrom(zero);
      
	// need to provide ogs for A*b 
	ellipticOperator2D(mesh, sendBuffer, recvBuffer, ogs, lambda, o_b, o_gradb, o_Ab, options);
      
	o_Ab.copyTo(Ab);
      
	// project onto coarse basis
	for(iint m=0;m<coarseNp;++m){
	  dfloat val = 0;
	  for(iint i=0;i<mesh->Np*mesh->Nelements;++i){
	    val  += precon->B[m*Ntotal+i]*Ab[i];
	  }

	  // only store entries larger than machine precision (dodgy)
	  if(fabs(val)>cutoff){
	    // now by symmetry
	    iint col = r*coarseNp + n;
	    iint row = rank*coarseNp + m; 
	    // save this non-zero
	    rowsA2[nnz2] = row;
	    colsA2[nnz2] = col;
	    valsA2[nnz2] = val;
	    ++nnz2;
	  }
	}
      }
    }
#if 1
    // TW: FOR TESTING CAN USE MPI_Allgather TO COLLECT ALL CHUNKS ON ALL PROCESSES - THEN USE dgesv
    
    char fname[BUFSIZ];
    sprintf(fname, "uberA%05d.dat", rank);
    FILE *fp = fopen(fname, "w");

    fprintf(fp, "%d  # number of local nodes\n", coarseNp);

    for(iint n=0;n<coarseNp;++n){
      fprintf(fp,"%d\n", globalNumbering2[n]);
    }

    fprintf(fp, "%d  # number of non-zeros\n", nnz2);
    for(iint n=0;n<nnz2;++n){
      fprintf(fp, "%d %d %17.15g\n", rowsA2[n], colsA2[n], valsA2[n]);
    }
    fclose(fp);
#endif
    
    // need to create numbering for really coarse grid on each process for xxt
    precon->xxt2 = xxtSetup(coarseNp*size,
			    globalNumbering2,
			    nnz2,
			    rowsA2,
			    colsA2,
			    valsA2,
			    0,
			    iintString,
			    dfloatString);

    // also need to store the b array for prolongation restriction (will require coarseNp vector inner products to compute rhs on each process

  
    free(sendBuffer);
    free(recvBuffer);
    free(Ab);
  
    o_b.free();
    o_gradb.free();
    o_Ab.free();
  }

}