#include <mpi.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <omp.h>
#include <alloca.h>
#include "utils.h"

#include "treewalk.h"
#include "partmanager.h"
#include "domain.h"
#include "forcetree.h"

#include <signal.h>
#define BREAKPOINT raise(SIGTRAP)

#define FACT1 0.366025403785	/* FACT1 = 0.5 * (sqrt(3)-1) */

/* Structure to store the (task-level) counts for particles
 * to be sent to each process*/
struct SendRecvBuffer
{
    int *Send_offset, *Send_count;
    int *Recv_offset, *Recv_count;
};

/*!< Memory factor to leave for (N imported particles) > (N exported particles). */
static int ImportBufferBoost;

/*!< Thread-local list of the particles to be exported,
 * and the destination tasks. This table allows the
results to be disentangled again and to be
assigned to the correct particle.*/
static struct data_index
{
    int Task;
    int Index;
    int NodeList[NODELISTLENGTH];
} *DataIndexTable;

/*Initialise global treewalk parameters*/
void set_treewalk_params(ParameterSet * ps)
{
    int ThisTask;
    MPI_Comm_rank(MPI_COMM_WORLD, &ThisTask);
    if(ThisTask == 0)
        ImportBufferBoost = param_get_int(ps, "ImportBufferBoost");
    MPI_Bcast(&ImportBufferBoost, 1, MPI_INT, 0, MPI_COMM_WORLD);
}

static void ev_init_thread(TreeWalk * const tw, LocalTreeWalk * lv);
static void ev_begin(TreeWalk * tw, int * active_set, const size_t size);
static void ev_finish(TreeWalk * tw);
static void ev_primary(TreeWalk * tw);
static struct SendRecvBuffer ev_get_remote(TreeWalk * tw);
static void ev_secondary(TreeWalk * tw);
static void ev_reduce_result(const struct SendRecvBuffer sndrcv, TreeWalk * tw);
static int ev_ndone(TreeWalk * tw);

static int
ngb_treefind_threads(TreeWalkQueryBase * I,
        TreeWalkResultBase * O,
        TreeWalkNgbIterBase * iter,
        int startnode,
        LocalTreeWalk * lv);

/*
 * for debugging
 */
#define WATCH { \
        printf("tw->WorkSet[0] = %d (%d) %s:%d\n", tw->WorkSet ? tw->WorkSet[0] : 0, tw->WorkSetSize, __FILE__, __LINE__); \
    }
static TreeWalk * GDB_current_ev = NULL;

static void
ev_init_thread(TreeWalk * const tw, LocalTreeWalk * lv)
{
    const size_t thread_id = omp_get_thread_num();
    lv->tw = tw;
    lv->maxNinteractions = 0;
    lv->minNinteractions = 1L<<45;
    lv->Ninteractions = 0;
    lv->Nlistprimary = 0;
    lv->Nexport = 0;
    size_t localbunch = tw->BunchSize/omp_get_max_threads();
    lv->DataIndexOffset = thread_id * localbunch;
    lv->BunchSize = localbunch;
    if(localbunch > tw->BunchSize - thread_id * localbunch)
        lv->BunchSize = tw->BunchSize - thread_id * localbunch;
    if(tw->Ngblist)
        lv->ngblist = tw->Ngblist + thread_id * tw->tree->NumParticles;
}

static void
ev_begin(TreeWalk * tw, int * active_set, const size_t size)
{
    /* Needs to be 64-bit so that the multiplication in Ngblist malloc doesn't overflow*/
    const size_t NumThreads = omp_get_max_threads();
    MPI_Comm_size(MPI_COMM_WORLD, &tw->NTask);
    /* The last argument is may_have_garbage: in practice the only
     * trivial haswork is the gravtree, which has no (active) garbage because
     * the active list was just rebuilt. If we ever add a trivial haswork after
     * sfr/bh we should change this*/
    treewalk_build_queue(tw, active_set, size, 0);

    /* Print some balance numbers*/
    int64_t nmin, nmax, total;
    MPI_Reduce(&tw->WorkSetSize, &nmin, 1, MPI_INT64, MPI_MIN, 0, MPI_COMM_WORLD);
    MPI_Reduce(&tw->WorkSetSize, &nmax, 1, MPI_INT64, MPI_MAX, 0, MPI_COMM_WORLD);
    MPI_Reduce(&tw->WorkSetSize, &total, 1, MPI_INT64, MPI_SUM, 0, MPI_COMM_WORLD);
    message(0, "Treewalk %s iter %ld: total part %ld max/MPI: %ld min/MPI: %ld balance: %g.\n",
            tw->ev_label, tw->Niteration, total, nmax, nmin, (double)nmax/((total+0.001)/tw->NTask));

    /* Start first iteration at the beginning*/
    tw->WorkSetStart = 0;

    if(!tw->NoNgblist)
        tw->Ngblist = (int*) mymalloc("Ngblist", tw->tree->NumParticles * NumThreads * sizeof(int));
    else
        tw->Ngblist = NULL;

    report_memory_usage(tw->ev_label);

    /* Assert that the query and result structures are aligned to  64-bit boundary,
     * so that our MPI Send/Recv's happen from aligned memory.*/
    if(tw->query_type_elsize % 8 != 0)
        endrun(0, "Query structure has size %ld, not aligned to 64-bit boundary.\n", tw->query_type_elsize);
    if(tw->result_type_elsize % 8 != 0)
        endrun(0, "Result structure has size %ld, not aligned to 64-bit boundary.\n", tw->result_type_elsize);

    /*The amount of memory eventually allocated per tree buffer*/
    size_t bytesperbuffer = sizeof(struct data_index) + tw->query_type_elsize;
    /*This memory scales like the number of imports. In principle this could be much larger than Nexport
     * if the tree is very imbalanced and many processors all need to export to this one. In practice I have
     * not seen this happen, but provide a parameter to boost the memory for Nimport just in case.*/
    bytesperbuffer += ImportBufferBoost * (tw->query_type_elsize + tw->result_type_elsize);
    /*Use all free bytes for the tree buffer, as in exchange. Leave some free memory for array overhead.*/
    size_t freebytes = mymalloc_freebytes();
    if(freebytes <= 4096 * 11 * bytesperbuffer) {
        endrun(1231245, "Not enough memory for exporting any particles: needed %ld bytes have %ld. \n", bytesperbuffer, freebytes-4096*10);
    }
    freebytes -= 4096 * 10 * bytesperbuffer;

    tw->BunchSize = (size_t) floor(((double)freebytes)/ bytesperbuffer);
    /* if the send/recv buffer is close to 4GB some MPIs have issues. */
    const size_t twogb = 1024*1024*3092L;
    if(tw->BunchSize * tw->query_type_elsize > twogb)
        tw->BunchSize = twogb / tw->query_type_elsize;

    if(tw->BunchSize < 100)
        endrun(2,"Only enough free memory to export %ld elements.\n", tw->BunchSize);

    DataIndexTable = (struct data_index *) mymalloc("DataIndexTable", tw->BunchSize * sizeof(struct data_index));
}

