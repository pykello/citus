/*-------------------------------------------------------------------------
 *
 * function.c
 *    Commands for FUNCTION statements.
 *
 *    We currently support replicating function definitions on the
 *    coordinator in all the worker nodes in the form of
 *
 *    CREATE OR REPLACE FUNCTION ... queries.
 *
 *    ALTER or DROP operations are not yet propagated.
 *
 * Copyright (c) 2019, Citus Data, Inc.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "miscadmin.h"
#include "funcapi.h"

#if PG_VERSION_NUM >= 120000
#include "access/genam.h"
#endif
#include "access/htup_details.h"
#include "access/xact.h"
#include "catalog/namespace.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/extension.h"
#include "distributed/colocation_utils.h"
#include "distributed/commands.h"
#include "distributed/commands/utility_hook.h"
#include "distributed/deparser.h"
#include "distributed/maintenanced.h"
#include "distributed/master_metadata_utility.h"
#include "distributed/master_protocol.h"
#include "distributed/metadata/distobject.h"
#include "distributed/metadata/pg_dist_object.h"
#include "distributed/metadata_sync.h"
#include "distributed/multi_executor.h"
#include "distributed/relation_access_tracking.h"
#include "distributed/worker_transaction.h"
#include "parser/parse_coerce.h"
#include "parser/parse_type.h"
#include "storage/lmgr.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/fmgrprotos.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

#define argumentStartsWith(arg, prefix) \
	(strncmp(arg, prefix, strlen(prefix)) == 0)

/* forward declaration for helper functions*/
static char * GetFunctionDDLCommand(const RegProcedure funcOid);
static char * GetFunctionAlterOwnerCommand(const RegProcedure funcOid);
static int GetDistributionArgIndex(Oid functionOid, char *distributionArgumentName,
								   Oid *distributionArgumentOid);
static int GetFunctionColocationId(Oid functionOid, char *colocateWithName, Oid
								   distributionArgumentOid);
static void EnsureFunctionCanBeColocatedWithTable(Oid functionOid, Oid
												  distributionColumnType, Oid
												  sourceRelationId);
static void UpdateFunctionDistributionInfo(const ObjectAddress *distAddress,
										   int *distribution_argument_index,
										   int *colocationId);
static void EnsureSequentialModeForFunctionDDL(void);
static void TriggerSyncMetadataToPrimaryNodes(void);
static bool ShouldPropagateCreateFunction(CreateFunctionStmt *stmt);
static bool ShouldPropagateAlterFunction(const ObjectAddress *address);
static ObjectAddress * FunctionToObjectAddress(ObjectType objectType,
											   ObjectWithArgs *objectWithArgs,
											   bool missing_ok);
static void ErrorIfUnsupportedAlterFunctionStmt(AlterFunctionStmt *stmt);
static void ErrorIfFunctionDependsOnExtension(const ObjectAddress *functionAddress);


PG_FUNCTION_INFO_V1(create_distributed_function);

#if PG_VERSION_NUM >= 110000
#define AssertIsFunctionOrProcedure(objtype) \
	Assert((objtype) == OBJECT_FUNCTION || (objtype) == OBJECT_PROCEDURE)
#else
#define AssertIsFunctionOrProcedure(objtype) \
	Assert(objtype == OBJECT_FUNCTION)
#endif


/*
 * create_distributed_function gets a function or procedure name with their list of
 * argument types in parantheses, then it creates a new distributed function.
 */
Datum
create_distributed_function(PG_FUNCTION_ARGS)
{
	RegProcedure funcOid = PG_GETARG_OID(0);

	text *distributionArgumentNameText = NULL; /* optional */
	text *colocateWithText = NULL; /* optional */

	const char *ddlCommand = NULL;
	ObjectAddress functionAddress = { 0 };

	int distributionArgumentIndex = -1;
	Oid distributionArgumentOid = InvalidOid;
	int colocationId = -1;

	char *distributionArgumentName = NULL;
	char *colocateWithTableName = NULL;

	/* if called on NULL input, error out */
	if (funcOid == InvalidOid)
	{
		ereport(ERROR, (errmsg("the first parameter for create_distributed_function() "
							   "should be a single a valid function or procedure name "
							   "followed by a list of parameters in parantheses"),
						errhint("skip the parameters with OUT argtype as they are not "
								"part of the signature in PostgreSQL")));
	}

	if (PG_ARGISNULL(1))
	{
		/*
		 * Using the  default value, so distribute the function but do not set
		 * the  distribution argument.
		 */
		distributionArgumentName = NULL;
	}
	else
	{
		distributionArgumentNameText = PG_GETARG_TEXT_P(1);
		distributionArgumentName = text_to_cstring(distributionArgumentNameText);
	}

	if (PG_ARGISNULL(2))
	{
		ereport(ERROR, (errmsg("colocate_with parameter should not be NULL"),
						errhint("To use the default value, set colocate_with option "
								"to \"default\"")));
	}
	else
	{
		colocateWithText = PG_GETARG_TEXT_P(2);
		colocateWithTableName = text_to_cstring(colocateWithText);
	}

	EnsureFunctionOwner(funcOid);

	ObjectAddressSet(functionAddress, ProcedureRelationId, funcOid);
	ErrorIfFunctionDependsOnExtension(&functionAddress);

	/*
	 * when we allow propagation within a transaction block we should make sure to only
	 * allow this in sequential mode
	 */
	EnsureSequentialModeForFunctionDDL();

	EnsureDependenciesExistsOnAllNodes(&functionAddress);

	ddlCommand = GetFunctionDDLCommand(funcOid);

	SendCommandToWorkersAsUser(ALL_WORKERS, CurrentUserName(), ddlCommand);

	MarkObjectDistributed(&functionAddress);

	if (distributionArgumentName == NULL)
	{
		/* cannot provide colocate_with without distribution_arg_name */
		if (pg_strncasecmp(colocateWithTableName, "default", NAMEDATALEN) != 0)
		{
			char *functionName = get_func_name(funcOid);


			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							errmsg("cannot distribute the function \"%s\" since the "
								   "distribution argument is not valid ", functionName),
							errhint("To provide \"colocate_with\" option, the"
									" distribution argument parameter should also "
									"be provided")));
		}

		/* set distribution argument and colocationId to NULL */
		UpdateFunctionDistributionInfo(&functionAddress, NULL, NULL);
	}
	else if (distributionArgumentName != NULL)
	{
		/* get the argument index, or error out if we cannot find a valid index */
		distributionArgumentIndex =
			GetDistributionArgIndex(funcOid, distributionArgumentName,
									&distributionArgumentOid);

		/* get the colocation id, or error out if we cannot find an appropriate one */
		colocationId =
			GetFunctionColocationId(funcOid, colocateWithTableName,
									distributionArgumentOid);

		/* if provided, make sure to record the distribution argument and colocationId */
		UpdateFunctionDistributionInfo(&functionAddress, &distributionArgumentIndex,
									   &colocationId);

		/*
		 * Once we have at least one distributed function/procedure with distribution
		 * argument, we sync the metadata to nodes so that the function/procedure
		 * delegation can be handled locally on the nodes.
		 */
		TriggerSyncMetadataToPrimaryNodes();
	}

	PG_RETURN_VOID();
}


