/******************************************************************************
 **	Filename:	cluster.c
 **	Purpose:	Routines for clustering points in N-D space
 **	Author:		Dan Johnson
 **	History:	5/29/89, DSJ, Created.
 **
 **	(c) Copyright Hewlett-Packard Company, 1988.
 ** Licensed under the Apache License, Version 2.0 (the "License");
 ** you may not use this file except in compliance with the License.
 ** You may obtain a copy of the License at
 ** http://www.apache.org/licenses/LICENSE-2.0
 ** Unless required by applicable law or agreed to in writing, software
 ** distributed under the License is distributed on an "AS IS" BASIS,
 ** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 ** See the License for the specific language governing permissions and
 ** limitations under the License.
 ******************************************************************************/
#include "oldheap.h"
#include "const.h"
#include "cluster.h"
#include "emalloc.h"
#include "danerror.h"
#include "freelist.h"
#include <math.h>

/* define the variance which will be used in place of a variance of 0.0
  when all samples in a prototype happen to be identical.  This is simply
  used as an easy way to avoid checking for divide-by-0.  It corresponds
  to a minimum standard deviation of 0.002, or 0.2% of the full scale
  of the parameter ( for parameters whose range is 1.0 ). */
#define MINVARIANCE     0.000004

/* define the absolute minimum number of samples which must be present in
  order to accurately test hypotheses about underlying probability
  distributions.  Define separately the minimum samples that are needed
  before a statistical analysis is attempted; this number should be
  equal to MINSAMPLES but can be set to a lower number for early testing
  when very few samples are available. */
#define MINBUCKETS      5
#define MINSAMPLESPERBUCKET 5
#define MINSAMPLES    (MINBUCKETS * MINSAMPLESPERBUCKET)
#define MINSAMPLESNEEDED  1

/* define the size of the table which maps normalized samples to
  histogram buckets.  Also define the number of standard deviations
  in a normal distribution which are considered to be significant.
  The mapping table will be defined in such a way that it covers
  the specified number of standard deviations on either side of
  the mean.  BUCKETTABLESIZE should always be even. */
#define BUCKETTABLESIZE   1024
#define NORMALEXTENT    3.0

typedef struct
{
  CLUSTER *Cluster;
  CLUSTER *Neighbor;
}


TEMPCLUSTER;

typedef struct
{
  FLOAT32 AvgVariance;
  FLOAT32 *CoVariance;
  FLOAT32 *Min;                  // largest negative distance from the mean
  FLOAT32 *Max;                  // largest positive distance from the mean
}


STATISTICS;

typedef struct
{
  DISTRIBUTION Distribution;     // distribution being tested for
  UINT32 SampleCount;            // # of samples in histogram
  FLOAT64 Confidence;            // confidence level of test
  FLOAT64 ChiSquared;            // test threshold
  UINT16 NumberOfBuckets;        // number of cells in histogram
  UINT16 Bucket[BUCKETTABLESIZE];// mapping to histogram buckets
  UINT32 *Count;                 // frequency of occurence histogram
  FLOAT32 *ExpectedCount;        // expected histogram
}


BUCKETS;

typedef struct
{
  UINT16 DegreesOfFreedom;
  FLOAT64 Alpha;
  FLOAT64 ChiSquared;
}


CHISTRUCT;

typedef FLOAT64 (*DENSITYFUNC) (INT32);
typedef FLOAT64 (*SOLVEFUNC) (CHISTRUCT *, double);

#define Odd(N) ((N)%2)
#define Mirror(N,R) ((R) - (N) - 1)
#define Abs(N) ( ( (N) < 0 ) ? ( -(N) ) : (N) )

//--------------Global Data Definitions and Declarations----------------------
/* the following variables are declared as global so that routines which
are called from the kd-tree walker can get to them. */
static HEAP *Heap;
static TEMPCLUSTER *TempCluster;
static KDTREE *Tree;
static INT32 CurrentTemp;

/* the following variables describe a discrete normal distribution
  which is used by NormalDensity() and NormalBucket().  The
  constant NORMALEXTENT determines how many standard
  deviations of the distribution are mapped onto the fixed
  discrete range of x.  x=0 is mapped to -NORMALEXTENT standard
  deviations and x=BUCKETTABLESIZE is mapped to
  +NORMALEXTENT standard deviations. */
#define SqrtOf2Pi     2.506628275
static FLOAT64 NormalStdDev = BUCKETTABLESIZE / (2.0 * NORMALEXTENT);
static FLOAT64 NormalVariance =
(BUCKETTABLESIZE * BUCKETTABLESIZE) / (4.0 * NORMALEXTENT * NORMALEXTENT);
static FLOAT64 NormalMagnitude =
(2.0 * NORMALEXTENT) / (SqrtOf2Pi * BUCKETTABLESIZE);
static FLOAT64 NormalMean = BUCKETTABLESIZE / 2;

// keep a list of histogram buckets to minimize recomputing them
static LIST OldBuckets[] = { NIL, NIL, NIL };

/* define lookup tables used to compute the number of histogram buckets
  that should be used for a given number of samples. */
#define LOOKUPTABLESIZE   8
#define MAXBUCKETS      39
#define MAXDEGREESOFFREEDOM MAXBUCKETS

static UINT32 CountTable[LOOKUPTABLESIZE] = {
  MINSAMPLES, 200, 400, 600, 800, 1000, 1500, 2000
};
static UINT16 BucketsTable[LOOKUPTABLESIZE] = {
  MINBUCKETS, 16, 20, 24, 27, 30, 35, MAXBUCKETS
};

/*-------------------------------------------------------------------------
          Private Function Prototypes
--------------------------------------------------------------------------*/
void CreateClusterTree(CLUSTERER *Clusterer); 

void MakePotentialClusters(CLUSTER *Cluster, VISIT Order, INT32 Level); 

CLUSTER *FindNearestNeighbor(KDTREE *Tree,
                             CLUSTER *Cluster,
                             FLOAT32 *Distance);

CLUSTER *MakeNewCluster(CLUSTERER *Clusterer, TEMPCLUSTER *TempCluster); 

INT32 MergeClusters (INT16 N,
register PARAM_DESC ParamDesc[],
register INT32 n1,
register INT32 n2,
register FLOAT32 m[],
register FLOAT32 m1[], register FLOAT32 m2[]);

void ComputePrototypes(CLUSTERER *Clusterer, CLUSTERCONFIG *Config); 

PROTOTYPE *MakePrototype(CLUSTERER *Clusterer,
                         CLUSTERCONFIG *Config,
                         CLUSTER *Cluster);

PROTOTYPE *MakeDegenerateProto(UINT16 N,
                               CLUSTER *Cluster,
                               STATISTICS *Statistics,
                               PROTOSTYLE Style,
                               INT32 MinSamples);

PROTOTYPE *MakeSphericalProto(CLUSTERER *Clusterer,
                              CLUSTER *Cluster,
                              STATISTICS *Statistics,
                              BUCKETS *Buckets);

PROTOTYPE *MakeEllipticalProto(CLUSTERER *Clusterer,
                               CLUSTER *Cluster,
                               STATISTICS *Statistics,
                               BUCKETS *Buckets);

PROTOTYPE *MakeMixedProto(CLUSTERER *Clusterer,
                          CLUSTER *Cluster,
                          STATISTICS *Statistics,
                          BUCKETS *NormalBuckets,
                          FLOAT64 Confidence);

void MakeDimRandom(UINT16 i, PROTOTYPE *Proto, PARAM_DESC *ParamDesc); 

void MakeDimUniform(UINT16 i, PROTOTYPE *Proto, STATISTICS *Statistics); 

STATISTICS *ComputeStatistics (INT16 N,
PARAM_DESC ParamDesc[], CLUSTER * Cluster);

PROTOTYPE *NewSphericalProto(UINT16 N,
                             CLUSTER *Cluster,
                             STATISTICS *Statistics);

PROTOTYPE *NewEllipticalProto(INT16 N,
                              CLUSTER *Cluster,
                              STATISTICS *Statistics);

PROTOTYPE *NewMixedProto(INT16 N, CLUSTER *Cluster, STATISTICS *Statistics); 

PROTOTYPE *NewSimpleProto(INT16 N, CLUSTER *Cluster); 

BOOL8 Independent (PARAM_DESC ParamDesc[],
INT16 N, FLOAT32 * CoVariance, FLOAT32 Independence);

BUCKETS *GetBuckets(DISTRIBUTION Distribution,
                    UINT32 SampleCount,
                    FLOAT64 Confidence);

BUCKETS *MakeBuckets(DISTRIBUTION Distribution,
                     UINT32 SampleCount,
                     FLOAT64 Confidence);

UINT16 OptimumNumberOfBuckets(UINT32 SampleCount); 

FLOAT64 ComputeChiSquared(UINT16 DegreesOfFreedom, FLOAT64 Alpha); 

FLOAT64 NormalDensity(INT32 x); 

FLOAT64 UniformDensity(INT32 x); 

FLOAT64 Integral(FLOAT64 f1, FLOAT64 f2, FLOAT64 Dx); 

void FillBuckets(BUCKETS *Buckets,
                 CLUSTER *Cluster,
                 UINT16 Dim,
                 PARAM_DESC *ParamDesc,
                 FLOAT32 Mean,
                 FLOAT32 StdDev);

UINT16 NormalBucket(PARAM_DESC *ParamDesc,
                    FLOAT32 x,
                    FLOAT32 Mean,
                    FLOAT32 StdDev);

UINT16 UniformBucket(PARAM_DESC *ParamDesc,
                     FLOAT32 x,
                     FLOAT32 Mean,
                     FLOAT32 StdDev);

BOOL8 DistributionOK(BUCKETS *Buckets); 

void FreeStatistics(STATISTICS *Statistics); 

void FreeBuckets(BUCKETS *Buckets); 

void FreeCluster(CLUSTER *Cluster); 

UINT16 DegreesOfFreedom(DISTRIBUTION Distribution, UINT16 HistogramBuckets); 

int NumBucketsMatch(void *arg1,   //BUCKETS                                       *Histogram,
                    void *arg2);  //UINT16                        *DesiredNumberOfBuckets);

int ListEntryMatch(void *arg1, void *arg2); 

void AdjustBuckets(BUCKETS *Buckets, UINT32 NewSampleCount); 

void InitBuckets(BUCKETS *Buckets); 

int AlphaMatch(void *arg1,   //CHISTRUCT                             *ChiStruct,
               void *arg2);  //CHISTRUCT                             *SearchKey);

CHISTRUCT *NewChiStruct(UINT16 DegreesOfFreedom, FLOAT64 Alpha); 

FLOAT64 Solve(SOLVEFUNC Function,
              void *FunctionParams,
              FLOAT64 InitialGuess,
              FLOAT64 Accuracy);

FLOAT64 ChiArea(CHISTRUCT *ChiParams, FLOAT64 x); 

BOOL8 MultipleCharSamples(CLUSTERER *Clusterer,
                          CLUSTER *Cluster,
                          FLOAT32 MaxIllegal);

