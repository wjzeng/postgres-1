/*-------------------------------------------------------------------------
 *
 * nodeAgg.c
 *	  Routines to handle aggregate nodes.
 *
 *	  ExecAgg normally evaluates each aggregate in the following steps:
 *
 *		 transvalue = initcond
 *		 foreach input_tuple do
 *			transvalue = transfunc(transvalue, input_value(s))
 *		 result = finalfunc(transvalue, direct_argument(s))
 *
 *	  If a finalfunc is not supplied then the result is just the ending
 *	  value of transvalue.
 *
 *	  Other behaviors can be selected by the "aggsplit" mode, which exists
 *	  to support partial aggregation.  It is possible to:
 *	  * Skip running the finalfunc, so that the output is always the
 *	  final transvalue state.
 *	  * Substitute the combinefunc for the transfunc, so that transvalue
 *	  states (propagated up from a child partial-aggregation step) are merged
 *	  rather than processing raw input rows.  (The statements below about
 *	  the transfunc apply equally to the combinefunc, when it's selected.)
 *	  * Apply the serializefunc to the output values (this only makes sense
 *	  when skipping the finalfunc, since the serializefunc works on the
 *	  transvalue data type).
 *	  * Apply the deserializefunc to the input values (this only makes sense
 *	  when using the combinefunc, for similar reasons).
 *	  It is the planner's responsibility to connect up Agg nodes using these
 *	  alternate behaviors in a way that makes sense, with partial aggregation
 *	  results being fed to nodes that expect them.
 *
 *	  If a normal aggregate call specifies DISTINCT or ORDER BY, we sort the
 *	  input tuples and eliminate duplicates (if required) before performing
 *	  the above-depicted process.  (However, we don't do that for ordered-set
 *	  aggregates; their "ORDER BY" inputs are ordinary aggregate arguments
 *	  so far as this module is concerned.)	Note that partial aggregation
 *	  is not supported in these cases, since we couldn't ensure global
 *	  ordering or distinctness of the inputs.
 *
 *	  If transfunc is marked "strict" in pg_proc and initcond is NULL,
 *	  then the first non-NULL input_value is assigned directly to transvalue,
 *	  and transfunc isn't applied until the second non-NULL input_value.
 *	  The agg's first input type and transtype must be the same in this case!
 *
 *	  If transfunc is marked "strict" then NULL input_values are skipped,
 *	  keeping the previous transvalue.  If transfunc is not strict then it
 *	  is called for every input tuple and must deal with NULL initcond
 *	  or NULL input_values for itself.
 *
 *	  If finalfunc is marked "strict" then it is not called when the
 *	  ending transvalue is NULL, instead a NULL result is created
 *	  automatically (this is just the usual handling of strict functions,
 *	  of course).  A non-strict finalfunc can make its own choice of
 *	  what to return for a NULL ending transvalue.
 *
 *	  Ordered-set aggregates are treated specially in one other way: we
 *	  evaluate any "direct" arguments and pass them to the finalfunc along
 *	  with the transition value.
 *
 *	  A finalfunc can have additional arguments beyond the transvalue and
 *	  any "direct" arguments, corresponding to the input arguments of the
 *	  aggregate.  These are always just passed as NULL.  Such arguments may be
 *	  needed to allow resolution of a polymorphic aggregate's result type.
 *
 *	  We compute aggregate input expressions and run the transition functions
 *	  in a temporary econtext (aggstate->tmpcontext).  This is reset at least
 *	  once per input tuple, so when the transvalue datatype is
 *	  pass-by-reference, we have to be careful to copy it into a longer-lived
 *	  memory context, and free the prior value to avoid memory leakage.  We
 *	  store transvalues in another set of econtexts, aggstate->aggcontexts
 *	  (one per grouping set, see below), which are also used for the hashtable
 *	  structures in AGG_HASHED mode.  These econtexts are rescanned, not just
 *	  reset, at group boundaries so that aggregate transition functions can
 *	  register shutdown callbacks via AggRegisterCallback.
 *
 *	  The node's regular econtext (aggstate->ss.ps.ps_ExprContext) is used to
 *	  run finalize functions and compute the output tuple; this context can be
 *	  reset once per output tuple.
 *
 *	  The executor's AggState node is passed as the fmgr "context" value in
 *	  all transfunc and finalfunc calls.  It is not recommended that the
 *	  transition functions look at the AggState node directly, but they can
 *	  use AggCheckCallContext() to verify that they are being called by
 *	  nodeAgg.c (and not as ordinary SQL functions).  The main reason a
 *	  transition function might want to know this is so that it can avoid
 *	  palloc'ing a fixed-size pass-by-ref transition value on every call:
 *	  it can instead just scribble on and return its left input.  Ordinarily
 *	  it is completely forbidden for functions to modify pass-by-ref inputs,
 *	  but in the aggregate case we know the left input is either the initial
 *	  transition value or a previous function result, and in either case its
 *	  value need not be preserved.  See int8inc() for an example.  Notice that
 *	  the EEOP_AGG_PLAIN_TRANS step is coded to avoid a data copy step when
 *	  the previous transition value pointer is returned.  It is also possible
 *	  to avoid repeated data copying when the transition value is an expanded
 *	  object: to do that, the transition function must take care to return
 *	  an expanded object that is in a child context of the memory context
 *	  returned by AggCheckCallContext().  Also, some transition functions want
 *	  to store working state in addition to the nominal transition value; they
 *	  can use the memory context returned by AggCheckCallContext() to do that.
 *
 *	  Note: AggCheckCallContext() is available as of PostgreSQL 9.0.  The
 *	  AggState is available as context in earlier releases (back to 8.1),
 *	  but direct examination of the node is needed to use it before 9.0.
 *
 *	  As of 9.4, aggregate transition functions can also use AggGetAggref()
 *	  to get hold of the Aggref expression node for their aggregate call.
 *	  This is mainly intended for ordered-set aggregates, which are not
 *	  supported as window functions.  (A regular aggregate function would
 *	  need some fallback logic to use this, since there's no Aggref node
 *	  for a window function.)
 *
 *	  Grouping sets:
 *
 *	  A list of grouping sets which is structurally equivalent to a ROLLUP
 *	  clause (e.g. (a,b,c), (a,b), (a)) can be processed in a single pass over
 *	  ordered data.  We do this by keeping a separate set of transition values
 *	  for each grouping set being concurrently processed; for each input tuple
 *	  we update them all, and on group boundaries we reset those states
 *	  (starting at the front of the list) whose grouping values have changed
 *	  (the list of grouping sets is ordered from most specific to least
 *	  specific).
 *
 *	  Where more complex grouping sets are used, we break them down into
 *	  "phases", where each phase has a different sort order (except phase 0
 *	  which is reserved for hashing).  During each phase but the last, the
 *	  input tuples are additionally stored in a tuplesort which is keyed to the
 *	  next phase's sort order; during each phase but the first, the input
 *	  tuples are drawn from the previously sorted data.  (The sorting of the
 *	  data for the first phase is handled by the planner, as it might be
 *	  satisfied by underlying nodes.)
 *
 *	  Hashing can be mixed with sorted grouping.  To do this, we have an
 *	  AGG_MIXED strategy that populates the hashtables during the first sorted
 *	  phase, and switches to reading them out after completing all sort phases.
 *	  We can also support AGG_HASHED with multiple hash tables and no sorting
 *	  at all.
 *
 *	  From the perspective of aggregate transition and final functions, the
 *	  only issue regarding grouping sets is this: a single call site (flinfo)
 *	  of an aggregate function may be used for updating several different
 *	  transition values in turn. So the function must not cache in the flinfo
 *	  anything which logically belongs as part of the transition value (most
 *	  importantly, the memory context in which the transition value exists).
 *	  The support API functions (AggCheckCallContext, AggRegisterCallback) are
 *	  sensitive to the grouping set for which the aggregate function is
 *	  currently being called.
 *
 *	  Plan structure:
 *
 *	  What we get from the planner is actually one "real" Agg node which is
 *	  part of the plan tree proper, but which optionally has an additional list
 *	  of Agg nodes hung off the side via the "chain" field.  This is because an
 *	  Agg node happens to be a convenient representation of all the data we
 *	  need for grouping sets.
 *
 *	  For many purposes, we treat the "real" node as if it were just the first
 *	  node in the chain.  The chain must be ordered such that hashed entries
 *	  come before sorted/plain entries; the real node is marked AGG_MIXED if
 *	  there are both types present (in which case the real node describes one
 *	  of the hashed groupings, other AGG_HASHED nodes may optionally follow in
 *	  the chain, followed in turn by AGG_SORTED or (one) AGG_PLAIN node).  If
 *	  the real node is marked AGG_HASHED or AGG_SORTED, then all the chained
 *	  nodes must be of the same type; if it is AGG_PLAIN, there can be no
 *	  chained nodes.
 *
 *	  We collect all hashed nodes into a single "phase", numbered 0, and create
 *	  a sorted phase (numbered 1..n) for each AGG_SORTED or AGG_PLAIN node.
 *	  Phase 0 is allocated even if there are no hashes, but remains unused in
 *	  that case.
 *
 *	  AGG_HASHED nodes actually refer to only a single grouping set each,
 *	  because for each hashed grouping we need a separate grpColIdx and
 *	  numGroups estimate.  AGG_SORTED nodes represent a "rollup", a list of
 *	  grouping sets that share a sort order.  Each AGG_SORTED node other than
 *	  the first one has an associated Sort node which describes the sort order
 *	  to be used; the first sorted node takes its input from the outer subtree,
 *	  which the planner has already arranged to provide ordered data.
 *
 *	  Memory and ExprContext usage:
 *
 *	  Because we're accumulating aggregate values across input rows, we need to
 *	  use more memory contexts than just simple input/output tuple contexts.
 *	  In fact, for a rollup, we need a separate context for each grouping set
 *	  so that we can reset the inner (finer-grained) aggregates on their group
 *	  boundaries while continuing to accumulate values for outer
 *	  (coarser-grained) groupings.  On top of this, we might be simultaneously
 *	  populating hashtables; however, we only need one context for all the
 *	  hashtables.
 *
 *	  So we create an array, aggcontexts, with an ExprContext for each grouping
 *	  set in the largest rollup that we're going to process, and use the
 *	  per-tuple memory context of those ExprContexts to store the aggregate
 *	  transition values.  hashcontext is the single context created to support
 *	  all hash tables.
 *
 *	  Spilling To Disk
 *
 *	  When performing hash aggregation, if the hash table memory exceeds the
 *	  limit (see hash_agg_check_limits()), we enter "spill mode". In spill
 *	  mode, we advance the transition states only for groups already in the
 *	  hash table. For tuples that would need to create a new hash table
 *	  entries (and initialize new transition states), we instead spill them to
 *	  disk to be processed later. The tuples are spilled in a partitioned
 *	  manner, so that subsequent batches are smaller and less likely to exceed
 *	  work_mem (if a batch does exceed work_mem, it must be spilled
 *	  recursively).
 *
 *	  Spilled data is written to logical tapes. These provide better control
 *	  over memory usage, disk space, and the number of files than if we were
 *	  to use a BufFile for each spill.
 *
 *	  Note that it's possible for transition states to start small but then
 *	  grow very large; for instance in the case of ARRAY_AGG. In such cases,
 *	  it's still possible to significantly exceed work_mem. We try to avoid
 *	  this situation by estimating what will fit in the available memory, and
 *	  imposing a limit on the number of groups separately from the amount of
 *	  memory consumed.
 *
 *    Transition / Combine function invocation:
 *
 *    For performance reasons transition functions, including combine
 *    functions, aren't invoked one-by-one from nodeAgg.c after computing
 *    arguments using the expression evaluation engine. Instead
 *    ExecBuildAggTrans() builds one large expression that does both argument
 *    evaluation and transition function invocation. That avoids performance
 *    issues due to repeated uses of expression evaluation, complications due
 *    to filter expressions having to be evaluated early, and allows to JIT
 *    the entire expression into one native function.
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeAgg.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "executor/execExpr.h"
#include "executor/executor.h"
#include "executor/nodeAgg.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pathnodes.h"
#include "optimizer/optimizer.h"
#include "parser/parse_agg.h"
#include "parser/parse_coerce.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/dynahash.h"
#include "utils/expandeddatum.h"
#include "utils/logtape.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/syscache.h"
#include "utils/tuplesort.h"

/*
 * Control how many partitions are created when spilling HashAgg to
 * disk.
 *
 * HASHAGG_PARTITION_FACTOR is multiplied by the estimated number of
 * partitions needed such that each partition will fit in memory. The factor
 * is set higher than one because there's not a high cost to having a few too
 * many partitions, and it makes it less likely that a partition will need to
 * be spilled recursively. Another benefit of having more, smaller partitions
 * is that small hash tables may perform better than large ones due to memory
 * caching effects.
 *
 * We also specify a min and max number of partitions per spill. Too few might
 * mean a lot of wasted I/O from repeated spilling of the same tuples. Too
 * many will result in lots of memory wasted buffering the spill files (which
 * could instead be spent on a larger hash table).
 */
#define HASHAGG_PARTITION_FACTOR 1.50
#define HASHAGG_MIN_PARTITIONS 4
#define HASHAGG_MAX_PARTITIONS 1024

/*
 * For reading from tapes, the buffer size must be a multiple of
 * BLCKSZ. Larger values help when reading from multiple tapes concurrently,
 * but that doesn't happen in HashAgg, so we simply use BLCKSZ. Writing to a
 * tape always uses a buffer of size BLCKSZ.
 */
#define HASHAGG_READ_BUFFER_SIZE BLCKSZ
#define HASHAGG_WRITE_BUFFER_SIZE BLCKSZ

/* minimum number of initial hash table buckets */
#define HASHAGG_MIN_BUCKETS 256

/*
 * Track all tapes needed for a HashAgg that spills. We don't know the maximum
 * number of tapes needed at the start of the algorithm (because it can
 * recurse), so one tape set is allocated and extended as needed for new
 * tapes. When a particular tape is already read, rewind it for write mode and
 * put it in the free list.
 *
 * Tapes' buffers can take up substantial memory when many tapes are open at
 * once. We only need one tape open at a time in read mode (using a buffer
 * that's a multiple of BLCKSZ); but we need one tape open in write mode (each
 * requiring a buffer of size BLCKSZ) for each partition.
 */
typedef struct HashTapeInfo
{
	LogicalTapeSet	*tapeset;
	int				 ntapes;
	int				*freetapes;
	int				 nfreetapes;
	int				 freetapes_alloc;
} HashTapeInfo;

/*
 * Represents partitioned spill data for a single hashtable. Contains the
 * necessary information to route tuples to the correct partition, and to
 * transform the spilled data into new batches.
 *
 * The high bits are used for partition selection (when recursing, we ignore
 * the bits that have already been used for partition selection at an earlier
 * level).
 */
typedef struct HashAggSpill
{
	LogicalTapeSet *tapeset;	/* borrowed reference to tape set */
	int		 npartitions;		/* number of partitions */
	int		*partitions;		/* spill partition tape numbers */
	int64   *ntuples;			/* number of tuples in each partition */
	uint32   mask;				/* mask to find partition from hash value */
	int      shift;				/* after masking, shift by this amount */
} HashAggSpill;

/*
 * Represents work to be done for one pass of hash aggregation (with only one
 * grouping set).
 *
 * Also tracks the bits of the hash already used for partition selection by
 * earlier iterations, so that this batch can use new bits. If all bits have
 * already been used, no partitioning will be done (any spilled data will go
 * to a single output tape).
 */
typedef struct HashAggBatch
{
	int				 phaseidx;		/* phase that own this batch */
	int				 used_bits;		/* number of bits of hash already used */
	LogicalTapeSet	*tapeset;		/* borrowed reference to tape set */
	int				 input_tapenum;	/* input partition tape */
	int64			 input_tuples;	/* number of tuples in this batch */
} HashAggBatch;

/*
 * Represents different stages of hash aggregate.
 *
 * HASHAGG_INITIAL: initial stage for hash aggregate, allow to do all hash
 * transitions in one expression, get input tuples from outer node and all
 * hash entries are filled. 
 *
 * HASHAGG_SPILL: enter hash spill mode, allow to do all hash transitions
 * in one expression, still get input tuples from outer node but some hash
 * entries might not be filled, so a null check is added into the transitions
 * expression.
 *
 * HASHAGG_REFILL: refilling the hash table group set by group set, disallow
 * doing all hash transitions in one expression, get input tuples from spill
 * files, only one hash entry is filled, we may reenter hash spill mode when
 * refilling the hash table, but the transition expression is not called if
 * the hash entry is not filled, so null check is not added in the transition
 * expression.
 */
typedef enum HashAggStage
{
	HASHAGG_INITIAL = 0,
	HASHAGG_SPILL,
	HASHAGG_REFILL,
} HashAggStage;

static void select_current_set(AggState *aggstate, int setno, bool is_hash);
static void initialize_phase(AggState *aggstate, int newphase);
static TupleTableSlot *fetch_input_tuple(AggState *aggstate);
static void initialize_aggregates(AggState *aggstate,
								  AggStatePerGroup *pergroups,
								  int numReset);
static void advance_transition_function(AggState *aggstate,
										AggStatePerTrans pertrans,
										AggStatePerGroup pergroupstate);
static void advance_aggregates(AggState *aggstate);
static void process_ordered_aggregate_single(AggState *aggstate,
											 AggStatePerTrans pertrans,
											 AggStatePerGroup pergroupstate);
static void process_ordered_aggregate_multi(AggState *aggstate,
											AggStatePerTrans pertrans,
											AggStatePerGroup pergroupstate);
static void finalize_aggregate(AggState *aggstate,
							   AggStatePerAgg peragg,
							   AggStatePerGroup pergroupstate,
							   Datum *resultVal, bool *resultIsNull);
static void finalize_partialaggregate(AggState *aggstate,
									  AggStatePerAgg peragg,
									  AggStatePerGroup pergroupstate,
									  Datum *resultVal, bool *resultIsNull);
static void prepare_hash_slot(AggState *aggstate, AggStatePerPhaseHash perhash);
static void prepare_projection_slot(AggState *aggstate,
									TupleTableSlot *slot,
									int currentSet);
static void finalize_aggregates(AggState *aggstate,
								AggStatePerAgg peragg,
								AggStatePerGroup pergroup);
static TupleTableSlot *project_aggregates(AggState *aggstate);
static Bitmapset *find_unaggregated_cols(AggState *aggstate);
static bool find_unaggregated_cols_walker(Node *node, Bitmapset **colnos);
static void build_hash_tables(AggState *aggstate);
static void build_hash_table(AggState *aggstate,
							 AggStatePerPhaseHash perhash, long nbuckets);
static void hashagg_recompile_expressions(AggState *aggstate);
static long hash_choose_num_buckets(double hashentrysize,
									long estimated_nbuckets,
									Size memory);
static int hash_choose_num_partitions(uint64 input_groups,
									  double hashentrysize,
									  int used_bits,
									  int *log2_npartittions);
static AggStatePerGroup lookup_hash_entry(AggState *aggstate,
										  AggStatePerPhaseHash perhash,
										  uint32 hash,
										  bool *in_hash_table);
static void lookup_hash_entries(AggState *aggstate,
								AggStatePerPhaseHash current_hash,
								List *concurrent_hashes);
static TupleTableSlot *agg_retrieve_direct(AggState *aggstate);
static void agg_fill_hash_table(AggState *aggstate);
static bool agg_refill_hash_table(AggState *aggstate);
static void agg_sort_input(AggState *aggstate);
static TupleTableSlot *agg_retrieve_hash_table(AggState *aggstate);
static TupleTableSlot *agg_retrieve_hash_table_in_memory(AggState *aggstate);
static void hash_agg_check_limits(AggState *aggstate);
static void hash_agg_enter_spill_mode(AggState *aggstate);
static void hash_agg_update_metrics(AggState *aggstate, bool from_tape,
									int npartitions);
static void hashagg_finish_initial_spills(AggState *aggstate);
static void hashagg_reset_spill_state(AggState *aggstate);
static HashAggBatch *hashagg_batch_new(LogicalTapeSet *tapeset,
									   int input_tapenum, int phaseidx,
									   int64 input_tuples, int used_bits);
static MinimalTuple hashagg_batch_read(HashAggBatch *batch, uint32 *hashp);
static void hashagg_spill_init(HashAggSpill *spill, HashTapeInfo *tapeinfo,
							   int used_bits, uint64 input_tuples,
							   double hashentrysize);
static Size hashagg_spill_tuple(HashAggSpill *spill, TupleTableSlot *slot,
								uint32 hash);
static void hashagg_spill_finish(AggState *aggstate, HashAggSpill *spill,
								 int phaseidx);
static void hashagg_tapeinfo_init(AggState *aggstate);
static void hashagg_tapeinfo_assign(HashTapeInfo *tapeinfo, int *dest,
									int ndest);
static void hashagg_tapeinfo_release(HashTapeInfo *tapeinfo, int tapenum);
static Datum GetAggInitVal(Datum textInitVal, Oid transtype);
static void build_pertrans_for_aggref(AggStatePerTrans pertrans,
									  AggState *aggstate, EState *estate,
									  Aggref *aggref, Oid aggtransfn, Oid aggtranstype,
									  Oid aggserialfn, Oid aggdeserialfn,
									  Datum initValue, bool initValueIsNull,
									  Oid *inputTypes, int numArguments);