/*
 * CreateFunctionDDLCommandsIdempotent returns a list of DDL statements (const char *) to be
 * executed on a node to recreate the function addressed by the functionAddress.
 */
List *
CreateFunctionDDLCommandsIdempotent(const ObjectAddress *functionAddress)
{
	char *ddlCommand = NULL;

	Assert(functionAddress->classId == ProcedureRelationId);

	ddlCommand = GetFunctionDDLCommand(functionAddress->objectId);
	return list_make1(ddlCommand);
}


/*
 * GetDistributionArgIndex calculates the distribution argument with the given
 * parameters. The function errors out if no valid argument is found.
 */
static int
GetDistributionArgIndex(Oid functionOid, char *distributionArgumentName,
						Oid *distributionArgumentOid)
{
	int distributionArgumentIndex = -1;

	int numberOfArgs = 0;
	int argIndex = 0;
	Oid *argTypes = NULL;
	char **argNames = NULL;
	char *argModes = NULL;

	HeapTuple proctup = NULL;

	*distributionArgumentOid = InvalidOid;

	proctup = SearchSysCache1(PROCOID, ObjectIdGetDatum(functionOid));
	if (!HeapTupleIsValid(proctup))
	{
		elog(ERROR, "cache lookup failed for function %u", functionOid);
	}

	numberOfArgs = get_func_arg_info(proctup, &argTypes, &argNames, &argModes);

	if (argumentStartsWith(distributionArgumentName, "$"))
	{
		/* skip the first character, we're safe because text_to_cstring pallocs */
		distributionArgumentName++;

		/* throws error if the input is not an integer */
		distributionArgumentIndex = pg_atoi(distributionArgumentName, 4, 0);

		if (distributionArgumentIndex < 1 || distributionArgumentIndex > numberOfArgs)
		{
			char *functionName = get_func_name(functionOid);

			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							errmsg("cannot distribute the function \"%s\" since "
								   "the distribution argument is not valid",
								   functionName),
							errhint("Either provide a valid function argument name "
									"or a valid \"$paramIndex\" to "
									"create_distributed_function()")));
		}

		/*
		 * Internal representation for the distributionArgumentIndex
		 * starts from 0 whereas user facing API starts from 1.
		 */
		distributionArgumentIndex -= 1;
		*distributionArgumentOid = argTypes[distributionArgumentIndex];

		ReleaseSysCache(proctup);

		Assert(*distributionArgumentOid != InvalidOid);

		return distributionArgumentIndex;
	}

	/*
	 * The user didn't provid "$paramIndex" but potentially the name of the paramater.
	 * So, loop over the arguments and try to find the argument name that matches
	 * the parameter that user provided.
	 */
	for (argIndex = 0; argIndex < numberOfArgs; ++argIndex)
	{
		char *argNameOnIndex = argNames != NULL ? argNames[argIndex] : NULL;

		if (argNameOnIndex != NULL &&
			pg_strncasecmp(argNameOnIndex, distributionArgumentName, NAMEDATALEN) == 0)
		{
			distributionArgumentIndex = argIndex;

			*distributionArgumentOid = argTypes[argIndex];

			/* we found, no need to continue */
			break;
		}
	}

	/* we still couldn't find the argument, so error out */
	if (distributionArgumentIndex == -1)
	{
		char *functionName = get_func_name(functionOid);

		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("cannot distribute the function \"%s\" since the "
							   "distribution argument is not valid ", functionName),
						errhint("Either provide a valid function argument name "
								"or a valid \"$paramIndex\" to "
								"create_distributed_function()")));
	}

	ReleaseSysCache(proctup);

	Assert(*distributionArgumentOid != InvalidOid);

	return distributionArgumentIndex;
}


/*
 * GetFunctionColocationId gets the parameters for deciding the colocationId
 * of the function that is being distributed. The function errors out if it is
 * not possible to assign a colocationId to the input function.
 */
static int
GetFunctionColocationId(Oid functionOid, char *colocateWithTableName,
						Oid distributionArgumentOid)
{
	int colocationId = INVALID_COLOCATION_ID;
	Relation pgDistColocation = heap_open(DistColocationRelationId(), ShareLock);

	if (pg_strncasecmp(colocateWithTableName, "default", NAMEDATALEN) == 0)
	{
		/* check for default colocation group */
		colocationId = ColocationId(ShardCount, ShardReplicationFactor,
									distributionArgumentOid);

		if (colocationId == INVALID_COLOCATION_ID)
		{
			char *functionName = get_func_name(functionOid);

			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							errmsg("cannot distribute the function \"%s\" since there "
								   "is no table to colocate with", functionName),
							errhint("Provide a distributed table via \"colocate_with\" "
									"option to create_distributed_function()")));
		}
	}
	else
	{
		Oid sourceRelationId =
			ResolveRelationId(cstring_to_text(colocateWithTableName), false);

		EnsureFunctionCanBeColocatedWithTable(functionOid, distributionArgumentOid,
											  sourceRelationId);

		colocationId = TableColocationId(sourceRelationId);
	}

	/* keep the lock */
	heap_close(pgDistColocation, NoLock);

	return colocationId;
}


