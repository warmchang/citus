/*
 * worker_shard_visibility.c
 *
 * Implements the functions for hiding shards on the Citus MX
 * worker (data) nodes.
 *
 * Copyright (c) Citus Data, Inc.
 */

#include "postgres.h"

#include "catalog/index.h"
#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "catalog/pg_type.h"
#include "distributed/metadata_cache.h"
#include "distributed/coordinator_protocol.h"
#include "distributed/listutils.h"
#include "distributed/local_executor.h"
#include "distributed/query_colocation_checker.h"
#include "distributed/worker_protocol.h"
#include "distributed/worker_shard_visibility.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "utils/varlena.h"


/* HideShardsMode is used to determine whether to hide shards */
typedef enum HideShardsMode
{
	CHECK_APPLICATION_NAME,
	HIDE_SHARDS_FROM_APPLICATION,
	DO_NOT_HIDE_SHARDS
} HideShardsMode;

/* Config variable managed via guc.c */
bool OverrideTableVisibility = true;
bool EnableManualChangesToShards = false;

/* hide shards when the application_name starts with one of: */
char *HideShardsFromAppNamePrefixes = "psql,pgAdmin,pg_dump";

/* if true, do not keep the hide shards decision cached at transaction end */
static bool ResetAtXactEnd = false;

/* cache of whether or not to hide shards */
static HideShardsMode HideShards = CHECK_APPLICATION_NAME;

static bool ShouldHideShards(void);
static bool ShouldHideShardsInternal(void);
static bool FilterShardsFromPgclass(Node *node, void *context);
static Node * CreateRelationIsAKnownShardFilter(int pgClassVarno);

PG_FUNCTION_INFO_V1(citus_table_is_visible);
PG_FUNCTION_INFO_V1(relation_is_a_known_shard);


/*
 * relation_is_a_known_shard a wrapper around RelationIsAKnownShard(), so
 * see the details there. The function also treats the indexes on shards
 * as if they were shards.
 */
Datum
relation_is_a_known_shard(PG_FUNCTION_ARGS)
{
	CheckCitusVersion(ERROR);

	Oid relationId = PG_GETARG_OID(0);
	PG_RETURN_BOOL(RelationIsAKnownShard(relationId));
}


/*
 * citus_table_is_visible aims to behave exactly the same with
 * pg_table_is_visible with only one exception. The former one
 * returns false for the relations that are known to be shards.
 */
Datum
citus_table_is_visible(PG_FUNCTION_ARGS)
{
	CheckCitusVersion(ERROR);

	Oid relationId = PG_GETARG_OID(0);
	char relKind = '\0';

	/*
	 * We don't want to deal with not valid/existing relations
	 * as pg_table_is_visible does.
	 */
	if (!SearchSysCacheExists1(RELOID, ObjectIdGetDatum(relationId)))
	{
		PG_RETURN_NULL();
	}

	if (!RelationIsVisible(relationId))
	{
		/* relation is not on the search path */
		PG_RETURN_BOOL(false);
	}

	if (RelationIsAKnownShard(relationId))
	{
		/*
		 * If the input relation is an index we simply replace the
		 * relationId with the corresponding relation to hide indexes
		 * as well. See RelationIsAKnownShard() for the details and give
		 * more meaningful debug message here.
		 */
		relKind = get_rel_relkind(relationId);
		if (relKind == RELKIND_INDEX || relKind == RELKIND_PARTITIONED_INDEX)
		{
			ereport(DEBUG2, (errmsg("skipping index \"%s\" since it belongs to a shard",
									get_rel_name(relationId))));
		}
		else
		{
			ereport(DEBUG2, (errmsg("skipping relation \"%s\" since it is a shard",
									get_rel_name(relationId))));
		}

		PG_RETURN_BOOL(false);
	}

	PG_RETURN_BOOL(RelationIsVisible(relationId));
}


/*
 * ErrorIfRelationIsAKnownShard errors out if the relation with relationId is
 * a shard relation.
 */
void
ErrorIfRelationIsAKnownShard(Oid relationId)
{
	if (!RelationIsAKnownShard(relationId))
	{
		return;
	}

	const char *relationName = get_rel_name(relationId);

	ereport(ERROR, (errmsg("relation \"%s\" is a shard relation ", relationName)));
}


/*
 * ErrorIfIllegallyChangingKnownShard errors out if the relation with relationId is
 * a known shard and manual changes on known shards are disabled. This is
 * valid for only non-citus (external) connections.
 */