static int	find_compatible_peragg(Aggref *newagg, AggState *aggstate,
								   int lastaggno, List **same_input_transnos);
static int	find_compatible_pertrans(AggState *aggstate, Aggref *newagg,
									 bool shareable,
									 Oid aggtransfn, Oid aggtranstype,
									 Oid aggserialfn, Oid aggdeserialfn,
									 Datum initValue, bool initValueIsNull,
									 List *transnos);


/*
 * Select the current grouping set; affects current_set and
 * curaggcontext.
 */
static void
select_current_set(AggState *aggstate, int setno, bool is_hash)
{
	/*
	 * When changing this, also adapt ExecAggPlainTransByVal() and
	 * ExecAggPlainTransByRef().
	 */
	if (is_hash)
	{
		Assert(setno == 0);
		aggstate->curaggcontext = aggstate->hashcontext;
	}
	else
		aggstate->curaggcontext = aggstate->aggcontexts[setno];

	aggstate->current_set = setno;
}

/*
 * Switch to phase "newphase", which must either be 0 (to reset) or
 * current_phase + 1. Juggle the tuplesorts accordingly.
 */
static void
initialize_phase(AggState *aggstate, int newphase)
{
	AggStatePerPhase current_phase;
	AggStatePerPhaseSort persort;

	/* Don't use aggstate->phase here, it might not be initialized yet*/
	current_phase = aggstate->phases[aggstate->current_phase];

	/*
	 * Whatever the previous state, we're now done with whatever input
	 * tuplesort was in use, cleanup them.
	 *
	 * Note: we keep the first tuplesort/tuplestore, this will benifit the
	 * rescan in some cases without resorting the input again.
	 */
	if (!current_phase->is_hashed && aggstate->current_phase > 0)
	{
		persort = (AggStatePerPhaseSort) current_phase;
		if (persort->sort_in)
		{
			tuplesort_end(persort->sort_in);
			persort->sort_in = NULL;
		}
	}

	/* advance to next phase */
	aggstate->current_phase = newphase;
	aggstate->phase = aggstate->phases[newphase];

	if (aggstate->phase->is_hashed)
		return;

	/* New phase is not hashed */
	persort = (AggStatePerPhaseSort) aggstate->phase;

	/* This is the right time to actually sort it. */
	if (persort->sort_in)
		tuplesort_performsort(persort->sort_in);

	/*
	 * If copy_out is set, we need to sort appropriately for the
	 * next phase in sequence.
	 */
	if (persort->copy_out)
	{
		AggStatePerPhaseSort next =
			(AggStatePerPhaseSort) aggstate->phases[newphase + 1];
		Sort *sortnode = (Sort *) next->phasedata.aggnode->sortnode;
		PlanState *outerNode = outerPlanState(aggstate);
		TupleDesc tupDesc = ExecGetResultType(outerNode);

		Assert(!next->phasedata.is_hashed);

		if (!next->sort_in)
			next->sort_in = tuplesort_begin_heap(tupDesc,
												 sortnode->numCols,
												 sortnode->sortColIdx,
												 sortnode->sortOperators,
												 sortnode->collations,
												 sortnode->nullsFirst,
												 work_mem,
												 NULL, false);
	}
}

/*
 * Fetch a tuple from either the outer plan (for phase 1) or from the sorter
 * populated by the previous phase.  Copy it to the sorter for the next phase
 * if any.
 *
 * Callers cannot rely on memory for tuple in returned slot remaining valid
 * past any subsequently fetched tuple.
 */
static TupleTableSlot *
fetch_input_tuple(AggState *aggstate)
{
	TupleTableSlot *slot;
	AggStatePerPhaseSort current_phase;

	Assert(!aggstate->phase->is_hashed);
	current_phase = (AggStatePerPhaseSort) aggstate->phase;

	if (current_phase->sort_in)
	{
		/* make sure we check for interrupts in either path through here */
		CHECK_FOR_INTERRUPTS();
		if (!tuplesort_gettupleslot(current_phase->sort_in, true, false,
									aggstate->sort_slot, NULL))
			return NULL;
		slot = aggstate->sort_slot;
	}
	else
		slot = ExecProcNode(outerPlanState(aggstate));

	if (!TupIsNull(slot) && current_phase->copy_out)
	{
		AggStatePerPhaseSort next =
			(AggStatePerPhaseSort) aggstate->phases[aggstate->current_phase + 1];
		Assert(!next->phasedata.is_hashed);
		tuplesort_puttupleslot(next->sort_in, slot);
	}

	return slot;
}

/*
 * (Re)Initialize an individual aggregate.
 *
 * This function handles only one grouping set, already set in
 * aggstate->current_set.
 *
 * When called, CurrentMemoryContext should be the per-query context.
 */
static void
initialize_aggregate(AggState *aggstate, AggStatePerTrans pertrans,
					 AggStatePerGroup pergroupstate)
{
	/*
	 * Start a fresh sort operation for each DISTINCT/ORDER BY aggregate.
	 */
	if (pertrans->numSortCols > 0)
	{
		/*
		 * In case of rescan, maybe there could be an uncompleted sort
		 * operation?  Clean it up if so.
		 */
		if (pertrans->sortstates[aggstate->current_set])
			tuplesort_end(pertrans->sortstates[aggstate->current_set]);


		/*
		 * We use a plain Datum sorter when there's a single input column;
		 * otherwise sort the full tuple.  (See comments for
		 * process_ordered_aggregate_single.)
		 */
		if (pertrans->numInputs == 1)
		{
			Form_pg_attribute attr = TupleDescAttr(pertrans->sortdesc, 0);

			pertrans->sortstates[aggstate->current_set] =
				tuplesort_begin_datum(attr->atttypid,
									  pertrans->sortOperators[0],
									  pertrans->sortCollations[0],
									  pertrans->sortNullsFirst[0],
									  work_mem, NULL, false);
		}
		else
			pertrans->sortstates[aggstate->current_set] =
				tuplesort_begin_heap(pertrans->sortdesc,
									 pertrans->numSortCols,
									 pertrans->sortColIdx,
									 pertrans->sortOperators,
									 pertrans->sortCollations,
									 pertrans->sortNullsFirst,
									 work_mem, NULL, false);
	}

	/*
	 * (Re)set transValue to the initial value.
	 *
	 * Note that when the initial value is pass-by-ref, we must copy it (into
	 * the aggcontext) since we will pfree the transValue later.
	 */
	if (pertrans->initValueIsNull)
		pergroupstate->transValue = pertrans->initValue;
	else
	{
		MemoryContext oldContext;

		oldContext = MemoryContextSwitchTo(aggstate->curaggcontext->ecxt_per_tuple_memory);
		pergroupstate->transValue = datumCopy(pertrans->initValue,
											  pertrans->transtypeByVal,
											  pertrans->transtypeLen);
		MemoryContextSwitchTo(oldContext);
	}
	pergroupstate->transValueIsNull = pertrans->initValueIsNull;

	/*
	 * If the initial value for the transition state doesn't exist in the
	 * pg_aggregate table then we will let the first non-NULL value returned
	 * from the outer procNode become the initial value. (This is useful for
	 * aggregates like max() and min().) The noTransValue flag signals that we
	 * still need to do this.
	 */
	pergroupstate->noTransValue = pertrans->initValueIsNull;
}

/*
 * Initialize all aggregate transition states for a new group of input values.
 *
 * If there are multiple grouping sets, we initialize only the first numReset
 * of them (the grouping sets are ordered so that the most specific one, which
 * is reset most often, is first). As a convenience, if numReset is 0, we
 * reinitialize all sets.
 *
 * NB: This cannot be used for hash aggregates, as for those the grouping set
 * number has to be specified from further up.
 *
 * When called, CurrentMemoryContext should be the per-query context.
 */
static void
initialize_aggregates(AggState *aggstate,
					  AggStatePerGroup *pergroups,
					  int numReset)
{
	int			transno;
	int			numGroupingSets = aggstate->phase->numsets;
	int			setno = 0;
	int			numTrans = aggstate->numtrans;
	AggStatePerTrans transstates = aggstate->pertrans;

	if (numReset == 0)
		numReset = numGroupingSets;

	for (setno = 0; setno < numReset; setno++)
	{
		AggStatePerGroup pergroup = pergroups[setno];

		select_current_set(aggstate, setno, false);

		for (transno = 0; transno < numTrans; transno++)
		{
			AggStatePerTrans pertrans = &transstates[transno];
			AggStatePerGroup pergroupstate = &pergroup[transno];

			initialize_aggregate(aggstate, pertrans, pergroupstate);
		}
	}
}

/*
 * Given new input value(s), advance the transition function of one aggregate
 * state within one grouping set only (already set in aggstate->current_set)
 *
 * The new values (and null flags) have been preloaded into argument positions
 * 1 and up in pertrans->transfn_fcinfo, so that we needn't copy them again to
 * pass to the transition function.  We also expect that the static fields of
 * the fcinfo are already initialized; that was done by ExecInitAgg().
 *
 * It doesn't matter which memory context this is called in.
 */
static void
advance_transition_function(AggState *aggstate,
							AggStatePerTrans pertrans,
							AggStatePerGroup pergroupstate)
{
	FunctionCallInfo fcinfo = pertrans->transfn_fcinfo;
	MemoryContext oldContext;
	Datum		newVal;

	if (pertrans->transfn.fn_strict)
	{
		/*
		 * For a strict transfn, nothing happens when there's a NULL input; we
		 * just keep the prior transValue.
		 */
		int			numTransInputs = pertrans->numTransInputs;
		int			i;

		for (i = 1; i <= numTransInputs; i++)
		{
			if (fcinfo->args[i].isnull)
				return;
		}
		if (pergroupstate->noTransValue)
		{
			/*
			 * transValue has not been initialized. This is the first non-NULL
			 * input value. We use it as the initial value for transValue. (We
			 * already checked that the agg's input type is binary-compatible
			 * with its transtype, so straight copy here is OK.)
			 *
			 * We must copy the datum into aggcontext if it is pass-by-ref. We
			 * do not need to pfree the old transValue, since it's NULL.
			 */
			oldContext = MemoryContextSwitchTo(aggstate->curaggcontext->ecxt_per_tuple_memory);
			pergroupstate->transValue = datumCopy(fcinfo->args[1].value,
												  pertrans->transtypeByVal,
												  pertrans->transtypeLen);
			pergroupstate->transValueIsNull = false;
			pergroupstate->noTransValue = false;
			MemoryContextSwitchTo(oldContext);
			return;
		}
		if (pergroupstate->transValueIsNull)
		{
			/*
			 * Don't call a strict function with NULL inputs.  Note it is
			 * possible to get here despite the above tests, if the transfn is
			 * strict *and* returned a NULL on a prior cycle. If that happens
			 * we will propagate the NULL all the way to the end.
			 */
			return;
		}
	}

	/* We run the transition functions in per-input-tuple memory context */
	oldContext = MemoryContextSwitchTo(aggstate->tmpcontext->ecxt_per_tuple_memory);

	/* set up aggstate->curpertrans for AggGetAggref() */
	aggstate->curpertrans = pertrans;

	/*
	 * OK to call the transition function
	 */
	fcinfo->args[0].value = pergroupstate->transValue;
	fcinfo->args[0].isnull = pergroupstate->transValueIsNull;
	fcinfo->isnull = false;		/* just in case transfn doesn't set it */

	newVal = FunctionCallInvoke(fcinfo);

	aggstate->curpertrans = NULL;

	/*
	 * If pass-by-ref datatype, must copy the new value into aggcontext and
	 * free the prior transValue.  But if transfn returned a pointer to its
	 * first input, we don't need to do anything.  Also, if transfn returned a
	 * pointer to a R/W expanded object that is already a child of the
	 * aggcontext, assume we can adopt that value without copying it.
	 *
	 * It's safe to compare newVal with pergroup->transValue without
	 * regard for either being NULL, because ExecAggTransReparent()
	 * takes care to set transValue to 0 when NULL. Otherwise we could
	 * end up accidentally not reparenting, when the transValue has
	 * the same numerical value as newValue, despite being NULL.  This
	 * is a somewhat hot path, making it undesirable to instead solve
	 * this with another branch for the common case of the transition
	 * function returning its (modified) input argument.
	 */
	if (!pertrans->transtypeByVal &&
		DatumGetPointer(newVal) != DatumGetPointer(pergroupstate->transValue))
		newVal = ExecAggTransReparent(aggstate, pertrans,
									  newVal, fcinfo->isnull,
									  pergroupstate->transValue,
									  pergroupstate->transValueIsNull);

	pergroupstate->transValue = newVal;
	pergroupstate->transValueIsNull = fcinfo->isnull;

	MemoryContextSwitchTo(oldContext);
}

/*
 * Advance each aggregate transition state for one input tuple.  The input
 * tuple has been stored in tmpcontext->ecxt_outertuple, so that it is
 * accessible to ExecEvalExpr.
 *
 * We have two sets of transition states to handle: one for sorted aggregation
 * and one for hashed; we do them both here, to avoid multiple evaluation of
 * the inputs.
 *
 * When called, CurrentMemoryContext should be the per-query context.
 */
static void
advance_aggregates(AggState *aggstate)
{
	bool		dummynull;

	ExecEvalExprSwitchContext(aggstate->phase->evaltrans,
							  aggstate->tmpcontext,
							  &dummynull);
}

/*
 * Run the transition function for a DISTINCT or ORDER BY aggregate
 * with only one input.  This is called after we have completed
 * entering all the input values into the sort object.  We complete the
 * sort, read out the values in sorted order, and run the transition
 * function on each value (applying DISTINCT if appropriate).
 *
 * Note that the strictness of the transition function was checked when
 * entering the values into the sort, so we don't check it again here;
 * we just apply standard SQL DISTINCT logic.
 *
 * The one-input case is handled separately from the multi-input case
 * for performance reasons: for single by-value inputs, such as the
 * common case of count(distinct id), the tuplesort_getdatum code path
 * is around 300% faster.  (The speedup for by-reference types is less
 * but still noticeable.)
 *
 * This function handles only one grouping set (already set in
 * aggstate->current_set).
 *
 * When called, CurrentMemoryContext should be the per-query context.
 */
static void
process_ordered_aggregate_single(AggState *aggstate,
								 AggStatePerTrans pertrans,
								 AggStatePerGroup pergroupstate)
{
	Datum		oldVal = (Datum) 0;
	bool		oldIsNull = true;
	bool		haveOldVal = false;
	MemoryContext workcontext = aggstate->tmpcontext->ecxt_per_tuple_memory;
	MemoryContext oldContext;
	bool		isDistinct = (pertrans->numDistinctCols > 0);
	Datum		newAbbrevVal = (Datum) 0;
	Datum		oldAbbrevVal = (Datum) 0;
	FunctionCallInfo fcinfo = pertrans->transfn_fcinfo;
	Datum	   *newVal;
	bool	   *isNull;

	Assert(pertrans->numDistinctCols < 2);

	tuplesort_performsort(pertrans->sortstates[aggstate->current_set]);

	/* Load the column into argument 1 (arg 0 will be transition value) */
	newVal = &fcinfo->args[1].value;
	isNull = &fcinfo->args[1].isnull;

	/*
	 * Note: if input type is pass-by-ref, the datums returned by the sort are
	 * freshly palloc'd in the per-query context, so we must be careful to
	 * pfree them when they are no longer needed.
	 */

	while (tuplesort_getdatum(pertrans->sortstates[aggstate->current_set],
							  true, newVal, isNull, &newAbbrevVal))
	{
		/*
		 * Clear and select the working context for evaluation of the equality
		 * function and transition function.
		 */
		MemoryContextReset(workcontext);
		oldContext = MemoryContextSwitchTo(workcontext);

		/*
		 * If DISTINCT mode, and not distinct from prior, skip it.
		 */
		if (isDistinct &&
			haveOldVal &&
			((oldIsNull && *isNull) ||
			 (!oldIsNull && !*isNull &&
			  oldAbbrevVal == newAbbrevVal &&
			  DatumGetBool(FunctionCall2Coll(&pertrans->equalfnOne,
											 pertrans->aggCollation,
											 oldVal, *newVal)))))
		{
			/* equal to prior, so forget this one */
			if (!pertrans->inputtypeByVal && !*isNull)
				pfree(DatumGetPointer(*newVal));
		}
		else
		{
			advance_transition_function(aggstate, pertrans, pergroupstate);
			/* forget the old value, if any */
			if (!oldIsNull && !pertrans->inputtypeByVal)
				pfree(DatumGetPointer(oldVal));
			/* and remember the new one for subsequent equality checks */
			oldVal = *newVal;
			oldAbbrevVal = newAbbrevVal;
			oldIsNull = *isNull;
			haveOldVal = true;
		}

		MemoryContextSwitchTo(oldContext);
	}

	if (!oldIsNull && !pertrans->inputtypeByVal)
		pfree(DatumGetPointer(oldVal));

	tuplesort_end(pertrans->sortstates[aggstate->current_set]);
	pertrans->sortstates[aggstate->current_set] = NULL;
}

/*
 * Run the transition function for a DISTINCT or ORDER BY aggregate
 * with more than one input.  This is called after we have completed
 * entering all the input values into the sort object.  We complete the
 * sort, read out the values in sorted order, and run the transition
 * function on each value (applying DISTINCT if appropriate).
 *
 * This function handles only one grouping set (already set in
 * aggstate->current_set).
 *
 * When called, CurrentMemoryContext should be the per-query context.
 */
static void
process_ordered_aggregate_multi(AggState *aggstate,
								AggStatePerTrans pertrans,
								AggStatePerGroup pergroupstate)
{
	ExprContext *tmpcontext = aggstate->tmpcontext;
	FunctionCallInfo fcinfo = pertrans->transfn_fcinfo;
	TupleTableSlot *slot1 = pertrans->sortslot;
	TupleTableSlot *slot2 = pertrans->uniqslot;
	int			numTransInputs = pertrans->numTransInputs;
	int			numDistinctCols = pertrans->numDistinctCols;
	Datum		newAbbrevVal = (Datum) 0;
	Datum		oldAbbrevVal = (Datum) 0;
	bool		haveOldValue = false;
	TupleTableSlot *save = aggstate->tmpcontext->ecxt_outertuple;
	int			i;

	tuplesort_performsort(pertrans->sortstates[aggstate->current_set]);

	ExecClearTuple(slot1);
	if (slot2)
		ExecClearTuple(slot2);

	while (tuplesort_gettupleslot(pertrans->sortstates[aggstate->current_set],
								  true, true, slot1, &newAbbrevVal))
	{
		CHECK_FOR_INTERRUPTS();

		tmpcontext->ecxt_outertuple = slot1;
		tmpcontext->ecxt_innertuple = slot2;

		if (numDistinctCols == 0 ||
			!haveOldValue ||
			newAbbrevVal != oldAbbrevVal ||
			!ExecQual(pertrans->equalfnMulti, tmpcontext))
		{
			/*
			 * Extract the first numTransInputs columns as datums to pass to
			 * the transfn.
			 */
			slot_getsomeattrs(slot1, numTransInputs);

			/* Load values into fcinfo */
			/* Start from 1, since the 0th arg will be the transition value */
			for (i = 0; i < numTransInputs; i++)
			{
				fcinfo->args[i + 1].value = slot1->tts_values[i];
				fcinfo->args[i + 1].isnull = slot1->tts_isnull[i];
			}

			advance_transition_function(aggstate, pertrans, pergroupstate);

			if (numDistinctCols > 0)
			{
				/* swap the slot pointers to retain the current tuple */
				TupleTableSlot *tmpslot = slot2;

				slot2 = slot1;
				slot1 = tmpslot;
				/* avoid ExecQual() calls by reusing abbreviated keys */
				oldAbbrevVal = newAbbrevVal;
				haveOldValue = true;
			}
		}

		/* Reset context each time */
		ResetExprContext(tmpcontext);

		ExecClearTuple(slot1);
	}

	if (slot2)
		ExecClearTuple(slot2);

	tuplesort_end(pertrans->sortstates[aggstate->current_set]);
	pertrans->sortstates[aggstate->current_set] = NULL;

	/* restore previous slot, potentially in use for grouping sets */
	tmpcontext->ecxt_outertuple = save;
}

/*
 * Compute the final value of one aggregate.
 *
 * This function handles only one grouping set (already set in
 * aggstate->current_set).
 *
 * The finalfn will be run, and the result delivered, in the
 * output-tuple context; caller's CurrentMemoryContext does not matter.
 *
 * The finalfn uses the state as set in the transno. This also might be
 * being used by another aggregate function, so it's important that we do
 * nothing destructive here.
 */