//--------------------------Public Code--------------------------------------
/** MakeClusterer **********************************************************
Parameters:	SampleSize	number of dimensions in feature space
      ParamDesc	description of each dimension
Globals:	None
Operation:	This routine creates a new clusterer data structure,
      initializes it, and returns a pointer to it.
Return:		pointer to the new clusterer data structure
Exceptions:	None
History:	5/29/89, DSJ, Created.
****************************************************************************/
CLUSTERER *
MakeClusterer (INT16 SampleSize, PARAM_DESC ParamDesc[]) {
  CLUSTERER *Clusterer;
  int i;

  // allocate main clusterer data structure and init simple fields
  Clusterer = (CLUSTERER *) Emalloc (sizeof (CLUSTERER));
  Clusterer->SampleSize = SampleSize;
  Clusterer->NumberOfSamples = 0;
  Clusterer->NumChar = 0;

  // init fields which will not be used initially
  Clusterer->Root = NULL;
  Clusterer->ProtoList = NIL;

  // maintain a copy of param descriptors in the clusterer data structure
  Clusterer->ParamDesc =
    (PARAM_DESC *) Emalloc (SampleSize * sizeof (PARAM_DESC));
  for (i = 0; i < SampleSize; i++) {
    Clusterer->ParamDesc[i].Circular = ParamDesc[i].Circular;
    Clusterer->ParamDesc[i].NonEssential = ParamDesc[i].NonEssential;
    Clusterer->ParamDesc[i].Min = ParamDesc[i].Min;
    Clusterer->ParamDesc[i].Max = ParamDesc[i].Max;
    Clusterer->ParamDesc[i].Range = ParamDesc[i].Max - ParamDesc[i].Min;
    Clusterer->ParamDesc[i].HalfRange = Clusterer->ParamDesc[i].Range / 2;
    Clusterer->ParamDesc[i].MidRange =
      (ParamDesc[i].Max + ParamDesc[i].Min) / 2;
  }

  // allocate a kd tree to hold the samples
  Clusterer->KDTree = MakeKDTree (SampleSize, ParamDesc);

  // execute hook for monitoring clustering operation
  // (*ClustererCreationHook)( Clusterer );

  return (Clusterer);
}                                // MakeClusterer


/** MakeSample ***********************************************************
Parameters:	Clusterer	clusterer data structure to add sample to
      Feature		feature to be added to clusterer
      CharID		unique ident. of char that sample came from
Globals:	None
Operation:	This routine creates a new sample data structure to hold
      the specified feature.  This sample is added to the clusterer
      data structure (so that it knows which samples are to be
      clustered later), and a pointer to the sample is returned to
      the caller.
Return:		Pointer to the new sample data structure
Exceptions:	ALREADYCLUSTERED	MakeSample can't be called after
      ClusterSamples has been called
History:	5/29/89, DSJ, Created.
*****************************************************************************/
SAMPLE *
MakeSample (CLUSTERER * Clusterer, FLOAT32 Feature[], INT32 CharID) {
  SAMPLE *Sample;
  int i;

  // see if the samples have already been clustered - if so trap an error
  if (Clusterer->Root != NULL)
    DoError (ALREADYCLUSTERED,
      "Can't add samples after they have been clustered");

  // allocate the new sample and initialize it
  Sample = (SAMPLE *) Emalloc (sizeof (SAMPLE) +
    (Clusterer->SampleSize -
    1) * sizeof (FLOAT32));
  Sample->Clustered = FALSE;
  Sample->Prototype = FALSE;
  Sample->SampleCount = 1;
  Sample->Left = NULL;
  Sample->Right = NULL;
  Sample->CharID = CharID;

  for (i = 0; i < Clusterer->SampleSize; i++)
    Sample->Mean[i] = Feature[i];

  // add the sample to the KD tree - keep track of the total # of samples
  Clusterer->NumberOfSamples++;
  KDStore (Clusterer->KDTree, Sample->Mean, (char *) Sample);
  if (CharID >= Clusterer->NumChar)
    Clusterer->NumChar = CharID + 1;

  // execute hook for monitoring clustering operation
  // (*SampleCreationHook)( Sample );

  return (Sample);
}                                // MakeSample


/** ClusterSamples ***********************************************************
Parameters:	Clusterer	data struct containing samples to be clustered
      Config		parameters which control clustering process
Globals:	None
Operation:	This routine first checks to see if the samples in this
      clusterer have already been clustered before; if so, it does
      not bother to recreate the cluster tree.  It simply recomputes
      the prototypes based on the new Config info.
        If the samples have not been clustered before, the
      samples in the KD tree are formed into a cluster tree and then
      the prototypes are computed from the cluster tree.
        In either case this routine returns a pointer to a
      list of prototypes that best represent the samples given
      the constraints specified in Config.
Return:		Pointer to a list of prototypes
Exceptions:	None
History:	5/29/89, DSJ, Created.
*******************************************************************************/
LIST ClusterSamples(CLUSTERER *Clusterer, CLUSTERCONFIG *Config) { 
  //only create cluster tree if samples have never been clustered before
  if (Clusterer->Root == NULL)
    CreateClusterTree(Clusterer); 

  //deallocate the old prototype list if one exists
  FreeProtoList (&Clusterer->ProtoList);
  Clusterer->ProtoList = NIL;

  //compute prototypes starting at the root node in the tree
  ComputePrototypes(Clusterer, Config); 
  return (Clusterer->ProtoList);
}                                // ClusterSamples


/** FreeClusterer *************************************************************
Parameters:	Clusterer	pointer to data structure to be freed
Globals:	None
Operation:	This routine frees all of the memory allocated to the
      specified data structure.  It will not, however, free
      the memory used by the prototype list.  The pointers to
      the clusters for each prototype in the list will be set
      to NULL to indicate that the cluster data structures no
      longer exist.  Any sample lists that have been obtained
      via calls to GetSamples are no longer valid.
Return:		None
Exceptions:	None
History:	6/6/89, DSJ, Created.
*******************************************************************************/
void FreeClusterer(CLUSTERER *Clusterer) { 
  if (Clusterer != NULL) {
    memfree (Clusterer->ParamDesc);
    if (Clusterer->KDTree != NULL)
      FreeKDTree (Clusterer->KDTree);
    if (Clusterer->Root != NULL)
      FreeCluster (Clusterer->Root);
    iterate (Clusterer->ProtoList) {
      ((PROTOTYPE *) (first (Clusterer->ProtoList)))->Cluster = NULL;
    }
    memfree(Clusterer); 
  }
}                                // FreeClusterer


/** FreeProtoList ************************************************************
Parameters:	ProtoList	pointer to list of prototypes to be freed
Globals:	None
Operation:	This routine frees all of the memory allocated to the
      specified list of prototypes.  The clusters which are
      pointed to by the prototypes are not freed.
Return:		None
Exceptions:	None
History:	6/6/89, DSJ, Created.
*****************************************************************************/
void FreeProtoList(LIST *ProtoList) { 
  destroy_nodes(*ProtoList, FreePrototype); 
}                                // FreeProtoList


/** FreePrototype ************************************************************
Parameters:	Prototype	prototype data structure to be deallocated
Globals:	None
Operation:	This routine deallocates the memory consumed by the specified
      prototype and modifies the corresponding cluster so that it
      is no longer marked as a prototype.  The cluster is NOT
      deallocated by this routine.
Return:		None
Exceptions:	None
History:	5/30/89, DSJ, Created.
*******************************************************************************/
void FreePrototype(void *arg) {  //PROTOTYPE     *Prototype)
  PROTOTYPE *Prototype = (PROTOTYPE *) arg;

  // unmark the corresponding cluster (if there is one
  if (Prototype->Cluster != NULL)
    Prototype->Cluster->Prototype = FALSE;

  // deallocate the prototype statistics and then the prototype itself
  if (Prototype->Distrib != NULL)
    memfree (Prototype->Distrib);
  if (Prototype->Mean != NULL)
    memfree (Prototype->Mean);
  if (Prototype->Style != spherical) {
    if (Prototype->Variance.Elliptical != NULL)
      memfree (Prototype->Variance.Elliptical);
    if (Prototype->Magnitude.Elliptical != NULL)
      memfree (Prototype->Magnitude.Elliptical);
    if (Prototype->Weight.Elliptical != NULL)
      memfree (Prototype->Weight.Elliptical);
  }
  memfree(Prototype); 
}                                // FreePrototype


/** NextSample ************************************************************
Parameters:	SearchState	ptr to list containing clusters to be searched
Globals:	None
Operation:	This routine is used to find all of the samples which
      belong to a cluster.  It starts by removing the top
      cluster on the cluster list (SearchState).  If this cluster is
      a leaf it is returned.  Otherwise, the right subcluster
      is pushed on the list and we continue the search in the
      left subcluster.  This continues until a leaf is found.
      If all samples have been found, NULL is returned.
      InitSampleSearch() must be called
      before NextSample() to initialize the search.
Return:		Pointer to the next leaf cluster (sample) or NULL.
Exceptions:	None
History:	6/16/89, DSJ, Created.
****************************************************************************/
CLUSTER *NextSample(LIST *SearchState) { 
  CLUSTER *Cluster;

  if (*SearchState == NIL)
    return (NULL);
  Cluster = (CLUSTER *) first (*SearchState);
  *SearchState = pop (*SearchState);
  while (TRUE) {
    if (Cluster->Left == NULL)
      return (Cluster);
    *SearchState = push (*SearchState, Cluster->Right);
    Cluster = Cluster->Left;
  }
}                                // NextSample


/** Mean ***********************************************************
Parameters:	Proto		prototype to return mean of
      Dimension	dimension whose mean is to be returned
Globals:	none
Operation:	This routine returns the mean of the specified
      prototype in the indicated dimension.
Return:		Mean of Prototype in Dimension
Exceptions: none
History:	7/6/89, DSJ, Created.
*********************************************************************/
FLOAT32 Mean(PROTOTYPE *Proto, UINT16 Dimension) { 
  return (Proto->Mean[Dimension]);
}                                // Mean


/** StandardDeviation *************************************************
Parameters:	Proto		prototype to return standard deviation of
      Dimension	dimension whose stddev is to be returned
Globals:	none
Operation:	This routine returns the standard deviation of the
      prototype in the indicated dimension.
Return:		Standard deviation of Prototype in Dimension
Exceptions: none
History:	7/6/89, DSJ, Created.
**********************************************************************/
FLOAT32 StandardDeviation(PROTOTYPE *Proto, UINT16 Dimension) { 
  switch (Proto->Style) {
    case spherical:
      return ((FLOAT32) sqrt ((double) Proto->Variance.Spherical));
    case elliptical:
      return ((FLOAT32)
        sqrt ((double) Proto->Variance.Elliptical[Dimension]));
    case mixed:
      switch (Proto->Distrib[Dimension]) {
        case normal:
          return ((FLOAT32)
            sqrt ((double) Proto->Variance.Elliptical[Dimension]));
        case uniform:
        case D_random:
          return (Proto->Variance.Elliptical[Dimension]);
      }
  }
  return 0.0f;
}                                // StandardDeviation


/*---------------------------------------------------------------------------
            Private Code
----------------------------------------------------------------------------*/
/** CreateClusterTree *******************************************************
Parameters:	Clusterer	data structure holdings samples to be clustered
Globals:	Tree		kd-tree holding samples
      TempCluster	array of temporary clusters
      CurrentTemp	index of next temp cluster to be used
      Heap		heap used to hold temp clusters - "best" on top
Operation:	This routine performs a bottoms-up clustering on the samples
      held in the kd-tree of the Clusterer data structure.  The
      result is a cluster tree.  Each node in the tree represents
      a cluster which conceptually contains a subset of the samples.
      More precisely, the cluster contains all of the samples which
      are contained in its two sub-clusters.  The leaves of the
      tree are the individual samples themselves; they have no
      sub-clusters.  The root node of the tree conceptually contains
      all of the samples.
Return:		None (the Clusterer data structure is changed)
Exceptions:	None
History:	5/29/89, DSJ, Created.
******************************************************************************/
void CreateClusterTree(CLUSTERER *Clusterer) { 
  HEAPENTRY HeapEntry;
  TEMPCLUSTER *PotentialCluster;

  // save the kd-tree in a global variable so kd-tree walker can get at it
  Tree = Clusterer->KDTree;

  // allocate memory to to hold all of the "potential" clusters
  TempCluster = (TEMPCLUSTER *)
    Emalloc (Clusterer->NumberOfSamples * sizeof (TEMPCLUSTER));
  CurrentTemp = 0;

  // each sample and its nearest neighbor form a "potential" cluster
  // save these in a heap with the "best" potential clusters on top
  Heap = MakeHeap (Clusterer->NumberOfSamples);
  KDWalk (Tree, (void_proc) MakePotentialClusters);

  // form potential clusters into actual clusters - always do "best" first
  while (GetTopOfHeap (Heap, &HeapEntry) != EMPTY) {
    PotentialCluster = (TEMPCLUSTER *) (HeapEntry.Data);

    // if main cluster of potential cluster is already in another cluster
    // then we don't need to worry about it
    if (PotentialCluster->Cluster->Clustered) {
      continue;
    }

    // if main cluster is not yet clustered, but its nearest neighbor is
    // then we must find a new nearest neighbor
    else if (PotentialCluster->Neighbor->Clustered) {
      PotentialCluster->Neighbor =
        FindNearestNeighbor (Tree, PotentialCluster->Cluster,
        &(HeapEntry.Key));
      if (PotentialCluster->Neighbor != NULL) {
        HeapStore(Heap, &HeapEntry); 
      }
    }

    // if neither cluster is already clustered, form permanent cluster
    else {
      PotentialCluster->Cluster =
        MakeNewCluster(Clusterer, PotentialCluster); 
      PotentialCluster->Neighbor =
        FindNearestNeighbor (Tree, PotentialCluster->Cluster,
        &(HeapEntry.Key));
      if (PotentialCluster->Neighbor != NULL) {
        HeapStore(Heap, &HeapEntry); 
      }
    }
  }

  // the root node in the cluster tree is now the only node in the kd-tree
  Clusterer->Root = (CLUSTER *) RootOf (Clusterer->KDTree);

  // free up the memory used by the K-D tree, heap, and temp clusters
  FreeKDTree(Tree); 
  Clusterer->KDTree = NULL;
  FreeHeap(Heap); 
  memfree(TempCluster); 
}                                // CreateClusterTree