/*
 * EnsureFunctionCanBeColocatedWithTable checks whether the given arguments are
 * suitable to distribute the function to be colocated with given source table.
 */
static void
EnsureFunctionCanBeColocatedWithTable(Oid functionOid, Oid distributionColumnType,
									  Oid sourceRelationId)
{
	DistTableCacheEntry *sourceTableEntry = DistributedTableCacheEntry(sourceRelationId);
	char sourceDistributionMethod = sourceTableEntry->partitionMethod;
	char sourceReplicationModel = sourceTableEntry->replicationModel;
	Var *sourceDistributionColumn = DistPartitionKey(sourceRelationId);
	Oid sourceDistributionColumnType = InvalidOid;

	if (sourceDistributionMethod != DISTRIBUTE_BY_HASH)
	{
		char *functionName = get_func_name(functionOid);
		char *sourceRelationName = get_rel_name(sourceRelationId);

		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("cannot colocate function \"%s\" and table \"%s\" because "
							   "colocate_with option is only supported for hash "
							   "distributed tables.", functionName,
							   sourceRelationName)));
	}

	if (sourceReplicationModel != REPLICATION_MODEL_STREAMING)
	{
		char *functionName = get_func_name(functionOid);
		char *sourceRelationName = get_rel_name(sourceRelationId);

		ereport(ERROR, (errmsg("cannot colocate function \"%s\" and table \"%s\"",
							   functionName, sourceRelationName),
						errdetail("Citus currently only supports colocating function "
								  "with distributed tables that are created using "
								  "streaming replication model."),
						errhint("When distributing tables make sure that "
								"\"citus.replication_model\" is set to \"streaming\"")));
	}

	/*
	 * If the types are the same, we're good. If not, we still check if there
	 * is any coercion path between the types.
	 */
	sourceDistributionColumnType = sourceDistributionColumn->vartype;
	if (sourceDistributionColumnType != distributionColumnType)
	{
		Oid coercionFuncId = InvalidOid;
		CoercionPathType coercionType = COERCION_PATH_NONE;

		coercionType =
			find_coercion_pathway(distributionColumnType, sourceDistributionColumnType,
								  COERCION_EXPLICIT, &coercionFuncId);

		/* if there is no path for coercion, error out*/
		if (coercionType == COERCION_PATH_NONE)
		{
			char *functionName = get_func_name(functionOid);
			char *sourceRelationName = get_rel_name(sourceRelationId);

			ereport(ERROR, (errmsg("cannot colocate function \"%s\" and table \"%s\" "
								   "because distribution column types don't match and "
								   "there is no coercion path", sourceRelationName,
								   functionName)));
		}
	}
}


/*
 * UpdateFunctionDistributionInfo gets object address of a function and
 * updates its distribution_argument_index and colocationId in pg_dist_object.
 */
static void
UpdateFunctionDistributionInfo(const ObjectAddress *distAddress,
							   int *distribution_argument_index,
							   int *colocationId)
{
	const bool indexOK = true;

	Relation pgDistObjectRel = NULL;
	TupleDesc tupleDescriptor = NULL;
	ScanKeyData scanKey[3];
	SysScanDesc scanDescriptor = NULL;
	HeapTuple heapTuple = NULL;
	Datum values[Natts_pg_dist_object];
	bool isnull[Natts_pg_dist_object];
	bool replace[Natts_pg_dist_object];

	pgDistObjectRel = heap_open(DistObjectRelationId(), RowExclusiveLock);
	tupleDescriptor = RelationGetDescr(pgDistObjectRel);

	/* scan pg_dist_object for classid = $1 AND objid = $2 AND objsubid = $3 via index */
	ScanKeyInit(&scanKey[0], Anum_pg_dist_object_classid, BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(distAddress->classId));
	ScanKeyInit(&scanKey[1], Anum_pg_dist_object_objid, BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(distAddress->objectId));
	ScanKeyInit(&scanKey[2], Anum_pg_dist_object_objsubid, BTEqualStrategyNumber,
				F_INT4EQ, ObjectIdGetDatum(distAddress->objectSubId));

	scanDescriptor = systable_beginscan(pgDistObjectRel, DistObjectPrimaryKeyIndexId(),
										indexOK,
										NULL, 3, scanKey);

	heapTuple = systable_getnext(scanDescriptor);
	if (!HeapTupleIsValid(heapTuple))
	{
		ereport(ERROR, (errmsg("could not find valid entry for node \"%d,%d,%d\" "
							   "in pg_dist_object", distAddress->classId,
							   distAddress->objectId, distAddress->objectSubId)));
	}

	memset(replace, 0, sizeof(replace));

	replace[Anum_pg_dist_object_distribution_argument_index - 1] = true;

	if (distribution_argument_index != NULL)
	{
		values[Anum_pg_dist_object_distribution_argument_index - 1] = Int32GetDatum(
			*distribution_argument_index);
		isnull[Anum_pg_dist_object_distribution_argument_index - 1] = false;
	}
	else
	{
		isnull[Anum_pg_dist_object_distribution_argument_index - 1] = true;
	}

	replace[Anum_pg_dist_object_colocationid - 1] = true;
	if (colocationId != NULL)
	{
		values[Anum_pg_dist_object_colocationid - 1] = Int32GetDatum(*colocationId);
		isnull[Anum_pg_dist_object_colocationid - 1] = false;
	}
	else
	{
		isnull[Anum_pg_dist_object_colocationid - 1] = true;
	}

	heapTuple = heap_modify_tuple(heapTuple, tupleDescriptor, values, isnull, replace);

	CatalogTupleUpdate(pgDistObjectRel, &heapTuple->t_self, heapTuple);

	CitusInvalidateRelcacheByRelid(DistObjectRelationId());

	CommandCounterIncrement();

	systable_endscan(scanDescriptor);

	heap_close(pgDistObjectRel, NoLock);
}