static void
finalize_aggregate(AggState *aggstate,
				   AggStatePerAgg peragg,
				   AggStatePerGroup pergroupstate,
				   Datum *resultVal, bool *resultIsNull)
{
	LOCAL_FCINFO(fcinfo, FUNC_MAX_ARGS);
	bool		anynull = false;
	MemoryContext oldContext;
	int			i;
	ListCell   *lc;
	AggStatePerTrans pertrans = &aggstate->pertrans[peragg->transno];

	oldContext = MemoryContextSwitchTo(aggstate->ss.ps.ps_ExprContext->ecxt_per_tuple_memory);

	/*
	 * Evaluate any direct arguments.  We do this even if there's no finalfn
	 * (which is unlikely anyway), so that side-effects happen as expected.
	 * The direct arguments go into arg positions 1 and up, leaving position 0
	 * for the transition state value.
	 */
	i = 1;
	foreach(lc, peragg->aggdirectargs)
	{
		ExprState  *expr = (ExprState *) lfirst(lc);

		fcinfo->args[i].value = ExecEvalExpr(expr,
											 aggstate->ss.ps.ps_ExprContext,
											 &fcinfo->args[i].isnull);
		anynull |= fcinfo->args[i].isnull;
		i++;
	}

	/*
	 * Apply the agg's finalfn if one is provided, else return transValue.
	 */
	if (OidIsValid(peragg->finalfn_oid))
	{
		int			numFinalArgs = peragg->numFinalArgs;

		/* set up aggstate->curperagg for AggGetAggref() */
		aggstate->curperagg = peragg;

		InitFunctionCallInfoData(*fcinfo, &peragg->finalfn,
								 numFinalArgs,
								 pertrans->aggCollation,
								 (void *) aggstate, NULL);

		/* Fill in the transition state value */
		fcinfo->args[0].value =
			MakeExpandedObjectReadOnly(pergroupstate->transValue,
									   pergroupstate->transValueIsNull,
									   pertrans->transtypeLen);
		fcinfo->args[0].isnull = pergroupstate->transValueIsNull;
		anynull |= pergroupstate->transValueIsNull;

		/* Fill any remaining argument positions with nulls */
		for (; i < numFinalArgs; i++)
		{
			fcinfo->args[i].value = (Datum) 0;
			fcinfo->args[i].isnull = true;
			anynull = true;
		}

		if (fcinfo->flinfo->fn_strict && anynull)
		{
			/* don't call a strict function with NULL inputs */
			*resultVal = (Datum) 0;
			*resultIsNull = true;
		}
		else
		{
			*resultVal = FunctionCallInvoke(fcinfo);
			*resultIsNull = fcinfo->isnull;
		}
		aggstate->curperagg = NULL;
	}
	else
	{
		/* Don't need MakeExpandedObjectReadOnly; datumCopy will copy it */
		*resultVal = pergroupstate->transValue;
		*resultIsNull = pergroupstate->transValueIsNull;
	}

	/*
	 * If result is pass-by-ref, make sure it is in the right context.
	 */
	if (!peragg->resulttypeByVal && !*resultIsNull &&
		!MemoryContextContains(CurrentMemoryContext,
							   DatumGetPointer(*resultVal)))
		*resultVal = datumCopy(*resultVal,
							   peragg->resulttypeByVal,
							   peragg->resulttypeLen);

	MemoryContextSwitchTo(oldContext);
}

/*
 * Compute the output value of one partial aggregate.
 *
 * The serialization function will be run, and the result delivered, in the
 * output-tuple context; caller's CurrentMemoryContext does not matter.
 */
static void
finalize_partialaggregate(AggState *aggstate,
						  AggStatePerAgg peragg,
						  AggStatePerGroup pergroupstate,
						  Datum *resultVal, bool *resultIsNull)
{
	AggStatePerTrans pertrans = &aggstate->pertrans[peragg->transno];
	MemoryContext oldContext;

	oldContext = MemoryContextSwitchTo(aggstate->ss.ps.ps_ExprContext->ecxt_per_tuple_memory);

	/*
	 * serialfn_oid will be set if we must serialize the transvalue before
	 * returning it
	 */
	if (OidIsValid(pertrans->serialfn_oid))
	{
		/* Don't call a strict serialization function with NULL input. */
		if (pertrans->serialfn.fn_strict && pergroupstate->transValueIsNull)
		{
			*resultVal = (Datum) 0;
			*resultIsNull = true;
		}
		else
		{
			FunctionCallInfo fcinfo = pertrans->serialfn_fcinfo;

			fcinfo->args[0].value =
				MakeExpandedObjectReadOnly(pergroupstate->transValue,
										   pergroupstate->transValueIsNull,
										   pertrans->transtypeLen);
			fcinfo->args[0].isnull = pergroupstate->transValueIsNull;

			*resultVal = FunctionCallInvoke(fcinfo);
			*resultIsNull = fcinfo->isnull;
		}
	}
	else
	{
		/* Don't need MakeExpandedObjectReadOnly; datumCopy will copy it */
		*resultVal = pergroupstate->transValue;
		*resultIsNull = pergroupstate->transValueIsNull;
	}

	/* If result is pass-by-ref, make sure it is in the right context. */
	if (!peragg->resulttypeByVal && !*resultIsNull &&
		!MemoryContextContains(CurrentMemoryContext,
							   DatumGetPointer(*resultVal)))
		*resultVal = datumCopy(*resultVal,
							   peragg->resulttypeByVal,
							   peragg->resulttypeLen);

	MemoryContextSwitchTo(oldContext);
}

/*
 * Extract the attributes that make up the grouping key into the
 * hashslot. This is necessary to compute the hash or perform a lookup.
 */
static void
prepare_hash_slot(AggState *aggstate, AggStatePerPhaseHash perhash)
{
	TupleTableSlot *inputslot = aggstate->tmpcontext->ecxt_outertuple;
	TupleTableSlot *hashslot = perhash->hashslot;
	int				i;

	/* transfer just the needed columns into hashslot */
	slot_getsomeattrs(inputslot, perhash->largestGrpColIdx);
	ExecClearTuple(hashslot);

	for (i = 0; i < perhash->numhashGrpCols; i++)
	{
		int			varNumber = perhash->hashGrpColIdxInput[i] - 1;

		hashslot->tts_values[i] = inputslot->tts_values[varNumber];
		hashslot->tts_isnull[i] = inputslot->tts_isnull[varNumber];
	}
	ExecStoreVirtualTuple(hashslot);
}

/*
 * Prepare to finalize and project based on the specified representative tuple
 * slot and grouping set.
 *
 * In the specified tuple slot, force to null all attributes that should be
 * read as null in the context of the current grouping set.  Also stash the
 * current group bitmap where GroupingExpr can get at it.
 *
 * This relies on three conditions:
 *
 * 1) Nothing is ever going to try and extract the whole tuple from this slot,
 * only reference it in evaluations, which will only access individual
 * attributes.
 *
 * 2) No system columns are going to need to be nulled. (If a system column is
 * referenced in a group clause, it is actually projected in the outer plan
 * tlist.)
 *
 * 3) Within a given phase, we never need to recover the value of an attribute
 * once it has been set to null.
 *
 * Poking into the slot this way is a bit ugly, but the consensus is that the
 * alternative was worse.
 */
static void
prepare_projection_slot(AggState *aggstate, TupleTableSlot *slot, int currentSet)
{
	if (aggstate->phase->grouped_cols)
	{
		Bitmapset  *grouped_cols = aggstate->phase->grouped_cols[currentSet];

		aggstate->grouped_cols = grouped_cols;

		if (TTS_EMPTY(slot))
		{
			/*
			 * Force all values to be NULL if working on an empty input tuple
			 * (i.e. an empty grouping set for which no input rows were
			 * supplied).
			 */
			ExecStoreAllNullTuple(slot);
		}
		else if (aggstate->all_grouped_cols)
		{
			ListCell   *lc;

			/* all_grouped_cols is arranged in desc order */
			slot_getsomeattrs(slot, linitial_int(aggstate->all_grouped_cols));

			foreach(lc, aggstate->all_grouped_cols)
			{
				int			attnum = lfirst_int(lc);

				if (!bms_is_member(attnum, grouped_cols))
					slot->tts_isnull[attnum - 1] = true;
			}
		}
	}
}

/*
 * Compute the final value of all aggregates for one group.
 *
 * This function handles only one grouping set at a time, which the caller must
 * have selected.  It's also the caller's responsibility to adjust the supplied
 * pergroup parameter to point to the current set's transvalues.
 *
 * Results are stored in the output econtext aggvalues/aggnulls.
 */
static void
finalize_aggregates(AggState *aggstate,
					AggStatePerAgg peraggs,
					AggStatePerGroup pergroup)
{
	ExprContext *econtext = aggstate->ss.ps.ps_ExprContext;
	Datum	   *aggvalues = econtext->ecxt_aggvalues;
	bool	   *aggnulls = econtext->ecxt_aggnulls;
	int			aggno;
	int			transno;

	/*
	 * If there were any DISTINCT and/or ORDER BY aggregates, sort their
	 * inputs and run the transition functions.
	 */
	for (transno = 0; transno < aggstate->numtrans; transno++)
	{
		AggStatePerTrans pertrans = &aggstate->pertrans[transno];
		AggStatePerGroup pergroupstate;

		pergroupstate = &pergroup[transno];

		if (pertrans->numSortCols > 0)
		{
			Assert(aggstate->aggstrategy != AGG_HASHED &&
				   aggstate->aggstrategy != AGG_MIXED);

			if (pertrans->numInputs == 1)
				process_ordered_aggregate_single(aggstate,
												 pertrans,
												 pergroupstate);
			else
				process_ordered_aggregate_multi(aggstate,
												pertrans,
												pergroupstate);
		}
	}

	/*
	 * Run the final functions.
	 */
	for (aggno = 0; aggno < aggstate->numaggs; aggno++)
	{
		AggStatePerAgg peragg = &peraggs[aggno];
		int			transno = peragg->transno;
		AggStatePerGroup pergroupstate;

		pergroupstate = &pergroup[transno];

		if (DO_AGGSPLIT_SKIPFINAL(aggstate->aggsplit))
			finalize_partialaggregate(aggstate, peragg, pergroupstate,
									  &aggvalues[aggno], &aggnulls[aggno]);
		else
			finalize_aggregate(aggstate, peragg, pergroupstate,
							   &aggvalues[aggno], &aggnulls[aggno]);
	}
}

/*
 * Project the result of a group (whose aggs have already been calculated by
 * finalize_aggregates). Returns the result slot, or NULL if no row is
 * projected (suppressed by qual).
 */
static TupleTableSlot *
project_aggregates(AggState *aggstate)
{
	ExprContext *econtext = aggstate->ss.ps.ps_ExprContext;

	/*
	 * Check the qual (HAVING clause); if the group does not match, ignore it.
	 */
	if (ExecQual(aggstate->ss.ps.qual, econtext))
	{
		/*
		 * Form and return projection tuple using the aggregate results and
		 * the representative input tuple.
		 */
		return ExecProject(aggstate->ss.ps.ps_ProjInfo);
	}
	else
		InstrCountFiltered1(aggstate, 1);

	return NULL;
}

/*
 * find_unaggregated_cols
 *	  Construct a bitmapset of the column numbers of un-aggregated Vars
 *	  appearing in our targetlist and qual (HAVING clause)
 */
static Bitmapset *
find_unaggregated_cols(AggState *aggstate)
{
	Agg		   *node = (Agg *) aggstate->ss.ps.plan;
	Bitmapset  *colnos;

	colnos = NULL;
	(void) find_unaggregated_cols_walker((Node *) node->plan.targetlist,
										 &colnos);
	(void) find_unaggregated_cols_walker((Node *) node->plan.qual,
										 &colnos);
	return colnos;
}

static bool
find_unaggregated_cols_walker(Node *node, Bitmapset **colnos)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		/* setrefs.c should have set the varno to OUTER_VAR */
		Assert(var->varno == OUTER_VAR);
		Assert(var->varlevelsup == 0);
		*colnos = bms_add_member(*colnos, var->varattno);
		return false;
	}
	if (IsA(node, Aggref) ||IsA(node, GroupingFunc))
	{
		/* do not descend into aggregate exprs */
		return false;
	}
	return expression_tree_walker(node, find_unaggregated_cols_walker,
								  (void *) colnos);
}

/*
 * (Re-)initialize the hash table(s) to empty.
 *
 * To implement hashed aggregation, we need a hashtable that stores a
 * representative tuple and an array of AggStatePerGroup structs for each
 * distinct set of GROUP BY column values.  We compute the hash key from the
 * GROUP BY columns.  The per-group data is allocated in lookup_hash_entry(),
 * for each entry.
 *
 * We have a separate hashtable and associated perhash data structure for each
 * grouping set for which we're doing hashing.
 *
 * The contents of the hash tables always live in the hashcontext's per-tuple
 * memory context (there is only one of these for all tables together, since
 * they are all reset at the same time).
 */
static void
build_hash_tables(AggState *aggstate)
{
	int	phaseidx;

	for (phaseidx = 0; phaseidx < aggstate->numphases; phaseidx++)
	{
		AggStatePerPhaseHash perhash;
		AggStatePerPhase phase = aggstate->phases[phaseidx];
		long			nbuckets;
		Size			memory;

		if (!phase->is_hashed)
			continue;

		perhash = (AggStatePerPhaseHash) phase;

		if (perhash->hashtable != NULL)
		{
			ResetTupleHashTable(perhash->hashtable);
			continue;
		}

		memory = aggstate->hash_mem_limit / aggstate->num_hashes;

		/* choose reasonable number of buckets per hashtable */
		nbuckets = hash_choose_num_buckets(
			aggstate->hashentrysize, phase->aggnode->numGroups, memory);

		build_hash_table(aggstate, perhash, nbuckets);
	}

	aggstate->hash_ngroups_current = 0;
}

/*
 * Build a single hashtable for this grouping set.
 */
static void
build_hash_table(AggState *aggstate, AggStatePerPhaseHash perhash, long nbuckets)
{
	MemoryContext	metacxt = aggstate->hash_metacxt;
	MemoryContext	hashcxt = aggstate->hashcontext->ecxt_per_tuple_memory;
	MemoryContext	tmpcxt	= aggstate->tmpcontext->ecxt_per_tuple_memory;
	Size            additionalsize;

	Assert(aggstate->aggstrategy == AGG_HASHED ||
		   aggstate->aggstrategy == AGG_MIXED);

	/*
	 * Used to make sure initial hash table allocation does not exceed
	 * work_mem. Note that the estimate does not include space for
	 * pass-by-reference transition data values, nor for the representative
	 * tuple of each group.
	 */
	additionalsize = aggstate->numtrans * sizeof(AggStatePerGroupData);

	perhash->hashtable = BuildTupleHashTableExt(
		&aggstate->ss.ps,
		perhash->hashslot->tts_tupleDescriptor,
		perhash->numCols,
		perhash->hashGrpColIdxHash,
		perhash->eqfuncoids,
		perhash->hashfunctions,
		perhash->phasedata.aggnode->grpCollations,
		nbuckets,
		additionalsize,
		metacxt,
		hashcxt,
		tmpcxt,
		DO_AGGSPLIT_SKIPFINAL(aggstate->aggsplit));
}

/*
 * Compute columns that actually need to be stored in hashtable entries.  The
 * incoming tuples from the child plan node will contain grouping columns,
 * other columns referenced in our targetlist and qual, columns used to
 * compute the aggregate functions, and perhaps just junk columns we don't use
 * at all.  Only columns of the first two types need to be stored in the
 * hashtable, and getting rid of the others can make the table entries
 * significantly smaller.  The hashtable only contains the relevant columns,
 * and is packed/unpacked in lookup_hash_entry() / agg_retrieve_hash_table()
 * into the format of the normal input descriptor.
 *
 * Additional columns, in addition to the columns grouped by, come from two
 * sources: Firstly functionally dependent columns that we don't need to group
 * by themselves, and secondly ctids for row-marks.
 *
 * To eliminate duplicates, we build a bitmapset of the needed columns, and
 * then build an array of the columns included in the hashtable. We might
 * still have duplicates if the passed-in grpColIdx has them, which can happen
 * in edge cases from semijoins/distinct; these can't always be removed,
 * because it's not certain that the duplicate cols will be using the same
 * hash function.
 *
 * Note that the array is preserved over ExecReScanAgg, so we allocate it in
 * the per-query context (unlike the hash table itself).
 */
static void
find_hash_columns(AggState *aggstate)
{
	Bitmapset  *base_colnos;
	List	   *outerTlist = outerPlanState(aggstate)->plan->targetlist;
	EState	   *estate = aggstate->ss.ps.state;
	int			j;

	/* Find Vars that will be needed in tlist and qual */
	base_colnos = find_unaggregated_cols(aggstate);

	for (j = 0; j < aggstate->numphases; ++j)
	{
		AggStatePerPhase perphase = aggstate->phases[j];
		AggStatePerPhaseHash perhash;
		Bitmapset  *colnos = bms_copy(base_colnos);
		Bitmapset  *grouped_cols = perphase->grouped_cols[0];
		AttrNumber *grpColIdx = perphase->aggnode->grpColIdx;
		List	   *hashTlist = NIL;
		ListCell   *lc;
		TupleDesc	hashDesc;
		int			maxCols;
		int			i;

		if (!perphase->is_hashed)
			continue;

		perhash = (AggStatePerPhaseHash) perphase;
		perhash->largestGrpColIdx = 0;

		/*
		 * If we're doing grouping sets, then some Vars might be referenced in
		 * tlist/qual for the benefit of other grouping sets, but not needed
		 * when hashing; i.e. prepare_projection_slot will null them out, so
		 * there'd be no point storing them.  Use prepare_projection_slot's
		 * logic to determine which.
		 */
		foreach(lc, aggstate->all_grouped_cols)
		{
			int			attnum = lfirst_int(lc);

			if (!bms_is_member(attnum, grouped_cols))
				colnos = bms_del_member(colnos, attnum);
		}

		/*
		 * Compute maximum number of input columns accounting for possible
		 * duplications in the grpColIdx array, which can happen in some edge
		 * cases where HashAggregate was generated as part of a semijoin or a
		 * DISTINCT.
		 */
		maxCols = bms_num_members(colnos) + perhash->numCols;

		perhash->hashGrpColIdxInput =
			palloc(maxCols * sizeof(AttrNumber));
		perhash->hashGrpColIdxHash =
			palloc(perhash->numCols * sizeof(AttrNumber));

		/* Add all the grouping columns to colnos */
		for (i = 0; i < perhash->numCols; i++)
			colnos = bms_add_member(colnos, grpColIdx[i]);

		/*
		 * First build mapping for columns directly hashed. These are the
		 * first, because they'll be accessed when computing hash values and
		 * comparing tuples for exact matches. We also build simple mapping
		 * for execGrouping, so it knows where to find the to-be-hashed /
		 * compared columns in the input.
		 */
		for (i = 0; i < perhash->numCols; i++)
		{
			perhash->hashGrpColIdxInput[i] = grpColIdx[i];
			perhash->hashGrpColIdxHash[i] = i + 1;
			perhash->numhashGrpCols++;
			/* delete already mapped columns */
			bms_del_member(colnos, grpColIdx[i]);
		}

		/* and add the remaining columns */
		while ((i = bms_first_member(colnos)) >= 0)
		{
			perhash->hashGrpColIdxInput[perhash->numhashGrpCols] = i;
			perhash->numhashGrpCols++;
		}

		/* and build a tuple descriptor for the hashtable */
		for (i = 0; i < perhash->numhashGrpCols; i++)
		{
			int			varNumber = perhash->hashGrpColIdxInput[i] - 1;

			hashTlist = lappend(hashTlist, list_nth(outerTlist, varNumber));
			perhash->largestGrpColIdx =
				Max(varNumber + 1, perhash->largestGrpColIdx);
		}

		hashDesc = ExecTypeFromTL(hashTlist);

		execTuplesHashPrepare(perhash->numCols,
							  perphase->aggnode->grpOperators,
							  &perhash->eqfuncoids,
							  &perhash->hashfunctions);
		perhash->hashslot =
			ExecAllocTableSlot(&estate->es_tupleTable, hashDesc,
							   &TTSOpsMinimalTuple);

		list_free(hashTlist);
		bms_free(colnos);
	}

	bms_free(base_colnos);
}

/*
 * Estimate per-hash-table-entry overhead.
 */
Size
hash_agg_entry_size(int numAggs, Size tupleWidth, Size transitionSpace)
{
	return
		MAXALIGN(SizeofMinimalTupleHeader) +
		MAXALIGN(tupleWidth) +
		MAXALIGN(sizeof(TupleHashEntryData) +
				 numAggs * sizeof(AggStatePerGroupData)) +
		transitionSpace;
}

/*
 * hashagg_recompile_expressions()
 *
 * Identifies the right phase, compiles the right expression given the
 * arguments, and then sets phase->evalfunc to that expression.
 *
 * Different versions of the compiled expression are needed depending on
 * whether hash aggregation has spilled or not, and whether it's reading from
 * the outer plan or a tape. Before spilling to disk, the expression reads
 * from the outer plan and does not need to perform a NULL check. After
 * HashAgg begins to spill, new groups will not be created in the hash table,
 * and the AggStatePerGroup array may be NULL; therefore we need to add a null
 * pointer check to the expression. Then, when reading spilled data from a
 * tape, we change the outer slot type to be a fixed minimal tuple slot.
 *
 * It would be wasteful to recompile every time, so cache the compiled
 * expressions in the AggStatePerPhase, and reuse when appropriate.
 */