void
ErrorIfIllegallyChangingKnownShard(Oid relationId)
{
	if (LocalExecutorLevel > 0 || IsCitusInitiatedRemoteBackend() ||
		EnableManualChangesToShards)
	{
		return;
	}

	if (RelationIsAKnownShard(relationId))
	{
		const char *relationName = get_rel_name(relationId);
		ereport(ERROR, (errmsg("cannot modify \"%s\" because it is a shard of "
							   "a distributed table",
							   relationName),
						errhint("Use the distributed table or set "
								"citus.enable_manual_changes_to_shards to on "
								"to modify shards directly")));
	}
}


/*
 * RelationIsAKnownShard gets a relationId, check whether it's a shard of
 * any distributed table.
 *
 * We can only do that in MX since both the metadata and tables are only
 * present there.
 */
bool
RelationIsAKnownShard(Oid shardRelationId)
{
	bool missingOk = true;
	char relKind = '\0';

	if (!OidIsValid(shardRelationId))
	{
		/* we cannot continue without a valid Oid */
		return false;
	}

	if (IsCoordinator())
	{
		bool coordinatorIsKnown = false;
		PrimaryNodeForGroup(0, &coordinatorIsKnown);

		if (!coordinatorIsKnown)
		{
			/*
			 * We're not interested in shards in the coordinator
			 * or non-mx worker nodes, unless the coordinator is
			 * in pg_dist_node.
			 */
			return false;
		}
	}

	Relation relation = try_relation_open(shardRelationId, AccessShareLock);
	if (relation == NULL)
	{
		return false;
	}
	relation_close(relation, NoLock);

	/*
	 * If the input relation is an index we simply replace the
	 * relationId with the corresponding relation to hide indexes
	 * as well.
	 */
	relKind = get_rel_relkind(shardRelationId);
	if (relKind == RELKIND_INDEX || relKind == RELKIND_PARTITIONED_INDEX)
	{
		shardRelationId = IndexGetRelation(shardRelationId, false);
	}

	/* get the shard's relation name */
	char *shardRelationName = get_rel_name(shardRelationId);

	uint64 shardId = ExtractShardIdFromTableName(shardRelationName, missingOk);
	if (shardId == INVALID_SHARD_ID)
	{
		/*
		 * The format of the table name does not align with
		 * our shard name definition.
		 */
		return false;
	}

	/* try to get the relation id */
	Oid relationId = LookupShardRelationFromCatalog(shardId, true);
	if (!OidIsValid(relationId))
	{
		/* there is no such relation */
		return false;
	}

	/* verify that their namespaces are the same */
	if (get_rel_namespace(shardRelationId) != get_rel_namespace(relationId))
	{
		return false;
	}

	/*
	 * Now get the relation name and append the shardId to it. We need
	 * to do that because otherwise a local table with a valid shardId
	 * appended to its name could be misleading.
	 */
	char *generatedRelationName = get_rel_name(relationId);
	AppendShardIdToName(&generatedRelationName, shardId);
	if (strncmp(shardRelationName, generatedRelationName, NAMEDATALEN) == 0)
	{
		/* we found the distributed table that the input shard belongs to */
		return true;
	}

	return false;
}


/*
 * HideShardsFromSomeApplications transforms queries to pg_class to
 * filter out known shards if the application_name matches any of
 * the prefixes in citus.hide_shards_from_app_name_prefixes.
 */
void
HideShardsFromSomeApplications(Query *query)
{
	if (!OverrideTableVisibility || HideShards == DO_NOT_HIDE_SHARDS ||
		!CitusHasBeenLoaded() || !CheckCitusVersion(DEBUG2))
	{
		return;
	}

	if (ShouldHideShards())
	{
		FilterShardsFromPgclass((Node *) query, NULL);
	}
}


/*
 * ShouldHideShards returns whether we should hide shards in the current
 * session. It only checks the application_name once and then uses a
 * cached response unless either the application_name or
 * citus.hide_shards_from_app_name_prefixes changes.
 */
static bool
ShouldHideShards(void)
{
	if (HideShards == CHECK_APPLICATION_NAME)
	{
		if (ShouldHideShardsInternal())
		{
			HideShards = HIDE_SHARDS_FROM_APPLICATION;
			return true;
		}
		else
		{
			HideShards = DO_NOT_HIDE_SHARDS;
			return false;
		}
	}
	else
	{
		return HideShards == HIDE_SHARDS_FROM_APPLICATION;
	}
}