static void ev_finish(TreeWalk * tw)
{
    myfree(DataIndexTable);
    if(tw->Ngblist)
        myfree(tw->Ngblist);
    if(!tw->work_set_stolen_from_active)
        myfree(tw->WorkSet);

}

static void
treewalk_init_query(TreeWalk * tw, TreeWalkQueryBase * query, int i, int * NodeList)
{
#ifdef DEBUG
    query->ID = P[i].ID;
#endif

    int d;
    for(d = 0; d < 3; d ++) {
        query->Pos[d] = P[i].Pos[d];
    }

    if(NodeList) {
        memcpy(query->NodeList, NodeList, sizeof(int) * NODELISTLENGTH);
    } else {
        query->NodeList[0] = tw->tree->firstnode; /* root node */
        query->NodeList[1] = -1; /* terminate immediately */
    }

    tw->fill(i, query, tw);
}

static void
treewalk_init_result(TreeWalk * tw, TreeWalkResultBase * result, TreeWalkQueryBase * query)
{
    memset(result, 0, tw->result_type_elsize);
#ifdef DEBUG
    result->ID = query->ID;
#endif
}

static void
treewalk_reduce_result(TreeWalk * tw, TreeWalkResultBase * result, int i, enum TreeWalkReduceMode mode)
{
    if(tw->reduce != NULL)
        tw->reduce(i, result, mode, tw);
#ifdef DEBUG
    if(P[i].ID != result->ID)
        endrun(2, "Mismatched ID (%ld != %ld) for particle %d in treewalk reduction, mode %d\n", P[i].ID, result->ID, i, mode);
#endif
}

static int real_ev(TreeWalk * tw, size_t * dataindexoffset, size_t * nexports, int * currentIndex, int64_t * maxNinteractions, int64_t * minNinteractions)
{
    LocalTreeWalk lv[1];
    /* Note: exportflag is local to each thread */
    ev_init_thread(tw, lv);
    lv->mode = 0;

    /* use old index to recover from a buffer overflow*/;
    TreeWalkQueryBase * input = (TreeWalkQueryBase *) alloca(tw->query_type_elsize);
    TreeWalkResultBase * output = (TreeWalkResultBase *) alloca(tw->result_type_elsize);

    int64_t lastSucceeded = tw->WorkSetStart - 1;
    /* We must schedule monotonically so that if the export buffer fills up
     * it is guaranteed that earlier particles are already done.
     * However, we schedule dynamically so that we have reduced imbalance.
     * We do not use the openmp dynamic scheduling, but roll our own
     * so that we can break from the loop if needed.*/
    int chnk = 0;
    /* chunk size: 1 and 1000 were slightly (3 percent) slower than 8.
     * FoF treewalk needs a larger chnksz to avoid contention.*/
    int chnksz = tw->WorkSetSize / (4*tw->NThread);
    if(chnksz < 1)
        chnksz = 1;
    if(chnksz > 100)
        chnksz = 100;
    do {
        /* Get another chunk from the global queue*/
        chnk = atomic_fetch_and_add(currentIndex, chnksz);
        /* This is a hand-rolled version of what openmp dynamic scheduling is doing.*/
        int end = chnk + chnksz;
        /* Make sure we do not overflow the loop*/
        if(end > tw->WorkSetSize)
            end = tw->WorkSetSize;
        /* Reduce the chunk size towards the end of the walk*/
        if((tw->WorkSetSize  < end + chnksz * tw->NThread) && chnksz >= 2)
            chnksz /= 2;
        int k;
        for(k = chnk; k < end; k++) {
            /* Skip already evaluated particles. This is only used if the buffer fills up.*/
            if(tw->evaluated && tw->evaluated[k])
                continue;

            const int i = tw->WorkSet ? tw->WorkSet[k] : k;
            /* Primary never uses node list */
            treewalk_init_query(tw, input, i, NULL);
            treewalk_init_result(tw, output, input);
            lv->target = i;
            /* Reset the number of exported particles.*/
            lv->NThisParticleExport = 0;
            const int rt = tw->visit(input, output, lv);
            if(lv->NThisParticleExport > 1000)
                message(5, "%ld exports for particle %d! Odd.\n", lv->NThisParticleExport, k);
            if(rt < 0) {
                /* export buffer has filled up, can't do more work.*/
                break;
            } else {
                treewalk_reduce_result(tw, output, i, TREEWALK_PRIMARY);
                /* We need lastSucceeded as well as currentIndex so that
                 * if the export buffer fills up in the middle of a
                 * chunk we still get the right answer. Notice it is thread-local*/
                lastSucceeded = k;
                if(tw->evaluated)
                    tw->evaluated[k] = 1;
            }
        }
        /* If we filled up, we need to remove the partially evaluated last particle from the export list and leave this loop.*/
        if(lv->Nexport >= lv->BunchSize) {
            message(1, "Tree export buffer full with %ld particles. start %ld lastsucceeded: %ld end %d size %ld.\n",
                    lv->Nexport, tw->WorkSetStart, lastSucceeded, end, tw->WorkSetSize);
            #pragma omp atomic write
            tw->BufferFullFlag = 1;
            /* If the above loop finished, we don't need to remove the fully exported particle*/
            if(lastSucceeded < end) {
                /* Touch up the DataIndexTable, so that partial particle exports are discarded.
                * Since this queue is per-thread, it is ordered.*/
                lv->Nexport-= lv->NThisParticleExport;
                const int lastreal = tw->WorkSet ? tw->WorkSet[k] : k;
                /* Index stores tw->target, which is the current particle.*/
                if(lv->NThisParticleExport > 0 && DataIndexTable[lv->DataIndexOffset + lv->Nexport].Index > lastreal)
                    endrun(5, "Something screwed up in export queue: nexp %ld (local %ld) last %d < index %d\n", lv->Nexport,
                        lv->NThisParticleExport, lastreal, DataIndexTable[lv->DataIndexOffset + lv->Nexport].Index);
                /* Leave this chunking loop.*/
            }
            break;
        }
    } while(chnk < tw->WorkSetSize);

    if(*maxNinteractions < lv->maxNinteractions)
        *maxNinteractions = lv->maxNinteractions;
    if(*minNinteractions > lv->maxNinteractions)
        *minNinteractions = lv->minNinteractions;
    #pragma omp atomic update
    tw->Ninteractions += lv->Ninteractions;
    #pragma omp atomic update
    tw->Nlistprimary += lv->Nlistprimary;
    *dataindexoffset = lv->DataIndexOffset;
    *nexports = lv->Nexport;
    return lastSucceeded;
}