static void
hashagg_recompile_expressions(AggState *aggstate)
{
	AggStatePerPhase		 phase = aggstate->phase;

	Assert(aggstate->aggstrategy == AGG_HASHED ||
		   aggstate->aggstrategy == AGG_MIXED);

	if (phase->evaltrans_cache[aggstate->hash_agg_stage] == NULL)
	{
		const TupleTableSlotOps *outerops	= aggstate->ss.ps.outerops;
		bool	outerfixed = aggstate->ss.ps.outeropsfixed;
		bool	minslot = false;
		int		nullcheck = false;
		int		allow_concurrent_hashing = true;

		/*
		 * we are refilling the hash table and we disallow concurrent hashing
		 * within transition expression because we refill the hash tables one
		 * set by one set, this can avoid unnecessary nullcheck, meanwhile, we
		 * get tuple from spill file, so it is a MinimalTuple.
		 */
		if (aggstate->hash_agg_stage == HASHAGG_REFILL)
		{
			minslot = true;
			nullcheck = false;
			allow_concurrent_hashing = false;
		}
		/*
		 * we entred the spill mode, the concurrent hashing still works in this
		 * mode, but some grouping sets need to put the tuple into spill files
		 * and their pergroup states will be NULL, so we need add nullcheck.
		 */
		else if (aggstate->hash_agg_stage == HASHAGG_SPILL)
		{
			minslot = false;
			nullcheck = true;
			allow_concurrent_hashing = true;
		}

		/* temporarily change the outerops while compiling the expression */
		if (minslot)
		{
			aggstate->ss.ps.outerops = &TTSOpsMinimalTuple;
			aggstate->ss.ps.outeropsfixed = true;
		}

		phase->evaltrans_cache[aggstate->hash_agg_stage] =
			ExecBuildAggTrans(aggstate, phase, nullcheck, allow_concurrent_hashing);

		/* change back */
		aggstate->ss.ps.outerops = outerops;
		aggstate->ss.ps.outeropsfixed = outerfixed;
	}

	phase->evaltrans = phase->evaltrans_cache[aggstate->hash_agg_stage];
}

/*
 * Set limits that trigger spilling to avoid exceeding work_mem. Consider the
 * number of partitions we expect to create (if we do spill).
 *
 * There are two limits: a memory limit, and also an ngroups limit. The
 * ngroups limit becomes important when we expect transition values to grow
 * substantially larger than the initial value.
 */
void
hash_agg_set_limits(double hashentrysize, uint64 input_groups, int used_bits,
					Size *mem_limit, uint64 *ngroups_limit,
					int *num_partitions)
{
	int npartitions;
	Size partition_mem;

	/* if not expected to spill, use all of work_mem */
	if (input_groups * hashentrysize < work_mem * 1024L)
	{
		*mem_limit = work_mem * 1024L;
		*ngroups_limit = *mem_limit / hashentrysize;
		return;
	}

	/*
	 * Calculate expected memory requirements for spilling, which is the size
	 * of the buffers needed for all the tapes that need to be open at
	 * once. Then, subtract that from the memory available for holding hash
	 * tables.
	 */
	npartitions = hash_choose_num_partitions(input_groups,
											 hashentrysize,
											 used_bits,
											 NULL);
	if (num_partitions != NULL)
		*num_partitions = npartitions;

	partition_mem =
		HASHAGG_READ_BUFFER_SIZE +
		HASHAGG_WRITE_BUFFER_SIZE * npartitions;

	/*
	 * Don't set the limit below 3/4 of work_mem. In that case, we are at the
	 * minimum number of partitions, so we aren't going to dramatically exceed
	 * work mem anyway.
	 */
	if (work_mem * 1024L > 4 * partition_mem)
		*mem_limit = work_mem * 1024L - partition_mem;
	else
		*mem_limit = work_mem * 1024L * 0.75;

	if (*mem_limit > hashentrysize)
		*ngroups_limit = *mem_limit / hashentrysize;
	else
		*ngroups_limit = 1;
}

/*
 * hash_agg_check_limits
 *
 * After adding a new group to the hash table, check whether we need to enter
 * spill mode. Allocations may happen without adding new groups (for instance,
 * if the transition state size grows), so this check is imperfect.
 */
static void
hash_agg_check_limits(AggState *aggstate)
{
	uint64 ngroups = aggstate->hash_ngroups_current;
	Size meta_mem = MemoryContextMemAllocated(
		aggstate->hash_metacxt, true);
	Size hash_mem = MemoryContextMemAllocated(
		aggstate->hashcontext->ecxt_per_tuple_memory, true);

	/*
	 * Don't spill unless there's at least one group in the hash table so we
	 * can be sure to make progress even in edge cases.
	 */
	if (aggstate->hash_ngroups_current > 0 &&
		(meta_mem + hash_mem > aggstate->hash_mem_limit ||
		 ngroups > aggstate->hash_ngroups_limit))
	{
		hash_agg_enter_spill_mode(aggstate);
	}
}

/*
 * Enter "spill mode", meaning that no new groups are added to any of the hash
 * tables. Tuples that would create a new group are instead spilled, and
 * processed later.
 */
static void
hash_agg_enter_spill_mode(AggState *aggstate)
{
	aggstate->hash_spill_mode = true;

	/* if table_filled is true, we must be refilling the hash table */
	if (aggstate->table_filled)
		aggstate->hash_agg_stage = HASHAGG_REFILL;
	else
		aggstate->hash_agg_stage = HASHAGG_SPILL;

	hashagg_recompile_expressions(aggstate);

	if (!aggstate->hash_ever_spilled)
	{
		Assert(aggstate->hash_tapeinfo == NULL);

		aggstate->hash_ever_spilled = true;

		hashagg_tapeinfo_init(aggstate);
	}
}

/*
 * Update metrics after filling the hash table.
 *
 * If reading from the outer plan, from_tape should be false; if reading from
 * another tape, from_tape should be true.
 */
static void
hash_agg_update_metrics(AggState *aggstate, bool from_tape, int npartitions)
{
	Size	meta_mem;
	Size	hash_mem;
	Size	buffer_mem;
	Size	total_mem;

	if (aggstate->aggstrategy != AGG_MIXED &&
		aggstate->aggstrategy != AGG_HASHED)
		return;

	/* memory for the hash table itself */
	meta_mem = MemoryContextMemAllocated(aggstate->hash_metacxt, true);

	/* memory for the group keys and transition states */
	hash_mem = MemoryContextMemAllocated(
		aggstate->hashcontext->ecxt_per_tuple_memory, true);

	/* memory for read/write tape buffers, if spilled */
	buffer_mem = npartitions * HASHAGG_WRITE_BUFFER_SIZE;
	if (from_tape)
		buffer_mem += HASHAGG_READ_BUFFER_SIZE;

	/* update peak mem */
	total_mem = meta_mem + hash_mem + buffer_mem;
	if (total_mem > aggstate->hash_mem_peak)
		aggstate->hash_mem_peak = total_mem;

	/* update disk usage */
	if (aggstate->hash_tapeinfo != NULL)
	{
		uint64	disk_used = LogicalTapeSetBlocks(
			aggstate->hash_tapeinfo->tapeset) * (BLCKSZ / 1024);

		if (aggstate->hash_disk_used < disk_used)
			aggstate->hash_disk_used = disk_used;
	}

	/* update hashentrysize estimate based on contents */
	if (aggstate->hash_ngroups_current > 0)
	{
		aggstate->hashentrysize =
			sizeof(TupleHashEntryData) +
			(hash_mem / (double)aggstate->hash_ngroups_current);
	}
}

/*
 * Choose a reasonable number of buckets for the initial hash table size.
 */
static long
hash_choose_num_buckets(double hashentrysize, long ngroups, Size memory)
{
	long	max_nbuckets;
	long	nbuckets = ngroups;

	max_nbuckets = memory / hashentrysize;

	/*
	 * Underestimating is better than overestimating. Too many buckets crowd
	 * out space for group keys and transition state values.
	 */
	max_nbuckets >>= 1;

	if (nbuckets > max_nbuckets)
		nbuckets = max_nbuckets;
	if (nbuckets < HASHAGG_MIN_BUCKETS)
		nbuckets = HASHAGG_MIN_BUCKETS;
	return nbuckets;
}

/*
 * Determine the number of partitions to create when spilling, which will
 * always be a power of two. If log2_npartitions is non-NULL, set
 * *log2_npartitions to the log2() of the number of partitions.
 */
static int
hash_choose_num_partitions(uint64 input_groups, double hashentrysize,
						   int used_bits, int *log2_npartitions)
{
	Size	mem_wanted;
	int		partition_limit;
	int		npartitions;
	int		partition_bits;

	/*
	 * Avoid creating so many partitions that the memory requirements of the
	 * open partition files are greater than 1/4 of work_mem.
	 */
	partition_limit =
		(work_mem * 1024L * 0.25 - HASHAGG_READ_BUFFER_SIZE) /
		HASHAGG_WRITE_BUFFER_SIZE;

	mem_wanted = HASHAGG_PARTITION_FACTOR * input_groups * hashentrysize;

	/* make enough partitions so that each one is likely to fit in memory */
	npartitions = 1 + (mem_wanted / (work_mem * 1024L));

	if (npartitions > partition_limit)
		npartitions = partition_limit;

	if (npartitions < HASHAGG_MIN_PARTITIONS)
		npartitions = HASHAGG_MIN_PARTITIONS;
	if (npartitions > HASHAGG_MAX_PARTITIONS)
		npartitions = HASHAGG_MAX_PARTITIONS;

	/* ceil(log2(npartitions)) */
	partition_bits = my_log2(npartitions);

	/* make sure that we don't exhaust the hash bits */
	if (partition_bits + used_bits >= 32)
		partition_bits = 32 - used_bits;

	if (log2_npartitions != NULL)
		*log2_npartitions = partition_bits;

	/* number of partitions will be a power of two */
	npartitions = 1L << partition_bits;

	return npartitions;
}

/*
 * Find or create a hashtable entry for the tuple group containing the current
 * tuple (already set in tmpcontext's outertuple slot), in the current grouping
 * set (which the caller must have selected - note that initialize_aggregate
 * depends on this).
 *
 * When called, CurrentMemoryContext should be the per-query context. The
 * already-calculated hash value for the tuple must be specified.
 *
 * If in "spill mode", then only find existing hashtable entries; don't create
 * new ones. If a tuple's group is not already present in the hash table for
 * the current grouping set, assign *in_hash_table=false and the caller will
 * spill it to disk.
 */
static AggStatePerGroup
lookup_hash_entry(AggState *aggstate, AggStatePerPhaseHash perhash,
				  uint32 hash, bool *in_hash_table)
{
	TupleTableSlot *hashslot = perhash->hashslot;
	TupleHashEntryData *entry;
	bool			isnew = false;
	bool		   *p_isnew;

	/* if hash table already spilled, don't create new entries */
	p_isnew = aggstate->hash_spill_mode ? NULL : &isnew;

	/* find or create the hashtable entry using the filtered tuple */
	entry = LookupTupleHashEntryHash(perhash->hashtable, hashslot, p_isnew,
									 hash);

	if (entry == NULL)
	{
		*in_hash_table = false;
		return NULL;
	}
	else
		*in_hash_table = true;

	if (isnew)
	{
		AggStatePerGroup	pergroup;
		int					transno;

		aggstate->hash_ngroups_current++;
		hash_agg_check_limits(aggstate);

		/* no need to allocate or initialize per-group state */
		if (aggstate->numtrans == 0)
			return NULL;

		pergroup = (AggStatePerGroup)
			MemoryContextAlloc(perhash->hashtable->tablecxt,
							   sizeof(AggStatePerGroupData) * aggstate->numtrans);

		entry->additional = pergroup;

		/*
		 * Initialize aggregates for new tuple group, lookup_hash_entries()
		 * already has selected the relevant grouping set.
		 */
		for (transno = 0; transno < aggstate->numtrans; transno++)
		{
			AggStatePerTrans pertrans = &aggstate->pertrans[transno];
			AggStatePerGroup pergroupstate = &pergroup[transno];

			initialize_aggregate(aggstate, pertrans, pergroupstate);
		}
	}

	return entry->additional;
}

/*
 * Look up hash entries for the current tuple in all hashed grouping sets,
 * returning an array of pergroup pointers suitable for advance_aggregates.
 *
 * Be aware that lookup_hash_entry can reset the tmpcontext.
 *
 * Some entries may be left NULL if we are in "spill mode". The same tuple
 * will belong to different groups for each grouping set, so may match a group
 * already in memory for one set and match a group not in memory for another
 * set. When in "spill mode", the tuple will be spilled for each grouping set
 * where it doesn't match a group in memory.
 *
 * NB: It's possible to spill the same tuple for several different grouping
 * sets. This may seem wasteful, but it's actually a trade-off: if we spill
 * the tuple multiple times for multiple grouping sets, it can be partitioned
 * for each grouping set, making the refilling of the hash table very
 * efficient.
 */
static void
lookup_hash_entries(AggState *aggstate, AggStatePerPhaseHash current_hash,
					List *concurrent_hashes)
{
	AggStatePerPhaseHash perhash;
	int		i;

	for (i = 0; i < 1 + list_length(concurrent_hashes); i++)
	{
		uint32			hash;
		bool			in_hash_table;

		if (i == 0)
			perhash = current_hash;
		else
			perhash = (AggStatePerPhaseHash) list_nth(concurrent_hashes, i - 1);

		if (!perhash)
			continue;

		select_current_set(aggstate, 0, true);
		prepare_hash_slot(aggstate, perhash);
		hash = TupleHashTableHash(perhash->hashtable, perhash->hashslot);
		perhash->phasedata.pergroups[0] =
			lookup_hash_entry(aggstate, perhash, hash, &in_hash_table);

		/* check to see if we need to spill the tuple for this grouping set */
		if (!in_hash_table)
		{
			TupleTableSlot	*slot	 = aggstate->tmpcontext->ecxt_outertuple;

			if (perhash->hash_spill == NULL)
				perhash->hash_spill = palloc0(sizeof(HashAggSpill));

			if (perhash->hash_spill->partitions == NULL)
				hashagg_spill_init(perhash->hash_spill,
								   aggstate->hash_tapeinfo, 0,
								   perhash->phasedata.aggnode->numGroups,
								   aggstate->hashentrysize);

			hashagg_spill_tuple(perhash->hash_spill,
								slot,
								hash);
		}
	}
}

/*
 * ExecAgg -
 *
 *	  ExecAgg receives tuples from its outer subplan and aggregates over
 *	  the appropriate attribute for each aggregate function use (Aggref
 *	  node) appearing in the targetlist or qual of the node.  The number
 *	  of tuples to aggregate over depends on whether grouped or plain
 *	  aggregation is selected.  In grouped aggregation, we produce a result
 *	  row for each group; in plain aggregation there's a single result row
 *	  for the whole query.  In either case, the value of each aggregate is
 *	  stored in the expression context to be used when ExecProject evaluates
 *	  the result tuple.
 */
static TupleTableSlot *
ExecAgg(PlanState *pstate)
{
	AggState   *node = castNode(AggState, pstate);
	TupleTableSlot *result = NULL;

	CHECK_FOR_INTERRUPTS();

	if (!node->agg_done)
	{
		/* Dispatch based on strategy */
		switch (node->phase->aggstrategy)
		{
			case AGG_HASHED:
				if (!node->table_filled)
					agg_fill_hash_table(node);
				result = agg_retrieve_hash_table(node);
				break;
			case AGG_PLAIN:
			case AGG_SORTED:
			case AGG_MIXED:
				if (!node->input_sorted)
					agg_sort_input(node);
				result = agg_retrieve_direct(node);
				break;
		}

		if (!TupIsNull(result))
			return result;
	}

	return NULL;
}

/*
 * ExecAgg for non-hashed case
 */
static TupleTableSlot *
agg_retrieve_direct(AggState *aggstate)
{
	Agg		   *node = aggstate->phase->aggnode;
	ExprContext *econtext;
	ExprContext *tmpcontext;
	AggStatePerAgg peragg;
	AggStatePerGroup *pergroups;
	TupleTableSlot *outerslot;
	TupleTableSlot *firstSlot;
	TupleTableSlot *result;
	bool		hasGroupingSets = aggstate->phase->aggnode->groupingSets != NULL;
	int			numGroupingSets = aggstate->phase->numsets;
	int			currentSet;
	int			nextSetSize;
	int			numReset;
	int			i;

	/*
	 * get state info from node
	 *
	 * econtext is the per-output-tuple expression context
	 *
	 * tmpcontext is the per-input-tuple expression context
	 */
	econtext = aggstate->ss.ps.ps_ExprContext;
	tmpcontext = aggstate->tmpcontext;

	peragg = aggstate->peragg;
	pergroups = aggstate->phase->pergroups;
	firstSlot = aggstate->ss.ss_ScanTupleSlot;

	/*
	 * We loop retrieving groups until we find one matching
	 * aggstate->ss.ps.qual
	 *
	 * For grouping sets, we have the invariant that aggstate->projected_set
	 * is either -1 (initial call) or the index (starting from 0) in
	 * gset_lengths for the group we just completed (either by projecting a
	 * row or by discarding it in the qual).
	 */
	while (!aggstate->agg_done)
	{
		/*
		 * Clear the per-output-tuple context for each group, as well as
		 * aggcontext (which contains any pass-by-ref transvalues of the old
		 * group).  Some aggregate functions store working state in child
		 * contexts; those now get reset automatically without us needing to
		 * do anything special.
		 *
		 * We use ReScanExprContext not just ResetExprContext because we want
		 * any registered shutdown callbacks to be called.  That allows
		 * aggregate functions to ensure they've cleaned up any non-memory
		 * resources.
		 */
		ReScanExprContext(econtext);

		/*
		 * Determine how many grouping sets need to be reset at this boundary.
		 */
		if (aggstate->projected_set >= 0 &&
			aggstate->projected_set < numGroupingSets)
			numReset = aggstate->projected_set + 1;
		else
			numReset = numGroupingSets;

		/*
		 * numReset can change on a phase boundary, but that's OK; we want to
		 * reset the contexts used in _this_ phase, and later, after possibly
		 * changing phase, initialize the right number of aggregates for the
		 * _new_ phase.
		 */

		for (i = 0; i < numReset; i++)
		{
			ReScanExprContext(aggstate->aggcontexts[i]);
		}

		/*
		 * Check if input is complete and there are no more groups to project
		 * in this phase; move to next phase or mark as done.
		 */
		if (aggstate->input_done == true &&
			aggstate->projected_set >= (numGroupingSets - 1))
		{
			if (aggstate->current_phase < aggstate->numphases - 1)
			{
				/* Advance to the next phase */
				initialize_phase(aggstate, aggstate->current_phase + 1);

				/* Check whether new phase is an AGG_HASHED */
				if (!aggstate->phase->is_hashed)
				{
					aggstate->input_done = false;
					aggstate->projected_set = -1;
					numGroupingSets = aggstate->phase->numsets;
					node = aggstate->phase->aggnode;
					numReset = numGroupingSets;
					pergroups = aggstate->phase->pergroups;
				}
				else
				{
					AggStatePerPhaseHash perhash = (AggStatePerPhaseHash) aggstate->phase;
					/* finalize any spills */
					hashagg_finish_initial_spills(aggstate);


					/*
					 * Mixed mode; we've output all the grouped stuff and have
					 * full hashtables, so switch to outputting those.
					 */
					aggstate->table_filled = true;
					ResetTupleHashIterator(perhash->hashtable, &perhash->hashiter);
					select_current_set(aggstate, 0, true);
					return agg_retrieve_hash_table(aggstate);
				}
			}
			else
			{
				aggstate->agg_done = true;
				break;
			}
		}

		/*
		 * Get the number of columns in the next grouping set after the last
		 * projected one (if any). This is the number of columns to compare to
		 * see if we reached the boundary of that set too.
		 */
		if (aggstate->projected_set >= 0 &&
			aggstate->projected_set < (numGroupingSets - 1))
			nextSetSize = aggstate->phase->gset_lengths[aggstate->projected_set + 1];
		else
			nextSetSize = 0;

		/*----------
		 * If a subgroup for the current grouping set is present, project it.
		 *
		 * We have a new group if:
		 *	- we're out of input but haven't projected all grouping sets
		 *	  (checked above)
		 * OR
		 *	  - we already projected a row that wasn't from the last grouping
		 *		set
		 *	  AND
		 *	  - the next grouping set has at least one grouping column (since
		 *		empty grouping sets project only once input is exhausted)
		 *	  AND
		 *	  - the previous and pending rows differ on the grouping columns
		 *		of the next grouping set
		 *----------
		 */
		tmpcontext->ecxt_innertuple = econtext->ecxt_outertuple;
		if (aggstate->input_done ||
			(aggstate->phase->aggnode->numCols > 0 &&
			 aggstate->projected_set != -1 &&
			 aggstate->projected_set < (numGroupingSets - 1) &&
			 nextSetSize > 0 &&
			 !ExecQualAndReset(((AggStatePerPhaseSort) aggstate->phase)->eqfunctions[nextSetSize - 1],
							   tmpcontext)))
		{
			aggstate->projected_set += 1;

			Assert(aggstate->projected_set < numGroupingSets);
			Assert(nextSetSize > 0 || aggstate->input_done);
		}
		else
		{
			/*
			 * We no longer care what group we just projected, the next
			 * projection will always be the first (or only) grouping set
			 * (unless the input proves to be empty).
			 */
			aggstate->projected_set = 0;

			/*
			 * If we don't already have the first tuple of the new group,
			 * fetch it from the outer plan.
			 */
			if (aggstate->grp_firstTuple == NULL)
			{
				outerslot = fetch_input_tuple(aggstate);
				if (!TupIsNull(outerslot))
				{
					/*
					 * Make a copy of the first input tuple; we will use this
					 * for comparisons (in group mode) and for projection.
					 */
					aggstate->grp_firstTuple = ExecCopySlotHeapTuple(outerslot);
				}
				else
				{
					/* outer plan produced no tuples at all */
					if (hasGroupingSets)
					{
						/*
						 * If there was no input at all, we need to project
						 * rows only if there are grouping sets of size 0.
						 * Note that this implies that there can't be any
						 * references to ungrouped Vars, which would otherwise
						 * cause issues with the empty output slot.
						 *
						 * XXX: This is no longer true, we currently deal with
						 * this in finalize_aggregates().
						 */
						aggstate->input_done = true;

						while (aggstate->phase->gset_lengths[aggstate->projected_set] > 0)
						{
							aggstate->projected_set += 1;
							if (aggstate->projected_set >= numGroupingSets)
							{
								/*
								 * We can't set agg_done here because we might
								 * have more phases to do, even though the
								 * input is empty. So we need to restart the
								 * whole outer loop.
								 */
								break;
							}
						}

						if (aggstate->projected_set >= numGroupingSets)
							continue;
					}
					else
					{
						aggstate->agg_done = true;
						/* If we are grouping, we should produce no tuples too */
						if (node->aggstrategy != AGG_PLAIN)
							return NULL;
					}
				}
			}

			/*
			 * Initialize working state for a new input tuple group.
			 */
			initialize_aggregates(aggstate, pergroups, numReset);

			if (aggstate->grp_firstTuple != NULL)
			{
				/*
				 * Store the copied first input tuple in the tuple table slot
				 * reserved for it.  The tuple will be deleted when it is
				 * cleared from the slot.
				 */
				ExecForceStoreHeapTuple(aggstate->grp_firstTuple,
										firstSlot, true);
				aggstate->grp_firstTuple = NULL;	/* don't keep two pointers */

				/* set up for first advance_aggregates call */
				tmpcontext->ecxt_outertuple = firstSlot;

				/*
				 * Process each outer-plan tuple, and then fetch the next one,
				 * until we exhaust the outer plan or cross a group boundary.
				 */
				for (;;)
				{
					/*
					 * If current phase can do transition concurrently, we need
					 * to update hashtables as well in advance_aggregates.
					 */
					if (aggstate->phase->concurrent_hashes)
					{
						lookup_hash_entries(aggstate, NULL,
											aggstate->phase->concurrent_hashes);
					}

					/* Advance the aggregates (or combine functions) */
					advance_aggregates(aggstate);

					/* Reset per-input-tuple context after each tuple */
					ResetExprContext(tmpcontext);

					outerslot = fetch_input_tuple(aggstate);
					if (TupIsNull(outerslot))
					{
						/* no more outer-plan tuples available */

						if (hasGroupingSets)
						{
							aggstate->input_done = true;
							break;
						}
						else
						{
							aggstate->agg_done = true;
							break;
						}
					}
					/* set up for next advance_aggregates call */
					tmpcontext->ecxt_outertuple = outerslot;

					/*
					 * If we are grouping, check whether we've crossed a group
					 * boundary.
					 */
					if (aggstate->phase->aggnode->numCols > 0)
					{
						tmpcontext->ecxt_innertuple = firstSlot;
						if (!ExecQual(((AggStatePerPhaseSort) aggstate->phase)->eqfunctions[node->numCols - 1],
									  tmpcontext))
						{
							aggstate->grp_firstTuple = ExecCopySlotHeapTuple(outerslot);
							break;
						}
					}
				}
			}

			/*
			 * Use the representative input tuple for any references to
			 * non-aggregated input columns in aggregate direct args, the node
			 * qual, and the tlist.  (If we are not grouping, and there are no
			 * input rows at all, we will come here with an empty firstSlot
			 * ... but if not grouping, there can't be any references to
			 * non-aggregated input columns, so no problem.)
			 */
			econtext->ecxt_outertuple = firstSlot;
		}

		Assert(aggstate->projected_set >= 0);

		currentSet = aggstate->projected_set;

		prepare_projection_slot(aggstate, econtext->ecxt_outertuple, currentSet);

		select_current_set(aggstate, currentSet, false);

		finalize_aggregates(aggstate,
							peragg,
							pergroups[currentSet]);

		/*
		 * If there's no row to project right now, we must continue rather
		 * than returning a null since there might be more groups.
		 */
		result = project_aggregates(aggstate);
		if (result)
			return result;
	}

	/* No more groups */
	return NULL;
}