/** MakePotentialClusters **************************************************
Parameters:	Cluster	current cluster being visited in kd-tree walk
      Order	order in which cluster is being visited
      Level	level of this cluster in the kd-tree
Globals:	Tree		kd-tree to be searched for neighbors
      TempCluster	array of temporary clusters
      CurrentTemp	index of next temp cluster to be used
      Heap		heap used to hold temp clusters - "best" on top
Operation:	This routine is designed to be used in concert with the
      KDWalk routine.  It will create a potential cluster for
      each sample in the kd-tree that is being walked.  This
      potential cluster will then be pushed on the heap.
Return:		none
Exceptions: none
History:	5/29/89, DSJ, Created.
      7/13/89, DSJ, Removed visibility of kd-tree node data struct.
******************************************************************************/
void MakePotentialClusters(CLUSTER *Cluster, VISIT Order, INT32 Level) { 
  HEAPENTRY HeapEntry;

  if ((Order == preorder) || (Order == leaf)) {
    TempCluster[CurrentTemp].Cluster = Cluster;
    HeapEntry.Data = (char *) &(TempCluster[CurrentTemp]);
    TempCluster[CurrentTemp].Neighbor =
      FindNearestNeighbor (Tree, TempCluster[CurrentTemp].Cluster,
      &(HeapEntry.Key));
    if (TempCluster[CurrentTemp].Neighbor != NULL) {
      HeapStore(Heap, &HeapEntry); 
      CurrentTemp++;
    }
  }
}                                // MakePotentialClusters


/** FindNearestNeighbor *********************************************************
Parameters:	Tree		kd-tree to search in for nearest neighbor
      Cluster		cluster whose nearest neighbor is to be found
      Distance	ptr to variable to report distance found
Globals:	none
Operation:	This routine searches the specified kd-tree for the nearest
      neighbor of the specified cluster.  It actually uses the
      kd routines to find the 2 nearest neighbors since one of them
      will be the original cluster.  A pointer to the nearest
      neighbor is returned, if it can be found, otherwise NULL is
      returned.  The distance between the 2 nodes is placed
      in the specified variable.
Return:		Pointer to the nearest neighbor of Cluster, or NULL
Exceptions: none
History:	5/29/89, DSJ, Created.
      7/13/89, DSJ, Removed visibility of kd-tree node data struct
********************************************************************************/
CLUSTER *
FindNearestNeighbor (KDTREE * Tree, CLUSTER * Cluster, FLOAT32 * Distance)
#define MAXNEIGHBORS  2
#define MAXDISTANCE   MAX_FLOAT32
{
  CLUSTER *Neighbor[MAXNEIGHBORS];
  FLOAT32 Dist[MAXNEIGHBORS];
  INT32 NumberOfNeighbors;
  INT32 i;
  CLUSTER *BestNeighbor;

  // find the 2 nearest neighbors of the cluster
  NumberOfNeighbors = KDNearestNeighborSearch
    (Tree, Cluster->Mean, MAXNEIGHBORS, MAXDISTANCE, Neighbor, Dist);

  // search for the nearest neighbor that is not the cluster itself
  *Distance = MAXDISTANCE;
  BestNeighbor = NULL;
  for (i = 0; i < NumberOfNeighbors; i++) {
    if ((Dist[i] < *Distance) && (Neighbor[i] != Cluster)) {
      *Distance = Dist[i];
      BestNeighbor = Neighbor[i];
    }
  }
  return (BestNeighbor);
}                                // FindNearestNeighbor


/** MakeNewCluster *************************************************************
Parameters:	Clusterer	current clustering environment
      TempCluster	potential cluster to make permanent
Globals:	none
Operation:	This routine creates a new permanent cluster from the
      clusters specified in TempCluster.  The 2 clusters in
      TempCluster are marked as "clustered" and deleted from
      the kd-tree.  The new cluster is then added to the kd-tree.
      Return: Pointer to the new permanent cluster
Exceptions:	none
History:	5/29/89, DSJ, Created.
      7/13/89, DSJ, Removed visibility of kd-tree node data struct
********************************************************************************/
CLUSTER *MakeNewCluster(CLUSTERER *Clusterer, TEMPCLUSTER *TempCluster) { 
  CLUSTER *Cluster;

  // allocate the new cluster and initialize it
  Cluster = (CLUSTER *) Emalloc (sizeof (CLUSTER) +
    (Clusterer->SampleSize -
    1) * sizeof (FLOAT32));
  Cluster->Clustered = FALSE;
  Cluster->Prototype = FALSE;
  Cluster->Left = TempCluster->Cluster;
  Cluster->Right = TempCluster->Neighbor;
  Cluster->CharID = -1;

  // mark the old clusters as "clustered" and delete them from the kd-tree
  Cluster->Left->Clustered = TRUE;
  Cluster->Right->Clustered = TRUE;
  KDDelete (Clusterer->KDTree, Cluster->Left->Mean, Cluster->Left);
  KDDelete (Clusterer->KDTree, Cluster->Right->Mean, Cluster->Right);

  // compute the mean and sample count for the new cluster
  Cluster->SampleCount =
    MergeClusters (Clusterer->SampleSize, Clusterer->ParamDesc,
    Cluster->Left->SampleCount, Cluster->Right->SampleCount,
    Cluster->Mean, Cluster->Left->Mean, Cluster->Right->Mean);

  // add the new cluster to the KD tree
  KDStore (Clusterer->KDTree, Cluster->Mean, Cluster);
  return (Cluster);
}                                // MakeNewCluster


/** MergeClusters ************************************************************
Parameters:	N	# of dimensions (size of arrays)
      ParamDesc	array of dimension descriptions
      n1, n2	number of samples in each old cluster
      m	array to hold mean of new cluster
      m1, m2	arrays containing means of old clusters
Globals:	None
Operation:	This routine merges two clusters into one larger cluster.
      To do this it computes the number of samples in the new
      cluster and the mean of the new cluster.  The ParamDesc
      information is used to ensure that circular dimensions
      are handled correctly.
Return:		The number of samples in the new cluster.
Exceptions:	None
History:	5/31/89, DSJ, Created.
*********************************************************************************/
INT32
MergeClusters (INT16 N,
register PARAM_DESC ParamDesc[],
register INT32 n1,
register INT32 n2,
register FLOAT32 m[],
register FLOAT32 m1[], register FLOAT32 m2[]) {
  register INT32 i, n;

  n = n1 + n2;
  for (i = N; i > 0; i--, ParamDesc++, m++, m1++, m2++) {
    if (ParamDesc->Circular) {
      // if distance between means is greater than allowed
      // reduce upper point by one "rotation" to compute mean
      // then normalize the mean back into the accepted range
      if ((*m2 - *m1) > ParamDesc->HalfRange) {
        *m = (n1 * *m1 + n2 * (*m2 - ParamDesc->Range)) / n;
        if (*m < ParamDesc->Min)
          *m += ParamDesc->Range;
      }
      else if ((*m1 - *m2) > ParamDesc->HalfRange) {
        *m = (n1 * (*m1 - ParamDesc->Range) + n2 * *m2) / n;
        if (*m < ParamDesc->Min)
          *m += ParamDesc->Range;
      }
      else
        *m = (n1 * *m1 + n2 * *m2) / n;
    }
    else
      *m = (n1 * *m1 + n2 * *m2) / n;
  }
  return (n);
}                                // MergeClusters


/** ComputePrototypes *******************************************************
Parameters:	Clusterer	data structure holding cluster tree
      Config		parameters used to control prototype generation
Globals:	None
Operation:	This routine decides which clusters in the cluster tree
      should be represented by prototypes, forms a list of these
      prototypes, and places the list in the Clusterer data
      structure.
Return:		None
Exceptions:	None
History:	5/30/89, DSJ, Created.
*******************************************************************************/
void ComputePrototypes(CLUSTERER *Clusterer, CLUSTERCONFIG *Config) { 
  LIST ClusterStack = NIL;
  CLUSTER *Cluster;
  PROTOTYPE *Prototype;

  // use a stack to keep track of clusters waiting to be processed
  // initially the only cluster on the stack is the root cluster
  if (Clusterer->Root != NULL)
    ClusterStack = push (NIL, Clusterer->Root);

  // loop until we have analyzed all clusters which are potential prototypes
  while (ClusterStack != NIL) {
    // remove the next cluster to be analyzed from the stack
    // try to make a prototype from the cluster
    // if successful, put it on the proto list, else split the cluster
    Cluster = (CLUSTER *) first (ClusterStack);
    ClusterStack = pop (ClusterStack);
    Prototype = MakePrototype (Clusterer, Config, Cluster);
    if (Prototype != NULL) {
      Clusterer->ProtoList = push (Clusterer->ProtoList, Prototype);
    }
    else {
      ClusterStack = push (ClusterStack, Cluster->Right);
      ClusterStack = push (ClusterStack, Cluster->Left);
    }
  }
}                                // ComputePrototypes


/** MakePrototype ***********************************************************
Parameters:	Clusterer	data structure holding cluster tree
      Config		parameters used to control prototype generation
      Cluster		cluster to be made into a prototype
Globals:	None
Operation:	This routine attempts to create a prototype from the
      specified cluster that conforms to the distribution
      specified in Config.  If there are too few samples in the
      cluster to perform a statistical analysis, then a prototype
      is generated but labelled as insignificant.  If the
      dimensions of the cluster are not independent, no prototype
      is generated and NULL is returned.  If a prototype can be
      found that matches the desired distribution then a pointer
      to it is returned, otherwise NULL is returned.
Return:		Pointer to new prototype or NULL
Exceptions:	None
History:	6/19/89, DSJ, Created.
*******************************************************************************/
PROTOTYPE *MakePrototype(CLUSTERER *Clusterer,
                         CLUSTERCONFIG *Config,
                         CLUSTER *Cluster) {
  STATISTICS *Statistics;
  PROTOTYPE *Proto;
  BUCKETS *Buckets;

  // filter out clusters which contain samples from the same character
  if (MultipleCharSamples (Clusterer, Cluster, Config->MaxIllegal))
    return (NULL);

  // compute the covariance matrix and ranges for the cluster
  Statistics =
    ComputeStatistics (Clusterer->SampleSize, Clusterer->ParamDesc, Cluster);

  // check for degenerate clusters which need not be analyzed further
  // note that the MinSamples test assumes that all clusters with multiple
  // character samples have been removed (as above)
  Proto = MakeDegenerateProto (Clusterer->SampleSize, Cluster, Statistics,
    Config->ProtoStyle,
    (INT32) (Config->MinSamples *
    Clusterer->NumChar));
  if (Proto != NULL) {
    FreeStatistics(Statistics); 
    return (Proto);
  }
  // check to ensure that all dimensions are independent
  if (!Independent (Clusterer->ParamDesc, Clusterer->SampleSize,
  Statistics->CoVariance, Config->Independence)) {
    FreeStatistics(Statistics); 
    return (NULL);
  }

  // create a histogram data structure used to evaluate distributions
  Buckets = GetBuckets (normal, Cluster->SampleCount, Config->Confidence);

  // create a prototype based on the statistics and test it
  switch (Config->ProtoStyle) {
    case spherical:
      Proto = MakeSphericalProto (Clusterer, Cluster, Statistics, Buckets);
      break;
    case elliptical:
      Proto = MakeEllipticalProto (Clusterer, Cluster, Statistics, Buckets);
      break;
    case mixed:
      Proto = MakeMixedProto (Clusterer, Cluster, Statistics, Buckets,
        Config->Confidence);
      break;
    case automatic:
      Proto = MakeSphericalProto (Clusterer, Cluster, Statistics, Buckets);
      if (Proto != NULL)
        break;
      Proto = MakeEllipticalProto (Clusterer, Cluster, Statistics, Buckets);
      if (Proto != NULL)
        break;
      Proto = MakeMixedProto (Clusterer, Cluster, Statistics, Buckets,
        Config->Confidence);
      break;
  }
  FreeBuckets(Buckets); 
  FreeStatistics(Statistics); 
  return (Proto);
}                                // MakePrototype