void
treewalk_build_queue(TreeWalk * tw, int * active_set, const size_t size, int may_have_garbage)
{
    tw->NThread = omp_get_max_threads();

    if(!tw->haswork && !may_have_garbage)
    {
        tw->WorkSetSize = size;
        tw->WorkSet = active_set;
        tw->work_set_stolen_from_active = 1;
        return;
    }

    /* Since we use a static schedule below we only need size / tw->NThread elements per thread.
     * Add NThread so that each thread has a little extra space.*/
    size_t tsize = size / tw->NThread + tw->NThread;
    /*Watch out: tw->WorkSet may change a few lines later due to the realloc*/
    tw->WorkSet = (int *) mymalloc("ActiveQueue", tsize * sizeof(int) * tw->NThread);
    tw->work_set_stolen_from_active = 0;
    size_t i;
    size_t nqueue = 0;

    /*We want a lockless algorithm which preserves the ordering of the particle list.*/
    size_t *nqthr = ta_malloc("nqthr", size_t, tw->NThread);
    int **thrqueue = ta_malloc("thrqueue", int *, tw->NThread);

    gadget_setup_thread_arrays(tw->WorkSet, thrqueue, nqthr, tsize, tw->NThread);

    /* We enforce schedule static to ensure that each thread executes on contiguous particles.
     * Note static enforces the monotonic modifier but on OpenMP 5.0 nonmonotonic is the default.
     * static also ensures that no single thread gets more than tsize elements.*/
    size_t schedsz = size/tw->NThread+1;
    #pragma omp parallel for schedule(static, schedsz)
    for(i=0; i < size; i++)
    {
        const int tid = omp_get_thread_num();
        /*Use raw particle number if active_set is null, otherwise use active_set*/
        const int p_i = active_set ? active_set[i] : (int) i;

        /* Skip the garbage particles */
        if(P[p_i].IsGarbage)
            continue;

        if(tw->haswork && !tw->haswork(p_i, tw))
            continue;
#ifdef DEBUG
        if(nqthr[tid] >= tsize)
            endrun(5, "tid = %d nqthr = %ld, tsize = %ld size = %ld, tw->Nthread = %ld i = %ld\n", tid, nqthr[tid], tsize, size, tw->NThread, i);
#endif
        thrqueue[tid][nqthr[tid]] = p_i;
        nqthr[tid]++;
    }
    /*Merge step for the queue.*/
    nqueue = gadget_compact_thread_arrays(tw->WorkSet, thrqueue, nqthr, tw->NThread);
    ta_free(thrqueue);
    ta_free(nqthr);
    /*Shrink memory*/
    tw->WorkSet = (int *) myrealloc(tw->WorkSet, sizeof(int) * nqueue);

#if 0
    /* check the uniqueness of the active_set list. This is very slow. */
    qsort_openmp(tw->WorkSet, nqueue, sizeof(int), cmpint);
    for(i = 0; i < nqueue - 1; i ++) {
        if(tw->WorkSet[i] == tw->WorkSet[i+1]) {
            endrun(8829, "A few particles are twicely active.\n");
        }
    }
#endif
    tw->WorkSetSize = nqueue;
}

/* returns struct containing export counts */
static void
ev_primary(TreeWalk * tw)
{
    double tstart, tend;
    tw->BufferFullFlag = 0;

    tstart = second();
    tw->Nexport_thread = ta_malloc2("localexports", size_t, 2*tw->NThread);
    tw->Nexport_threadoffset = tw->Nexport_thread + tw->NThread;


    int currentIndex = tw->WorkSetStart;
    int lastSucceeded = tw->WorkSetSize;

    int64_t maxNinteractions = 0, minNinteractions = 1L << 45;
#pragma omp parallel reduction(min: lastSucceeded) reduction(min:minNinteractions) reduction(max:maxNinteractions)
    {
        int tid = omp_get_thread_num();
        lastSucceeded = real_ev(tw, &tw->Nexport_threadoffset[tid], &tw->Nexport_thread[tid], &currentIndex, &maxNinteractions, &minNinteractions);
    }
    tw->maxNinteractions = maxNinteractions;
    tw->minNinteractions = minNinteractions;

    /* Set the place to start the next iteration. Note that because lastSucceeded
     * is the minimum entry across all threads, some particles may have their trees partially walked twice locally.
     * This is fine because we walk the tree to fill up the Ngbiter list.
     * Only a full Ngbiter list is executed; partial lists are discarded.*/
    tw->WorkSetStart = lastSucceeded + 1;

    tend = second();
    tw->timecomp1 += timediff(tstart, tend);
}

static int ev_ndone(TreeWalk * tw)
{
    int ndone;
    double tstart, tend;
    tstart = second();
    int done = !(tw->BufferFullFlag);
    MPI_Allreduce(&done, &ndone, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);
    tend = second();
    tw->timewait2 += timediff(tstart, tend);
    return ndone;

}

static void ev_secondary(TreeWalk * tw)
{
    double tstart, tend;

    tstart = second();
    tw->dataresult = (char *) mymalloc("EvDataResult", tw->Nimport * tw->result_type_elsize);

#pragma omp parallel
    {
        size_t j;
        LocalTreeWalk lv[1];

        ev_init_thread(tw, lv);
        lv->mode = 1;
#pragma omp for
        for(j = 0; j < tw->Nimport; j++) {
            TreeWalkQueryBase * input = (TreeWalkQueryBase*) (tw->dataget + j * tw->query_type_elsize);
            TreeWalkResultBase * output = (TreeWalkResultBase*)(tw->dataresult + j * tw->result_type_elsize);
            treewalk_init_result(tw, output, input);
            lv->target = -1;
            tw->visit(input, output, lv);
        }
    }
    tend = second();
    tw->timecomp2 += timediff(tstart, tend);
}