/*
 * ResetHideShardsDecision resets the decision whether to hide shards.
 */
void
ResetHideShardsDecision(void)
{
	HideShards = CHECK_APPLICATION_NAME;
}


/*
 * ResetHideShardsDecisionAtXactEnd signals that we should reset the hide shards
 * decision after the transaction.
 */
void
ResetHideShardsDecisionAtXactEnd(void)
{
	ResetAtXactEnd = true;
}


/*
 * AfterXactResetHideShards is called at the end of the transaction to signal
 * that we should reset the transaction decision.
 */
void
AfterXactResetHideShards(void)
{
	if (ResetAtXactEnd)
	{
		ResetHideShardsDecision();
	}
}


/*
 * ShouldHideShardsInternal determines whether we should hide shards based on
 * the current application name.
 */
static bool
ShouldHideShardsInternal(void)
{
	if (IsCitusInitiatedRemoteBackend())
	{
		/* we never hide shards from Citus */
		return false;
	}

	List *prefixList = NIL;

	/* SplitGUCList scribbles on the input */
	char *splitCopy = pstrdup(HideShardsFromAppNamePrefixes);

	if (!SplitGUCList(splitCopy, ',', &prefixList))
	{
		/* invalid GUC value, ignore */
		return false;
	}

	char *appNamePrefix = NULL;
	foreach_ptr(appNamePrefix, prefixList)
	{
		/* always hide shards when one of the prefixes is * */
		if (strcmp(appNamePrefix, "*") == 0)
		{
			return true;
		}

		/* compare only the first first <prefixLength> characters */
		int prefixLength = strlen(appNamePrefix);
		if (strncmp(application_name, appNamePrefix, prefixLength) == 0)
		{
			return true;
		}
	}

	return false;
}


/*
 * FilterShardsFromPgclass adds a NOT relation_is_a_known_shard(oid) filter
 * to the security quals of pg_class RTEs.
 */
static bool
FilterShardsFromPgclass(Node *node, void *context)
{
	if (node == NULL)
	{
		return false;
	}

	if (IsA(node, Query))
	{
		Query *query = (Query *) node;
		MemoryContext queryContext = GetMemoryChunkContext(query);

		/*
		 * We process the whole rtable rather than visiting individual RangeTblEntry's
		 * in the walker, since we need to know the varno to generate the right
		 * fiter.
		 */
		int varno = 0;
		RangeTblEntry *rangeTableEntry = NULL;

		foreach_ptr(rangeTableEntry, query->rtable)
		{
			varno++;

			if (rangeTableEntry->rtekind != RTE_RELATION ||
				rangeTableEntry->relid != RelationRelationId)
			{
				/* not pg_class */
				continue;
			}

			/* make sure the expression is in the right memory context */
			MemoryContext originalContext = MemoryContextSwitchTo(queryContext);

			/* add NOT relation_is_a_known_shard(oid) to the security quals of the RTE */
			rangeTableEntry->securityQuals =
				list_make1(CreateRelationIsAKnownShardFilter(varno));

			MemoryContextSwitchTo(originalContext);
		}

		return query_tree_walker((Query *) node, FilterShardsFromPgclass, context, 0);
	}

	return expression_tree_walker(node, FilterShardsFromPgclass, context);
}


/*
 * CreateRelationIsAKnownShardFilter constructs an expression of the form:
 * NOT pg_catalog.relation_is_a_known_shard(oid)
 */
static Node *
CreateRelationIsAKnownShardFilter(int pgClassVarno)
{
	/* oid is always the first column */
	AttrNumber oidAttNum = 1;

	Var *oidVar = makeVar(pgClassVarno, oidAttNum, OIDOID, -1, InvalidOid, 0);

	/* build the call to read_intermediate_result */
	FuncExpr *funcExpr = makeNode(FuncExpr);
	funcExpr->funcid = RelationIsAKnownShardFuncId();
	funcExpr->funcretset = false;
	funcExpr->funcvariadic = false;
	funcExpr->funcformat = 0;
	funcExpr->funccollid = 0;
	funcExpr->inputcollid = 0;
	funcExpr->location = -1;
	funcExpr->args = list_make1(oidVar);

	BoolExpr *notExpr = makeNode(BoolExpr);
	notExpr->boolop = NOT_EXPR;
	notExpr->args = list_make1(funcExpr);
	notExpr->location = -1;

	return (Node *) notExpr;
}