/** MakeDegenerateProto ******************************************************
Parameters:	N		number of dimensions
      Cluster		cluster being analyzed
      Statistics	statistical info about cluster
      Style		type of prototype to be generated
      MinSamples	minimum number of samples in a cluster
Globals:	None
Operation:	This routine checks for clusters which are degenerate and
      therefore cannot be analyzed in a statistically valid way.
      A cluster is defined as degenerate if it does not have at
      least MINSAMPLESNEEDED samples in it.  If the cluster is
      found to be degenerate, a prototype of the specified style
      is generated and marked as insignificant.  A cluster is
      also degenerate if it does not have at least MinSamples
      samples in it.
      If the cluster is not degenerate, NULL is returned.
Return:		Pointer to degenerate prototype or NULL.
Exceptions:	None
History:	6/20/89, DSJ, Created.
      7/12/89, DSJ, Changed name and added check for 0 stddev.
      8/8/89, DSJ, Removed check for 0 stddev (handled elsewhere).
********************************************************************************/
PROTOTYPE *MakeDegenerateProto(  //this was MinSample
                               UINT16 N,
                               CLUSTER *Cluster,
                               STATISTICS *Statistics,
                               PROTOSTYLE Style,
                               INT32 MinSamples) {
  PROTOTYPE *Proto = NULL;

  if (MinSamples < MINSAMPLESNEEDED)
    MinSamples = MINSAMPLESNEEDED;

  if (Cluster->SampleCount < MinSamples) {
    switch (Style) {
      case spherical:
        Proto = NewSphericalProto (N, Cluster, Statistics);
        break;
      case elliptical:
      case automatic:
        Proto = NewEllipticalProto (N, Cluster, Statistics);
        break;
      case mixed:
        Proto = NewMixedProto (N, Cluster, Statistics);
        break;
    }
    Proto->Significant = FALSE;
  }
  return (Proto);
}                                // MakeDegenerateProto


/* MakeSphericalProto *******************************************************
Parameters:	Clusterer	data struct containing samples being clustered
      Cluster		cluster to be made into a spherical prototype
      Statistics	statistical info about cluster
      Buckets		histogram struct used to analyze distribution
Globals:	None
Operation:	This routine tests the specified cluster to see if it can
      be approximated by a spherical normal distribution.  If it
      can be, then a new prototype is formed and returned to the
      caller.  If it can't be, then NULL is returned to the caller.
Return:		Pointer to new spherical prototype or NULL.
Exceptions:	None
History:	6/1/89, DSJ, Created.
******************************************************************************/
PROTOTYPE *MakeSphericalProto(CLUSTERER *Clusterer,
                              CLUSTER *Cluster,
                              STATISTICS *Statistics,
                              BUCKETS *Buckets) {
  PROTOTYPE *Proto = NULL;
  int i;

  // check that each dimension is a normal distribution
  for (i = 0; i < Clusterer->SampleSize; i++) {
    if (Clusterer->ParamDesc[i].NonEssential)
      continue;

    FillBuckets (Buckets, Cluster, i, &(Clusterer->ParamDesc[i]),
      Cluster->Mean[i],
      sqrt ((FLOAT64) (Statistics->AvgVariance)));
    if (!DistributionOK (Buckets))
      break;
  }
  // if all dimensions matched a normal distribution, make a proto
  if (i >= Clusterer->SampleSize)
    Proto = NewSphericalProto (Clusterer->SampleSize, Cluster, Statistics);
  return (Proto);
}                                // MakeSphericalProto


/** MakeEllipticalProto ****************************************************
Parameters:	Clusterer	data struct containing samples being clustered
      Cluster		cluster to be made into an elliptical prototype
      Statistics	statistical info about cluster
      Buckets		histogram struct used to analyze distribution
Globals:	None
Operation:	This routine tests the specified cluster to see if it can
      be approximated by an elliptical normal distribution.  If it
      can be, then a new prototype is formed and returned to the
      caller.  If it can't be, then NULL is returned to the caller.
Return:		Pointer to new elliptical prototype or NULL.
Exceptions:	None
History:	6/12/89, DSJ, Created.
****************************************************************************/
PROTOTYPE *MakeEllipticalProto(CLUSTERER *Clusterer,
                               CLUSTER *Cluster,
                               STATISTICS *Statistics,
                               BUCKETS *Buckets) {
  PROTOTYPE *Proto = NULL;
  int i;

  // check that each dimension is a normal distribution
  for (i = 0; i < Clusterer->SampleSize; i++) {
    if (Clusterer->ParamDesc[i].NonEssential)
      continue;

    FillBuckets (Buckets, Cluster, i, &(Clusterer->ParamDesc[i]),
      Cluster->Mean[i],
      sqrt ((FLOAT64) Statistics->
      CoVariance[i * (Clusterer->SampleSize + 1)]));
    if (!DistributionOK (Buckets))
      break;
  }
  // if all dimensions matched a normal distribution, make a proto
  if (i >= Clusterer->SampleSize)
    Proto = NewEllipticalProto (Clusterer->SampleSize, Cluster, Statistics);
  return (Proto);
}                                // MakeEllipticalProto


/** MakeMixedProto ***********************************************************
Parameters:	Clusterer	data struct containing samples being clustered
      Cluster		cluster to be made into a prototype
      Statistics	statistical info about cluster
      NormalBuckets	histogram struct used to analyze distribution
      Confidence	confidence level for alternate distributions
Globals:	None
Operation:	This routine tests each dimension of the specified cluster to
      see what distribution would best approximate that dimension.
      Each dimension is compared to the following distributions
      in order: normal, random, uniform.  If each dimension can
      be represented by one of these distributions,
      then a new prototype is formed and returned to the
      caller.  If it can't be, then NULL is returned to the caller.
Return:		Pointer to new mixed prototype or NULL.
Exceptions:	None
History:	6/12/89, DSJ, Created.
********************************************************************************/
PROTOTYPE *MakeMixedProto(CLUSTERER *Clusterer,
                          CLUSTER *Cluster,
                          STATISTICS *Statistics,
                          BUCKETS *NormalBuckets,
                          FLOAT64 Confidence) {
  PROTOTYPE *Proto;
  int i;
  BUCKETS *UniformBuckets = NULL;
  BUCKETS *RandomBuckets = NULL;

  // create a mixed proto to work on - initially assume all dimensions normal*/
  Proto = NewMixedProto (Clusterer->SampleSize, Cluster, Statistics);

  // find the proper distribution for each dimension
  for (i = 0; i < Clusterer->SampleSize; i++) {
    if (Clusterer->ParamDesc[i].NonEssential)
      continue;

    FillBuckets (NormalBuckets, Cluster, i, &(Clusterer->ParamDesc[i]),
      Proto->Mean[i],
      sqrt ((FLOAT64) Proto->Variance.Elliptical[i]));
    if (DistributionOK (NormalBuckets))
      continue;

    if (RandomBuckets == NULL)
      RandomBuckets =
        GetBuckets (D_random, Cluster->SampleCount, Confidence);
    MakeDimRandom (i, Proto, &(Clusterer->ParamDesc[i]));
    FillBuckets (RandomBuckets, Cluster, i, &(Clusterer->ParamDesc[i]),
      Proto->Mean[i], Proto->Variance.Elliptical[i]);
    if (DistributionOK (RandomBuckets))
      continue;

    if (UniformBuckets == NULL)
      UniformBuckets =
        GetBuckets (uniform, Cluster->SampleCount, Confidence);
    MakeDimUniform(i, Proto, Statistics); 
    FillBuckets (UniformBuckets, Cluster, i, &(Clusterer->ParamDesc[i]),
      Proto->Mean[i], Proto->Variance.Elliptical[i]);
    if (DistributionOK (UniformBuckets))
      continue;
    break;
  }
  // if any dimension failed to match a distribution, discard the proto
  if (i < Clusterer->SampleSize) {
    FreePrototype(Proto); 
    Proto = NULL;
  }
  if (UniformBuckets != NULL)
    FreeBuckets(UniformBuckets); 
  if (RandomBuckets != NULL)
    FreeBuckets(RandomBuckets); 
  return (Proto);
}                                // MakeMixedProto


/* MakeDimRandom *************************************************************
Parameters:	i		index of dimension to be changed
      Proto		prototype whose dimension is to be altered
      ParamDesc	description of specified dimension
Globals:	None
Operation:	This routine alters the ith dimension of the specified
      mixed prototype to be D_random.
Return:		None
Exceptions:	None
History:	6/20/89, DSJ, Created.
******************************************************************************/
void MakeDimRandom(UINT16 i, PROTOTYPE *Proto, PARAM_DESC *ParamDesc) { 
  Proto->Distrib[i] = D_random;
  Proto->Mean[i] = ParamDesc->MidRange;
  Proto->Variance.Elliptical[i] = ParamDesc->HalfRange;

  // subtract out the previous magnitude of this dimension from the total
  Proto->TotalMagnitude /= Proto->Magnitude.Elliptical[i];
  Proto->Magnitude.Elliptical[i] = 1.0 / ParamDesc->Range;
  Proto->TotalMagnitude *= Proto->Magnitude.Elliptical[i];
  Proto->LogMagnitude = log ((double) Proto->TotalMagnitude);

  // note that the proto Weight is irrelevant for D_random protos
}                                // MakeDimRandom


/** MakeDimUniform ***********************************************************
Parameters:	i		index of dimension to be changed
      Proto		prototype whose dimension is to be altered
      Statistics	statistical info about prototype
Globals:	None
Operation:	This routine alters the ith dimension of the specified
      mixed prototype to be uniform.
Return:		None
Exceptions:	None
History:	6/20/89, DSJ, Created.
******************************************************************************/
void MakeDimUniform(UINT16 i, PROTOTYPE *Proto, STATISTICS *Statistics) { 
  Proto->Distrib[i] = uniform;
  Proto->Mean[i] = Proto->Cluster->Mean[i] +
    (Statistics->Min[i] + Statistics->Max[i]) / 2;
  Proto->Variance.Elliptical[i] =
    (Statistics->Max[i] - Statistics->Min[i]) / 2;
  if (Proto->Variance.Elliptical[i] < MINVARIANCE)
    Proto->Variance.Elliptical[i] = MINVARIANCE;

  // subtract out the previous magnitude of this dimension from the total
  Proto->TotalMagnitude /= Proto->Magnitude.Elliptical[i];
  Proto->Magnitude.Elliptical[i] =
    1.0 / (2.0 * Proto->Variance.Elliptical[i]);
  Proto->TotalMagnitude *= Proto->Magnitude.Elliptical[i];
  Proto->LogMagnitude = log ((double) Proto->TotalMagnitude);

  // note that the proto Weight is irrelevant for uniform protos
}                                // MakeDimUniform