#if NODELISTLENGTH != 2
#error "treewalk_export_particle assumes NODELISTLENGTH is 2"
#endif

/* export a particle at target and no, thread safely
 *
 * This can also be called from a nonthreaded code
 *
 * */
int treewalk_export_particle(LocalTreeWalk * lv, int no)
{
    if(lv->mode != 0) {
        endrun(1, "Trying to export a ghost particle.\n");
    }
    const int target = lv->target;
    TreeWalk * tw = lv->tw;
    const int task = tw->tree->TopLeaves[no - tw->tree->lastnode].Task;
    /* This index is a unique entry in the global DataIndexTable.*/
    size_t nexp = lv->Nexport + lv->DataIndexOffset;
    /* If the last export was to this task, we can perhaps just add this export to the existing NodeList. We can
     * be sure that all exports of this particle are contiguous.*/
    if(lv->NThisParticleExport >= 1 && DataIndexTable[nexp-1].Task == task) {
#ifdef DEBUG
        /* This is just to be safe: only happens if our indices are off.*/
        if(DataIndexTable[nexp - 1].Index != target)
            endrun(1, "Previous of %ld exports is target %d not current %d\n", lv->NThisParticleExport, DataIndexTable[nexp-1].Index, target);
#endif
        if(DataIndexTable[nexp-1].NodeList[1] == -1) {
            DataIndexTable[nexp-1].NodeList[1] = tw->tree->TopLeaves[no - tw->tree->lastnode].treenode;
            return 0;
        }
    }
    /* out of buffer space. Need to interrupt. */
    if(lv->Nexport >= lv->BunchSize) {
        return -1;
    }
    DataIndexTable[nexp].Task = task;
    DataIndexTable[nexp].Index = target;
    DataIndexTable[nexp].NodeList[0] = tw->tree->TopLeaves[no - tw->tree->lastnode].treenode;
    DataIndexTable[nexp].NodeList[1] = -1;
    lv->Nexport++;
    lv->NThisParticleExport++;
    return 0;
}

/* run a treewalk on an active_set.
 *
 * active_set : a list of indices of particles. If active_set is NULL,
 *              all (NumPart) particles are used.
 *
 * */
void
treewalk_run(TreeWalk * tw, int * active_set, size_t size)
{
    if(!force_tree_allocated(tw->tree)) {
        endrun(0, "Tree has been freed before this treewalk.\n");
    }

    GDB_current_ev = tw;

    ev_begin(tw, active_set, size);

    if(tw->preprocess) {
        int64_t i;
        #pragma omp parallel for
        for(i = 0; i < tw->WorkSetSize; i ++) {
            const int p_i = tw->WorkSet ? tw->WorkSet[i] : i;
            tw->preprocess(p_i, tw);
        }
    }

    if(tw->visit) {
        tw->Nexportfull = 0;
        tw->Nexport_sum = 0;
        tw->evaluated = NULL;
        do
        {
            /* Keep track of which particles have been evaluated across buffer fill ups.
             * Do this if we are not allowed to evaluate anything twice,
             * or if the buffer filled up already.*/
            if((!tw->evaluated) && (tw->Nexportfull == 1 || tw->repeatdisallowed)) {
                tw->evaluated = (char *) mymalloc("evaluated", sizeof(char)*tw->WorkSetSize);
                memset(tw->evaluated, 0, sizeof(char)*tw->WorkSetSize);
            }
            ev_primary(tw); /* do local particles and prepare export list */
            /* exchange particle data */
            const struct SendRecvBuffer sndrcv = ev_get_remote(tw);
            /* now do the particles that were sent to us */
            ev_secondary(tw);

            /* import the result to local particles */
            ev_reduce_result(sndrcv, tw);

            tw->Nexportfull ++;
            ta_free(sndrcv.Send_count);
        } while(ev_ndone(tw) < tw->NTask);
        if(tw->evaluated)
            myfree(tw->evaluated);
    }

    double tstart, tend;

    tstart = second();

    if(tw->postprocess) {
        int64_t i;
        #pragma omp parallel for
        for(i = 0; i < tw->WorkSetSize; i ++) {
            const int p_i = tw->WorkSet ? tw->WorkSet[i] : i;
            tw->postprocess(p_i, tw);
        }
    }
    tend = second();
    tw->timecomp3 = timediff(tstart, tend);
    ev_finish(tw);
    tw->Niteration++;
}

static void
ev_communicate(void * sendbuf, void * recvbuf, size_t elsize, const struct SendRecvBuffer sndrcv, int import) {
    /* if import is 1, import the results from neigbhours */
    MPI_Datatype type;
    MPI_Type_contiguous(elsize, MPI_BYTE, &type);
    MPI_Type_commit(&type);

    if(import) {
        MPI_Alltoallv_sparse(
                sendbuf, sndrcv.Recv_count, sndrcv.Recv_offset, type,
                recvbuf, sndrcv.Send_count, sndrcv.Send_offset, type, MPI_COMM_WORLD);
    } else {
        MPI_Alltoallv_sparse(
                sendbuf, sndrcv.Send_count, sndrcv.Send_offset, type,
                recvbuf, sndrcv.Recv_count, sndrcv.Recv_offset, type, MPI_COMM_WORLD);
    }
    MPI_Type_free(&type);
}

/* returns the remote particles */
static struct SendRecvBuffer ev_get_remote(TreeWalk * tw)
{
    size_t i, NTask = tw->NTask;
    double tstart, tend;

    struct SendRecvBuffer sndrcv = {0};
    sndrcv.Send_count = ta_malloc("Send_count", int, 4*NTask);
    sndrcv.Recv_count = sndrcv.Send_count + NTask;
    sndrcv.Send_offset = sndrcv.Send_count + 2*NTask;
    sndrcv.Recv_offset = sndrcv.Send_count + 3*NTask;

    size_t Nexport=0;
    /* Fill the communication layouts */
    tstart = second();
    memset(sndrcv.Send_count, 0, sizeof(int)*NTask);
    for(i = 0; i < (size_t) tw->NThread; i++)
    {
        size_t k;
        for(k = 0; k < tw->Nexport_thread[i]; k++)
            sndrcv.Send_count[DataIndexTable[k+tw->Nexport_threadoffset[i]].Task]++;
        /* This is over all full buffers.*/
        tw->Nexport_sum += tw->Nexport_thread[i];
        /* This is the send count*/
        Nexport += tw->Nexport_thread[i];
    }
    tend = second();
    tw->timecommsumm3 += timediff(tstart, tend);

