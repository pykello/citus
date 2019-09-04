
#include "postgres.h"

#include "distributed/commands.h"
#include "distributed/deparser.h"

static const ObjectAddress * AlterTableStmtObjectAddress(AlterTableStmt *stmt,
														 bool missing_ok);
static const ObjectAddress * RenameStmtObjectAddress(RenameStmt *stmt, bool missing_ok);


/*
 * GetObjectAddressFromParseTree returns the ObjectAdderss of the main target of the parse
 * tree.
 */
const ObjectAddress *
GetObjectAddressFromParseTree(Node *parseTree, bool missing_ok)
{
	switch (parseTree->type)
	{
		case T_CompositeTypeStmt:
		{
			return CompositeTypeStmtObjectAddress(castNode(CompositeTypeStmt, parseTree),
												  missing_ok);
		}

		case T_AlterTableStmt:
		{
			return AlterTableStmtObjectAddress(castNode(AlterTableStmt, parseTree),
											   missing_ok);
		}

		case T_CreateEnumStmt:
		{
			return CreateEnumStmtObjectAddress(castNode(CreateEnumStmt, parseTree),
											   missing_ok);
		}

		case T_AlterEnumStmt:
		{
			return AlterEnumStmtObjectAddress(castNode(AlterEnumStmt, parseTree),
											  missing_ok);
		}

		case T_RenameStmt:
		{
			return RenameStmtObjectAddress(castNode(RenameStmt, parseTree), missing_ok);
		}

		default:
		{
			/*
			 * should not be reached, indicates the coordinator is sending unsupported
			 * statements
			 */
			ereport(ERROR, (errmsg("unsupported statement to get object address for")));
			return NULL;
		}
	}
}


static const ObjectAddress *
AlterTableStmtObjectAddress(AlterTableStmt *stmt, bool missing_ok)
{
	switch (stmt->relkind)
	{
		case OBJECT_TYPE:
		{
			return AlterTypeStmtObjectAddress(stmt, missing_ok);
		}

		default:
		{
			ereport(ERROR, (errmsg("unsupported alter statement to get object address for"
								   )));
		}
	}
}


static const ObjectAddress *
RenameStmtObjectAddress(RenameStmt *stmt, bool missing_ok)
{
	switch (stmt->renameType)
	{
		case OBJECT_TYPE:
		{
			return RenameTypeStmtObjectAddress(stmt, missing_ok);
		}

		default:
		{
			ereport(ERROR, (errmsg("unsupported rename statement to get object address "
								   "for")));
		}
	}
}