static void
agg_sort_input(AggState *aggstate)
{
	AggStatePerPhase phase = aggstate->phases[0];
	AggStatePerPhaseSort persort = (AggStatePerPhaseSort) phase;
	TupleDesc	tupDesc;
	Sort		*sortnode;
	bool		randomAccess;

	Assert(!aggstate->input_sorted);
	Assert(!phase->is_hashed);
	Assert(phase->aggnode->sortnode);

	sortnode = (Sort *) phase->aggnode->sortnode;
	tupDesc = ExecGetResultType(outerPlanState(aggstate));
	randomAccess = (aggstate->eflags & (EXEC_FLAG_REWIND |
										EXEC_FLAG_BACKWARD |
										EXEC_FLAG_MARK)) != 0;


	persort->sort_in = tuplesort_begin_heap(tupDesc,
											sortnode->numCols,
											sortnode->sortColIdx,
											sortnode->sortOperators,
											sortnode->collations,
											sortnode->nullsFirst,
											work_mem,
											NULL, randomAccess);
	for (;;)
	{
		TupleTableSlot *outerslot;

		outerslot = ExecProcNode(outerPlanState(aggstate));
		if (TupIsNull(outerslot))
			break;

		tuplesort_puttupleslot(persort->sort_in, outerslot);
	}

	/* Sort the first phase */
	tuplesort_performsort(persort->sort_in);

	/* Mark the input to be sorted */
	aggstate->input_sorted = true;
}

/*
 * ExecAgg for hashed case: read input and build hash table
 */
static void
agg_fill_hash_table(AggState *aggstate)
{
	AggStatePerPhaseHash current_hash;
	TupleTableSlot *outerslot;
	ExprContext *tmpcontext = aggstate->tmpcontext;
	List *concurrent_hashes = aggstate->phase->concurrent_hashes;

	/* Current phase must be the first phase */
	Assert(aggstate->current_phase == 0);
	current_hash = (AggStatePerPhaseHash) aggstate->phase;

	/*
	 * Process each outer-plan tuple, and then fetch the next one, until we
	 * exhaust the outer plan.
	 */
	for (;;)
	{
		outerslot = ExecProcNode(outerPlanState(aggstate));
		if (TupIsNull(outerslot))
			break;

		/* set up for lookup_hash_entries and advance_aggregates */
		tmpcontext->ecxt_outertuple = outerslot;

		/* Find or build hashtable entries */
		lookup_hash_entries(aggstate, current_hash, concurrent_hashes);

		/* Advance the aggregates (or combine functions) */
		advance_aggregates(aggstate);

		/*
		 * Reset per-input-tuple context after each tuple, but note that the
		 * hash lookups do this too
		 */
		ResetExprContext(aggstate->tmpcontext);
	}

	/* finalize spills, if any */
	hashagg_finish_initial_spills(aggstate);

	aggstate->table_filled = true;
	/* Initialize to walk the first hash table */
	select_current_set(aggstate, 0, true);
	ResetTupleHashIterator(current_hash->hashtable, &current_hash->hashiter);
}

/*
 * If any data was spilled during hash aggregation, reset the hash table and
 * reprocess one batch of spilled data. After reprocessing a batch, the hash
 * table will again contain data, ready to be consumed by
 * agg_retrieve_hash_table_in_memory().
 *
 * Should only be called after all in memory hash table entries have been
 * finalized and emitted.
 *
 * Return false when input is exhausted and there's no more work to be done;
 * otherwise return true.
 */
static bool
agg_refill_hash_table(AggState *aggstate)
{
	AggStatePerPhaseHash perhash;
	HashAggBatch	*batch;
	HashAggSpill	 spill;
	HashTapeInfo	*tapeinfo = aggstate->hash_tapeinfo;
	uint64			 ngroups_estimate;
	bool			 spill_initialized = false;

	if (aggstate->hash_batches == NIL)
		return false;

	batch = linitial(aggstate->hash_batches);
	aggstate->hash_batches = list_delete_first(aggstate->hash_batches);
	perhash = (AggStatePerPhaseHash) aggstate->phases[batch->phaseidx];

	/*
	 * Estimate the number of groups for this batch as the total number of
	 * tuples in its input file. Although that's a worst case, it's not bad
	 * here for two reasons: (1) overestimating is better than
	 * underestimating; and (2) we've already scanned the relation once, so
	 * it's likely that we've already finalized many of the common values.
	 */
	ngroups_estimate = batch->input_tuples;

	hash_agg_set_limits(aggstate->hashentrysize, ngroups_estimate,
						batch->used_bits, &aggstate->hash_mem_limit,
						&aggstate->hash_ngroups_limit, NULL);

	/* free memory and reset hash tables */
	ReScanExprContext(aggstate->hashcontext);
	ResetTupleHashTable(perhash->hashtable);

	aggstate->hash_ngroups_current = 0;

	/* switch to the phase of current batch */
	initialize_phase(aggstate, batch->phaseidx);
	select_current_set(aggstate, 0, true);

	/*
	 * Spilled tuples are always read back as MinimalTuples, which may be
	 * different from the outer plan, so recompile the aggregate expressions.
	 *
	 * We still need the NULL check, because we are only processing one
	 * grouping set at a time and the rest will be NULL.
	 */
	aggstate->hash_agg_stage = HASHAGG_REFILL;
	hashagg_recompile_expressions(aggstate);

	LogicalTapeRewindForRead(tapeinfo->tapeset, batch->input_tapenum,
							 HASHAGG_READ_BUFFER_SIZE);
	for (;;) {
		TupleTableSlot	*slot = aggstate->hash_spill_slot;
		MinimalTuple	 tuple;
		uint32			 hash;
		bool			 in_hash_table;

		CHECK_FOR_INTERRUPTS();

		tuple = hashagg_batch_read(batch, &hash);
		if (tuple == NULL)
			break;

		ExecStoreMinimalTuple(tuple, slot, true);
		aggstate->tmpcontext->ecxt_outertuple = slot;

		prepare_hash_slot(aggstate, perhash);
		perhash->phasedata.pergroups[0] =
			lookup_hash_entry(aggstate, perhash, hash, &in_hash_table);

		if (in_hash_table)
		{
			/* Advance the aggregates (or combine functions) */
			advance_aggregates(aggstate);
		}
		else
		{
			if (!spill_initialized)
			{
				/*
				 * Avoid initializing the spill until we actually need it so
				 * that we don't assign tapes that will never be used.
				 */
				spill_initialized = true;
				hashagg_spill_init(&spill, tapeinfo, batch->used_bits,
					   ngroups_estimate, aggstate->hashentrysize);
			}
			/* no memory for a new group, spill */
			hashagg_spill_tuple(&spill, slot, hash);
		}

		/*
		 * Reset per-input-tuple context after each tuple, but note that the
		 * hash lookups do this too
		 */
		ResetExprContext(aggstate->tmpcontext);
	}

	hashagg_tapeinfo_release(tapeinfo, batch->input_tapenum);

	if (spill_initialized)
	{
		hash_agg_update_metrics(aggstate, true, spill.npartitions);
		hashagg_spill_finish(aggstate, &spill, batch->phaseidx);
	}
	else
		hash_agg_update_metrics(aggstate, true, 0);

	aggstate->hash_spill_mode = false;

	/* prepare to walk the first hash table */
	ResetTupleHashIterator(perhash->hashtable, &perhash->hashiter);

	pfree(batch);

	return true;
}

/*
 * ExecAgg for hashed case: retrieving groups from hash table
 *
 * After exhausting in-memory tuples, also try refilling the hash table using
 * previously-spilled tuples. Only returns NULL after all in-memory and
 * spilled tuples are exhausted.
 */
static TupleTableSlot *
agg_retrieve_hash_table(AggState *aggstate)
{
	TupleTableSlot *result = NULL;

	while (result == NULL)
	{
		result = agg_retrieve_hash_table_in_memory(aggstate);
		if (result == NULL)
		{
			if (!agg_refill_hash_table(aggstate))
			{
				aggstate->agg_done = true;
				break;
			}
		}
	}

	return result;
}

/*
 * Retrieve the groups from the in-memory hash tables without considering any
 * spilled tuples.
 */
static TupleTableSlot *
agg_retrieve_hash_table_in_memory(AggState *aggstate)
{
	ExprContext *econtext;
	AggStatePerAgg peragg;
	AggStatePerGroup pergroup;
	TupleHashEntryData *entry;
	TupleTableSlot *firstSlot;
	TupleTableSlot *result;
	AggStatePerPhaseHash perhash;

	/*
	 * get state info from node.
	 *
	 * econtext is the per-output-tuple expression context.
	 */
	econtext = aggstate->ss.ps.ps_ExprContext;
	peragg = aggstate->peragg;
	firstSlot = aggstate->ss.ss_ScanTupleSlot;

	perhash = (AggStatePerPhaseHash) aggstate->phase;

	/*
	 * We loop retrieving groups until we find one satisfying
	 * aggstate->ss.ps.qual
	 */
	for (;;)
	{
		TupleTableSlot *hashslot = perhash->hashslot;
		int			i;

		CHECK_FOR_INTERRUPTS();

		/*
		 * Find the next entry in the hash table
		 */
		entry = ScanTupleHashTable(perhash->hashtable, &perhash->hashiter);
		if (entry == NULL)
		{
			if (aggstate->current_phase + 1 < aggstate->numphases &&
				aggstate->hash_agg_stage != HASHAGG_REFILL)
			{
				/*
				 * Switch to next grouping set, reinitialize, and restart the
				 * loop.
				 */
				select_current_set(aggstate, 0, true);
				initialize_phase(aggstate, aggstate->current_phase + 1);
				perhash = (AggStatePerPhaseHash) aggstate->phase;
				ResetTupleHashIterator(perhash->hashtable, &perhash->hashiter);

				continue;
			}
			else
			{
				return NULL;
			}
		}

		/*
		 * Clear the per-output-tuple context for each group
		 *
		 * We intentionally don't use ReScanExprContext here; if any aggs have
		 * registered shutdown callbacks, they mustn't be called yet, since we
		 * might not be done with that agg.
		 */
		ResetExprContext(econtext);

		/*
		 * Transform representative tuple back into one with the right
		 * columns.
		 */
		ExecStoreMinimalTuple(entry->firstTuple, hashslot, false);
		slot_getallattrs(hashslot);

		ExecClearTuple(firstSlot);
		memset(firstSlot->tts_isnull, true,
			   firstSlot->tts_tupleDescriptor->natts * sizeof(bool));

		for (i = 0; i < perhash->numhashGrpCols; i++)
		{
			int			varNumber = perhash->hashGrpColIdxInput[i] - 1;

			firstSlot->tts_values[varNumber] = hashslot->tts_values[i];
			firstSlot->tts_isnull[varNumber] = hashslot->tts_isnull[i];
		}
		ExecStoreVirtualTuple(firstSlot);

		pergroup = (AggStatePerGroup) entry->additional;

		/*
		 * Use the representative input tuple for any references to
		 * non-aggregated input columns in the qual and tlist.
		 */
		econtext->ecxt_outertuple = firstSlot;

		prepare_projection_slot(aggstate,
								econtext->ecxt_outertuple,
								aggstate->current_set);

		finalize_aggregates(aggstate, peragg, pergroup);

		result = project_aggregates(aggstate);
		if (result)
			return result;
	}

	/* No more groups */
	return NULL;
}

/*
 * Initialize HashTapeInfo
 */
static void
hashagg_tapeinfo_init(AggState *aggstate)
{
	HashTapeInfo	*tapeinfo	= palloc(sizeof(HashTapeInfo));
	int				 init_tapes = 16;	/* expanded dynamically */

	tapeinfo->tapeset = LogicalTapeSetCreate(init_tapes, NULL, NULL, -1);
	tapeinfo->ntapes = init_tapes;
	tapeinfo->nfreetapes = init_tapes;
	tapeinfo->freetapes_alloc = init_tapes;
	tapeinfo->freetapes = palloc(init_tapes * sizeof(int));
	for (int i = 0; i < init_tapes; i++)
		tapeinfo->freetapes[i] = i;

	aggstate->hash_tapeinfo = tapeinfo;
}

/*
 * Assign unused tapes to spill partitions, extending the tape set if
 * necessary.
 */
static void
hashagg_tapeinfo_assign(HashTapeInfo *tapeinfo, int *partitions,
						int npartitions)
{
	int partidx = 0;

	/* use free tapes if available */
	while (partidx < npartitions && tapeinfo->nfreetapes > 0)
		partitions[partidx++] = tapeinfo->freetapes[--tapeinfo->nfreetapes];

	if (partidx < npartitions)
	{
		LogicalTapeSetExtend(tapeinfo->tapeset, npartitions - partidx);

		while (partidx < npartitions)
			partitions[partidx++] = tapeinfo->ntapes++;
	}
}

/*
 * After a tape has already been written to and then read, this function
 * rewinds it for writing and adds it to the free list.
 */
static void
hashagg_tapeinfo_release(HashTapeInfo *tapeinfo, int tapenum)
{
	LogicalTapeRewindForWrite(tapeinfo->tapeset, tapenum);
	if (tapeinfo->freetapes_alloc == tapeinfo->nfreetapes)
	{
		tapeinfo->freetapes_alloc <<= 1;
		tapeinfo->freetapes = repalloc(
			tapeinfo->freetapes, tapeinfo->freetapes_alloc * sizeof(int));
	}
	tapeinfo->freetapes[tapeinfo->nfreetapes++] = tapenum;
}

/*
 * hashagg_spill_init
 *
 * Called after we determined that spilling is necessary. Chooses the number
 * of partitions to create, and initializes them.
 */
static void
hashagg_spill_init(HashAggSpill *spill, HashTapeInfo *tapeinfo, int used_bits,
				   uint64 input_groups, double hashentrysize)
{
	int		npartitions;
	int     partition_bits;

	npartitions = hash_choose_num_partitions(
		input_groups, hashentrysize, used_bits, &partition_bits);

	spill->partitions = palloc0(sizeof(int) * npartitions);
	spill->ntuples = palloc0(sizeof(int64) * npartitions);

	hashagg_tapeinfo_assign(tapeinfo, spill->partitions, npartitions);

	spill->tapeset = tapeinfo->tapeset;
	spill->shift = 32 - used_bits - partition_bits;
	spill->mask = (npartitions - 1) << spill->shift;
	spill->npartitions = npartitions;
}

/*
 * hashagg_spill_tuple
 *
 * No room for new groups in the hash table. Save for later in the appropriate
 * partition.
 */
static Size
hashagg_spill_tuple(HashAggSpill *spill, TupleTableSlot *slot, uint32 hash)
{
	LogicalTapeSet		*tapeset = spill->tapeset;
	int					 partition;
	MinimalTuple		 tuple;
	int					 tapenum;
	int					 total_written = 0;
	bool				 shouldFree;

	Assert(spill->partitions != NULL);

	/* XXX: may contain unnecessary attributes, should project */
	tuple = ExecFetchSlotMinimalTuple(slot, &shouldFree);

	partition = (hash & spill->mask) >> spill->shift;
	spill->ntuples[partition]++;

	tapenum = spill->partitions[partition];

	LogicalTapeWrite(tapeset, tapenum, (void *) &hash, sizeof(uint32));
	total_written += sizeof(uint32);

	LogicalTapeWrite(tapeset, tapenum, (void *) tuple, tuple->t_len);
	total_written += tuple->t_len;

	if (shouldFree)
		pfree(tuple);

	return total_written;
}

/*
 * hashagg_batch_new
 *
 * Construct a HashAggBatch item, which represents one iteration of HashAgg to
 * be done.
 */
static HashAggBatch *
hashagg_batch_new(LogicalTapeSet *tapeset, int tapenum, int phaseidx,
				  int64 input_tuples, int used_bits)
{
	HashAggBatch *batch = palloc0(sizeof(HashAggBatch));

	batch->phaseidx = phaseidx;
	batch->used_bits = used_bits;
	batch->tapeset = tapeset;
	batch->input_tapenum = tapenum;
	batch->input_tuples = input_tuples;

	return batch;
}

/*
 * read_spilled_tuple
 * 		read the next tuple from a batch's tape.  Return NULL if no more.
 */
static MinimalTuple
hashagg_batch_read(HashAggBatch *batch, uint32 *hashp)
{
	LogicalTapeSet *tapeset = batch->tapeset;
	int				tapenum = batch->input_tapenum;
	MinimalTuple	tuple;
	uint32			t_len;
	size_t			nread;
	uint32			hash;

	nread = LogicalTapeRead(tapeset, tapenum, &hash, sizeof(uint32));
	if (nread == 0)
		return NULL;
	if (nread != sizeof(uint32))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("unexpected EOF for tape %d: requested %zu bytes, read %zu bytes",
						tapenum, sizeof(uint32), nread)));
	if (hashp != NULL)
		*hashp = hash;

	nread = LogicalTapeRead(tapeset, tapenum, &t_len, sizeof(t_len));
	if (nread != sizeof(uint32))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("unexpected EOF for tape %d: requested %zu bytes, read %zu bytes",
						tapenum, sizeof(uint32), nread)));

	tuple = (MinimalTuple) palloc(t_len);
	tuple->t_len = t_len;

	nread = LogicalTapeRead(tapeset, tapenum,
							(void *)((char *)tuple + sizeof(uint32)),
							t_len - sizeof(uint32));
	if (nread != t_len - sizeof(uint32))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("unexpected EOF for tape %d: requested %zu bytes, read %zu bytes",
						tapenum, t_len - sizeof(uint32), nread)));

	return tuple;
}