/*
 * GetFunctionDDLCommand returns the complete "CREATE OR REPLACE FUNCTION ..." statement for
 * the specified function followed by "ALTER FUNCTION .. SET OWNER ..".
 */
static char *
GetFunctionDDLCommand(const RegProcedure funcOid)
{
	StringInfo ddlCommand = makeStringInfo();

	OverrideSearchPath *overridePath = NULL;
	Datum sqlTextDatum = 0;
	char *createFunctionSQL = NULL;
	char *alterFunctionOwnerSQL = NULL;

	/*
	 * Set search_path to NIL so that all objects outside of pg_catalog will be
	 * schema-prefixed. pg_catalog will be added automatically when we call
	 * PushOverrideSearchPath(), since we set addCatalog to true;
	 */
	overridePath = GetOverrideSearchPath(CurrentMemoryContext);
	overridePath->schemas = NIL;
	overridePath->addCatalog = true;
	PushOverrideSearchPath(overridePath);

	sqlTextDatum = DirectFunctionCall1(pg_get_functiondef,
									   ObjectIdGetDatum(funcOid));

	/* revert back to original search_path */
	PopOverrideSearchPath();

	createFunctionSQL = TextDatumGetCString(sqlTextDatum);
	alterFunctionOwnerSQL = GetFunctionAlterOwnerCommand(funcOid);

	appendStringInfo(ddlCommand, "%s;%s", createFunctionSQL, alterFunctionOwnerSQL);

	return ddlCommand->data;
}


/*
 * GetFunctionAlterOwnerCommand returns "ALTER FUNCTION .. SET OWNER .." statement for
 * the specified function.
 */
static char *
GetFunctionAlterOwnerCommand(const RegProcedure funcOid)
{
	HeapTuple proctup = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcOid));
	StringInfo alterCommand = makeStringInfo();
	bool isProcedure = false;
	Oid procOwner = InvalidOid;

	char *functionSignature = NULL;
	char *functionOwner = NULL;

	OverrideSearchPath *overridePath = NULL;
	Datum functionSignatureDatum = 0;

	if (HeapTupleIsValid(proctup))
	{
		Form_pg_proc procform;

		procform = (Form_pg_proc) GETSTRUCT(proctup);

		procOwner = procform->proowner;

#if (PG_VERSION_NUM >= 110000)
		isProcedure = procform->prokind == PROKIND_PROCEDURE;
#endif

		ReleaseSysCache(proctup);
	}
	else if (!OidIsValid(funcOid) || !HeapTupleIsValid(proctup))
	{
		ereport(ERROR, (errmsg("cannot find function with oid: %d", funcOid)));
	}

	/*
	 * Set search_path to NIL so that all objects outside of pg_catalog will be
	 * schema-prefixed. pg_catalog will be added automatically when we call
	 * PushOverrideSearchPath(), since we set addCatalog to true;
	 */
	overridePath = GetOverrideSearchPath(CurrentMemoryContext);
	overridePath->schemas = NIL;
	overridePath->addCatalog = true;

	PushOverrideSearchPath(overridePath);

	/*
	 * If the function exists we want to use pg_get_function_identity_arguments to
	 * serialize its canonical arguments
	 */
	functionSignatureDatum =
		DirectFunctionCall1(regprocedureout, ObjectIdGetDatum(funcOid));

	/* revert back to original search_path */
	PopOverrideSearchPath();

	/* regprocedureout returns cstring */
	functionSignature = DatumGetCString(functionSignatureDatum);

	functionOwner = GetUserNameFromId(procOwner, false);

	appendStringInfo(alterCommand, "ALTER %s %s OWNER TO %s;",
					 (isProcedure ? "PROCEDURE" : "FUNCTION"),
					 functionSignature,
					 quote_identifier(functionOwner));

	return alterCommand->data;
}


/*
 * EnsureSequentialModeForFunctionDDL makes sure that the current transaction is already in
 * sequential mode, or can still safely be put in sequential mode, it errors if that is
 * not possible. The error contains information for the user to retry the transaction with
 * sequential mode set from the beginnig.
 *
 * As functions are node scoped objects there exists only 1 instance of the function used by
 * potentially multiple shards. To make sure all shards in the transaction can interact
 * with the function the function needs to be visible on all connections used by the transaction,
 * meaning we can only use 1 connection per node.
 */
static void
EnsureSequentialModeForFunctionDDL(void)
{
	if (ParallelQueryExecutedInTransaction())
	{
		ereport(ERROR, (errmsg("cannot create function because there was a "
							   "parallel operation on a distributed table in the "
							   "transaction"),
						errdetail("When creating a distributed function, Citus needs to "
								  "perform all operations over a single connection per "
								  "node to ensure consistency."),
						errhint("Try re-running the transaction with "
								"\"SET LOCAL citus.multi_shard_modify_mode TO "
								"\'sequential\';\"")));
	}

	ereport(DEBUG1, (errmsg("switching to sequential query execution mode"),
					 errdetail(
						 "A distributed function is created. To make sure subsequent "
						 "commands see the type correctly we need to make sure to "
						 "use only one connection for all future commands")));
	SetLocalMultiShardModifyModeToSequential();
}


/*
 * TriggerSyncMetadataToPrimaryNodes iterates over the active primary nodes,
 * and triggers the metadata syncs if the node has not the metadata. Later,
 * maintenance daemon will sync the metadata to nodes.
 */