    tstart = second();
    MPI_Alltoall(sndrcv.Send_count, 1, MPI_INT, sndrcv.Recv_count, 1, MPI_INT, MPI_COMM_WORLD);
    tend = second();
    tw->timewait1 += timediff(tstart, tend);

    for(i = 0, tw->Nimport = 0, sndrcv.Recv_offset[0] = 0, sndrcv.Send_offset[0] = 0; i < NTask; i++)
    {
        tw->Nimport += sndrcv.Recv_count[i];
        if(i > 0)
        {
            sndrcv.Send_offset[i] = sndrcv.Send_offset[i - 1] + sndrcv.Send_count[i - 1];
            sndrcv.Recv_offset[i] = sndrcv.Recv_offset[i - 1] + sndrcv.Recv_count[i - 1];
        }
    }

    void * recvbuf = mymalloc("EvDataGet", tw->Nimport * tw->query_type_elsize);
    char * sendbuf = (char *) mymalloc("EvDataIn", Nexport * tw->query_type_elsize);

    tstart = second();
    /* prepare particle data for export */
    int * real_send_count = ta_malloc("tmp_send_count", int, NTask);
    memset(real_send_count, 0, sizeof(int)*NTask);
    for(i = 0; i < (size_t) tw->NThread; i++)
    {
        size_t k;
        for(k = 0; k < tw->Nexport_thread[i]; k++) {
            const int ditind = k+tw->Nexport_threadoffset[i];
            const int place = DataIndexTable[ditind].Index;
            const int task = DataIndexTable[ditind].Task;
            int bufpos = real_send_count[task] + sndrcv.Send_offset[task];
            TreeWalkQueryBase * input = (TreeWalkQueryBase*) (sendbuf + bufpos * tw->query_type_elsize);
            real_send_count[DataIndexTable[ditind].Task]++;
            treewalk_init_query(tw, input, place, DataIndexTable[ditind].NodeList);
        }
    }
#ifdef DEBUG
/* Checks!*/
    for(i = 0; i < NTask; i++)
        if(real_send_count[i] != sndrcv.Send_count[i])
            endrun(6, "Inconsistent export to task %ld of %ld: %d expected %d\n", i, NTask, real_send_count[i], sndrcv.Send_count[i]);
#endif
    myfree(real_send_count);
    tend = second();
    tw->timecommsumm3 += timediff(tstart, tend);

    tstart = second();
    ev_communicate(sendbuf, recvbuf, tw->query_type_elsize, sndrcv, 0);
    tend = second();
    tw->timecommsumm1 += timediff(tstart, tend);
    myfree(sendbuf);
    tw->dataget = (char *) recvbuf;
    return sndrcv;
}

static void ev_reduce_result(const struct SendRecvBuffer sndrcv, TreeWalk * tw)
{

    int64_t i;
    double tstart, tend;
    size_t Nexport = 0;

    for(i = 0; i < tw->NThread; i++) {
        Nexport += tw->Nexport_thread[i];
    }

    void * sendbuf = tw->dataresult;
    char * recvbuf = (char*) mymalloc("EvDataOut",
                Nexport * tw->result_type_elsize);

    tstart = second();
    ev_communicate(sendbuf, recvbuf, tw->result_type_elsize, sndrcv, 1);
    tend = second();
    tw->timecommsumm2 += timediff(tstart, tend);

    tstart = second();

    /* Notice that we build the dataindex table individually
     * on each thread, so we are ordered by particle and have memory locality.*/
    if(tw->reduce != NULL) {
        int * real_recv_count = ta_malloc("tmp_recv_count", int, tw->NTask);
        memset(real_recv_count, 0, sizeof(int)*tw->NTask);
        for(i = 0; i < tw->NThread; i++)
        {
            size_t k;
            for(k = 0; k < tw->Nexport_thread[i]; k++) {
                const int ditind = k + tw->Nexport_threadoffset[i];
                const int place = DataIndexTable[ditind].Index;
                const int task = DataIndexTable[ditind].Task;
                int bufpos = real_recv_count[task] + sndrcv.Send_offset[task];
                real_recv_count[task]++;
                TreeWalkResultBase * output = (TreeWalkResultBase*) (recvbuf + tw->result_type_elsize * bufpos);
                treewalk_reduce_result(tw, output, place, TREEWALK_GHOSTS);
#ifdef DEBUG
                if(output->ID != P[place].ID)
                    endrun(8, "Error in communication: IDs mismatch %ld %ld\n", output->ID, P[place].ID);
#endif

            }
        }
        myfree(real_recv_count);
    }
    tend = second();
    tw->timecommsumm3 += timediff(tstart, tend);
    myfree(recvbuf);
    myfree(tw->dataresult);
    myfree(tw->dataget);
    myfree(tw->Nexport_thread);
}

void
treewalk_add_counters(LocalTreeWalk * lv, const int64_t ninteractions)
{
    if(lv->mode == 0) {
        if(lv->maxNinteractions < ninteractions)
            lv->maxNinteractions = ninteractions;
        if(lv->minNinteractions > ninteractions)
            lv->minNinteractions = ninteractions;
        lv->Ninteractions += ninteractions;
        lv->Nlistprimary += 1;
    }
}

/**********
 *
 * This particular TreeWalkVisitFunction that uses the nbgiter memeber of
 * The TreeWalk object to iterate over the neighbours of a Query.
 *
 * All Pairwise interactions are implemented this way.
 *
 * Note: Short range gravity is not based on pair enumeration.
 * We may want to port it over and see if gravtree.c receives any speed up.
 *
 * Required fields in TreeWalk: ngbiter, ngbiter_type_elsize.
 *
 * Before the iteration starts, ngbiter is called with iter->base.other == -1.
 * The callback function shall initialize the interator with Hsml, mask, and symmetric.
 *
 *****/