/*
 * hashagg_finish_initial_spills
 *
 * After a HashAggBatch has been processed, it may have spilled tuples to
 * disk. If so, turn the spilled partitions into new batches that must later
 * be executed.
 */
static void
hashagg_finish_initial_spills(AggState *aggstate)
{
	int phaseidx;
	int total_npartitions = 0;

	for (phaseidx = 0; phaseidx < aggstate->numphases; phaseidx++)
	{
		AggStatePerPhaseHash	perhash;
		AggStatePerPhase		phase = aggstate->phases[phaseidx];

		if (!phase->is_hashed)
			continue;

		perhash = (AggStatePerPhaseHash) phase;
		if (perhash->hash_spill)
		{
			total_npartitions += perhash->hash_spill->npartitions;
			hashagg_spill_finish(aggstate, perhash->hash_spill, phase->phaseidx);

			/*
			 * We're not processing tuples from outer plan any more; only
			 * processing batches of spilled tuples. The initial spill structures
			 * are no longer needed.
			 */
			pfree(perhash->hash_spill);
			perhash->hash_spill = NULL;
		}
	}

	hash_agg_update_metrics(aggstate, false, total_npartitions);
	aggstate->hash_spill_mode = false;
}

/*
 * hashagg_spill_finish
 *
 * Transform spill partitions into new batches.
 */
static void
hashagg_spill_finish(AggState *aggstate, HashAggSpill *spill, int phaseidx)
{
	int i;
	int used_bits = 32 - spill->shift;

	if (spill->npartitions == 0)
		return;	/* didn't spill */

	for (i = 0; i < spill->npartitions; i++)
	{
		int				 tapenum = spill->partitions[i];
		HashAggBatch    *new_batch;

		/* if the partition is empty, don't create a new batch of work */
		if (spill->ntuples[i] == 0)
			continue;

		new_batch = hashagg_batch_new(aggstate->hash_tapeinfo->tapeset,
									  tapenum, phaseidx, spill->ntuples[i],
									  used_bits);
		aggstate->hash_batches = lcons(new_batch, aggstate->hash_batches);
		aggstate->hash_batches_used++;
	}

	pfree(spill->ntuples);
	pfree(spill->partitions);
}

/*
 * Free resources related to a spilled HashAgg.
 */
static void
hashagg_reset_spill_state(AggState *aggstate)
{
	ListCell	*lc;
	int			phaseidx;

	/* free spills from initial pass */
	for (phaseidx = 0; phaseidx < aggstate->numphases; phaseidx++)
	{
		AggStatePerPhaseHash	perhash;
		AggStatePerPhase		phase = aggstate->phases[phaseidx];

		if (!phase->is_hashed)
			continue;

		perhash = (AggStatePerPhaseHash) phase;

		if (perhash->hash_spill)
		{
			pfree(perhash->hash_spill);
			perhash->hash_spill = NULL;
		}
	}

	/* free batches */
	foreach(lc, aggstate->hash_batches)
	{
		HashAggBatch *batch = (HashAggBatch*) lfirst(lc);
		pfree(batch);
	}
	list_free(aggstate->hash_batches);
	aggstate->hash_batches = NIL;

	/* close tape set */
	if (aggstate->hash_tapeinfo != NULL)
	{
		HashTapeInfo	*tapeinfo = aggstate->hash_tapeinfo;

		LogicalTapeSetClose(tapeinfo->tapeset);
		pfree(tapeinfo->freetapes);
		pfree(tapeinfo);
		aggstate->hash_tapeinfo = NULL;
	}
}


/* -----------------
 * ExecInitAgg
 *
 *	Creates the run-time information for the agg node produced by the
 *	planner and initializes its outer subtree.
 *
 * -----------------
 */
AggState *
ExecInitAgg(Agg *node, EState *estate, int eflags)
{
	AggState   *aggstate;
	AggStatePerAgg peraggs;
	AggStatePerTrans pertransstates;
	Plan	   *outerPlan;
	ExprContext *econtext;
	TupleDesc	scanDesc;
	int			numaggs,
				transno,
				aggno;
	int			phaseidx;
	ListCell   *l;
	Bitmapset  *all_grouped_cols = NULL;
	int			numGroupingSets = 1;
	int			i = 0;
	int			j = 0;
	bool		need_extra_slot = false;
	bool		use_hashing = (node->aggstrategy == AGG_HASHED ||
							   node->aggstrategy == AGG_MIXED);
	uint64		totalHashGroups = 0;

	/* check for unsupported flags */
	Assert(!(eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)));

	/*
	 * create state structure
	 */
	aggstate = makeNode(AggState);
	aggstate->ss.ps.plan = (Plan *) node;
	aggstate->ss.ps.state = estate;
	aggstate->ss.ps.ExecProcNode = ExecAgg;

	aggstate->aggs = NIL;
	aggstate->numaggs = 0;
	aggstate->numtrans = 0;
	aggstate->aggstrategy = node->aggstrategy;
	aggstate->aggsplit = node->aggsplit;
	aggstate->maxsets = 0;
	aggstate->projected_set = -1;
	aggstate->current_set = 0;
	aggstate->peragg = NULL;
	aggstate->pertrans = NULL;
	aggstate->curperagg = NULL;
	aggstate->curpertrans = NULL;
	aggstate->input_done = false;
	aggstate->agg_done = false;
	aggstate->grp_firstTuple = NULL;
	aggstate->input_sorted = true;
	aggstate->eflags = eflags;
	aggstate->num_hashes = 0;
	aggstate->hash_agg_stage = HASHAGG_INITIAL;

	/*
	 * Calculate the maximum number of grouping sets in any phase; this
	 * determines the size of some allocations.
	 */
	if (node->groupingSets)
	{
		numGroupingSets = list_length(node->groupingSets);

		foreach(l, node->chain)
		{
			Agg		   *agg = lfirst(l);

			numGroupingSets = Max(numGroupingSets,
								  list_length(agg->groupingSets));

			if (agg->aggstrategy != AGG_HASHED)
				need_extra_slot = true;
		}
	}

	aggstate->maxsets = numGroupingSets;
	aggstate->numphases = 1 + list_length(node->chain);

	/*
	 * The first phase is not sorted, agg need to do its own sort. See
	 * agg_sort_input(), this can only happen in groupingsets case.
	 */
	if (node->sortnode)
		aggstate->input_sorted = false;	

	aggstate->aggcontexts = (ExprContext **)
		palloc0(sizeof(ExprContext *) * numGroupingSets);

	/*
	 * Create expression contexts.  We need three or more, one for
	 * per-input-tuple processing, one for per-output-tuple processing, one
	 * for all the hashtables, and one for each grouping set.  The per-tuple
	 * memory context of the per-grouping-set ExprContexts (aggcontexts)
	 * replaces the standalone memory context formerly used to hold transition
	 * values.  We cheat a little by using ExecAssignExprContext() to build
	 * all of them.
	 *
	 * NOTE: the details of what is stored in aggcontexts and what is stored
	 * in the regular per-query memory context are driven by a simple
	 * decision: we want to reset the aggcontext at group boundaries (if not
	 * hashing) and in ExecReScanAgg to recover no-longer-wanted space.
	 */
	ExecAssignExprContext(estate, &aggstate->ss.ps);
	aggstate->tmpcontext = aggstate->ss.ps.ps_ExprContext;

	for (i = 0; i < numGroupingSets; ++i)
	{
		ExecAssignExprContext(estate, &aggstate->ss.ps);
		aggstate->aggcontexts[i] = aggstate->ss.ps.ps_ExprContext;
	}

	if (use_hashing)
	{
		ExecAssignExprContext(estate, &aggstate->ss.ps);
		aggstate->hashcontext = aggstate->ss.ps.ps_ExprContext;
	}

	ExecAssignExprContext(estate, &aggstate->ss.ps);

	/*
	 * Initialize child nodes.
	 *
	 * If we are doing a hashed aggregation then the child plan does not need
	 * to handle REWIND efficiently; see ExecReScanAgg.
	 */
	if (node->aggstrategy == AGG_HASHED)
		eflags &= ~EXEC_FLAG_REWIND;
	outerPlan = outerPlan(node);
	outerPlanState(aggstate) = ExecInitNode(outerPlan, estate, eflags);

	/*
	 * initialize source tuple type.
	 */
	aggstate->ss.ps.outerops =
		ExecGetResultSlotOps(outerPlanState(&aggstate->ss),
							 &aggstate->ss.ps.outeropsfixed);
	aggstate->ss.ps.outeropsset = true;

	ExecCreateScanSlotFromOuterPlan(estate, &aggstate->ss,
									aggstate->ss.ps.outerops);
	scanDesc = aggstate->ss.ss_ScanTupleSlot->tts_tupleDescriptor;

	/*
	 * An extra slot is needed if 1) agg need to do its own sort 2) agg
	 * has more than one non-hashed phases
	 */
	if (node->sortnode || need_extra_slot)
	{
		aggstate->sort_slot = ExecInitExtraTupleSlot(estate, scanDesc,
													 &TTSOpsMinimalTuple);

		/*
		 * The output of the tuplesort, and the output from the outer child
		 * might not use the same type of slot. In most cases the child will
		 * be a Sort, and thus return a TTSOpsMinimalTuple type slot - but the
		 * input can also be presorted due an index, in which case it could be
		 * a different type of slot.
		 *
		 * XXX: For efficiency it would be good to instead/additionally
		 * generate expressions with corresponding settings of outerops* for
		 * the individual phases - deforming is often a bottleneck for
		 * aggregations with lots of rows per group. If there's multiple
		 * sorts, we know that all but the first use TTSOpsMinimalTuple (via
		 * the nodeAgg.c internal tuplesort).
		 */
		if (aggstate->ss.ps.outeropsfixed &&
			aggstate->ss.ps.outerops != &TTSOpsMinimalTuple)
			aggstate->ss.ps.outeropsfixed = false;
	}

	/*
	 * Initialize result type, slot and projection.
	 */
	ExecInitResultTupleSlotTL(&aggstate->ss.ps, &TTSOpsVirtual);
	ExecAssignProjectionInfo(&aggstate->ss.ps, NULL);

	/*
	 * initialize child expressions
	 *
	 * We expect the parser to have checked that no aggs contain other agg
	 * calls in their arguments (and just to be sure, we verify it again while
	 * initializing the plan node).  This would make no sense under SQL
	 * semantics, and it's forbidden by the spec.  Because it is true, we
	 * don't need to worry about evaluating the aggs in any particular order.
	 *
	 * Note: execExpr.c finds Aggrefs for us, and adds their AggrefExprState
	 * nodes to aggstate->aggs.  Aggrefs in the qual are found here; Aggrefs
	 * in the targetlist are found during ExecAssignProjectionInfo, below.
	 */
	aggstate->ss.ps.qual =
		ExecInitQual(node->plan.qual, (PlanState *) aggstate);

	/*
	 * We should now have found all Aggrefs in the targetlist and quals.
	 */
	numaggs = aggstate->numaggs;
	Assert(numaggs == list_length(aggstate->aggs));

	/*
	 * For each phase, prepare grouping set data and fmgr lookup data for
	 * compare functions.  Accumulate all_grouped_cols in passing.
	 */
	aggstate->phases = palloc0(aggstate->numphases * sizeof(AggStatePerPhase));

	for (phaseidx = 0; phaseidx < aggstate->numphases; phaseidx++)
	{
		Agg		   *aggnode;
		AggStatePerPhase phasedata = NULL;

		if (phaseidx > 0)
			aggnode = list_nth_node(Agg, node->chain, phaseidx - 1);
		else
			aggnode = node;

		if (aggnode->aggstrategy == AGG_HASHED)
		{
			AggStatePerPhaseHash perhash;
			Bitmapset *cols = NULL;

			aggstate->num_hashes++;
			totalHashGroups += aggnode->numGroups;

			perhash = (AggStatePerPhaseHash) palloc0(sizeof(AggStatePerPhaseHashData));
			phasedata = (AggStatePerPhase) perhash;
			phasedata->is_hashed = true;
			phasedata->aggnode = aggnode;
			phasedata->aggstrategy = aggnode->aggstrategy;

			/* AGG_HASHED always has only one set */
			phasedata->numsets = 1;
			phasedata->gset_lengths = palloc(sizeof(int));
			phasedata->gset_lengths[0] = perhash->numCols = aggnode->numCols;

			phasedata->grouped_cols = palloc(sizeof(Bitmapset *));
			for (j = 0; j < aggnode->numCols; ++j)
				cols = bms_add_member(cols, aggnode->grpColIdx[j]);
			phasedata->grouped_cols[0] = cols;

			all_grouped_cols = bms_add_members(all_grouped_cols, cols);

			/*
			 * Initialize pergroup state. For AGG_HASHED, all groups do transition
			 * on the fly, all pergroup states are kept in hashtable, everytime
			 * a tuple is processed, lookup_hash_entry() choose one group and
			 * set phasedata->pergroups[0], then advance_aggregates can use it
			 * to do transition in this group.
			 * We do not need to allocate a real pergroup and set the pointer
			 * here, there are too many pergroup states, lookup_hash_entry() will
			 * allocate it.
			 */
			phasedata->pergroups =
				(AggStatePerGroup *) palloc0(sizeof(AggStatePerGroup));

			/*
			 * Hash aggregate does not require the order of input tuples, so
			 * we can do the transition immediately when a tuple is fetched,
			 * which means we can do the transition concurrently with the
			 * first phase.
			 */
			if (phaseidx > 0)
			{
				aggstate->phases[0]->concurrent_hashes =
					lappend(aggstate->phases[0]->concurrent_hashes, perhash);
				/* skip evaltrans for this phase */
				phasedata->skip_evaltrans = true;
			}
		}
		else
		{
			AggStatePerPhaseSort persort;

			persort = (AggStatePerPhaseSort) palloc0(sizeof(AggStatePerPhaseSortData));
			phasedata = (AggStatePerPhase) persort;
			phasedata->is_hashed = false;
			phasedata->aggnode = aggnode;
			phasedata->aggstrategy = aggnode->aggstrategy;

			if (aggnode->groupingSets)
			{
				phasedata->numsets = list_length(aggnode->groupingSets);
				phasedata->gset_lengths = palloc(phasedata->numsets * sizeof(int));
				phasedata->grouped_cols = palloc(phasedata->numsets * sizeof(Bitmapset *));

				i = 0;
				foreach(l, aggnode->groupingSets)
				{
					int		current_length = list_length(lfirst(l));
					Bitmapset	*cols = NULL;

					/* planner forces this to be correct */
					for (j = 0; j < current_length; ++j)
						cols = bms_add_member(cols, aggnode->grpColIdx[j]);

					phasedata->grouped_cols[i] = cols;
					phasedata->gset_lengths[i] = current_length;

					++i;
				}

				all_grouped_cols = bms_add_members(all_grouped_cols,
												   phasedata->grouped_cols[0]);
			}
			else
			{
				phasedata->numsets = 1;
				phasedata->gset_lengths = NULL;
				phasedata->grouped_cols = NULL;
			}

			/*
			 * Initialize pergroup states for AGG_SORTED/AGG_PLAIN/AGG_MIXED
			 * phases, each set only have one group on the fly, all groups in
			 * a set can reuse a pergroup state. Unlike AGG_HASHED, we
			 * pre-allocate the pergroup states here.
			 */
			phasedata->pergroups =
				(AggStatePerGroup *) palloc0(sizeof(AggStatePerGroup) * phasedata->numsets);

			for (i = 0; i < phasedata->numsets; i++)
			{
				phasedata->pergroups[i] =
					(AggStatePerGroup) palloc0(sizeof(AggStatePerGroupData) * numaggs);
			}

			/*
			 * If we are grouping, precompute fmgr lookup data for inner loop.
			 */
			if (aggnode->numCols > 0)
			{
				int			i = 0;

				/*
				 * Build a separate function for each subset of columns that
				 * need to be compared.
				 */
				persort->eqfunctions =
					(ExprState **) palloc0(aggnode->numCols * sizeof(ExprState *));

				/* for each grouping set */
				for (i = 0; i < phasedata->numsets && phasedata->gset_lengths; i++)
				{
					int			length = phasedata->gset_lengths[i];

					if (persort->eqfunctions[length - 1] != NULL)
						continue;

					persort->eqfunctions[length - 1] =
						execTuplesMatchPrepare(scanDesc,
											   length,
											   aggnode->grpColIdx,
											   aggnode->grpOperators,
											   aggnode->grpCollations,
											   (PlanState *) aggstate);
				}

				/* and for all grouped columns, unless already computed */
				if (persort->eqfunctions[aggnode->numCols - 1] == NULL)
				{
					persort->eqfunctions[aggnode->numCols - 1] =
						execTuplesMatchPrepare(scanDesc,
											   aggnode->numCols,
											   aggnode->grpColIdx,
											   aggnode->grpOperators,
											   aggnode->grpCollations,
											   (PlanState *) aggstate);
				}
			}

			/*
			 * For non-first AGG_SORTED phase, it processes the same input
			 * tuples with previous phase except that it need to resort the
			 * input tuples. Tell the previous phase to copy out the tuples.
			 */
			if (phaseidx > 0)
			{
				AggStatePerPhaseSort prev =
					(AggStatePerPhaseSort) aggstate->phases[phaseidx - 1];

				Assert(!prev->phasedata.is_hashed);
				/* Tell the previous phase to copy the tuple to the sort_in */
				prev->copy_out = true;
			}
		}

		phasedata->phaseidx = phaseidx;
		aggstate->phases[phaseidx] = phasedata;
	}

	/*
	 * Convert all_grouped_cols to a descending-order list.
	 */
	i = -1;
	while ((i = bms_next_member(all_grouped_cols, i)) >= 0)
		aggstate->all_grouped_cols = lcons_int(i, aggstate->all_grouped_cols);

	/*
	 * Set up aggregate-result storage in the output expr context, and also
	 * allocate my private per-agg working storage
	 */
	econtext = aggstate->ss.ps.ps_ExprContext;
	econtext->ecxt_aggvalues = (Datum *) palloc0(sizeof(Datum) * numaggs);
	econtext->ecxt_aggnulls = (bool *) palloc0(sizeof(bool) * numaggs);

	peraggs = (AggStatePerAgg) palloc0(sizeof(AggStatePerAggData) * numaggs);
	pertransstates = (AggStatePerTrans) palloc0(sizeof(AggStatePerTransData) * numaggs);

	aggstate->peragg = peraggs;
	aggstate->pertrans = pertransstates;

	aggstate->current_phase = 0;
	initialize_phase(aggstate, 0);
	select_current_set(aggstate, 0, aggstate->aggstrategy == AGG_HASHED);

	/* -----------------
	 * Perform lookups of aggregate function info, and initialize the
	 * unchanging fields of the per-agg and per-trans data.
	 *
	 * We try to optimize by detecting duplicate aggregate functions so that
	 * their state and final values are re-used, rather than needlessly being
	 * re-calculated independently. We also detect aggregates that are not
	 * the same, but which can share the same transition state.
	 *
	 * Scenarios:
	 *
	 * 1. Identical aggregate function calls appear in the query:
	 *
	 *	  SELECT SUM(x) FROM ... HAVING SUM(x) > 0
	 *
	 *	  Since these aggregates are identical, we only need to calculate
	 *	  the value once. Both aggregates will share the same 'aggno' value.
	 *
	 * 2. Two different aggregate functions appear in the query, but the
	 *	  aggregates have the same arguments, transition functions and
	 *	  initial values (and, presumably, different final functions):
	 *
	 *	  SELECT AVG(x), STDDEV(x) FROM ...
	 *
	 *	  In this case we must create a new peragg for the varying aggregate,
	 *	  and we need to call the final functions separately, but we need
	 *	  only run the transition function once.  (This requires that the
	 *	  final functions be nondestructive of the transition state, but
	 *	  that's required anyway for other reasons.)
	 *
	 * For either of these optimizations to be valid, all aggregate properties
	 * used in the transition phase must be the same, including any modifiers
	 * such as ORDER BY, DISTINCT and FILTER, and the arguments mustn't
	 * contain any volatile functions.
	 * -----------------
	 */
	aggno = -1;
	transno = -1;
	foreach(l, aggstate->aggs)
	{
		AggrefExprState *aggrefstate = (AggrefExprState *) lfirst(l);
		Aggref	   *aggref = aggrefstate->aggref;
		AggStatePerAgg peragg;
		AggStatePerTrans pertrans;
		int			existing_aggno;
		int			existing_transno;
		List	   *same_input_transnos;
		Oid			inputTypes[FUNC_MAX_ARGS];
		int			numArguments;
		int			numDirectArgs;
		HeapTuple	aggTuple;
		Form_pg_aggregate aggform;
		AclResult	aclresult;
		Oid			transfn_oid,
					finalfn_oid;
		bool		shareable;
		Oid			serialfn_oid,
					deserialfn_oid;
		Expr	   *finalfnexpr;
		Oid			aggtranstype;
		Datum		textInitVal;
		Datum		initValue;
		bool		initValueIsNull;

		/* Planner should have assigned aggregate to correct level */
		Assert(aggref->agglevelsup == 0);
		/* ... and the split mode should match */
		Assert(aggref->aggsplit == aggstate->aggsplit);

		/* 1. Check for already processed aggs which can be re-used */
		existing_aggno = find_compatible_peragg(aggref, aggstate, aggno,
												&same_input_transnos);
		if (existing_aggno != -1)
		{
			/*
			 * Existing compatible agg found. so just point the Aggref to the
			 * same per-agg struct.
			 */
			aggrefstate->aggno = existing_aggno;
			continue;
		}

		/* Mark Aggref state node with assigned index in the result array */
		peragg = &peraggs[++aggno];
		peragg->aggref = aggref;
		aggrefstate->aggno = aggno;

		/* Fetch the pg_aggregate row */
		aggTuple = SearchSysCache1(AGGFNOID,
								   ObjectIdGetDatum(aggref->aggfnoid));
		if (!HeapTupleIsValid(aggTuple))
			elog(ERROR, "cache lookup failed for aggregate %u",
				 aggref->aggfnoid);
		aggform = (Form_pg_aggregate) GETSTRUCT(aggTuple);

		/* Check permission to call aggregate function */
		aclresult = pg_proc_aclcheck(aggref->aggfnoid, GetUserId(),
									 ACL_EXECUTE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, OBJECT_AGGREGATE,
						   get_func_name(aggref->aggfnoid));
		InvokeFunctionExecuteHook(aggref->aggfnoid);

		/* planner recorded transition state type in the Aggref itself */
		aggtranstype = aggref->aggtranstype;
		Assert(OidIsValid(aggtranstype));

		/*
		 * If this aggregation is performing state combines, then instead of
		 * using the transition function, we'll use the combine function
		 */
		if (DO_AGGSPLIT_COMBINE(aggstate->aggsplit))
		{
			transfn_oid = aggform->aggcombinefn;

			/* If not set then the planner messed up */
			if (!OidIsValid(transfn_oid))
				elog(ERROR, "combinefn not set for aggregate function");
		}
		else
			transfn_oid = aggform->aggtransfn;

		/* Final function only required if we're finalizing the aggregates */
		if (DO_AGGSPLIT_SKIPFINAL(aggstate->aggsplit))
			peragg->finalfn_oid = finalfn_oid = InvalidOid;
		else
			peragg->finalfn_oid = finalfn_oid = aggform->aggfinalfn;

		/*
		 * If finalfn is marked read-write, we can't share transition states;
		 * but it is okay to share states for AGGMODIFY_SHAREABLE aggs.  Also,
		 * if we're not executing the finalfn here, we can share regardless.
		 */
		shareable = (aggform->aggfinalmodify != AGGMODIFY_READ_WRITE) ||
			(finalfn_oid == InvalidOid);
		peragg->shareable = shareable;

		serialfn_oid = InvalidOid;
		deserialfn_oid = InvalidOid;

		/*
		 * Check if serialization/deserialization is required.  We only do it
		 * for aggregates that have transtype INTERNAL.
		 */
		if (aggtranstype == INTERNALOID)
		{
			/*
			 * The planner should only have generated a serialize agg node if
			 * every aggregate with an INTERNAL state has a serialization
			 * function.  Verify that.
			 */
			if (DO_AGGSPLIT_SERIALIZE(aggstate->aggsplit))
			{
				/* serialization only valid when not running finalfn */
				Assert(DO_AGGSPLIT_SKIPFINAL(aggstate->aggsplit));

				if (!OidIsValid(aggform->aggserialfn))
					elog(ERROR, "serialfunc not provided for serialization aggregation");
				serialfn_oid = aggform->aggserialfn;
			}

			/* Likewise for deserialization functions */
			if (DO_AGGSPLIT_DESERIALIZE(aggstate->aggsplit))
			{
				/* deserialization only valid when combining states */
				Assert(DO_AGGSPLIT_COMBINE(aggstate->aggsplit));

				if (!OidIsValid(aggform->aggdeserialfn))
					elog(ERROR, "deserialfunc not provided for deserialization aggregation");
				deserialfn_oid = aggform->aggdeserialfn;
			}
		}

		/* Check that aggregate owner has permission to call component fns */
		{
			HeapTuple	procTuple;
			Oid			aggOwner;

			procTuple = SearchSysCache1(PROCOID,
										ObjectIdGetDatum(aggref->aggfnoid));
			if (!HeapTupleIsValid(procTuple))
				elog(ERROR, "cache lookup failed for function %u",
					 aggref->aggfnoid);
			aggOwner = ((Form_pg_proc) GETSTRUCT(procTuple))->proowner;
			ReleaseSysCache(procTuple);

			aclresult = pg_proc_aclcheck(transfn_oid, aggOwner,
										 ACL_EXECUTE);
			if (aclresult != ACLCHECK_OK)
				aclcheck_error(aclresult, OBJECT_FUNCTION,
							   get_func_name(transfn_oid));
			InvokeFunctionExecuteHook(transfn_oid);
			if (OidIsValid(finalfn_oid))
			{
				aclresult = pg_proc_aclcheck(finalfn_oid, aggOwner,
											 ACL_EXECUTE);
				if (aclresult != ACLCHECK_OK)
					aclcheck_error(aclresult, OBJECT_FUNCTION,
								   get_func_name(finalfn_oid));
				InvokeFunctionExecuteHook(finalfn_oid);
			}
			if (OidIsValid(serialfn_oid))
			{
				aclresult = pg_proc_aclcheck(serialfn_oid, aggOwner,
											 ACL_EXECUTE);
				if (aclresult != ACLCHECK_OK)
					aclcheck_error(aclresult, OBJECT_FUNCTION,
								   get_func_name(serialfn_oid));
				InvokeFunctionExecuteHook(serialfn_oid);
			}
			if (OidIsValid(deserialfn_oid))
			{
				aclresult = pg_proc_aclcheck(deserialfn_oid, aggOwner,
											 ACL_EXECUTE);
				if (aclresult != ACLCHECK_OK)
					aclcheck_error(aclresult, OBJECT_FUNCTION,
								   get_func_name(deserialfn_oid));
				InvokeFunctionExecuteHook(deserialfn_oid);
			}
		}

		/*
		 * Get actual datatypes of the (nominal) aggregate inputs.  These
		 * could be different from the agg's declared input types, when the
		 * agg accepts ANY or a polymorphic type.
		 */
		numArguments = get_aggregate_argtypes(aggref, inputTypes);

		/* Count the "direct" arguments, if any */
		numDirectArgs = list_length(aggref->aggdirectargs);

		/* Detect how many arguments to pass to the finalfn */
		if (aggform->aggfinalextra)
			peragg->numFinalArgs = numArguments + 1;
		else
			peragg->numFinalArgs = numDirectArgs + 1;

		/* Initialize any direct-argument expressions */
		peragg->aggdirectargs = ExecInitExprList(aggref->aggdirectargs,
												 (PlanState *) aggstate);

		/*
		 * build expression trees using actual argument & result types for the
		 * finalfn, if it exists and is required.
		 */
		if (OidIsValid(finalfn_oid))
		{
			build_aggregate_finalfn_expr(inputTypes,
										 peragg->numFinalArgs,
										 aggtranstype,
										 aggref->aggtype,
										 aggref->inputcollid,
										 finalfn_oid,
										 &finalfnexpr);
			fmgr_info(finalfn_oid, &peragg->finalfn);
			fmgr_info_set_expr((Node *) finalfnexpr, &peragg->finalfn);
		}

		/* get info about the output value's datatype */
		get_typlenbyval(aggref->aggtype,
						&peragg->resulttypeLen,
						&peragg->resulttypeByVal);

		/*
		 * initval is potentially null, so don't try to access it as a struct
		 * field. Must do it the hard way with SysCacheGetAttr.
		 */
		textInitVal = SysCacheGetAttr(AGGFNOID, aggTuple,
									  Anum_pg_aggregate_agginitval,
									  &initValueIsNull);
		if (initValueIsNull)
			initValue = (Datum) 0;
		else
			initValue = GetAggInitVal(textInitVal, aggtranstype);

		/*
		 * 2. Build working state for invoking the transition function, or
		 * look up previously initialized working state, if we can share it.
		 *
		 * find_compatible_peragg() already collected a list of shareable
		 * per-Trans's with the same inputs. Check if any of them have the
		 * same transition function and initial value.
		 */
		existing_transno = find_compatible_pertrans(aggstate, aggref,
													shareable,
													transfn_oid, aggtranstype,
													serialfn_oid, deserialfn_oid,
													initValue, initValueIsNull,
													same_input_transnos);
		if (existing_transno != -1)
		{
			/*
			 * Existing compatible trans found, so just point the 'peragg' to
			 * the same per-trans struct, and mark the trans state as shared.
			 */
			pertrans = &pertransstates[existing_transno];
			pertrans->aggshared = true;
			peragg->transno = existing_transno;
		}
		else
		{
			pertrans = &pertransstates[++transno];
			build_pertrans_for_aggref(pertrans, aggstate, estate,
									  aggref, transfn_oid, aggtranstype,
									  serialfn_oid, deserialfn_oid,
									  initValue, initValueIsNull,
									  inputTypes, numArguments);
			peragg->transno = transno;
		}
		ReleaseSysCache(aggTuple);
	}

	/*
	 * Update aggstate->numaggs to be the number of unique aggregates found.
	 * Also set numstates to the number of unique transition states found.
	 */
	aggstate->numaggs = aggno + 1;
	aggstate->numtrans = transno + 1;

	/*
	 * Last, check whether any more aggregates got added onto the node while
	 * we processed the expressions for the aggregate arguments (including not
	 * only the regular arguments and FILTER expressions handled immediately
	 * above, but any direct arguments we might've handled earlier).  If so,
	 * we have nested aggregate functions, which is semantically nonsensical,
	 * so complain.  (This should have been caught by the parser, so we don't
	 * need to work hard on a helpful error message; but we defend against it
	 * here anyway, just to be sure.)
	 */
	if (numaggs != list_length(aggstate->aggs))
		ereport(ERROR,
				(errcode(ERRCODE_GROUPING_ERROR),
				 errmsg("aggregate function calls cannot be nested")));

	/*
	 * Initialize current phase-dependent values to initial phase.
	 */
	if (use_hashing)
	{
		Plan   *outerplan = outerPlan(node);

		aggstate->hash_metacxt = AllocSetContextCreate(
			aggstate->ss.ps.state->es_query_cxt,
			"HashAgg meta context",
			ALLOCSET_DEFAULT_SIZES);
		aggstate->hash_spill_slot = ExecInitExtraTupleSlot(
			estate, scanDesc, &TTSOpsMinimalTuple);

		aggstate->hashentrysize = hash_agg_entry_size(
			aggstate->numtrans, outerplan->plan_width, node->transitionSpace);

		/*
		 * Consider all of the grouping sets together when setting the limits
		 * and estimating the number of partitions. This can be inaccurate
		 * when there is more than one grouping set, but should still be
		 * reasonable.
		 */
		hash_agg_set_limits(aggstate->hashentrysize, totalHashGroups, 0,
							&aggstate->hash_mem_limit,
							&aggstate->hash_ngroups_limit,
							&aggstate->hash_planned_partitions);

		find_hash_columns(aggstate);
		build_hash_tables(aggstate);
		aggstate->table_filled = false;
	}

	/*
	 * Build expressions doing all the transition work at once. We build a
	 * different one for each phase, as the number of transition function
	 * invocation can differ between phases. Note this'll work both for
	 * transition and combination functions (although there'll only be one
	 * phase in the latter case).
	 */
	for (phaseidx = 0; phaseidx < aggstate->numphases; phaseidx++)
	{
		AggStatePerPhase phase = aggstate->phases[phaseidx];

		if (phase->skip_evaltrans)
			continue;

		phase->evaltrans = ExecBuildAggTrans(aggstate, phase, false, true);

		/* cache compiled expression for outer slot without NULL check */
		phase->evaltrans_cache[HASHAGG_INITIAL] = phase->evaltrans;
	}

	return aggstate;
}