static void
TriggerSyncMetadataToPrimaryNodes(void)
{
	List *workerList = ActivePrimaryNodeList(ShareLock);
	ListCell *workerCell = NULL;
	bool triggerMetadataSync = false;

	foreach(workerCell, workerList)
	{
		WorkerNode *workerNode = (WorkerNode *) lfirst(workerCell);

		/* if already has metadata, no need to do it again */
		if (!workerNode->hasMetadata)
		{
			/*
			 * Let the maintanince deamon do the hard work of syncing the metadata. We prefer
			 * this because otherwise node activation might fail withing transaction blocks.
			 */
			LockRelationOid(DistNodeRelationId(), ExclusiveLock);
			MarkNodeHasMetadata(workerNode->workerName, workerNode->workerPort, true);

			triggerMetadataSync = true;
		}
	}

	/* let the maintanince deamon know about the metadata sync */
	if (triggerMetadataSync)
	{
		TriggerMetadataSync(MyDatabaseId);
	}
}


/*
 * ShouldPropagateCreateFunction tests if we need to propagate a CREATE FUNCTION
 * statement. We only propagate replace's of distributed functions to keep the function on
 * the workers in sync with the one on the coordinator.
 */
static bool
ShouldPropagateCreateFunction(CreateFunctionStmt *stmt)
{
	const ObjectAddress *address = NULL;

	if (creating_extension)
	{
		/*
		 * extensions should be created separately on the workers, functions cascading
		 * from an extension should therefore not be propagated.
		 */
		return false;
	}

	if (!EnableDependencyCreation)
	{
		/*
		 * we are configured to disable object propagation, should not propagate anything
		 */
		return false;
	}

	if (!stmt->replace)
	{
		/*
		 * Since we only care for a replace of distributed functions if the statement is
		 * not a replace we are going to ignore.
		 */
		return false;
	}

	/*
	 * Even though its a replace we should accept an non-existing function, it will just
	 * not be distributed
	 */
	address = GetObjectAddressFromParseTree((Node *) stmt, true);
	if (!IsObjectDistributed(address))
	{
		/* do not propagate alter function for non-distributed functions */
		return false;
	}

	return true;
}


/*
 * ShouldPropagateAlterFunction returns, based on the address of a function, if alter
 * statements targeting the function should be propagated.
 */
static bool
ShouldPropagateAlterFunction(const ObjectAddress *address)
{
	if (creating_extension)
	{
		/*
		 * extensions should be created separately on the workers, functions cascading
		 * from an extension should therefore not be propagated.
		 */
		return false;
	}

	if (!EnableDependencyCreation)
	{
		/*
		 * we are configured to disable object propagation, should not propagate anything
		 */
		return false;
	}

	if (!IsObjectDistributed(address))
	{
		/* do not propagate alter function for non-distributed functions */
		return false;
	}

	return true;
}


/*
 * PlanCreateFunctionStmt is called during the planning phase for CREATE [OR REPLACE]
 * FUNCTION. We primarily care for the replace variant of this statement to keep
 * distributed functions in sync. We bail via a check on ShouldPropagateCreateFunction
 * which checks for the OR REPLACE modifier.
 *
 * Since we use pg_get_functiondef to get the ddl command we actually do not do any
 * planning here, instead we defer the plan creation to the processing step.
 *
 * Instead we do our basic housekeeping where we make sure we are on the coordinator and
 * can propagate the function in sequential mode.
 */
List *
PlanCreateFunctionStmt(CreateFunctionStmt *stmt, const char *queryString)
{
	if (!ShouldPropagateCreateFunction(stmt))
	{
		return NIL;
	}

	EnsureCoordinator();

	EnsureSequentialModeForFunctionDDL();

	/*
	 * ddl jobs will be generated during the Processing phase as we need the function to
	 * be updated in the catalog to get its sql representation
	 */
	return NIL;
}


/*
 * ProcessCreateFunctionStmt actually creates the plan we need to execute for function
 * propagation. This is the downside of using pg_get_functiondef to get the sql statement.
 *
 * Besides creating the plan we also make sure all (new) dependencies of the function are
 * created on all nodes.
 */
List *
ProcessCreateFunctionStmt(CreateFunctionStmt *stmt, const char *queryString)
{
	const ObjectAddress *address = NULL;
	const char *sql = NULL;
	List *commands = NIL;

	if (!ShouldPropagateCreateFunction(stmt))
	{
		return NIL;
	}

	address = GetObjectAddressFromParseTree((Node *) stmt, false);
	EnsureDependenciesExistsOnAllNodes(address);

	sql = GetFunctionDDLCommand(address->objectId);

	commands = list_make3(DISABLE_DDL_PROPAGATION,
						  (void *) sql,
						  ENABLE_DDL_PROPAGATION);

	return NodeDDLTaskList(ALL_WORKERS, commands);
}


/*
 * CreateFunctionStmtObjectAddress returns the ObjectAddress for the subject of the
 * CREATE [OR REPLACE] FUNCTION statement. If missing_ok is false it will error with the
 * normal postgres error for unfound functions.
 */
const ObjectAddress *
CreateFunctionStmtObjectAddress(CreateFunctionStmt *stmt, bool missing_ok)
{
	ObjectType objectType = OBJECT_FUNCTION;
	ObjectWithArgs *objectWithArgs = NULL;
	ListCell *parameterCell = NULL;

#if PG_VERSION_NUM >= 110000
	if (stmt->is_procedure)
	{
		objectType = OBJECT_PROCEDURE;
	}
#endif

	objectWithArgs = makeNode(ObjectWithArgs);
	objectWithArgs->objname = stmt->funcname;

	foreach(parameterCell, stmt->parameters)
	{
		FunctionParameter *funcParam = castNode(FunctionParameter, lfirst(parameterCell));
		objectWithArgs->objargs = lappend(objectWithArgs->objargs, funcParam->argType);
	}

	return FunctionToObjectAddress(objectType, objectWithArgs, missing_ok);
}