/** ComputeStatistics *********************************************************
Parameters:	N		number of dimensions
      ParamDesc	array of dimension descriptions
      Cluster		cluster whose stats are to be computed
Globals:	None
Operation:	This routine searches the cluster tree for all leaf nodes
      which are samples in the specified cluster.  It computes
      a full covariance matrix for these samples as well as
      keeping track of the ranges (min and max) for each
      dimension.  A special data structure is allocated to
      return this information to the caller.  An incremental
      algorithm for computing statistics is not used because
      it will not work with circular dimensions.
Return:		Pointer to new data structure containing statistics
Exceptions:	None
History:	6/2/89, DSJ, Created.
*********************************************************************************/
STATISTICS *
ComputeStatistics (INT16 N, PARAM_DESC ParamDesc[], CLUSTER * Cluster) {
  STATISTICS *Statistics;
  int i, j;
  FLOAT32 *CoVariance;
  FLOAT32 *Distance;
  LIST SearchState;
  SAMPLE *Sample;
  UINT32 SampleCountAdjustedForBias;

  // allocate memory to hold the statistics results
  Statistics = (STATISTICS *) Emalloc (sizeof (STATISTICS));
  Statistics->CoVariance = (FLOAT32 *) Emalloc (N * N * sizeof (FLOAT32));
  Statistics->Min = (FLOAT32 *) Emalloc (N * sizeof (FLOAT32));
  Statistics->Max = (FLOAT32 *) Emalloc (N * sizeof (FLOAT32));

  // allocate temporary memory to hold the sample to mean distances
  Distance = (FLOAT32 *) Emalloc (N * sizeof (FLOAT32));

  // initialize the statistics
  Statistics->AvgVariance = 1.0;
  CoVariance = Statistics->CoVariance;
  for (i = 0; i < N; i++) {
    Statistics->Min[i] = 0.0;
    Statistics->Max[i] = 0.0;
    for (j = 0; j < N; j++, CoVariance++)
      *CoVariance = 0;
  }
  // find each sample in the cluster and merge it into the statistics
  InitSampleSearch(SearchState, Cluster); 
  while ((Sample = NextSample (&SearchState)) != NULL) {
    for (i = 0; i < N; i++) {
      Distance[i] = Sample->Mean[i] - Cluster->Mean[i];
      if (ParamDesc[i].Circular) {
        if (Distance[i] > ParamDesc[i].HalfRange)
          Distance[i] -= ParamDesc[i].Range;
        if (Distance[i] < -ParamDesc[i].HalfRange)
          Distance[i] += ParamDesc[i].Range;
      }
      if (Distance[i] < Statistics->Min[i])
        Statistics->Min[i] = Distance[i];
      if (Distance[i] > Statistics->Max[i])
        Statistics->Max[i] = Distance[i];
    }
    CoVariance = Statistics->CoVariance;
    for (i = 0; i < N; i++)
      for (j = 0; j < N; j++, CoVariance++)
        *CoVariance += Distance[i] * Distance[j];
  }
  // normalize the variances by the total number of samples
  // use SampleCount-1 instead of SampleCount to get an unbiased estimate
  // also compute the geometic mean of the diagonal variances
  // ensure that clusters with only 1 sample are handled correctly
  if (Cluster->SampleCount > 1)
    SampleCountAdjustedForBias = Cluster->SampleCount - 1;
  else
    SampleCountAdjustedForBias = 1;
  CoVariance = Statistics->CoVariance;
  for (i = 0; i < N; i++)
  for (j = 0; j < N; j++, CoVariance++) {
    *CoVariance /= SampleCountAdjustedForBias;
    if (j == i)
      Statistics->AvgVariance *= *CoVariance;
  }
  Statistics->AvgVariance = pow (Statistics->AvgVariance, 1.0 / N);

  // release temporary memory and return
  memfree(Distance); 
  return (Statistics);
}                                // ComputeStatistics


/** NewSpericalProto *********************************************************
Parameters:	N		number of dimensions
      Cluster		cluster to be made into a spherical prototype
      Statistics	statistical info about samples in cluster
Globals:	None
Operation:	This routine creates a spherical prototype data structure to
      approximate the samples in the specified cluster.
      Spherical prototypes have a single variance which is
      common across all dimensions.  All dimensions are normally
      distributed and independent.
Return:		Pointer to a new spherical prototype data structure
Exceptions:	None
History:	6/19/89, DSJ, Created.
******************************************************************************/
PROTOTYPE *NewSphericalProto(UINT16 N,
                             CLUSTER *Cluster,
                             STATISTICS *Statistics) {
  PROTOTYPE *Proto;

  Proto = NewSimpleProto (N, Cluster);

  Proto->Variance.Spherical = Statistics->AvgVariance;
  if (Proto->Variance.Spherical < MINVARIANCE)
    Proto->Variance.Spherical = MINVARIANCE;

  Proto->Magnitude.Spherical =
    1.0 / sqrt ((double) (2.0 * PI * Proto->Variance.Spherical));
  Proto->TotalMagnitude = pow (Proto->Magnitude.Spherical, (double) N);
  Proto->Weight.Spherical = 1.0 / Proto->Variance.Spherical;
  Proto->LogMagnitude = log ((double) Proto->TotalMagnitude);

  return (Proto);
}                                // NewSphericalProto


/** NewEllipticalProto *******************************************************
Parameters:	N		number of dimensions
      Cluster		cluster to be made into an elliptical prototype
      Statistics	statistical info about samples in cluster
Globals:	None
Operation:	This routine creates an elliptical prototype data structure to
      approximate the samples in the specified cluster.
      Elliptical prototypes have a variance for each dimension.
      All dimensions are normally distributed and independent.
Return:		Pointer to a new elliptical prototype data structure
Exceptions:	None
History:	6/19/89, DSJ, Created.
*******************************************************************************/
PROTOTYPE *NewEllipticalProto(INT16 N,
                              CLUSTER *Cluster,
                              STATISTICS *Statistics) {
  PROTOTYPE *Proto;
  FLOAT32 *CoVariance;
  int i;

  Proto = NewSimpleProto (N, Cluster);
  Proto->Variance.Elliptical = (FLOAT32 *) Emalloc (N * sizeof (FLOAT32));
  Proto->Magnitude.Elliptical = (FLOAT32 *) Emalloc (N * sizeof (FLOAT32));
  Proto->Weight.Elliptical = (FLOAT32 *) Emalloc (N * sizeof (FLOAT32));

  CoVariance = Statistics->CoVariance;
  Proto->TotalMagnitude = 1.0;
  for (i = 0; i < N; i++, CoVariance += N + 1) {
    Proto->Variance.Elliptical[i] = *CoVariance;
    if (Proto->Variance.Elliptical[i] < MINVARIANCE)
      Proto->Variance.Elliptical[i] = MINVARIANCE;

    Proto->Magnitude.Elliptical[i] =
      1.0 / sqrt ((double) (2.0 * PI * Proto->Variance.Elliptical[i]));
    Proto->Weight.Elliptical[i] = 1.0 / Proto->Variance.Elliptical[i];
    Proto->TotalMagnitude *= Proto->Magnitude.Elliptical[i];
  }
  Proto->LogMagnitude = log ((double) Proto->TotalMagnitude);
  Proto->Style = elliptical;
  return (Proto);
}                                // NewEllipticalProto


/** MewMixedProto ************************************************************
Parameters:	N		number of dimensions
      Cluster		cluster to be made into a mixed prototype
      Statistics	statistical info about samples in cluster
Globals:	None
Operation:	This routine creates a mixed prototype data structure to
      approximate the samples in the specified cluster.
      Mixed prototypes can have different distributions for
      each dimension.  All dimensions are independent.  The
      structure is initially filled in as though it were an
      elliptical prototype.  The actual distributions of the
      dimensions can be altered by other routines.
Return:		Pointer to a new mixed prototype data structure
Exceptions:	None
History:	6/19/89, DSJ, Created.
********************************************************************************/
PROTOTYPE *NewMixedProto(INT16 N, CLUSTER *Cluster, STATISTICS *Statistics) { 
  PROTOTYPE *Proto;
  int i;

  Proto = NewEllipticalProto (N, Cluster, Statistics);
  Proto->Distrib = (DISTRIBUTION *) Emalloc (N * sizeof (DISTRIBUTION));

  for (i = 0; i < N; i++) {
    Proto->Distrib[i] = normal;
  }
  Proto->Style = mixed;
  return (Proto);
}                                // NewMixedProto


/** NewSimpleProto ***********************************************************
Parameters:	N		number of dimensions
      Cluster		cluster to be made into a prototype
Globals:	None
Operation:	This routine allocates memory to hold a simple prototype
      data structure, i.e. one without independent distributions
      and variances for each dimension.
Return:		Pointer to new simple prototype
Exceptions:	None
History:	6/19/89, DSJ, Created.
*******************************************************************************/
PROTOTYPE *NewSimpleProto(INT16 N, CLUSTER *Cluster) { 
  PROTOTYPE *Proto;
  int i;

  Proto = (PROTOTYPE *) Emalloc (sizeof (PROTOTYPE));
  Proto->Mean = (FLOAT32 *) Emalloc (N * sizeof (FLOAT32));

  for (i = 0; i < N; i++)
    Proto->Mean[i] = Cluster->Mean[i];
  Proto->Distrib = NULL;

  Proto->Significant = TRUE;
  Proto->Style = spherical;
  Proto->NumSamples = Cluster->SampleCount;
  Proto->Cluster = Cluster;
  Proto->Cluster->Prototype = TRUE;
  return (Proto);
}                                // NewSimpleProto


/** Independent ***************************************************************
Parameters:	ParamDesc	descriptions of each feature space dimension
      N		number of dimensions
      CoVariance	ptr to a covariance matrix
      Independence	max off-diagonal correlation coefficient
Globals:	None
Operation:	This routine returns TRUE if the specified covariance
      matrix indicates that all N dimensions are independent of
      one another.  One dimension is judged to be independent of
      another when the magnitude of the corresponding correlation
      coefficient is
      less than the specified Independence factor.  The
      correlation coefficient is calculated as: (see Duda and
      Hart, pg. 247)
      coeff[ij] = stddev[ij] / sqrt (stddev[ii] * stddev[jj])
      The covariance matrix is assumed to be symmetric (which
      should always be true).
Return:		TRUE if dimensions are independent, FALSE otherwise
Exceptions:	None
History:	6/4/89, DSJ, Created.
*******************************************************************************/
BOOL8
Independent (PARAM_DESC ParamDesc[],
INT16 N, FLOAT32 * CoVariance, FLOAT32 Independence) {
  int i, j;
  FLOAT32 *VARii;                // points to ith on-diagonal element
  FLOAT32 *VARjj;                // points to jth on-diagonal element
  FLOAT32 CorrelationCoeff;

  VARii = CoVariance;
  for (i = 0; i < N; i++, VARii += N + 1) {
    if (ParamDesc[i].NonEssential)
      continue;

    VARjj = VARii + N + 1;
    CoVariance = VARii + 1;
    for (j = i + 1; j < N; j++, CoVariance++, VARjj += N + 1) {
      if (ParamDesc[j].NonEssential)
        continue;

      if ((*VARii == 0.0) || (*VARjj == 0.0))
        CorrelationCoeff = 0.0;
      else
        CorrelationCoeff =
          sqrt (sqrt (*CoVariance * *CoVariance / (*VARii * *VARjj)));
      if (CorrelationCoeff > Independence)
        return (FALSE);
    }
  }
  return (TRUE);
}                                // Independent