int treewalk_visit_ngbiter(TreeWalkQueryBase * I,
            TreeWalkResultBase * O,
            LocalTreeWalk * lv)
{

    TreeWalkNgbIterBase * iter = (TreeWalkNgbIterBase *) alloca(lv->tw->ngbiter_type_elsize);

    /* Kick-start the iteration with other == -1 */
    iter->other = -1;
    lv->tw->ngbiter(I, O, iter, lv);
    /* Check whether the tree contains the particles we are looking for*/
    if((lv->tw->tree->mask & iter->mask) != iter->mask)
        endrun(5, "Treewalk for particles with mask %d but tree mask is only %d overlap %d.\n", iter->mask, lv->tw->tree->mask, lv->tw->tree->mask & iter->mask);
    /* If symmetric, make sure we did hmax first*/
    if(iter->symmetric == NGB_TREEFIND_SYMMETRIC && !lv->tw->tree->hmax_computed_flag)
        endrun(3, "%s tried to do a symmetric treewalk without computing hmax!\n", lv->tw->ev_label);
    const double BoxSize = lv->tw->tree->BoxSize;

    int64_t ninteractions = 0;
    int inode = 0;

    for(inode = 0; inode < NODELISTLENGTH && I->NodeList[inode] >= 0; inode++)
    {
        int numcand = ngb_treefind_threads(I, O, iter, I->NodeList[inode], lv);
        /* Export buffer is full end prematurally */
        if(numcand < 0)
            return numcand;

        /* If we are here, export is successful. Work on this particle -- first
         * filter out all of the candidates that are actually outside. */
        int numngb;

        for(numngb = 0; numngb < numcand; numngb ++) {
            int other = lv->ngblist[numngb];

            /* Skip garbage*/
            if(P[other].IsGarbage)
                continue;
            /* In case the type of the particle has changed since the tree was built.
             * Happens for wind treewalk for gas turned into stars on this timestep.*/
            if(!((1<<P[other].Type) & iter->mask)) {
                continue;
            }

            double dist;

            if(iter->symmetric == NGB_TREEFIND_SYMMETRIC) {
                dist = DMAX(P[other].Hsml, iter->Hsml);
            } else {
                dist = iter->Hsml;
            }

            double r2 = 0;
            int d;
            double h2 = dist * dist;
            for(d = 0; d < 3; d ++) {
                /* the distance vector points to 'other' */
                iter->dist[d] = NEAREST(I->Pos[d] - P[other].Pos[d], BoxSize);
                r2 += iter->dist[d] * iter->dist[d];
                if(r2 > h2) break;
            }
            if(r2 > h2) continue;

            /* update the iter and call the iteration function*/
            iter->r2 = r2;
            iter->r = sqrt(r2);
            iter->other = other;

            lv->tw->ngbiter(I, O, iter, lv);
        }

        ninteractions += numngb;
    }

    treewalk_add_counters(lv, ninteractions);

    return 0;
}

/**
 * Cull a node.
 *
 * Returns 1 if the node shall be opened;
 * Returns 0 if the node has no business with this query.
 */
static int
cull_node(const TreeWalkQueryBase * const I, const TreeWalkNgbIterBase * const iter, const struct NODE * const current, const double BoxSize)
{
    double dist;
    if(iter->symmetric == NGB_TREEFIND_SYMMETRIC) {
        dist = DMAX(current->mom.hmax, iter->Hsml) + 0.5 * current->len;
    } else {
        dist = iter->Hsml + 0.5 * current->len;
    }

    double r2 = 0;
    double dx = 0;
    /* do each direction */
    int d;
    for(d = 0; d < 3; d ++) {
        dx = NEAREST(current->center[d] - I->Pos[d], BoxSize);
        if(dx > dist) return 0;
        if(dx < -dist) return 0;
        r2 += dx * dx;
    }
    /* now test against the minimal sphere enclosing everything */
    dist += FACT1 * current->len;

    if(r2 > dist * dist) {
        return 0;
    }
    return 1;
}
/*****
 * This is the internal code that looks for particles in the ngb tree from
 * searchcenter upto hsml. if iter->symmetric is NGB_TREE_FIND_SYMMETRIC, then upto
 * max(P[other].Hsml, iter->Hsml).
 *
 * Particle that intersects with other domains are marked for export.
 * The hosting nodes (leaves of the global tree) are exported as well.
 *
 * For all 'other' particle within the neighbourhood and are local on this processor,
 * this function calls the ngbiter member of the TreeWalk object.
 * iter->base.other, iter->base.dist iter->base.r2, iter->base.r, are properly initialized.
 *
 * */
static int
ngb_treefind_threads(TreeWalkQueryBase * I,
        TreeWalkResultBase * O,
        TreeWalkNgbIterBase * iter,
        int startnode,
        LocalTreeWalk * lv)
{
    int no;
    int numcand = 0;

    const ForceTree * tree = lv->tw->tree;
    const double BoxSize = tree->BoxSize;

    no = startnode;

    while(no >= 0)
    {
        if(node_is_particle(no, tree)) {
            int fat = force_get_father(no, tree);
            endrun(12312, "Particles should be added before getting here! no = %d, father = %d (ptype = %d) start=%d mode = %d\n", no, fat, tree->Nodes[fat].f.ChildType, startnode, lv->mode);
        }
        if(node_is_pseudo_particle(no, tree)) {
            int fat = force_get_father(no, tree);
            endrun(12312, "Pseudo-Particles should be added before getting here! no = %d, father = %d (ptype = %d)\n", no, fat, tree->Nodes[fat].f.ChildType);
        }

        struct NODE *current = &tree->Nodes[no];

        /* When walking exported particles we start from the encompassing top-level node,
         * so if we get back to a top-level node again we are done.*/
        if(lv->mode == 1) {
            /* The first node is always top-level*/
            if(current->f.TopLevel && no != startnode) {
                /* we reached a top-level node again, which means that we are done with the branch */
                break;
            }
        }

        /* Cull the node */
        if(0 == cull_node(I, iter, current, BoxSize)) {
            /* in case the node can be discarded */
            no = current->sibling;
            continue;
        }

        /* Node contains relevant particles, add them.*/
        if(current->f.ChildType == PARTICLE_NODE_TYPE) {
            int i;
            int * suns = current->s.suns;
            for (i = 0; i < current->s.noccupied; i++) {
                lv->ngblist[numcand++] = suns[i];
            }
            /* Move sideways*/
            no = current->sibling;
            continue;
        }
        else if(current->f.ChildType == PSEUDO_NODE_TYPE) {
            /* pseudo particle */
            if(lv->mode == 1) {
                endrun(12312, "Secondary for particle %d from node %d found pseudo at %d.\n", lv->target, startnode, no);
            } else {
                /* Export the pseudo particle*/
                if(-1 == treewalk_export_particle(lv, current->s.suns[0]))
                    return -1;
                /* Move sideways*/
                no = current->sibling;
                continue;
            }
        }
        /* ok, we need to open the node */
        no = current->s.suns[0];
    }

    return numcand;
}