/*
 * PlanAlterFunctionStmt is invoked for alter function statements with actions. Here we
 * plan the jobs to be executed on the workers for functions that have been distributed in
 * the cluster.
 */
List *
PlanAlterFunctionStmt(AlterFunctionStmt *stmt, const char *queryString)
{
	const char *sql = NULL;
	const ObjectAddress *address = NULL;
	List *commands = NIL;

	/* AlterFunctionStmt->objtype has only been added since pg11 */
#if PG_VERSION_NUM >= 110000
	AssertIsFunctionOrProcedure(stmt->objtype);
#endif

	address = GetObjectAddressFromParseTree((Node *) stmt, false);
	if (!ShouldPropagateAlterFunction(address))
	{
		return NIL;
	}

	EnsureCoordinator();
	ErrorIfUnsupportedAlterFunctionStmt(stmt);
	EnsureSequentialModeForFunctionDDL();
	QualifyTreeNode((Node *) stmt);
	sql = DeparseTreeNode((Node *) stmt);

	commands = list_make3(DISABLE_DDL_PROPAGATION,
						  (void *) sql,
						  ENABLE_DDL_PROPAGATION);

	return NodeDDLTaskList(ALL_WORKERS, commands);
}


/*
 * PlanRenameFunctionStmt is called when the user is renaming a function. The invocation
 * happens before the statement is applied locally.
 *
 * As the function already exists we have access to the ObjectAddress, this is used to
 * check if it is distributed. If so the rename is executed on all the workers to keep the
 * types in sync across the cluster.
 */
List *
PlanRenameFunctionStmt(RenameStmt *stmt, const char *queryString)
{
	const char *sql = NULL;
	const ObjectAddress *address = NULL;
	List *commands = NIL;

	AssertIsFunctionOrProcedure(stmt->renameType);

	address = GetObjectAddressFromParseTree((Node *) stmt, false);
	if (!ShouldPropagateAlterFunction(address))
	{
		return NIL;
	}

	EnsureCoordinator();
	EnsureSequentialModeForFunctionDDL();
	QualifyTreeNode((Node *) stmt);
	sql = DeparseTreeNode((Node *) stmt);

	commands = list_make3(DISABLE_DDL_PROPAGATION,
						  (void *) sql,
						  ENABLE_DDL_PROPAGATION);

	return NodeDDLTaskList(ALL_WORKERS, commands);
}


/*
 * PlanAlterFunctionSchemaStmt is executed before the statement is applied to the local
 * postgres instance.
 *
 * In this stage we can prepare the commands that need to be run on all workers.
 */
List *
PlanAlterFunctionSchemaStmt(AlterObjectSchemaStmt *stmt, const char *queryString)
{
	const char *sql = NULL;
	const ObjectAddress *address = NULL;
	List *commands = NIL;

	AssertIsFunctionOrProcedure(stmt->objectType);

	address = GetObjectAddressFromParseTree((Node *) stmt, false);
	if (!ShouldPropagateAlterFunction(address))
	{
		return NIL;
	}

	EnsureCoordinator();
	EnsureSequentialModeForFunctionDDL();
	QualifyTreeNode((Node *) stmt);
	sql = DeparseTreeNode((Node *) stmt);

	commands = list_make3(DISABLE_DDL_PROPAGATION,
						  (void *) sql,
						  ENABLE_DDL_PROPAGATION);

	return NodeDDLTaskList(ALL_WORKERS, commands);
}


/*
 * PlanAlterTypeOwnerStmt is called for change of owner ship of functions before the owner
 * ship is changed on the local instance.
 *
 * If the function for which the owner is changed is distributed we execute the change on
 * all the workers to keep the type in sync across the cluster.
 */
List *
PlanAlterFunctionOwnerStmt(AlterOwnerStmt *stmt, const char *queryString)
{
	const ObjectAddress *address = NULL;
	const char *sql = NULL;
	List *commands = NULL;

	AssertIsFunctionOrProcedure(stmt->objectType);

	address = GetObjectAddressFromParseTree((Node *) stmt, false);
	if (!ShouldPropagateAlterFunction(address))
	{
		return NIL;
	}

	EnsureCoordinator();
	EnsureSequentialModeForFunctionDDL();
	QualifyTreeNode((Node *) stmt);
	sql = DeparseTreeNode((Node *) stmt);

	commands = list_make3(DISABLE_DDL_PROPAGATION,
						  (void *) sql,
						  ENABLE_DDL_PROPAGATION);

	return NodeDDLTaskList(ALL_WORKERS, commands);
}


/*
 * PlanDropFunctionStmt gets called during the planning phase of a DROP FUNCTION statement
 * and returns a list of DDLJob's that will drop any distributed functions from the
 * workers.
 *
 * The DropStmt could have multiple objects to drop, the list of objects will be filtered
 * to only keep the distributed functions for deletion on the workers. Non-distributed
 * functions will still be dropped locally but not on the workers.
 */