/** GetBuckets **************************************************************
Parameters:	Distribution	type of probability distribution to test for
      SampleCount	number of samples that are available
      Confidence	probability of a Type I error
Globals:	none
Operation:	This routine returns a histogram data structure which can
      be used by other routines to place samples into histogram
      buckets, and then apply a goodness of fit test to the
      histogram data to determine if the samples belong to the
      specified probability distribution.  The routine keeps
      a list of bucket data structures which have already been
      created so that it minimizes the computation time needed
      to create a new bucket.
Return:		Bucket data structure
Exceptions: none
History:	Thu Aug  3 12:58:10 1989, DSJ, Created.
*****************************************************************************/
BUCKETS *GetBuckets(DISTRIBUTION Distribution,
                    UINT32 SampleCount,
                    FLOAT64 Confidence) {
  UINT16 NumberOfBuckets;
  BUCKETS *Buckets;

  // search for an old bucket structure with the same number of buckets
  NumberOfBuckets = OptimumNumberOfBuckets (SampleCount);
  Buckets = (BUCKETS *) first (search (OldBuckets[(int) Distribution],
    &NumberOfBuckets, NumBucketsMatch));

  // if a matching bucket structure is found, delete it from the list
  if (Buckets != NULL) {
    OldBuckets[(int) Distribution] =
      delete_d (OldBuckets[(int) Distribution], Buckets, ListEntryMatch);
    if (SampleCount != Buckets->SampleCount)
      AdjustBuckets(Buckets, SampleCount); 
    if (Confidence != Buckets->Confidence) {
      Buckets->Confidence = Confidence;
      Buckets->ChiSquared = ComputeChiSquared
        (DegreesOfFreedom (Distribution, Buckets->NumberOfBuckets),
        Confidence);
    }
    InitBuckets(Buckets); 
  }
  else                           // otherwise create a new structure
    Buckets = MakeBuckets (Distribution, SampleCount, Confidence);
  return (Buckets);
}                                // GetBuckets


/** Makebuckets *************************************************************
Parameters:	Distribution	type of probability distribution to test for
      SampleCount	number of samples that are available
      Confidence	probability of a Type I error
Globals:	None
Operation:	This routine creates a histogram data structure which can
      be used by other routines to place samples into histogram
      buckets, and then apply a goodness of fit test to the
      histogram data to determine if the samples belong to the
      specified probability distribution.  The buckets are
      allocated in such a way that the expected frequency of
      samples in each bucket is approximately the same.  In
      order to make this possible, a mapping table is
      computed which maps "normalized" samples into the
      appropriate bucket.
Return:		Pointer to new histogram data structure
Exceptions:	None
History:	6/4/89, DSJ, Created.
*****************************************************************************/
BUCKETS *MakeBuckets(DISTRIBUTION Distribution,
                     UINT32 SampleCount,
                     FLOAT64 Confidence) {
  static DENSITYFUNC DensityFunction[] =
    { NormalDensity, UniformDensity, UniformDensity };
  int i, j;
  BUCKETS *Buckets;
  FLOAT64 BucketProbability;
  FLOAT64 NextBucketBoundary;
  FLOAT64 Probability;
  FLOAT64 ProbabilityDelta;
  FLOAT64 LastProbDensity;
  FLOAT64 ProbDensity;
  UINT16 CurrentBucket;
  BOOL8 Symmetrical;

  // allocate memory needed for data structure
  Buckets = (BUCKETS *) Emalloc (sizeof (BUCKETS));
  Buckets->NumberOfBuckets = OptimumNumberOfBuckets (SampleCount);
  Buckets->SampleCount = SampleCount;
  Buckets->Confidence = Confidence;
  Buckets->Count =
    (UINT32 *) Emalloc (Buckets->NumberOfBuckets * sizeof (UINT32));
  Buckets->ExpectedCount =
    (FLOAT32 *) Emalloc (Buckets->NumberOfBuckets * sizeof (FLOAT32));

  // initialize simple fields
  Buckets->Distribution = Distribution;
  for (i = 0; i < Buckets->NumberOfBuckets; i++) {
    Buckets->Count[i] = 0;
    Buckets->ExpectedCount[i] = 0.0;
  }

  // all currently defined distributions are symmetrical
  Symmetrical = TRUE;
  Buckets->ChiSquared = ComputeChiSquared
    (DegreesOfFreedom (Distribution, Buckets->NumberOfBuckets), Confidence);

  if (Symmetrical) {
    // allocate buckets so that all have approx. equal probability
    BucketProbability = 1.0 / (FLOAT64) (Buckets->NumberOfBuckets);

    // distribution is symmetric so fill in upper half then copy
    CurrentBucket = Buckets->NumberOfBuckets / 2;
    if (Odd (Buckets->NumberOfBuckets))
      NextBucketBoundary = BucketProbability / 2;
    else
      NextBucketBoundary = BucketProbability;

    Probability = 0.0;
    LastProbDensity =
      (*DensityFunction[(int) Distribution]) (BUCKETTABLESIZE / 2);
    for (i = BUCKETTABLESIZE / 2; i < BUCKETTABLESIZE; i++) {
      ProbDensity = (*DensityFunction[(int) Distribution]) (i + 1);
      ProbabilityDelta = Integral (LastProbDensity, ProbDensity, 1.0);
      Probability += ProbabilityDelta;
      if (Probability > NextBucketBoundary) {
        if (CurrentBucket < Buckets->NumberOfBuckets - 1)
          CurrentBucket++;
        NextBucketBoundary += BucketProbability;
      }
      Buckets->Bucket[i] = CurrentBucket;
      Buckets->ExpectedCount[CurrentBucket] +=
        (FLOAT32) (ProbabilityDelta * SampleCount);
      LastProbDensity = ProbDensity;
    }
    // place any leftover probability into the last bucket
    Buckets->ExpectedCount[CurrentBucket] +=
      (FLOAT32) ((0.5 - Probability) * SampleCount);

    // copy upper half of distribution to lower half
    for (i = 0, j = BUCKETTABLESIZE - 1; i < j; i++, j--)
      Buckets->Bucket[i] =
        Mirror (Buckets->Bucket[j], Buckets->NumberOfBuckets);

    // copy upper half of expected counts to lower half
    for (i = 0, j = Buckets->NumberOfBuckets - 1; i <= j; i++, j--)
      Buckets->ExpectedCount[i] += Buckets->ExpectedCount[j];
  }
  return (Buckets);
}                                // MakeBuckets


//---------------------------------------------------------------------------
UINT16 OptimumNumberOfBuckets(UINT32 SampleCount) { 
/*
 **	Parameters:
 **		SampleCount	number of samples to be tested
 **	Globals:
 **		CountTable	lookup table for number of samples
 **		BucketsTable	lookup table for necessary number of buckets
 **	Operation:
 **		This routine computes the optimum number of histogram
 **		buckets that should be used in a chi-squared goodness of
 **		fit test for the specified number of samples.  The optimum
 **		number is computed based on Table 4.1 on pg. 147 of
 **		"Measurement and Analysis of Random Data" by Bendat & Piersol.
 **		Linear interpolation is used to interpolate between table
 **		values.  The table is intended for a 0.05 level of
 **		significance (alpha).  This routine assumes that it is
 **		equally valid for other alpha's, which may not be true.
 **	Return:
 **		Optimum number of histogram buckets
 **	Exceptions:
 **		None
 **	History:
 **		6/5/89, DSJ, Created.
 */
  UINT8 Last, Next;
  FLOAT32 Slope;

  if (SampleCount < CountTable[0])
    return (BucketsTable[0]);

  for (Last = 0, Next = 1; Next < LOOKUPTABLESIZE; Last++, Next++) {
    if (SampleCount <= CountTable[Next]) {
      Slope = (FLOAT32) (BucketsTable[Next] - BucketsTable[Last]) /
        (FLOAT32) (CountTable[Next] - CountTable[Last]);
      return ((UINT16) (BucketsTable[Last] +
        Slope * (SampleCount - CountTable[Last])));
    }
  }
  return (BucketsTable[Last]);
}                                // OptimumNumberOfBuckets


//---------------------------------------------------------------------------
FLOAT64
ComputeChiSquared (UINT16 DegreesOfFreedom, FLOAT64 Alpha)
/*
 **	Parameters:
 **		DegreesOfFreedom	determines shape of distribution
 **		Alpha			probability of right tail
 **	Globals: none
 **	Operation:
 **		This routine computes the chi-squared value which will
 **		leave a cumulative probability of Alpha in the right tail
 **		of a chi-squared distribution with the specified number of
 **		degrees of freedom.  Alpha must be between 0 and 1.
 **		DegreesOfFreedom must be even.  The routine maintains an
 **		array of lists.  Each list corresponds to a different
 **		number of degrees of freedom.  Each entry in the list
 **		corresponds to a different alpha value and its corresponding
 **		chi-squared value.  Therefore, once a particular chi-squared
 **		value is computed, it is stored in the list and never
 **		needs to be computed again.
 **	Return: Desired chi-squared value
 **	Exceptions: none
 **	History: 6/5/89, DSJ, Created.
 */
#define CHIACCURACY     0.01
#define MINALPHA  (1e-200)
{
  static LIST ChiWith[MAXDEGREESOFFREEDOM + 1];

  CHISTRUCT *OldChiSquared;
  CHISTRUCT SearchKey;

  // limit the minimum alpha that can be used - if alpha is too small
  //      it may not be possible to compute chi-squared.
  if (Alpha < MINALPHA)
    Alpha = MINALPHA;
  if (Alpha > 1.0)
    Alpha = 1.0;
  if (Odd (DegreesOfFreedom))
    DegreesOfFreedom++;

  /* find the list of chi-squared values which have already been computed
     for the specified number of degrees of freedom.  Search the list for
     the desired chi-squared. */
  SearchKey.Alpha = Alpha;
  OldChiSquared = (CHISTRUCT *) first (search (ChiWith[DegreesOfFreedom],
    &SearchKey, AlphaMatch));

  if (OldChiSquared == NULL) {
    OldChiSquared = NewChiStruct (DegreesOfFreedom, Alpha);
    OldChiSquared->ChiSquared = Solve (ChiArea, OldChiSquared,
      (FLOAT64) DegreesOfFreedom,
      (FLOAT64) CHIACCURACY);
    ChiWith[DegreesOfFreedom] = push (ChiWith[DegreesOfFreedom],
      OldChiSquared);
  }
  else {
    // further optimization might move OldChiSquared to front of list
  }

  return (OldChiSquared->ChiSquared);

}                                // ComputeChiSquared


//---------------------------------------------------------------------------
FLOAT64 NormalDensity(INT32 x) { 
/*
 **	Parameters:
 **		x	number to compute the normal probability density for
 **	Globals:
 **		NormalMean	mean of a discrete normal distribution
 **		NormalVariance	variance of a discrete normal distribution
 **		NormalMagnitude	magnitude of a discrete normal distribution
 **	Operation:
 **		This routine computes the probability density function
 **		of a discrete normal distribution defined by the global
 **		variables NormalMean, NormalVariance, and NormalMagnitude.
 **		Normal magnitude could, of course, be computed in terms of
 **		the normal variance but it is precomputed for efficiency.
 **	Return:
 **		The value of the normal distribution at x.
 **	Exceptions:
 **		None
 **	History:
 **		6/4/89, DSJ, Created.
 */
  FLOAT64 Distance;

  Distance = x - NormalMean;
  return (NormalMagnitude *
    exp (-0.5 * Distance * Distance / NormalVariance));
}                                // NormalDensity


//---------------------------------------------------------------------------
FLOAT64 UniformDensity(INT32 x) { 
/*
 **	Parameters:
 **		x	number to compute the uniform probability density for
 **	Globals:
 **		BUCKETTABLESIZE		determines range of distribution
 **	Operation:
 **		This routine computes the probability density function
 **		of a uniform distribution at the specified point.  The
 **		range of the distribution is from 0 to BUCKETTABLESIZE.
 **	Return:
 **		The value of the uniform distribution at x.
 **	Exceptions:
 **		None
 **	History:
 **		6/5/89, DSJ, Created.
 */
  static FLOAT64 UniformDistributionDensity = (FLOAT64) 1.0 / BUCKETTABLESIZE;

  if ((x >= 0.0) && (x <= BUCKETTABLESIZE))
    return (UniformDistributionDensity);
  else
    return ((FLOAT64) 0.0);
}                                // UniformDensity