/*****
 * Variant of ngbiter that doesn't use the Ngblist.
 * The ngblist is generally preferred for memory locality reasons and
 * to avoid particles being partially evaluated
 * twice if the buffer fills up. Use this variant if the evaluation
 * wants to change the search radius, such as for knn algorithms
 * or some density code. Don't use it if the treewalk modifies other particles.
 * */
int treewalk_visit_nolist_ngbiter(TreeWalkQueryBase * I,
            TreeWalkResultBase * O,
            LocalTreeWalk * lv)
{
    TreeWalkNgbIterBase * iter = (TreeWalkNgbIterBase *) alloca(lv->tw->ngbiter_type_elsize);

    /* Kick-start the iteration with other == -1 */
    iter->other = -1;
    lv->tw->ngbiter(I, O, iter, lv);

    int64_t ninteractions = 0;
    int inode;
    for(inode = 0; (lv->mode == 0 && inode < 1)|| (lv->mode == 1 && inode < NODELISTLENGTH && I->NodeList[inode] >= 0); inode++)
    {
        int no = I->NodeList[inode];
        const ForceTree * tree = lv->tw->tree;
        const double BoxSize = tree->BoxSize;

        while(no >= 0)
        {
            struct NODE *current = &tree->Nodes[no];

            /* When walking exported particles we start from the encompassing top-level node,
            * so if we get back to a top-level node again we are done.*/
            if(lv->mode == 1) {
                /* The first node is always top-level*/
                if(current->f.TopLevel && no != I->NodeList[inode]) {
                    /* we reached a top-level node again, which means that we are done with the branch */
                    break;
                }
            }

            /* Cull the node */
            if(0 == cull_node(I, iter, current, BoxSize)) {
                /* in case the node can be discarded */
                no = current->sibling;
                continue;
            }

            /* Node contains relevant particles, add them.*/
            if(current->f.ChildType == PARTICLE_NODE_TYPE) {
                int i;
                int * suns = current->s.suns;
                for (i = 0; i < current->s.noccupied; i++) {
                    /* Now evaluate a particle for the list*/
                    int other = suns[i];
                    /* Skip garbage*/
                    if(P[other].IsGarbage)
                        continue;
                    /* In case the type of the particle has changed since the tree was built.
                    * Happens for wind treewalk for gas turned into stars on this timestep.*/
                    if(!((1<<P[other].Type) & iter->mask))
                        continue;

                    double dist = iter->Hsml;
                    double r2 = 0;
                    int d;
                    double h2 = dist * dist;
                    for(d = 0; d < 3; d ++) {
                        /* the distance vector points to 'other' */
                        iter->dist[d] = NEAREST(I->Pos[d] - P[other].Pos[d], BoxSize);
                        r2 += iter->dist[d] * iter->dist[d];
                        if(r2 > h2) break;
                    }
                    if(r2 > h2) continue;

                    /* update the iter and call the iteration function*/
                    iter->r2 = r2;
                    iter->other = other;
                    iter->r = sqrt(r2);
                    lv->tw->ngbiter(I, O, iter, lv);
                    ninteractions++;
                }
                /* Move sideways*/
                no = current->sibling;
                continue;
            }
            else if(current->f.ChildType == PSEUDO_NODE_TYPE) {
                /* pseudo particle */
                if(lv->mode == 1) {
                    endrun(12312, "Secondary for particle %d from node %d found pseudo at %d.\n", lv->target, I->NodeList[inode], no);
                } else {
                    /* Export the pseudo particle*/
                    if(-1 == treewalk_export_particle(lv, current->s.suns[0]))
                        return -1;
                    /* Move sideways*/
                    no = current->sibling;
                    continue;
                }
            }
            /* ok, we need to open the node */
            no = current->s.suns[0];
        }
    }

    treewalk_add_counters(lv, ninteractions);

    return 0;
}

/* This function does treewalk_run in a loop, allocating a queue to allow some particles to be redone.
 * This loop is used primarily in density estimation.*/
