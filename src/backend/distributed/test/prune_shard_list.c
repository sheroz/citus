/*-------------------------------------------------------------------------
 *
 * test/src/create_shards.c
 *
 * This file contains functions to exercise shard creation functionality
 * within Citus.
 *
 * Copyright (c) 2014-2016, Citus Data, Inc.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "c.h"
#include "fmgr.h"

#include <string.h>

#if (PG_VERSION_NUM >= 90500 && PG_VERSION_NUM < 90600)
#include "access/stratnum.h"
#else
#include "access/skey.h"
#endif
#include "catalog/pg_type.h"
#include "distributed/master_metadata_utility.h"
#include "distributed/multi_join_order.h"
#include "distributed/multi_physical_planner.h"
#include "distributed/resource_lock.h"
#include "distributed/test_helper_functions.h" /* IWYU pragma: keep */
#include "nodes/pg_list.h"
#include "nodes/primnodes.h"
#include "nodes/nodes.h"
#include "optimizer/clauses.h"
#include "utils/array.h"
#include "utils/palloc.h"


/* local function forward declarations */
static Expr * MakeTextPartitionExpression(Oid distributedTableId, text *value);
static ArrayType * PrunedShardIdsForTable(Oid distributedTableId, List *whereClauseList);


/* declarations for dynamic loading */
PG_FUNCTION_INFO_V1(prune_using_no_values);
PG_FUNCTION_INFO_V1(prune_using_single_value);
PG_FUNCTION_INFO_V1(prune_using_either_value);
PG_FUNCTION_INFO_V1(prune_using_both_values);
PG_FUNCTION_INFO_V1(debug_equality_expression);


/*
 * prune_using_no_values returns the shards for the specified distributed table
 * after pruning using an empty clause list.
 */
Datum
prune_using_no_values(PG_FUNCTION_ARGS)
{
	Oid distributedTableId = PG_GETARG_OID(0);
	List *whereClauseList = NIL;
	ArrayType *shardIdArrayType = PrunedShardIdsForTable(distributedTableId,
														 whereClauseList);

	PG_RETURN_ARRAYTYPE_P(shardIdArrayType);
}


/*
 * prune_using_single_value returns the shards for the specified distributed
 * table after pruning using a single value provided by the caller.
 */
Datum
prune_using_single_value(PG_FUNCTION_ARGS)
{
	Oid distributedTableId = PG_GETARG_OID(0);
	text *value = (PG_ARGISNULL(1)) ? NULL : PG_GETARG_TEXT_P(1);
	Expr *equalityExpr = MakeTextPartitionExpression(distributedTableId, value);
	List *whereClauseList = list_make1(equalityExpr);
	ArrayType *shardIdArrayType = PrunedShardIdsForTable(distributedTableId,
														 whereClauseList);

	PG_RETURN_ARRAYTYPE_P(shardIdArrayType);
}


/*
 * prune_using_either_value returns the shards for the specified distributed
 * table after pruning using either of two values provided by the caller (OR).
 */
Datum
prune_using_either_value(PG_FUNCTION_ARGS)
{
	Oid distributedTableId = PG_GETARG_OID(0);
	text *firstValue = PG_GETARG_TEXT_P(1);
	text *secondValue = PG_GETARG_TEXT_P(2);
	Expr *firstQual = MakeTextPartitionExpression(distributedTableId, firstValue);
	Expr *secondQual = MakeTextPartitionExpression(distributedTableId, secondValue);
	Expr *orClause = make_orclause(list_make2(firstQual, secondQual));
	List *whereClauseList = list_make1(orClause);
	ArrayType *shardIdArrayType = PrunedShardIdsForTable(distributedTableId,
														 whereClauseList);

	PG_RETURN_ARRAYTYPE_P(shardIdArrayType);
}


/*
 * prune_using_both_values returns the shards for the specified distributed
 * table after pruning using both of the values provided by the caller (AND).
 */
Datum
prune_using_both_values(PG_FUNCTION_ARGS)
{
	Oid distributedTableId = PG_GETARG_OID(0);
	text *firstValue = PG_GETARG_TEXT_P(1);
	text *secondValue = PG_GETARG_TEXT_P(2);
	Expr *firstQual = MakeTextPartitionExpression(distributedTableId, firstValue);
	Expr *secondQual = MakeTextPartitionExpression(distributedTableId, secondValue);

	List *whereClauseList = list_make2(firstQual, secondQual);
	ArrayType *shardIdArrayType = PrunedShardIdsForTable(distributedTableId,
														 whereClauseList);

	PG_RETURN_ARRAYTYPE_P(shardIdArrayType);
}


/*
 * debug_equality_expression returns the textual representation of an equality
 * expression generated by a call to MakeOpExpression.
 */
Datum
debug_equality_expression(PG_FUNCTION_ARGS)
{
	Oid distributedTableId = PG_GETARG_OID(0);
	uint32 rangeTableId = 1;
	Var *partitionColumn = PartitionColumn(distributedTableId, rangeTableId);
	OpExpr *equalityExpression = MakeOpExpression(partitionColumn, BTEqualStrategyNumber);

	PG_RETURN_CSTRING(nodeToString(equalityExpression));
}


/*
 * MakeTextPartitionExpression returns an equality expression between the
 * specified table's partition column and the provided values.
 */
static Expr *
MakeTextPartitionExpression(Oid distributedTableId, text *value)
{
	uint32 rangeTableId = 1;
	Var *partitionColumn = PartitionColumn(distributedTableId, rangeTableId);
	Expr *partitionExpression = NULL;

	if (value != NULL)
	{
		OpExpr *equalityExpr = MakeOpExpression(partitionColumn, BTEqualStrategyNumber);
		Const *rightConst = (Const *) get_rightop((Expr *) equalityExpr);

		rightConst->constvalue = (Datum) value;
		rightConst->constisnull = false;
		rightConst->constbyval = false;

		partitionExpression = (Expr *) equalityExpr;
	}
	else
	{
		NullTest *nullTest = makeNode(NullTest);
		nullTest->arg = (Expr *) partitionColumn;
		nullTest->nulltesttype = IS_NULL;

		partitionExpression = (Expr *) nullTest;
	}

	return partitionExpression;
}


/*
 * PrunedShardIdsForTable loads the shard intervals for the specified table,
 * prunes them using the provided clauses. It returns an ArrayType containing
 * the shard identifiers, suitable for return from an SQL-facing function.
 */
static ArrayType *
PrunedShardIdsForTable(Oid distributedTableId, List *whereClauseList)
{
	ArrayType *shardIdArrayType = NULL;
	ListCell *shardCell = NULL;
	int shardIdIndex = 0;
	Oid shardIdTypeId = INT8OID;
	Index tableId = 1;

	List *shardList = LoadShardIntervalList(distributedTableId);
	int shardIdCount = -1;
	Datum *shardIdDatumArray = NULL;

	shardList = PruneShardList(distributedTableId, tableId, whereClauseList, shardList);

	shardIdCount = list_length(shardList);
	shardIdDatumArray = palloc0(shardIdCount * sizeof(Datum));

	foreach(shardCell, shardList)
	{
		ShardInterval *shardId = (ShardInterval *) lfirst(shardCell);
		Datum shardIdDatum = Int64GetDatum(shardId->shardId);

		shardIdDatumArray[shardIdIndex] = shardIdDatum;
		shardIdIndex++;
	}

	shardIdArrayType = DatumArrayToArrayType(shardIdDatumArray, shardIdCount,
											 shardIdTypeId);

	return shardIdArrayType;
}