//---------------------------------------------------------------------------
FLOAT64 Integral(FLOAT64 f1, FLOAT64 f2, FLOAT64 Dx) { 
/*
 **	Parameters:
 **		f1	value of function at x1
 **		f2	value of function at x2
 **		Dx	x2 - x1 (should always be positive)
 **	Globals:
 **		None
 **	Operation:
 **		This routine computes a trapezoidal approximation to the
 **		integral of a function over a small delta in x.
 **	Return:
 **		Approximation of the integral of the function from x1 to x2.
 **	Exceptions:
 **		None
 **	History:
 **		6/5/89, DSJ, Created.
 */
  return ((f1 + f2) * Dx / 2.0);
}                                // Integral


//---------------------------------------------------------------------------
void FillBuckets(BUCKETS *Buckets,
                 CLUSTER *Cluster,
                 UINT16 Dim,
                 PARAM_DESC *ParamDesc,
                 FLOAT32 Mean,
                 FLOAT32 StdDev) {
/*
 **	Parameters:
 **		Buckets		histogram buckets to count samples
 **		Cluster		cluster whose samples are being analyzed
 **		Dim		dimension of samples which is being analyzed
 **		ParamDesc	description of the dimension
 **		Mean		"mean" of the distribution
 **		StdDev		"standard deviation" of the distribution
 **	Globals:
 **		None
 **	Operation:
 **		This routine counts the number of cluster samples which
 **		fall within the various histogram buckets in Buckets.  Only
 **		one dimension of each sample is examined.  The exact meaning
 **		of the Mean and StdDev parameters depends on the
 **		distribution which is being analyzed (this info is in the
 **		Buckets data structure).  For normal distributions, Mean
 **		and StdDev have the expected meanings.  For uniform and
 **		random distributions the Mean is the center point of the
 **		range and the StdDev is 1/2 the range.  A dimension with
 **		zero standard deviation cannot be statistically analyzed.
 **		In this case, a pseudo-analysis is used.
 **	Return:
 **		None (the Buckets data structure is filled in)
 **	Exceptions:
 **		None
 **	History:
 **		6/5/89, DSJ, Created.
 */
  UINT16 BucketID;
  int i;
  LIST SearchState;
  SAMPLE *Sample;

  // initialize the histogram bucket counts to 0
  for (i = 0; i < Buckets->NumberOfBuckets; i++)
    Buckets->Count[i] = 0;

  if (StdDev == 0.0) {
    /* if the standard deviation is zero, then we can't statistically
       analyze the cluster.  Use a pseudo-analysis: samples exactly on
       the mean are distributed evenly across all buckets.  Samples greater
       than the mean are placed in the last bucket; samples less than the
       mean are placed in the first bucket. */

    InitSampleSearch(SearchState, Cluster); 
    i = 0;
    while ((Sample = NextSample (&SearchState)) != NULL) {
      if (Sample->Mean[Dim] > Mean)
        BucketID = Buckets->NumberOfBuckets - 1;
      else if (Sample->Mean[Dim] < Mean)
        BucketID = 0;
      else
        BucketID = i;
      Buckets->Count[BucketID] += 1;
      i++;
      if (i >= Buckets->NumberOfBuckets)
        i = 0;
    }
  }
  else {
    // search for all samples in the cluster and add to histogram buckets
    InitSampleSearch(SearchState, Cluster); 
    while ((Sample = NextSample (&SearchState)) != NULL) {
      switch (Buckets->Distribution) {
        case normal:
          BucketID = NormalBucket (ParamDesc, Sample->Mean[Dim],
            Mean, StdDev);
          break;
        case D_random:
        case uniform:
          BucketID = UniformBucket (ParamDesc, Sample->Mean[Dim],
            Mean, StdDev);
          break;
        default:
          BucketID = 0;
      }
      Buckets->Count[Buckets->Bucket[BucketID]] += 1;
    }
  }
}                                // FillBuckets


//---------------------------------------------------------------------------*/
UINT16 NormalBucket(PARAM_DESC *ParamDesc,
                    FLOAT32 x,
                    FLOAT32 Mean,
                    FLOAT32 StdDev) {
/*
 **	Parameters:
 **		ParamDesc	used to identify circular dimensions
 **		x		value to be normalized
 **		Mean		mean of normal distribution
 **		StdDev		standard deviation of normal distribution
 **	Globals:
 **		NormalMean	mean of discrete normal distribution
 **		NormalStdDev	standard deviation of discrete normal dist.
 **		BUCKETTABLESIZE	range of the discrete distribution
 **	Operation:
 **		This routine determines which bucket x falls into in the
 **		discrete normal distribution defined by NormalMean
 **		and NormalStdDev.  x values which exceed the range of
 **		the discrete distribution are clipped.
 **	Return:
 **		Bucket number into which x falls
 **	Exceptions:
 **		None
 **	History:
 **		6/5/89, DSJ, Created.
 */
  FLOAT32 X;

  // wraparound circular parameters if necessary
  if (ParamDesc->Circular) {
    if (x - Mean > ParamDesc->HalfRange)
      x -= ParamDesc->Range;
    else if (x - Mean < -ParamDesc->HalfRange)
      x += ParamDesc->Range;
  }

  X = ((x - Mean) / StdDev) * NormalStdDev + NormalMean;
  if (X < 0)
    return ((UINT16) 0);
  if (X > BUCKETTABLESIZE - 1)
    return ((UINT16) (BUCKETTABLESIZE - 1));
  return ((UINT16) floor ((FLOAT64) X));
}                                // NormalBucket


//---------------------------------------------------------------------------
UINT16 UniformBucket(PARAM_DESC *ParamDesc,
                     FLOAT32 x,
                     FLOAT32 Mean,
                     FLOAT32 StdDev) {
/*
 **	Parameters:
 **		ParamDesc	used to identify circular dimensions
 **		x		value to be normalized
 **		Mean		center of range of uniform distribution
 **		StdDev		1/2 the range of the uniform distribution
 **	Globals:
 **		BUCKETTABLESIZE	range of the discrete distribution
 **	Operation:
 **		This routine determines which bucket x falls into in the
 **		discrete uniform distribution defined by
 **		BUCKETTABLESIZE.  x values which exceed the range of
 **		the discrete distribution are clipped.
 **	Return:
 **		Bucket number into which x falls
 **	Exceptions:
 **		None
 **	History:
 **		6/5/89, DSJ, Created.
 */
  FLOAT32 X;

  // wraparound circular parameters if necessary
  if (ParamDesc->Circular) {
    if (x - Mean > ParamDesc->HalfRange)
      x -= ParamDesc->Range;
    else if (x - Mean < -ParamDesc->HalfRange)
      x += ParamDesc->Range;
  }

  X = ((x - Mean) / (2 * StdDev) * BUCKETTABLESIZE + BUCKETTABLESIZE / 2.0);
  if (X < 0)
    return ((UINT16) 0);
  if (X > BUCKETTABLESIZE - 1)
    return ((UINT16) (BUCKETTABLESIZE - 1));
  return ((UINT16) floor ((FLOAT64) X));
}                                // UniformBucket


//---------------------------------------------------------------------------
BOOL8 DistributionOK(BUCKETS *Buckets) { 
/*
 **	Parameters:
 **		Buckets		histogram data to perform chi-square test on
 **	Globals:
 **		None
 **	Operation:
 **		This routine performs a chi-square goodness of fit test
 **		on the histogram data in the Buckets data structure.  TRUE
 **		is returned if the histogram matches the probability
 **		distribution which was specified when the Buckets
 **		structure was originally created.  Otherwise FALSE is
 **		returned.
 **	Return:
 **		TRUE if samples match distribution, FALSE otherwise
 **	Exceptions:
 **		None
 **	History:
 **		6/5/89, DSJ, Created.
 */
  FLOAT32 FrequencyDifference;
  FLOAT32 TotalDifference;
  int i;

  // compute how well the histogram matches the expected histogram
  TotalDifference = 0.0;
  for (i = 0; i < Buckets->NumberOfBuckets; i++) {
    FrequencyDifference = Buckets->Count[i] - Buckets->ExpectedCount[i];
    TotalDifference += (FrequencyDifference * FrequencyDifference) /
      Buckets->ExpectedCount[i];
  }

  // test to see if the difference is more than expected
  if (TotalDifference > Buckets->ChiSquared)
    return (FALSE);
  else
    return (TRUE);
}                                // DistributionOK


//---------------------------------------------------------------------------
void FreeStatistics(STATISTICS *Statistics) { 
/*
 **	Parameters:
 **		Statistics	pointer to data structure to be freed
 **	Globals:
 **		None
 **	Operation:
 **		This routine frees the memory used by the statistics
 **		data structure.
 **	Return:
 **		None
 **	Exceptions:
 **		None
 **	History:
 **		6/5/89, DSJ, Created.
 */
  memfree (Statistics->CoVariance);
  memfree (Statistics->Min);
  memfree (Statistics->Max);
  memfree(Statistics); 
}                                // FreeStatistics


//---------------------------------------------------------------------------
void FreeBuckets(BUCKETS *Buckets) { 
/*
 **	Parameters:
 **		Buckets		pointer to data structure to be freed
 **	Globals: none
 **	Operation:
 **		This routine places the specified histogram data structure
 **		at the front of a list of histograms so that it can be
 **		reused later if necessary.  A separate list is maintained
 **		for each different type of distribution.
 **	Return: none
 **	Exceptions: none
 **	History: 6/5/89, DSJ, Created.
 */
  int Dist;

  if (Buckets != NULL) {
    Dist = (int) Buckets->Distribution;
    OldBuckets[Dist] = (LIST) push (OldBuckets[Dist], Buckets);
  }

}                                // FreeBuckets


//---------------------------------------------------------------------------
void FreeCluster(CLUSTER *Cluster) { 
/*
 **	Parameters:
 **		Cluster		pointer to cluster to be freed
 **	Globals:
 **		None
 **	Operation:
 **		This routine frees the memory consumed by the specified
 **		cluster and all of its subclusters.  This is done by
 **		recursive calls to FreeCluster().
 **	Return:
 **		None
 **	Exceptions:
 **		None
 **	History:
 **		6/6/89, DSJ, Created.
 */
  if (Cluster != NULL) {
    FreeCluster (Cluster->Left);
    FreeCluster (Cluster->Right);
    memfree(Cluster); 
  }
}                                // FreeCluster


//---------------------------------------------------------------------------
UINT16 DegreesOfFreedom(DISTRIBUTION Distribution, UINT16 HistogramBuckets) { 
/*
 **	Parameters:
 **		Distribution		distribution being tested for
 **		HistogramBuckets	number of buckets in chi-square test
 **	Globals: none
 **	Operation:
 **		This routine computes the degrees of freedom that should
 **		be used in a chi-squared test with the specified number of
 **		histogram buckets.  The result is always rounded up to
 **		the next even number so that the value of chi-squared can be
 **		computed more easily.  This will cause the value of
 **		chi-squared to be higher than the optimum value, resulting
 **		in the chi-square test being more lenient than optimum.
 **	Return: The number of degrees of freedom for a chi-square test
 **	Exceptions: none
 **	History: Thu Aug  3 14:04:18 1989, DSJ, Created.
 */
  static UINT8 DegreeOffsets[] = { 3, 3, 1 };

  UINT16 AdjustedNumBuckets;

  AdjustedNumBuckets = HistogramBuckets - DegreeOffsets[(int) Distribution];
  if (Odd (AdjustedNumBuckets))
    AdjustedNumBuckets++;
  return (AdjustedNumBuckets);

}                                // DegreesOfFreedom