void
treewalk_do_hsml_loop(TreeWalk * tw, int * queue, int64_t queuesize, int update_hsml)
{
    int64_t ntot = 0;
    int NumThreads = omp_get_max_threads();
    tw->NPLeft = ta_malloc("NPLeft", size_t, NumThreads);
    tw->NPRedo = ta_malloc("NPRedo", int *, NumThreads);
    tw->maxnumngb = ta_malloc("numngb", double, NumThreads);
    tw->minnumngb = ta_malloc("numngb2", double, NumThreads);

    /* Build the first queue */
    treewalk_build_queue(tw, queue, queuesize, 0);
    /* Next call to treewalk_run will over-write these pointers*/
    int64_t size = tw->WorkSetSize;
    int * ReDoQueue = tw->WorkSet;
    /* First queue is allocated low*/
    int alloc_high = 0;
    /* We don't need to redo the queue generation
     * but need to keep track of allocated memory.*/
    int orig_queue_alloc = (tw->haswork != NULL);
    tw->haswork = NULL;

    /* we will repeat the whole thing for those particles where we didn't find enough neighbours */
    do {
        /* The RedoQueue needs enough memory to store every workset particle on every thread, because
         * we cannot guarantee that the sph particles are evenly spread across threads!*/
        int * CurQueue = ReDoQueue;
        int i;
        for(i = 0; i < NumThreads; i++) {
            tw->maxnumngb[i] = 0;
            tw->minnumngb[i] = 1e50;
        }

        /* The ReDoQueue swaps between high and low allocations so we can have two allocated alternately*/
        if(update_hsml) {
            if(!alloc_high) {
                ReDoQueue = (int *) mymalloc2("ReDoQueue", size * sizeof(int) * NumThreads);
                alloc_high = 1;
            }
            else {
                ReDoQueue = (int *) mymalloc("ReDoQueue", size * sizeof(int) * NumThreads);
                alloc_high = 0;
            }
            tw->Redo_thread_alloc = size;
            gadget_setup_thread_arrays(ReDoQueue, tw->NPRedo, tw->NPLeft, size, NumThreads);
        }
        treewalk_run(tw, CurQueue, size);

        /* Now done with the current queue*/
        if(orig_queue_alloc || tw->Niteration > 1)
            myfree(CurQueue);

        /* We can stop if we are not updating hsml*/
        if(!update_hsml)
            break;

        /* Set up the next queue*/
        size = gadget_compact_thread_arrays(ReDoQueue, tw->NPRedo, tw->NPLeft, NumThreads);

        MPI_Allreduce(&size, &ntot, 1, MPI_INT64, MPI_SUM, MPI_COMM_WORLD);
        if(ntot == 0){
            myfree(ReDoQueue);
            break;
        }
        for(i = 1; i < NumThreads; i++) {
            if(tw->maxnumngb[0] < tw->maxnumngb[i])
                tw->maxnumngb[0] = tw->maxnumngb[i];
            if(tw->minnumngb[0] > tw->minnumngb[i])
                tw->minnumngb[0] = tw->minnumngb[i];
        }
        double minngb, maxngb;
        MPI_Reduce(&tw->maxnumngb[0], &maxngb, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);
        MPI_Reduce(&tw->minnumngb[0], &minngb, 1, MPI_DOUBLE, MPI_MIN, 0, MPI_COMM_WORLD);
        message(0, "Max ngb=%g, min ngb=%g\n", maxngb, minngb);
        treewalk_print_stats(tw);

        /*Shrink memory*/
        ReDoQueue = (int *) myrealloc(ReDoQueue, sizeof(int) * size);

        /*
        if(ntot < 1 ) {
            foreach(ActiveParticle)
            {
                if(density_haswork(i)) {
                    MyFloat Left = DENSITY_GET_PRIV(tw)->Left[i];
                    MyFloat Right = DENSITY_GET_PRIV(tw)->Right[i];
                    message (1, "i=%d task=%d ID=%llu type=%d, Hsml=%g Left=%g Right=%g Ngbs=%g Right-Left=%g\n   pos=(%g|%g|%g)\n",
                         i, ThisTask, P[i].ID, P[i].Type, P[i].Hsml, Left, Right,
                         (float) P[i].NumNgb, Right - Left, P[i].Pos[0], P[i].Pos[1], P[i].Pos[2]);
                }
            }

        }
        */
#ifdef DEBUG
        if(ntot == 1 && size > 0 && tw->Niteration > 20 ) {
            int pp = ReDoQueue[0];
            message(1, "Remaining i=%d, t %d, pos %g %g %g, hsml: %g\n", pp, P[pp].Type, P[pp].Pos[0], P[pp].Pos[1], P[pp].Pos[2], P[pp].Hsml);
        }
#endif

        if(size > 0 && tw->Niteration > MAXITER) {
            endrun(1155, "failed to converge density for %ld particles\n", ntot);
        }
    } while(1);
    ta_free(tw->minnumngb);
    ta_free(tw->maxnumngb);
    ta_free(tw->NPRedo);
    ta_free(tw->NPLeft);
}


/* find the closest index from radius and numNgb list, update left and right bound, return new hsml */
double
ngb_narrow_down(double *right, double *left, const double *radius, const double *numNgb, int maxcmpt, int desnumngb, int *closeidx, double BoxSize)
{
    int j;
    int close = 0;
    double ngbdist = fabs(numNgb[0] - desnumngb);
    for(j = 1; j < maxcmpt; j++){
        double newdist = fabs(numNgb[j] - desnumngb);
        if(newdist < ngbdist){
            ngbdist = newdist;
            close = j;
        }
    }
    if(closeidx)
        *closeidx = close;

    for(j = 0; j < maxcmpt; j++){
        if(numNgb[j] < desnumngb)
            *left = radius[j];
        if(numNgb[j] > desnumngb){
            *right = radius[j];
            break;
        }
    }

    double hsml = radius[close];

    if(*right > 0.99 * BoxSize){
        double dngbdv = 0;
        if(maxcmpt > 1 && (radius[maxcmpt-1]>radius[maxcmpt-2]))
            dngbdv = (numNgb[maxcmpt-1]-numNgb[maxcmpt-2])/(pow(radius[maxcmpt-1],3) - pow(radius[maxcmpt-2],3));
        /* Increase hsml by a maximum factor to avoid madness. We can be fairly aggressive about this factor.*/
        double newhsml = 4 * hsml;
        if(dngbdv > 0) {
            double dngb = (desnumngb - numNgb[maxcmpt-1]);
            double newvolume = pow(hsml,3) + dngb / dngbdv;
            if(pow(newvolume, 1./3) < newhsml)
                newhsml = pow(newvolume, 1./3);
        }
        hsml = newhsml;
    }
    if(hsml > *right)
        hsml = *right;

    if(*left == 0) {
        /* Extrapolate using volume, ie locally constant density*/
        double dngbdv = 0;
        if(radius[1] > radius[0])
            dngbdv = (numNgb[1] - numNgb[0]) / (pow(radius[1],3) - pow(radius[0],3));
        /* Derivative is not defined for minimum, so use 0.*/
        if(maxcmpt == 1 && radius[0] > 0)
            dngbdv = numNgb[0] / pow(radius[0],3);

        if(dngbdv > 0) {
            double dngb = desnumngb - numNgb[0];
            double newvolume = pow(hsml,3) + dngb / dngbdv;
            hsml = pow(newvolume, 1./3);
        }
    }
    if(hsml < *left)
        hsml = *left;

    return hsml;
}

void
treewalk_print_stats(const TreeWalk * tw)
{
    int64_t minNinteractions, maxNinteractions, Ninteractions, Nlistprimary, Nexport;
    MPI_Reduce(&tw->minNinteractions, &minNinteractions, 1, MPI_INT64, MPI_MIN, 0, MPI_COMM_WORLD);
    MPI_Reduce(&tw->maxNinteractions, &maxNinteractions, 1, MPI_INT64, MPI_MAX, 0, MPI_COMM_WORLD);
    MPI_Reduce(&tw->Ninteractions, &Ninteractions, 1, MPI_INT64, MPI_SUM, 0, MPI_COMM_WORLD);
    MPI_Reduce(&tw->Nlistprimary, &Nlistprimary, 1, MPI_INT64, MPI_SUM, 0, MPI_COMM_WORLD);
    MPI_Reduce(&tw->Nexport_sum, &Nexport, 1, MPI_INT64, MPI_SUM, 0, MPI_COMM_WORLD);
    message(0, "%s Ngblist: min %ld max %ld avg %g average exports per task: %g\n", tw->ev_label, minNinteractions, maxNinteractions, (double) Ninteractions / Nlistprimary, ((double) Nexport)/ tw->NTask);
}