/*
 * Build the state needed to calculate a state value for an aggregate.
 *
 * This initializes all the fields in 'pertrans'. 'aggref' is the aggregate
 * to initialize the state for. 'aggtransfn', 'aggtranstype', and the rest
 * of the arguments could be calculated from 'aggref', but the caller has
 * calculated them already, so might as well pass them.
 */
static void
build_pertrans_for_aggref(AggStatePerTrans pertrans,
						  AggState *aggstate, EState *estate,
						  Aggref *aggref,
						  Oid aggtransfn, Oid aggtranstype,
						  Oid aggserialfn, Oid aggdeserialfn,
						  Datum initValue, bool initValueIsNull,
						  Oid *inputTypes, int numArguments)
{
	int			numGroupingSets = Max(aggstate->maxsets, 1);
	Expr	   *serialfnexpr = NULL;
	Expr	   *deserialfnexpr = NULL;
	ListCell   *lc;
	int			numInputs;
	int			numDirectArgs;
	List	   *sortlist;
	int			numSortCols;
	int			numDistinctCols;
	int			i;

	/* Begin filling in the pertrans data */
	pertrans->aggref = aggref;
	pertrans->aggshared = false;
	pertrans->aggCollation = aggref->inputcollid;
	pertrans->transfn_oid = aggtransfn;
	pertrans->serialfn_oid = aggserialfn;
	pertrans->deserialfn_oid = aggdeserialfn;
	pertrans->initValue = initValue;
	pertrans->initValueIsNull = initValueIsNull;

	/* Count the "direct" arguments, if any */
	numDirectArgs = list_length(aggref->aggdirectargs);

	/* Count the number of aggregated input columns */
	pertrans->numInputs = numInputs = list_length(aggref->args);

	pertrans->aggtranstype = aggtranstype;

	/*
	 * When combining states, we have no use at all for the aggregate
	 * function's transfn. Instead we use the combinefn.  In this case, the
	 * transfn and transfn_oid fields of pertrans refer to the combine
	 * function rather than the transition function.
	 */
	if (DO_AGGSPLIT_COMBINE(aggstate->aggsplit))
	{
		Expr	   *combinefnexpr;
		size_t		numTransArgs;

		/*
		 * When combining there's only one input, the to-be-combined added
		 * transition value from below (this node's transition value is
		 * counted separately).
		 */
		pertrans->numTransInputs = 1;

		/* account for the current transition state */
		numTransArgs = pertrans->numTransInputs + 1;

		build_aggregate_combinefn_expr(aggtranstype,
									   aggref->inputcollid,
									   aggtransfn,
									   &combinefnexpr);
		fmgr_info(aggtransfn, &pertrans->transfn);
		fmgr_info_set_expr((Node *) combinefnexpr, &pertrans->transfn);

		pertrans->transfn_fcinfo =
			(FunctionCallInfo) palloc(SizeForFunctionCallInfo(2));
		InitFunctionCallInfoData(*pertrans->transfn_fcinfo,
								 &pertrans->transfn,
								 numTransArgs,
								 pertrans->aggCollation,
								 (void *) aggstate, NULL);

		/*
		 * Ensure that a combine function to combine INTERNAL states is not
		 * strict. This should have been checked during CREATE AGGREGATE, but
		 * the strict property could have been changed since then.
		 */
		if (pertrans->transfn.fn_strict && aggtranstype == INTERNALOID)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("combine function with transition type %s must not be declared STRICT",
							format_type_be(aggtranstype))));
	}
	else
	{
		Expr	   *transfnexpr;
		size_t		numTransArgs;

		/* Detect how many arguments to pass to the transfn */
		if (AGGKIND_IS_ORDERED_SET(aggref->aggkind))
			pertrans->numTransInputs = numInputs;
		else
			pertrans->numTransInputs = numArguments;

		/* account for the current transition state */
		numTransArgs = pertrans->numTransInputs + 1;

		/*
		 * Set up infrastructure for calling the transfn.  Note that
		 * invtransfn is not needed here.
		 */
		build_aggregate_transfn_expr(inputTypes,
									 numArguments,
									 numDirectArgs,
									 aggref->aggvariadic,
									 aggtranstype,
									 aggref->inputcollid,
									 aggtransfn,
									 InvalidOid,
									 &transfnexpr,
									 NULL);
		fmgr_info(aggtransfn, &pertrans->transfn);
		fmgr_info_set_expr((Node *) transfnexpr, &pertrans->transfn);

		pertrans->transfn_fcinfo =
			(FunctionCallInfo) palloc(SizeForFunctionCallInfo(numTransArgs));
		InitFunctionCallInfoData(*pertrans->transfn_fcinfo,
								 &pertrans->transfn,
								 numTransArgs,
								 pertrans->aggCollation,
								 (void *) aggstate, NULL);

		/*
		 * If the transfn is strict and the initval is NULL, make sure input
		 * type and transtype are the same (or at least binary-compatible), so
		 * that it's OK to use the first aggregated input value as the initial
		 * transValue.  This should have been checked at agg definition time,
		 * but we must check again in case the transfn's strictness property
		 * has been changed.
		 */
		if (pertrans->transfn.fn_strict && pertrans->initValueIsNull)
		{
			if (numArguments <= numDirectArgs ||
				!IsBinaryCoercible(inputTypes[numDirectArgs],
								   aggtranstype))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
						 errmsg("aggregate %u needs to have compatible input type and transition type",
								aggref->aggfnoid)));
		}
	}

	/* get info about the state value's datatype */
	get_typlenbyval(aggtranstype,
					&pertrans->transtypeLen,
					&pertrans->transtypeByVal);

	if (OidIsValid(aggserialfn))
	{
		build_aggregate_serialfn_expr(aggserialfn,
									  &serialfnexpr);
		fmgr_info(aggserialfn, &pertrans->serialfn);
		fmgr_info_set_expr((Node *) serialfnexpr, &pertrans->serialfn);

		pertrans->serialfn_fcinfo =
			(FunctionCallInfo) palloc(SizeForFunctionCallInfo(1));
		InitFunctionCallInfoData(*pertrans->serialfn_fcinfo,
								 &pertrans->serialfn,
								 1,
								 InvalidOid,
								 (void *) aggstate, NULL);
	}

	if (OidIsValid(aggdeserialfn))
	{
		build_aggregate_deserialfn_expr(aggdeserialfn,
										&deserialfnexpr);
		fmgr_info(aggdeserialfn, &pertrans->deserialfn);
		fmgr_info_set_expr((Node *) deserialfnexpr, &pertrans->deserialfn);

		pertrans->deserialfn_fcinfo =
			(FunctionCallInfo) palloc(SizeForFunctionCallInfo(2));
		InitFunctionCallInfoData(*pertrans->deserialfn_fcinfo,
								 &pertrans->deserialfn,
								 2,
								 InvalidOid,
								 (void *) aggstate, NULL);

	}

	/*
	 * If we're doing either DISTINCT or ORDER BY for a plain agg, then we
	 * have a list of SortGroupClause nodes; fish out the data in them and
	 * stick them into arrays.  We ignore ORDER BY for an ordered-set agg,
	 * however; the agg's transfn and finalfn are responsible for that.
	 *
	 * Note that by construction, if there is a DISTINCT clause then the ORDER
	 * BY clause is a prefix of it (see transformDistinctClause).
	 */
	if (AGGKIND_IS_ORDERED_SET(aggref->aggkind))
	{
		sortlist = NIL;
		numSortCols = numDistinctCols = 0;
	}
	else if (aggref->aggdistinct)
	{
		sortlist = aggref->aggdistinct;
		numSortCols = numDistinctCols = list_length(sortlist);
		Assert(numSortCols >= list_length(aggref->aggorder));
	}
	else
	{
		sortlist = aggref->aggorder;
		numSortCols = list_length(sortlist);
		numDistinctCols = 0;
	}

	pertrans->numSortCols = numSortCols;
	pertrans->numDistinctCols = numDistinctCols;

	/*
	 * If we have either sorting or filtering to do, create a tupledesc and
	 * slot corresponding to the aggregated inputs (including sort
	 * expressions) of the agg.
	 */
	if (numSortCols > 0 || aggref->aggfilter)
	{
		pertrans->sortdesc = ExecTypeFromTL(aggref->args);
		pertrans->sortslot =
			ExecInitExtraTupleSlot(estate, pertrans->sortdesc,
								   &TTSOpsMinimalTuple);
	}

	if (numSortCols > 0)
	{
		/*
		 * We don't implement DISTINCT or ORDER BY aggs in the HASHED case
		 * (yet)
		 */
		Assert(aggstate->aggstrategy != AGG_HASHED && aggstate->aggstrategy != AGG_MIXED);

		/* If we have only one input, we need its len/byval info. */
		if (numInputs == 1)
		{
			get_typlenbyval(inputTypes[numDirectArgs],
							&pertrans->inputtypeLen,
							&pertrans->inputtypeByVal);
		}
		else if (numDistinctCols > 0)
		{
			/* we will need an extra slot to store prior values */
			pertrans->uniqslot =
				ExecInitExtraTupleSlot(estate, pertrans->sortdesc,
									   &TTSOpsMinimalTuple);
		}

		/* Extract the sort information for use later */
		pertrans->sortColIdx =
			(AttrNumber *) palloc(numSortCols * sizeof(AttrNumber));
		pertrans->sortOperators =
			(Oid *) palloc(numSortCols * sizeof(Oid));
		pertrans->sortCollations =
			(Oid *) palloc(numSortCols * sizeof(Oid));
		pertrans->sortNullsFirst =
			(bool *) palloc(numSortCols * sizeof(bool));

		i = 0;
		foreach(lc, sortlist)
		{
			SortGroupClause *sortcl = (SortGroupClause *) lfirst(lc);
			TargetEntry *tle = get_sortgroupclause_tle(sortcl, aggref->args);

			/* the parser should have made sure of this */
			Assert(OidIsValid(sortcl->sortop));

			pertrans->sortColIdx[i] = tle->resno;
			pertrans->sortOperators[i] = sortcl->sortop;
			pertrans->sortCollations[i] = exprCollation((Node *) tle->expr);
			pertrans->sortNullsFirst[i] = sortcl->nulls_first;
			i++;
		}
		Assert(i == numSortCols);
	}

	if (aggref->aggdistinct)
	{
		Oid		   *ops;

		Assert(numArguments > 0);
		Assert(list_length(aggref->aggdistinct) == numDistinctCols);

		ops = palloc(numDistinctCols * sizeof(Oid));

		i = 0;
		foreach(lc, aggref->aggdistinct)
			ops[i++] = ((SortGroupClause *) lfirst(lc))->eqop;

		/* lookup / build the necessary comparators */
		if (numDistinctCols == 1)
			fmgr_info(get_opcode(ops[0]), &pertrans->equalfnOne);
		else
			pertrans->equalfnMulti =
				execTuplesMatchPrepare(pertrans->sortdesc,
									   numDistinctCols,
									   pertrans->sortColIdx,
									   ops,
									   pertrans->sortCollations,
									   &aggstate->ss.ps);
		pfree(ops);
	}

	pertrans->sortstates = (Tuplesortstate **)
		palloc0(sizeof(Tuplesortstate *) * numGroupingSets);
}