//---------------------------------------------------------------------------
int NumBucketsMatch(void *arg1,    //BUCKETS                                       *Histogram,
                    void *arg2) {  //UINT16                                        *DesiredNumberOfBuckets)
/*
 **	Parameters:
 **		Histogram	current histogram being tested for a match
 **		DesiredNumberOfBuckets	match key
 **	Globals: none
 **	Operation:
 **		This routine is used to search a list of histogram data
 **		structures to find one with the specified number of
 **		buckets.  It is called by the list search routines.
 **	Return: TRUE if Histogram matches DesiredNumberOfBuckets
 **	Exceptions: none
 **	History: Thu Aug  3 14:17:33 1989, DSJ, Created.
 */
  BUCKETS *Histogram = (BUCKETS *) arg1;
  UINT16 *DesiredNumberOfBuckets = (UINT16 *) arg2;

  return (*DesiredNumberOfBuckets == Histogram->NumberOfBuckets);

}                                // NumBucketsMatch


//---------------------------------------------------------------------------
int ListEntryMatch(void *arg1,    //ListNode
                   void *arg2) {  //Key
/*
 **	Parameters: none
 **	Globals: none
 **	Operation:
 **		This routine is used to search a list for a list node
 **		whose contents match Key.  It is called by the list
 **		delete_d routine.
 **	Return: TRUE if ListNode matches Key
 **	Exceptions: none
 **	History: Thu Aug  3 14:23:58 1989, DSJ, Created.
 */
  return (arg1 == arg2);

}                                // ListEntryMatch


//---------------------------------------------------------------------------
void AdjustBuckets(BUCKETS *Buckets, UINT32 NewSampleCount) { 
/*
 **	Parameters:
 **		Buckets		histogram data structure to adjust
 **		NewSampleCount	new sample count to adjust to
 **	Globals: none
 **	Operation:
 **		This routine multiplies each ExpectedCount histogram entry
 **		by NewSampleCount/OldSampleCount so that the histogram
 **		is now adjusted to the new sample count.
 **	Return: none
 **	Exceptions: none
 **	History: Thu Aug  3 14:31:14 1989, DSJ, Created.
 */
  int i;
  FLOAT64 AdjustFactor;

  AdjustFactor = (((FLOAT64) NewSampleCount) /
    ((FLOAT64) Buckets->SampleCount));

  for (i = 0; i < Buckets->NumberOfBuckets; i++) {
    Buckets->ExpectedCount[i] *= AdjustFactor;
  }

  Buckets->SampleCount = NewSampleCount;

}                                // AdjustBuckets


//---------------------------------------------------------------------------
void InitBuckets(BUCKETS *Buckets) { 
/*
 **	Parameters:
 **		Buckets		histogram data structure to init
 **	Globals: none
 **	Operation:
 **		This routine sets the bucket counts in the specified histogram
 **		to zero.
 **	Return: none
 **	Exceptions: none
 **	History: Thu Aug  3 14:31:14 1989, DSJ, Created.
 */
  int i;

  for (i = 0; i < Buckets->NumberOfBuckets; i++) {
    Buckets->Count[i] = 0;
  }

}                                // InitBuckets


//---------------------------------------------------------------------------
int AlphaMatch(void *arg1,    //CHISTRUCT                             *ChiStruct,
               void *arg2) {  //CHISTRUCT                             *SearchKey)
/*
 **	Parameters:
 **		ChiStruct	chi-squared struct being tested for a match
 **		SearchKey	chi-squared struct that is the search key
 **	Globals: none
 **	Operation:
 **		This routine is used to search a list of structures which
 **		hold pre-computed chi-squared values for a chi-squared
 **		value whose corresponding alpha field matches the alpha
 **		field of SearchKey.
 **		It is called by the list search routines.
 **	Return: TRUE if ChiStruct's Alpha matches SearchKey's Alpha
 **	Exceptions: none
 **	History: Thu Aug  3 14:17:33 1989, DSJ, Created.
 */
  CHISTRUCT *ChiStruct = (CHISTRUCT *) arg1;
  CHISTRUCT *SearchKey = (CHISTRUCT *) arg2;

  return (ChiStruct->Alpha == SearchKey->Alpha);

}                                // AlphaMatch


//---------------------------------------------------------------------------
CHISTRUCT *NewChiStruct(UINT16 DegreesOfFreedom, FLOAT64 Alpha) { 
/*
 **	Parameters:
 **		DegreesOfFreedom	degrees of freedom for new chi value
 **		Alpha			confidence level for new chi value
 **	Globals: none
 **	Operation:
 **		This routine allocates a new data structure which is used
 **		to hold a chi-squared value along with its associated
 **		number of degrees of freedom and alpha value.
 **	Return: none
 **	Exceptions: none
 **	History: Fri Aug  4 11:04:59 1989, DSJ, Created.
 */
  CHISTRUCT *NewChiStruct;

  NewChiStruct = (CHISTRUCT *) Emalloc (sizeof (CHISTRUCT));
  NewChiStruct->DegreesOfFreedom = DegreesOfFreedom;
  NewChiStruct->Alpha = Alpha;
  return (NewChiStruct);

}                                // NewChiStruct


//---------------------------------------------------------------------------
FLOAT64
Solve (SOLVEFUNC Function,
void *FunctionParams, FLOAT64 InitialGuess, FLOAT64 Accuracy)
/*
 **	Parameters:
 **		Function	function whose zero is to be found
 **		FunctionParams	arbitrary data to pass to function
 **		InitialGuess	point to start solution search at
 **		Accuracy	maximum allowed error
 **	Globals: none
 **	Operation:
 **		This routine attempts to find an x value at which Function
 **		goes to zero (i.e. a root of the function ).  It will only
 **		work correctly if a solution actually exists and there
 **		are no extrema between the solution and the InitialGuess.
 **		The algorithms used are extremely primitive.
 **	Return: Solution of function ( x for which f(x) = 0 ).
 **	Exceptions: none
 **	History: Fri Aug  4 11:08:59 1989, DSJ, Created.
 */
#define INITIALDELTA    0.1
#define  DELTARATIO     0.1
{
  FLOAT64 x;
  FLOAT64 f;
  FLOAT64 Slope;
  FLOAT64 Delta;
  FLOAT64 NewDelta;
  FLOAT64 xDelta;
  FLOAT64 LastPosX, LastNegX;

  x = InitialGuess;
  Delta = INITIALDELTA;
  LastPosX = MAX_FLOAT32;
  LastNegX = -MAX_FLOAT32;
  f = (*Function) ((CHISTRUCT *) FunctionParams, x);
  while (Abs (LastPosX - LastNegX) > Accuracy) {
    // keep track of outer bounds of current estimate
    if (f < 0)
      LastNegX = x;
    else
      LastPosX = x;

    // compute the approx. slope of f(x) at the current point
    Slope =
      ((*Function) ((CHISTRUCT *) FunctionParams, x + Delta) - f) / Delta;

    // compute the next solution guess */
    xDelta = f / Slope;
    x -= xDelta;

    // reduce the delta used for computing slope to be a fraction of
    //the amount moved to get to the new guess
    NewDelta = Abs (xDelta) * DELTARATIO;
    if (NewDelta < Delta)
      Delta = NewDelta;

    // compute the value of the function at the new guess
    f = (*Function) ((CHISTRUCT *) FunctionParams, x);
  }
  return (x);

}                                // Solve


//---------------------------------------------------------------------------
FLOAT64 ChiArea(CHISTRUCT *ChiParams, FLOAT64 x) { 
/*
 **	Parameters:
 **		ChiParams	contains degrees of freedom and alpha
 **		x		value of chi-squared to evaluate
 **	Globals: none
 **	Operation:
 **		This routine computes the area under a chi density curve
 **		from 0 to x, minus the desired area under the curve.  The
 **		number of degrees of freedom of the chi curve is specified
 **		in the ChiParams structure.  The desired area is also
 **		specified in the ChiParams structure as Alpha ( or 1 minus
 **		the desired area ).  This routine is intended to be passed
 **		to the Solve() function to find the value of chi-squared
 **		which will yield a desired area under the right tail of
 **		the chi density curve.  The function will only work for
 **		even degrees of freedom.  The equations are based on
 **		integrating the chi density curve in parts to obtain
 **		a series that can be used to compute the area under the
 **		curve.
 **	Return: Error between actual and desired area under the chi curve.
 **	Exceptions: none
 **	History: Fri Aug  4 12:48:41 1989, DSJ, Created.
 */
  int i, N;
  FLOAT64 SeriesTotal;
  FLOAT64 Denominator;
  FLOAT64 PowerOfx;

  N = ChiParams->DegreesOfFreedom / 2 - 1;
  SeriesTotal = 1;
  Denominator = 1;
  PowerOfx = 1;
  for (i = 1; i <= N; i++) {
    Denominator *= 2 * i;
    PowerOfx *= x;
    SeriesTotal += PowerOfx / Denominator;
  }
  return ((SeriesTotal * exp (-0.5 * x)) - ChiParams->Alpha);

}                                // ChiArea


//---------------------------------------------------------------------------
BOOL8
MultipleCharSamples (CLUSTERER * Clusterer,
CLUSTER * Cluster, FLOAT32 MaxIllegal)
/*
 **	Parameters:
 **		Clusterer	data structure holding cluster tree
 **		Cluster		cluster containing samples to be tested
 **		MaxIllegal	max percentage of samples allowed to have
 **				more than 1 feature in the cluster
 **	Globals: none
 **	Operation:
 **		This routine looks at all samples in the specified cluster.
 **		It computes a running estimate of the percentage of the
 **		charaters which have more than 1 sample in the cluster.
 **		When this percentage exceeds MaxIllegal, TRUE is returned.
 **		Otherwise FALSE is returned.  The CharID
 **		fields must contain integers which identify the training
 **		characters which were used to generate the sample.  One
 **		integer is used for each sample.  The NumChar field in
 **		the Clusterer must contain the number of characters in the
 **		training set.  All CharID fields must be between 0 and
 **		NumChar-1.  The main function of this routine is to help
 **		identify clusters which need to be split further, i.e. if
 **		numerous training characters have 2 or more features which are
 **		contained in the same cluster, then the cluster should be
 **		split.
 **	Return: TRUE if the cluster should be split, FALSE otherwise.
 **	Exceptions: none
 **	History: Wed Aug 30 11:13:05 1989, DSJ, Created.
 **		2/22/90, DSJ, Added MaxIllegal control rather than always
 **				splitting illegal clusters.
 */
#define ILLEGAL_CHAR    2
{
  static BOOL8 *CharFlags = NULL;
  static INT32 NumFlags = 0;
  int i;
  LIST SearchState;
  SAMPLE *Sample;
  INT32 CharID;
  INT32 NumCharInCluster;
  INT32 NumIllegalInCluster;
  FLOAT32 PercentIllegal;

  // initial estimate assumes that no illegal chars exist in the cluster
  NumCharInCluster = Cluster->SampleCount;
  NumIllegalInCluster = 0;

  if (Clusterer->NumChar > NumFlags) {
    if (CharFlags != NULL)
      memfree(CharFlags); 
    NumFlags = Clusterer->NumChar;
    CharFlags = (BOOL8 *) Emalloc (NumFlags * sizeof (BOOL8));
  }

  for (i = 0; i < NumFlags; i++)
    CharFlags[i] = FALSE;

  // find each sample in the cluster and check if we have seen it before
  InitSampleSearch(SearchState, Cluster); 
  while ((Sample = NextSample (&SearchState)) != NULL) {
    CharID = Sample->CharID;
    if (CharFlags[CharID] == FALSE) {
      CharFlags[CharID] = TRUE;
    }
    else {
      if (CharFlags[CharID] == TRUE) {
        NumIllegalInCluster++;
        CharFlags[CharID] = ILLEGAL_CHAR;
      }
      NumCharInCluster--;
      PercentIllegal = (FLOAT32) NumIllegalInCluster / NumCharInCluster;
      if (PercentIllegal > MaxIllegal)
        return (TRUE);
    }
  }
  return (FALSE);

}                                // MultipleCharSamples