List *
PlanDropFunctionStmt(DropStmt *stmt, const char *queryString)
{
	List *deletingObjectWithArgsList = stmt->objects;
	List *distributedObjectWithArgsList = NIL;
	List *distributedFunctionAddresses = NIL;
	ListCell *addressCell = NULL;
	const char *dropStmtSql = NULL;
	List *commands = NULL;
	ListCell *objectWithArgsListCell = NULL;
	DropStmt *stmtCopy = NULL;

	AssertIsFunctionOrProcedure(stmt->removeType);

	if (creating_extension)
	{
		/*
		 * extensions should be created separately on the workers, types cascading from an
		 * extension should therefor not be propagated here.
		 */
		return NIL;
	}

	if (!EnableDependencyCreation)
	{
		/*
		 * we are configured to disable object propagation, should not propagate anything
		 */
		return NIL;
	}


	/*
	 * Our statements need to be fully qualified so we can drop them from the right schema
	 * on the workers
	 */
	QualifyTreeNode((Node *) stmt);

	/*
	 * iterate over all functions to be dropped and filter to keep only distributed
	 * functions.
	 */
	foreach(objectWithArgsListCell, deletingObjectWithArgsList)
	{
		ObjectWithArgs *func = NULL;
		ObjectAddress *address = NULL;

		func = castNode(ObjectWithArgs, lfirst(objectWithArgsListCell));
		address = FunctionToObjectAddress(stmt->removeType, func, stmt->missing_ok);

		if (!IsObjectDistributed(address))
		{
			continue;
		}

		/* collect information for all distributed functions */
		distributedFunctionAddresses = lappend(distributedFunctionAddresses, address);
		distributedObjectWithArgsList = lappend(distributedObjectWithArgsList, func);
	}

	if (list_length(distributedObjectWithArgsList) <= 0)
	{
		/* no distributed functions to drop */
		return NIL;
	}

	/*
	 * managing types can only be done on the coordinator if ddl propagation is on. when
	 * it is off we will never get here. MX workers don't have a notion of distributed
	 * types, so we block the call.
	 */
	EnsureCoordinator();
	EnsureSequentialModeForFunctionDDL();

	/* remove the entries for the distributed objects on dropping */
	foreach(addressCell, distributedFunctionAddresses)
	{
		ObjectAddress *address = (ObjectAddress *) lfirst(addressCell);
		UnmarkObjectDistributed(address);
	}

	/*
	 * Swap the list of objects before deparsing and restore the old list after. This
	 * ensures we only have distributed functions in the deparsed drop statement.
	 */
	stmtCopy = copyObject(stmt);
	stmtCopy->objects = distributedObjectWithArgsList;
	dropStmtSql = DeparseTreeNode((Node *) stmtCopy);

	commands = list_make3(DISABLE_DDL_PROPAGATION,
						  (void *) dropStmtSql,
						  ENABLE_DDL_PROPAGATION);

	return NodeDDLTaskList(ALL_WORKERS, commands);
}


/*
 * PlanAlterFunctionDependsStmt is called during the planning phase of an
 * ALTER FUNCION ... DEPENDS ON EXTENSION ... statement. Since functions depending on
 * extensions are assumed to be Owned by an extension we assume the extension to keep the
 * function in sync.
 *
 * If we would allow users to create a dependency between a distributed function and an
 * extension our pruning logic for which objects to distribute as dependencies of other
 * objects will change significantly which could cause issues adding new workers. Hence we
 * don't allow this dependency to be created.
 */
List *
PlanAlterFunctionDependsStmt(AlterObjectDependsStmt *stmt, const char *queryString)
{
	const ObjectAddress *address = NULL;
	const char *functionName = NULL;

	AssertIsFunctionOrProcedure(stmt->objectType);

	if (creating_extension)
	{
		/*
		 * extensions should be created separately on the workers, types cascading from an
		 * extension should therefor not be propagated here.
		 */
		return NIL;
	}

	if (!EnableDependencyCreation)
	{
		/*
		 * we are configured to disable object propagation, should not propagate anything
		 */
		return NIL;
	}

	address = GetObjectAddressFromParseTree((Node *) stmt, true);
	if (!IsObjectDistributed(address))
	{
		return NIL;
	}

	/*
	 * Distributed objects should not start depending on an extension, this will break
	 * the dependency resolving mechanism we use to replicate distributed objects to new
	 * workers
	 */

	functionName = getObjectIdentity(address);
	ereport(ERROR, (errmsg("distrtibuted functions are not allowed to depend on an "
						   "extension"),
					errdetail("Function \"%s\" is already distributed. Functions from "
							  "extensions are expected to be created on the workers by "
							  "the extension they depend on.", functionName)));
}


/*
 * AlterFunctionDependsStmtObjectAddress resolves the ObjectAddress of the function that
 * is the subject of an ALTER FUNCTION ... DEPENS ON EXTENSION ... statement. If
 * missing_ok is set to false the lookup will raise an error.
 */
const ObjectAddress *
AlterFunctionDependsStmtObjectAddress(AlterObjectDependsStmt *stmt, bool missing_ok)
{
	AssertIsFunctionOrProcedure(stmt->objectType);

	return FunctionToObjectAddress(stmt->objectType,
								   castNode(ObjectWithArgs, stmt->object), missing_ok);
}


/*
 * ProcessAlterFunctionSchemaStmt is executed after the change has been applied locally,
 * we can now use the new dependencies of the function to ensure all its dependencies
 * exist on the workers before we apply the commands remotely.
 */
void
ProcessAlterFunctionSchemaStmt(AlterObjectSchemaStmt *stmt, const char *queryString)
{
	const ObjectAddress *address = NULL;

	AssertIsFunctionOrProcedure(stmt->objectType);

	address = GetObjectAddressFromParseTree((Node *) stmt, false);
	if (!ShouldPropagateAlterFunction(address))
	{
		return;
	}

	/* dependencies have changed (schema) lets ensure they exist */
	EnsureDependenciesExistsOnAllNodes(address);
}


/*
 * AlterFunctionStmtObjectAddress returns the ObjectAddress of the subject in the
 * AlterFunctionStmt. If missing_ok is set to false an error will be raised if postgres
 * was unable to find the function/procedure that was the target of the statement.
 */
const ObjectAddress *
AlterFunctionStmtObjectAddress(AlterFunctionStmt *stmt, bool missing_ok)
{
	ObjectType objectType = OBJECT_FUNCTION;

#if PG_VERSION_NUM >= 110000
	objectType = stmt->objtype;
#endif

	return FunctionToObjectAddress(objectType, stmt->func, missing_ok);
}


/*
 * RenameFunctionStmtObjectAddress returns the ObjectAddress of the function that is the
 * subject of the RenameStmt. Errors if missing_ok is false.
 */