static Datum
GetAggInitVal(Datum textInitVal, Oid transtype)
{
	Oid			typinput,
				typioparam;
	char	   *strInitVal;
	Datum		initVal;

	getTypeInputInfo(transtype, &typinput, &typioparam);
	strInitVal = TextDatumGetCString(textInitVal);
	initVal = OidInputFunctionCall(typinput, strInitVal,
								   typioparam, -1);
	pfree(strInitVal);
	return initVal;
}

/*
 * find_compatible_peragg - search for a previously initialized per-Agg struct
 *
 * Searches the previously looked at aggregates to find one which is compatible
 * with this one, with the same input parameters. If no compatible aggregate
 * can be found, returns -1.
 *
 * As a side-effect, this also collects a list of existing, shareable per-Trans
 * structs with matching inputs. If no identical Aggref is found, the list is
 * passed later to find_compatible_pertrans, to see if we can at least reuse
 * the state value of another aggregate.
 */
static int
find_compatible_peragg(Aggref *newagg, AggState *aggstate,
					   int lastaggno, List **same_input_transnos)
{
	int			aggno;
	AggStatePerAgg peraggs;

	*same_input_transnos = NIL;

	/* we mustn't reuse the aggref if it contains volatile function calls */
	if (contain_volatile_functions((Node *) newagg))
		return -1;

	peraggs = aggstate->peragg;

	/*
	 * Search through the list of already seen aggregates. If we find an
	 * existing identical aggregate call, then we can re-use that one. While
	 * searching, we'll also collect a list of Aggrefs with the same input
	 * parameters. If no matching Aggref is found, the caller can potentially
	 * still re-use the transition state of one of them.  (At this stage we
	 * just compare the parsetrees; whether different aggregates share the
	 * same transition function will be checked later.)
	 */
	for (aggno = 0; aggno <= lastaggno; aggno++)
	{
		AggStatePerAgg peragg;
		Aggref	   *existingRef;

		peragg = &peraggs[aggno];
		existingRef = peragg->aggref;

		/* all of the following must be the same or it's no match */
		if (newagg->inputcollid != existingRef->inputcollid ||
			newagg->aggtranstype != existingRef->aggtranstype ||
			newagg->aggstar != existingRef->aggstar ||
			newagg->aggvariadic != existingRef->aggvariadic ||
			newagg->aggkind != existingRef->aggkind ||
			!equal(newagg->args, existingRef->args) ||
			!equal(newagg->aggorder, existingRef->aggorder) ||
			!equal(newagg->aggdistinct, existingRef->aggdistinct) ||
			!equal(newagg->aggfilter, existingRef->aggfilter))
			continue;

		/* if it's the same aggregate function then report exact match */
		if (newagg->aggfnoid == existingRef->aggfnoid &&
			newagg->aggtype == existingRef->aggtype &&
			newagg->aggcollid == existingRef->aggcollid &&
			equal(newagg->aggdirectargs, existingRef->aggdirectargs))
		{
			list_free(*same_input_transnos);
			*same_input_transnos = NIL;
			return aggno;
		}

		/*
		 * Not identical, but it had the same inputs.  If the final function
		 * permits sharing, return its transno to the caller, in case we can
		 * re-use its per-trans state.  (If there's already sharing going on,
		 * we might report a transno more than once.  find_compatible_pertrans
		 * is cheap enough that it's not worth spending cycles to avoid that.)
		 */
		if (peragg->shareable)
			*same_input_transnos = lappend_int(*same_input_transnos,
											   peragg->transno);
	}

	return -1;
}

/*
 * find_compatible_pertrans - search for a previously initialized per-Trans
 * struct
 *
 * Searches the list of transnos for a per-Trans struct with the same
 * transition function and initial condition. (The inputs have already been
 * verified to match.)
 */
static int
find_compatible_pertrans(AggState *aggstate, Aggref *newagg, bool shareable,
						 Oid aggtransfn, Oid aggtranstype,
						 Oid aggserialfn, Oid aggdeserialfn,
						 Datum initValue, bool initValueIsNull,
						 List *transnos)
{
	ListCell   *lc;

	/* If this aggregate can't share transition states, give up */
	if (!shareable)
		return -1;

	foreach(lc, transnos)
	{
		int			transno = lfirst_int(lc);
		AggStatePerTrans pertrans = &aggstate->pertrans[transno];

		/*
		 * if the transfns or transition state types are not the same then the
		 * state can't be shared.
		 */
		if (aggtransfn != pertrans->transfn_oid ||
			aggtranstype != pertrans->aggtranstype)
			continue;

		/*
		 * The serialization and deserialization functions must match, if
		 * present, as we're unable to share the trans state for aggregates
		 * which will serialize or deserialize into different formats.
		 * Remember that these will be InvalidOid if they're not required for
		 * this agg node.
		 */
		if (aggserialfn != pertrans->serialfn_oid ||
			aggdeserialfn != pertrans->deserialfn_oid)
			continue;

		/*
		 * Check that the initial condition matches, too.
		 */
		if (initValueIsNull && pertrans->initValueIsNull)
			return transno;

		if (!initValueIsNull && !pertrans->initValueIsNull &&
			datumIsEqual(initValue, pertrans->initValue,
						 pertrans->transtypeByVal, pertrans->transtypeLen))
			return transno;
	}
	return -1;
}

void
ExecEndAgg(AggState *node)
{
	PlanState  *outerPlan;
	int			transno;
	int			numGroupingSets = Max(node->maxsets, 1);
	int			setno;
	int			phaseidx;

	/* Make sure we have closed any open tuplesorts */
	for (phaseidx = 0; phaseidx < node->numphases; phaseidx++)
	{
		AggStatePerPhase		phase = node->phases[phaseidx];
		AggStatePerPhaseSort	persort;

		if (phase->is_hashed)
			continue;

		persort = (AggStatePerPhaseSort) phase;
		if (persort->sort_in)
			tuplesort_end(persort->sort_in);
	}

	hashagg_reset_spill_state(node);

	if (node->hash_metacxt != NULL)
	{
		MemoryContextDelete(node->hash_metacxt);
		node->hash_metacxt = NULL;
	}

	for (transno = 0; transno < node->numtrans; transno++)
	{
		AggStatePerTrans pertrans = &node->pertrans[transno];

		for (setno = 0; setno < numGroupingSets; setno++)
		{
			if (pertrans->sortstates[setno])
				tuplesort_end(pertrans->sortstates[setno]);
		}
	}

	/* And ensure any agg shutdown callbacks have been called */
	for (setno = 0; setno < numGroupingSets; setno++)
		ReScanExprContext(node->aggcontexts[setno]);
	if (node->hashcontext)
		ReScanExprContext(node->hashcontext);

	/*
	 * We don't actually free any ExprContexts here (see comment in
	 * ExecFreeExprContext), just unlinking the output one from the plan node
	 * suffices.
	 */
	ExecFreeExprContext(&node->ss.ps);

	/* clean up tuple table */
	ExecClearTuple(node->ss.ss_ScanTupleSlot);

	outerPlan = outerPlanState(node);
	ExecEndNode(outerPlan);
}

void
ExecReScanAgg(AggState *node)
{
	ExprContext *econtext = node->ss.ps.ps_ExprContext;
	PlanState  *outerPlan = outerPlanState(node);
	Agg		   *aggnode = (Agg *) node->ss.ps.plan;
	int			transno;
	int			numGroupingSets = Max(node->maxsets, 1);
	int			setno;
	int			phaseidx;

	node->agg_done = false;

	if (node->aggstrategy == AGG_HASHED)
	{
		/*
		 * In the hashed case, if we haven't yet built the hash table then we
		 * can just return; nothing done yet, so nothing to undo. If subnode's
		 * chgParam is not NULL then it will be re-scanned by ExecProcNode,
		 * else no reason to re-scan it at all.
		 */
		if (!node->table_filled)
			return;

		/*
		 * If we do have the hash table, and it never spilled, and the subplan
		 * does not have any parameter changes, and none of our own parameter
		 * changes affect input expressions of the aggregated functions, then
		 * we can just rescan the existing hash table; no need to build it
		 * again.
		 */
		if (outerPlan->chgParam == NULL && !node->hash_ever_spilled &&
			!bms_overlap(node->ss.ps.chgParam, aggnode->aggParams))
		{
			AggStatePerPhaseHash perhash = (AggStatePerPhaseHash) node->phases[0];
			ResetTupleHashIterator(perhash->hashtable,
								   &perhash->hashiter);

			/* reset to phase 0 */
			initialize_phase(node, 0);
			select_current_set(node, 0, true);
			return;
		}
	}

	/* Make sure we have closed any open tuplesorts */
	for (transno = 0; transno < node->numtrans; transno++)
	{
		for (setno = 0; setno < numGroupingSets; setno++)
		{
			AggStatePerTrans pertrans = &node->pertrans[transno];

			if (pertrans->sortstates[setno])
			{
				tuplesort_end(pertrans->sortstates[setno]);
				pertrans->sortstates[setno] = NULL;
			}
		}
	}

	/*
	 * We don't need to ReScanExprContext the output tuple context here;
	 * ExecReScan already did it. But we do need to reset our per-grouping-set
	 * contexts, which may have transvalues stored in them. (We use rescan
	 * rather than just reset because transfns may have registered callbacks
	 * that need to be run now.) For the AGG_HASHED case, see below.
	 */

	for (setno = 0; setno < numGroupingSets; setno++)
	{
		ReScanExprContext(node->aggcontexts[setno]);
	}

	/* Release first tuple of group, if we have made a copy */
	if (node->grp_firstTuple != NULL)
	{
		heap_freetuple(node->grp_firstTuple);
		node->grp_firstTuple = NULL;
	}
	ExecClearTuple(node->ss.ss_ScanTupleSlot);

	/* Forget current agg values */
	MemSet(econtext->ecxt_aggvalues, 0, sizeof(Datum) * node->numaggs);
	MemSet(econtext->ecxt_aggnulls, 0, sizeof(bool) * node->numaggs);

	/*
	 * With AGG_HASHED/MIXED, the hash table is allocated in a sub-context of
	 * the hashcontext. This used to be an issue, but now, resetting a context
	 * automatically deletes sub-contexts too.
	 */
	if (node->aggstrategy == AGG_HASHED || node->aggstrategy == AGG_MIXED)
	{
		hashagg_reset_spill_state(node);

		node->hash_ever_spilled = false;
		node->hash_spill_mode = false;
		node->hash_ngroups_current = 0;

		ReScanExprContext(node->hashcontext);
		/* Rebuild an empty hash table */
		build_hash_tables(node);
		node->table_filled = false;
		/* iterator will be reset when the table is filled */

		node->hash_agg_stage = HASHAGG_INITIAL;
		hashagg_recompile_expressions(node);
	}

	if (node->aggstrategy != AGG_HASHED)
	{
		/*
		 * Reset the per-group state (in particular, mark transvalues null)
		 */
		for (phaseidx = 0; phaseidx < node->numphases; phaseidx++)
		{
			AggStatePerPhase phase = node->phases[phaseidx];

			/* hash pergroups is reset by build_hash_tables */
			if (phase->is_hashed)
				continue;

			for (setno = 0; setno < phase->numsets; setno++)
				MemSet(phase->pergroups[setno], 0,
					   sizeof(AggStatePerGroupData) * node->numaggs);
		}

		/*
		 * The agg did its own first sort using tuplesort and the first
		 * tuplesort is kept (see initialize_phase), if the subplan does
		 * not have any parameter changes, and none of our own parameter
		 * changes affect input expressions of the aggregated functions,
		 * then we can just rescan the first tuplesort, no need to build
		 * it again.
		 *
		 * Note: agg only do its own sort for groupingsets now.
		 */
		if (aggnode->sortnode)
		{
			AggStatePerPhaseSort firstphase = (AggStatePerPhaseSort) node->phases[0];
			bool randomAccess = (node->eflags & (EXEC_FLAG_REWIND |
												 EXEC_FLAG_BACKWARD |
												 EXEC_FLAG_MARK)) != 0;
			if (firstphase->sort_in &&
				randomAccess &&
				outerPlan->chgParam == NULL &&
				!bms_overlap(node->ss.ps.chgParam, aggnode->aggParams))
			{
				tuplesort_rescan(firstphase->sort_in);
				node->input_sorted = true;
			}
			else
			{
				if (firstphase->sort_in)
					tuplesort_end(firstphase->sort_in);
				firstphase->sort_in = NULL;
				node->input_sorted = false;
			}
		}

		/* reset to phase 0 */
		initialize_phase(node, 0);

		node->input_done = false;
		node->projected_set = -1;
	}

	if (outerPlan->chgParam == NULL)
		ExecReScan(outerPlan);
}


/***********************************************************************
 * API exposed to aggregate functions
 ***********************************************************************/


/*
 * AggCheckCallContext - test if a SQL function is being called as an aggregate
 *
 * The transition and/or final functions of an aggregate may want to verify
 * that they are being called as aggregates, rather than as plain SQL
 * functions.  They should use this function to do so.  The return value
 * is nonzero if being called as an aggregate, or zero if not.  (Specific
 * nonzero values are AGG_CONTEXT_AGGREGATE or AGG_CONTEXT_WINDOW, but more
 * values could conceivably appear in future.)
 *
 * If aggcontext isn't NULL, the function also stores at *aggcontext the
 * identity of the memory context that aggregate transition values are being
 * stored in.  Note that the same aggregate call site (flinfo) may be called
 * interleaved on different transition values in different contexts, so it's
 * not kosher to cache aggcontext under fn_extra.  It is, however, kosher to
 * cache it in the transvalue itself (for internal-type transvalues).
 */
int
AggCheckCallContext(FunctionCallInfo fcinfo, MemoryContext *aggcontext)
{
	if (fcinfo->context && IsA(fcinfo->context, AggState))
	{
		if (aggcontext)
		{
			AggState   *aggstate = ((AggState *) fcinfo->context);
			ExprContext *cxt = aggstate->curaggcontext;

			*aggcontext = cxt->ecxt_per_tuple_memory;
		}
		return AGG_CONTEXT_AGGREGATE;
	}
	if (fcinfo->context && IsA(fcinfo->context, WindowAggState))
	{
		if (aggcontext)
			*aggcontext = ((WindowAggState *) fcinfo->context)->curaggcontext;
		return AGG_CONTEXT_WINDOW;
	}

	/* this is just to prevent "uninitialized variable" warnings */
	if (aggcontext)
		*aggcontext = NULL;
	return 0;
}

/*
 * AggGetAggref - allow an aggregate support function to get its Aggref
 *
 * If the function is being called as an aggregate support function,
 * return the Aggref node for the aggregate call.  Otherwise, return NULL.
 *
 * Aggregates sharing the same inputs and transition functions can get
 * merged into a single transition calculation.  If the transition function
 * calls AggGetAggref, it will get some one of the Aggrefs for which it is
 * executing.  It must therefore not pay attention to the Aggref fields that
 * relate to the final function, as those are indeterminate.  But if a final
 * function calls AggGetAggref, it will get a precise result.
 *
 * Note that if an aggregate is being used as a window function, this will
 * return NULL.  We could provide a similar function to return the relevant
 * WindowFunc node in such cases, but it's not needed yet.
 */
Aggref *
AggGetAggref(FunctionCallInfo fcinfo)
{
	if (fcinfo->context && IsA(fcinfo->context, AggState))
	{
		AggState   *aggstate = (AggState *) fcinfo->context;
		AggStatePerAgg curperagg;
		AggStatePerTrans curpertrans;

		/* check curperagg (valid when in a final function) */
		curperagg = aggstate->curperagg;

		if (curperagg)
			return curperagg->aggref;

		/* check curpertrans (valid when in a transition function) */
		curpertrans = aggstate->curpertrans;

		if (curpertrans)
			return curpertrans->aggref;
	}
	return NULL;
}

/*
 * AggGetTempMemoryContext - fetch short-term memory context for aggregates
 *
 * This is useful in agg final functions; the context returned is one that
 * the final function can safely reset as desired.  This isn't useful for
 * transition functions, since the context returned MAY (we don't promise)
 * be the same as the context those are called in.
 *
 * As above, this is currently not useful for aggs called as window functions.
 */
MemoryContext
AggGetTempMemoryContext(FunctionCallInfo fcinfo)
{
	if (fcinfo->context && IsA(fcinfo->context, AggState))
	{
		AggState   *aggstate = (AggState *) fcinfo->context;

		return aggstate->tmpcontext->ecxt_per_tuple_memory;
	}
	return NULL;
}

/*
 * AggStateIsShared - find out whether transition state is shared
 *
 * If the function is being called as an aggregate support function,
 * return true if the aggregate's transition state is shared across
 * multiple aggregates, false if it is not.
 *
 * Returns true if not called as an aggregate support function.
 * This is intended as a conservative answer, ie "no you'd better not
 * scribble on your input".  In particular, will return true if the
 * aggregate is being used as a window function, which is a scenario
 * in which changing the transition state is a bad idea.  We might
 * want to refine the behavior for the window case in future.
 */
bool
AggStateIsShared(FunctionCallInfo fcinfo)
{
	if (fcinfo->context && IsA(fcinfo->context, AggState))
	{
		AggState   *aggstate = (AggState *) fcinfo->context;
		AggStatePerAgg curperagg;
		AggStatePerTrans curpertrans;

		/* check curperagg (valid when in a final function) */
		curperagg = aggstate->curperagg;

		if (curperagg)
			return aggstate->pertrans[curperagg->transno].aggshared;

		/* check curpertrans (valid when in a transition function) */
		curpertrans = aggstate->curpertrans;

		if (curpertrans)
			return curpertrans->aggshared;
	}
	return true;
}

/*
 * AggRegisterCallback - register a cleanup callback for an aggregate
 *
 * This is useful for aggs to register shutdown callbacks, which will ensure
 * that non-memory resources are freed.  The callback will occur just before
 * the associated aggcontext (as returned by AggCheckCallContext) is reset,
 * either between groups or as a result of rescanning the query.  The callback
 * will NOT be called on error paths.  The typical use-case is for freeing of
 * tuplestores or tuplesorts maintained in aggcontext, or pins held by slots
 * created by the agg functions.  (The callback will not be called until after
 * the result of the finalfn is no longer needed, so it's safe for the finalfn
 * to return data that will be freed by the callback.)
 *
 * As above, this is currently not useful for aggs called as window functions.
 */
void
AggRegisterCallback(FunctionCallInfo fcinfo,
					ExprContextCallbackFunction func,
					Datum arg)
{
	if (fcinfo->context && IsA(fcinfo->context, AggState))
	{
		AggState   *aggstate = (AggState *) fcinfo->context;
		ExprContext *cxt = aggstate->curaggcontext;

		RegisterExprContextCallback(cxt, func, arg);

		return;
	}
	elog(ERROR, "aggregate function cannot register a callback in this context");
}


/*
 * aggregate_dummy - dummy execution routine for aggregate functions
 *
 * This function is listed as the implementation (prosrc field) of pg_proc
 * entries for aggregate functions.  Its only purpose is to throw an error
 * if someone mistakenly executes such a function in the normal way.
 *
 * Perhaps someday we could assign real meaning to the prosrc field of
 * an aggregate?
 */
Datum
aggregate_dummy(PG_FUNCTION_ARGS)
{
	elog(ERROR, "aggregate function %u called as normal function",
		 fcinfo->flinfo->fn_oid);
	return (Datum) 0;			/* keep compiler quiet */
}