const ObjectAddress *
RenameFunctionStmtObjectAddress(RenameStmt *stmt, bool missing_ok)
{
	return FunctionToObjectAddress(stmt->renameType,
								   castNode(ObjectWithArgs, stmt->object), missing_ok);
}


/*
 * AlterFunctionOwnerObjectAddress returns the ObjectAddress of the function that is the
 * subject of the AlterOwnerStmt. Errors if missing_ok is false.
 */
const ObjectAddress *
AlterFunctionOwnerObjectAddress(AlterOwnerStmt *stmt, bool missing_ok)
{
	return FunctionToObjectAddress(stmt->objectType,
								   castNode(ObjectWithArgs, stmt->object), missing_ok);
}


/*
 * AlterFunctionSchemaStmtObjectAddress returns the ObjectAddress of the function that is
 * the subject of the AlterObjectSchemaStmt. Errors if missing_ok is false.
 *
 * This could be called both before or after it has been applied locally. It will look in
 * the old schema first, if the function cannot be found in that schema it will look in
 * the new schema. Errors if missing_ok is false and the type cannot be found in either of
 * the schemas.
 */
const ObjectAddress *
AlterFunctionSchemaStmtObjectAddress(AlterObjectSchemaStmt *stmt, bool missing_ok)
{
	ObjectWithArgs *objectWithArgs = NULL;
	Oid funcOid = InvalidOid;
	List *names = NIL;
	ObjectAddress *address = NULL;

	AssertIsFunctionOrProcedure(stmt->objectType);

	objectWithArgs = castNode(ObjectWithArgs, stmt->object);
	funcOid = LookupFuncWithArgsCompat(stmt->objectType, objectWithArgs, true);
	names = objectWithArgs->objname;

	if (funcOid == InvalidOid)
	{
		/*
		 * couldn't find the function, might have already been moved to the new schema, we
		 * construct a new objname that uses the new schema to search in.
		 */

		/* the name of the function is the last in the list of names */
		Value *funcNameStr = lfirst(list_tail(names));
		List *newNames = list_make2(makeString(stmt->newschema), funcNameStr);

		/*
		 * we don't error here either, as the error would be not a good user facing
		 * error if the type didn't exist in the first place.
		 */
		objectWithArgs->objname = newNames;
		funcOid = LookupFuncWithArgsCompat(stmt->objectType, objectWithArgs, true);
		objectWithArgs->objname = names; /* restore the original names */

		/*
		 * if the function is still invalid we couldn't find the function, cause postgres
		 * to error by preforming a lookup once more. Since we know the
		 */
		if (!missing_ok && funcOid == InvalidOid)
		{
			/*
			 * this will most probably throw an error, unless for some reason the function
			 * has just been created (if possible at all). For safety we assign the
			 * funcOid.
			 */
			funcOid = LookupFuncWithArgsCompat(stmt->objectType, objectWithArgs,
											   missing_ok);
		}
	}

	address = palloc0(sizeof(ObjectAddress));
	ObjectAddressSet(*address, ProcedureRelationId, funcOid);

	return address;
}


/*
 * FunctionToObjectAddress returns the ObjectAddress of a Function or Procedure based on
 * its type and ObjectWithArgs describing the Function/Procedure. If missing_ok is set to
 * false an error will be raised by postgres explaining the Function/Procedure could not
 * be found.
 */
static ObjectAddress *
FunctionToObjectAddress(ObjectType objectType, ObjectWithArgs *objectWithArgs,
						bool missing_ok)
{
	Oid funcOid = InvalidOid;
	ObjectAddress *address = NULL;

	AssertIsFunctionOrProcedure(objectType);

	funcOid = LookupFuncWithArgsCompat(objectType, objectWithArgs, missing_ok);
	address = palloc0(sizeof(ObjectAddress));
	ObjectAddressSet(*address, ProcedureRelationId, funcOid);

	return address;
}


/*
 * ErrorIfUnsupportedAlterFunctionStmt raises an error if the AlterFunctionStmt contains a
 * construct that is not supported to be altered on a distributed function. It is assumed
 * the statement passed in is already tested to be targeting a distributed function, and
 * will only execute the checks to error on unsupported constructs.
 *
 * Unsupported Constructs:
 *  - ALTER FUNCTION ... SET ... FROM CURRENT
 */
static void
ErrorIfUnsupportedAlterFunctionStmt(AlterFunctionStmt *stmt)
{
	ListCell *actionCell = NULL;

	foreach(actionCell, stmt->actions)
	{
		DefElem *action = castNode(DefElem, lfirst(actionCell));
		if (strcmp(action->defname, "set") == 0)
		{
			VariableSetStmt *setStmt = castNode(VariableSetStmt, action->arg);
			if (setStmt->kind == VAR_SET_CURRENT)
			{
				/* check if the set action is a SET ... FROM CURRENT */
				ereport(ERROR, (errmsg("unsupported ALTER FUNCTION ... SET ... FROM "
									   "CURRENT for a distributed function"),
								errhint("SET FROM CURRENT is not supported for "
										"distributed functions, instead use the SET ... "
										"TO ... syntax with a constant value.")));
			}
		}
	}
}


/*
 * ErrorIfFunctionDependsOnExtension functions depending on extensions should raise an
 * error informing the user why they can't be distributed.
 */
static void
ErrorIfFunctionDependsOnExtension(const ObjectAddress *functionAddress)
{
	/* captures the extension address during lookup */
	ObjectAddress extensionAddress = { 0 };

	if (IsObjectAddressOwnedByExtension(functionAddress, &extensionAddress))
	{
		char *functionName = getObjectIdentity(functionAddress);
		char *extensionName = getObjectIdentity(&extensionAddress);
		ereport(ERROR, (errmsg("unable to create a distributed function from functions "
							   "owned by an extension"),
						errdetail("Function \"%s\" has a dependency on extension \"%s\". "
								  "Functions depending on an extension cannot be "
								  "distributed. Create the function by creating the "
								  "extension on the workers.", functionName,
								  extensionName)));
	}
}
