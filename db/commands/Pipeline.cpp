/**
 * Copyright (c) 2011 10gen Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "pch.h"
#include "Pipeline.h"

#include "../cursor.h"
#include "../AccumulatorSum.h"
#include "../Document.h"
#include "../DocumentSource.h"
#include "../DocumentSourceCursor.h"
#include "../DocumentSourceFilter.h"
#include "../DocumentSourceGroup.h"
#include "../DocumentSourceProject.h"
#include "../ExpressionAnd.h"
#include "../ExpressionCompare.h"
#include "../ExpressionConstant.h"
#include "../ExpressionDocument.h"
#include "../ExpressionFieldPath.h"
#include "../ExpressionOr.h"
#include "../FieldIterator.h"
#include "../pdfile.h"

namespace mongo
{
    // singleton static self-registering instance
    static Pipeline pipelineCmd;

    Pipeline::Pipeline() :
	Command("pipeline")
    {
    }

    Command::LockType Pipeline::locktype() const
    {
	return READ;
    }

    bool Pipeline::slaveOk() const
    {
	return true;
    }

    void Pipeline::help(stringstream &help) const
    {
	help << "{ pipeline : [ { <data-pipe-op>: {...}}, ... ] }";
    }

    Pipeline::~Pipeline()
    {
    }

    bool Pipeline::run(const string &db, BSONObj &cmdObj,
			string &errmsg,
			BSONObjBuilder &result, bool fromRepl)
    {
	string collectionName;
	vector<BSONElement> pipeline;

	/* gather the specification for the aggregation */
	for(BSONObj::iterator cmdIterator = cmdObj.begin();
	    cmdIterator.more(); )
	{
	    BSONElement cmdElement(cmdIterator.next());
	    const char *pFieldName = cmdElement.fieldName();

	    /* look for the aggregation command */
	    if (!strcmp(pFieldName, "pipeline"))
	    {
		pipeline = cmdElement.Array();
		continue;
	    }

	    /* check for the collection name */
	    if (!strcmp(pFieldName, "collection"))
	    {
		collectionName = cmdElement.String();
		continue;
	    }

	    /* we didn't recognize a field in the command */
	    ostringstream sb;
	    sb <<
		"Pipeline::run(): unrecognized field \"" <<
		cmdElement.fieldName();
	    errmsg = sb.str();
	    return false;
	}

	/*
	  If we get here, we've harvested the fields we expect

	  Set up the document source pipeline.
	*/
	BSONArrayBuilder resultArray; // where we'll stash the results

	/* connect up a cursor to the specified collection */
	shared_ptr<Cursor> pCursor(
	    findTableScan(collectionName.c_str(), BSONObj()));
	shared_ptr<DocumentSource> pSource(new DocumentSourceCursor(pCursor));

	/* iterate over the steps in the pipeline */
	const size_t nSteps = pipeline.size();
	for(size_t iStep = 0; iStep < nSteps; ++iStep)
	{
	    /* pull out the pipeline element as an object */
	    BSONElement pipeElement(pipeline[iStep]);
	    assert(pipeElement.type() == Object); // CW TODO user error
	    BSONObj bsonObj(pipeElement.Obj());

	    /* use the object to add a DocumentSource to the processing chain */
	    BSONObjIterator bsonIterator(bsonObj);
	    while(bsonIterator.more())
	    {
		BSONElement bsonElement(bsonIterator.next());
		const char *pFieldName = bsonElement.fieldName();

		/* select the appropriate operation */
		if (strcmp(pFieldName, "$project") == 0)
		    pSource = setupProject(&bsonElement, pSource);
		else if (strcmp(pFieldName, "$filter") == 0)
		    pSource = setupFilter(&bsonElement, pSource);
		else if (strcmp(pFieldName, "$group") == 0)
		    pSource = setupGroup(&bsonElement, pSource);
		else
		{
		    ostringstream sb;
		    sb <<
			"Pipeline::run(): unrecognized pipeline op \"" <<
			pFieldName;
		    errmsg = sb.str();
		    return false;
		}
	    }
	}

	/*
	  Iterate through the resulting documents, and add them to the result.
	*/
	for(bool hasDocument = !pSource->eof(); hasDocument;
	    hasDocument = pSource->advance())
	{
	    shared_ptr<Document> pDocument(pSource->getCurrent());

	    /* add the document to the result set */
	    BSONObjBuilder documentBuilder;
	    pDocument->toBson(&documentBuilder);
	    resultArray.append(documentBuilder.done());
	}

	result.appendArray("result", resultArray.done());
	
	return true;
    }

    shared_ptr<Expression> Pipeline::parseOperand(BSONElement *pBsonElement)
    {
	BSONType type = pBsonElement->type();

	switch(type)
	{
	case String:
	{
	    /*
	      This could be a field path, or it could be a constant
	      string.

	      We make a copy of the BSONElement reader so we can read its
	      value without advancing its state, in case we need to read it
	      again in the constant code path.
	    */
	    BSONElement opCopy(*pBsonElement);
	    string value(opCopy.String());
	    
	    /* check for a field path */
	    if (value.compare(0, 10, "$document.") != 0)
		goto ExpectConstant;  // assume plain string constant

	    /* if we got here, this is a field path expression */
	    string fieldPath(value.substr(10));
	    shared_ptr<Expression> pFieldExpr(
		ExpressionFieldPath::create(fieldPath));
	    return pFieldExpr;
	}
	    
	case Object:
	{
	    shared_ptr<Expression> pSubExpression(
		parseDocument(pBsonElement));
	    return pSubExpression;
	}

	default:
	ExpectConstant:
	{
	    shared_ptr<Expression> pOperand(
		ExpressionConstant::createFromBsonElement(pBsonElement));
	    return pOperand;
	}
	
	} // switch(type)

	/* NOTREACHED */
	assert(false);
	return shared_ptr<Expression>();
    }

    struct OpDesc
    {
	const char *pName;
	shared_ptr<ExpressionNary> (*pFactory)(void);
    };

    static int OpDescCmp(const void *pL, const void *pR)
    {
	return strcmp(((const OpDesc *)pL)->pName, ((const OpDesc *)pR)->pName);
    }

    /*
      Keep these sorted alphabetically so we can bsearch() them using
      OpDescCmp() above.
    */
    static const OpDesc OpTable[] =
    {
//	{"$add", ExpressionAdd::create},
	{"$and", ExpressionAnd::create},
	{"$cmp", ExpressionCompare::createCmp},
//	{"$divide", ExpressionDivide::create},
	{"$eq", ExpressionCompare::createEq},
	{"$gt", ExpressionCompare::createGt},
	{"$gte", ExpressionCompare::createGte},
	{"$lt", ExpressionCompare::createLt},
	{"$lte", ExpressionCompare::createLte},
	{"$ne", ExpressionCompare::createNe},
//	{"$not", ExpressionNot::create},
	{"$or", ExpressionOr::create},
    };

    static const size_t NOp = sizeof(OpTable)/sizeof(OpTable[0]);

    shared_ptr<Expression> Pipeline::parseExpression(
	const char *pOpName, BSONElement *pBsonElement)
    {
	/* look for the specified operator */
	OpDesc key;
	key.pName = pOpName;
	const OpDesc *pOp = (const OpDesc *)bsearch(
	    &key, OpTable, NOp, sizeof(OpDesc), OpDescCmp);

	assert(pOp); // CW TODO error: invalid operator
	shared_ptr<ExpressionNary> pExpression((*pOp->pFactory)());

	parseExpressionOperands(pExpression, pBsonElement);
	return pExpression;
    }

    void Pipeline::parseExpressionOperands(
	shared_ptr<ExpressionNary> pExpression, BSONElement *pBsonElement)
    {
	/* add the operands to the operator expression */
	BSONType elementType = pBsonElement->type();
	if (elementType == Object)
	{
	    shared_ptr<Expression> pOperand(parseDocument(pBsonElement));
	    pExpression->addOperand(pOperand);
	}
	else if (elementType == Array)
	{
	    /* multiple operands - an n-ary operator */
	    vector<BSONElement> bsonArray(pBsonElement->Array());
	    const size_t n = bsonArray.size();
	    for(size_t i = 0; i < n; ++i)
	    {
		BSONElement *pBsonOperand = &bsonArray[i];
		shared_ptr<Expression> pOperand(parseOperand(pBsonOperand));
		pExpression->addOperand(pOperand);
	    }
	}
	else /* assume its an atomic operand */
	{
	    shared_ptr<Expression> pOperand(parseOperand(pBsonElement));
	    pExpression->addOperand(pOperand);
	}
    }

    shared_ptr<Expression> Pipeline::parseDocument(BSONElement *pBsonElement)
    {
	shared_ptr<Expression> pExpression; // the result
	shared_ptr<ExpressionDocument> pExpressionDocument; // alt result
	int isOp = -1; /* -1 -> unknown, 0 -> not an operator, 1 -> operator */

	/*
	  We're looking at an operand or the value of a field.  This could be
	  a virtual document, or it could be an expression.
	*/
	BSONObj opObj(pBsonElement->Obj());

	BSONObjIterator objIterator(opObj);
	for(size_t fieldCount = 0; objIterator.more(); ++fieldCount)
	{
	    BSONElement subField(objIterator.next());
	    string subFieldName(subField.fieldName());

	    /* check the field to see if it is an operator */
	    if (subFieldName.at(0) == '$')
	    {
		assert(fieldCount == 0); // CW TODO operator must be only field
		pExpression = parseExpression(subFieldName.c_str(), &subField);
		isOp = 1;
	    }
	    else /* it's just a regular field name */
	    {
		assert(isOp != 1); // can't accompany an operator

		/* if it's out first time, create the document expression */
		if (!pExpression.get())
		{
		    pExpressionDocument = ExpressionDocument::create();
		    pExpression = pExpressionDocument;
		}

		BSONType subType = subField.type();
		if (subType == Object)
		{
		    /* its a nested document */
		    shared_ptr<Expression> pNested(parseDocument(&subField));
		    pExpressionDocument->addField(subFieldName, pNested);
		}
		else if (subType == String)
		{
		    /* its a renamed field */
		    shared_ptr<Expression> pPath(
			ExpressionFieldPath::create(subField.String()));
		    pExpressionDocument->addField(subFieldName, pPath);
		}
		else if (subType == NumberDouble)
		{
		    /* its an inclusion specification */
		    shared_ptr<Expression> pPath(
			ExpressionFieldPath::create(subFieldName));
		    pExpressionDocument->addField(subFieldName, pPath);
		}
		else /* nothing else is allowed */
		{
		    assert(false); // CW TODO error
		}

		isOp = 0;
	    }
	}

	return pExpression;
    }

    shared_ptr<DocumentSource> Pipeline::setupProject(
	BSONElement *pBsonElement, shared_ptr<DocumentSource> pSource)
    {
	/* validate */
	assert(pBsonElement->type() == Object); // CW TODO user error

	/* chain the projection onto the original source */
	shared_ptr<DocumentSourceProject> pProject(
	    DocumentSourceProject::create(pSource));

	/*
	  Pull out the $project object.  This should just be a list of
	  field inclusion or exclusion specifications.  Note you can't do
	  both, except for the case of _id.
	 */
	BSONObj projectObj(pBsonElement->Obj());
	BSONObjIterator fieldIterator(projectObj);
	while(fieldIterator.more())
	{
	    BSONElement outFieldElement(fieldIterator.next());
	    string outFieldName(outFieldElement.fieldName());
	    string inFieldName(outFieldName);
	    BSONType specType = outFieldElement.type();
	    int fieldInclusion = -1;
	    bool ravelArray = false;

	    assert(outFieldName.find('.') == outFieldName.npos);
	        // CW TODO user error: out field name can't use dot notation

	    switch(specType)
	    {
	    case NumberDouble:
	    {
		double inclusion = outFieldElement.numberDouble();
		if ((inclusion == 0) || (inclusion == 1))
		    fieldInclusion = (int)inclusion;
		else
		{
		    assert(false); // CW TODO unimplemented constant expression
		}

		goto AddField;
	    }

	    case NumberInt:
		/* just a plain integer include/exclude specification */
		fieldInclusion = outFieldElement.numberInt();
		assert((fieldInclusion >= 0) && (fieldInclusion <= 1));
    		    // CW TODO invalid field projection specification

	    AddField:
		if (fieldInclusion == 0)
		    assert(false); // CW TODO unimplemented
		else
		{
		    shared_ptr<Expression> pExpression(
			ExpressionFieldPath::create(inFieldName));
		    pProject->addField(outFieldName, pExpression, ravelArray);
		}
		break;

	    case Bool:
		/* just a plain boolean include/exclude specification */
		fieldInclusion = outFieldElement.Bool() ? 1 : 0;
		goto AddField;

	    case String:
		/* include a field, with rename */
		fieldInclusion = 1;
		inFieldName = outFieldElement.String();
		goto AddField;

	    case Object:
	    {
		/*
		  A computed expression, or a ravel (unwinding an array
		  one element at a time).

		  We handle $ravel as a special case,
		  because this is done by the projection source.  For any
		  other expression, we hand over control to code that parses
		  the expression and returns an expression.

		  field expressions will look like one of
		  f0: {f1: ..., f2: ..., f3: ...}
		  f0: {$operator:[operand1, operand2, ...]}
		  f0: {$ravel:"f1"}
		*/
		BSONObj fieldExprObj(outFieldElement.Obj());
		BSONObjIterator exprIterator(fieldExprObj);
		size_t subFieldCount = 0;
		while(exprIterator.more())
		{
		    ++subFieldCount;

		    BSONElement exprElement(exprIterator.next());
		    const char *pOpName = exprElement.fieldName();
		    if (pOpName[0] != '$')
		    {
			/* create a virtual document */
			shared_ptr<Expression> pDocExpr(
			    parseDocument(&exprElement));
			pProject->addField(outFieldName, pDocExpr, false);
		    }
		    else if (strcmp(pOpName, "$ravel") == 0)
		    {
			assert(subFieldCount == 1);
			    // CW TODO usage error
			assert(exprElement.type() == String);
			    // CW TODO $ravel operand must be single field name
			ravelArray = true;
			inFieldName = exprElement.String();

			shared_ptr<Expression> pExpression(
			    ExpressionFieldPath::create(inFieldName));
			pProject->addField(
			    outFieldName, pExpression, ravelArray);
		    }
		    else
		    {
			shared_ptr<Expression> pExpression(
			    parseExpression(pOpName, &exprElement));

			pProject->addField(
			    outFieldName, pExpression, false);
		    }
		}

		break;
	    }

	    default:
		assert(false); // CW TODO invalid field projection specification
	    }

	}

	return pProject;
    }

    shared_ptr<Expression> Pipeline::parseExpressionObject(
	BSONElement *pBsonElement)
    {
	assert(pBsonElement->type() == Object);
  	    // CW TODO expression object must be an object

	shared_ptr<Expression> pExpression;

	BSONObj filterObj(pBsonElement->Obj());
	BSONObjIterator filterIterator(filterObj);
	for(size_t i = 0; filterIterator.more(); ++i)
	{
	    assert(i == 0);
	        // CW TODO error only one field allowed in an expression object

	    BSONElement filterExpr(filterIterator.next());
	    const char *pOpName = filterExpr.fieldName();
	    pExpression = parseExpression(pOpName, &filterExpr);
	}

	return pExpression;
    }

    shared_ptr<DocumentSource> Pipeline::setupFilter(
	BSONElement *pBsonElement, shared_ptr<DocumentSource> pSource)
    {
	shared_ptr<Expression> pExpression(parseExpressionObject(pBsonElement));
	shared_ptr<DocumentSourceFilter> pFilter(
	    DocumentSourceFilter::create(pExpression, pSource));

	return pFilter;
    }

    struct GroupOpDesc
    {
	const char *pName;
	shared_ptr<Accumulator> (*pFactory)(void);
    };

    static int GroupOpDescCmp(const void *pL, const void *pR)
    {
	return strcmp(((const GroupOpDesc *)pL)->pName,
		      ((const GroupOpDesc *)pR)->pName);
    }

    /*
      Keep these sorted alphabetically so we can bsearch() them using
      GroupOpDescCmp() above.
    */
    static const GroupOpDesc GroupOpTable[] =
    {
//	{"$min", AccumulatorMin::create},
//	{"$max", AccumulatorMax::create},
	{"$sum", AccumulatorSum::create},
    };

    static const size_t NGroupOp = sizeof(GroupOpTable)/sizeof(GroupOpTable[0]);

    shared_ptr<DocumentSource> Pipeline::setupGroup(
	BSONElement *pBsonElement, shared_ptr<DocumentSource> pSource)
    {
	assert(pBsonElement->type() == Object); // CW TODO must be an object

	shared_ptr<DocumentSourceGroup> pGroup(
	    DocumentSourceGroup::create(pSource));
	bool idSet = false;

	BSONObj groupObj(pBsonElement->Obj());
	BSONObjIterator groupIterator(groupObj);
	while(groupIterator.more())
	{
	    BSONElement groupField(groupIterator.next());
	    const char *pFieldName = groupField.fieldName();

	    if (strcmp(pFieldName, "_id") == 0)
	    {
		assert(!idSet); // CW TODO _id specified multiple times
		assert(groupField.type() == Object); // CW TODO error message

		/*
		  Use the projection-like set of field paths to create the
		  group-by key.
		 */
		shared_ptr<Expression> pId(parseDocument(&groupField));
		pGroup->setIdExpression(pId);
		idSet = true;
	    }
	    else
	    {
		/*
		  Treat as a projection field with the additional ability to
		  add aggregation operators.
		*/
		assert(*pFieldName != '$');
                    // CW TODO error: field name can't be an operator
		assert(groupField.type() == Object);
		    // CW TODO error: must be an operator expression

		BSONObj subField(groupField.Obj());
		BSONObjIterator subIterator(subField);
		size_t subCount = 0;
		for(; subIterator.more(); ++subCount)
		{
		    BSONElement subElement(subIterator.next());

		    /* look for the specified operator */
		    GroupOpDesc key;
		    key.pName = subElement.fieldName();
		    const GroupOpDesc *pOp = (const GroupOpDesc *)bsearch(
			&key, GroupOpTable, NGroupOp, sizeof(GroupOpDesc),
			GroupOpDescCmp);

		    assert(pOp); // CW TODO error: operator not found

		    shared_ptr<Expression> pGroupExpr;

		    BSONType elementType = subElement.type();
		    if (elementType == Object)
			pGroupExpr = parseDocument(&subElement);
		    else if (elementType == Array)
		    {
			assert(false); // CW TODO group operators are unary
		    }
		    else /* assume its an atomic single operand */
		    {
			pGroupExpr = parseOperand(&subElement);
		    }

		    pGroup->addAccumulator(
			pFieldName, pOp->pFactory, pGroupExpr); 
		}

		assert(subCount == 1);
                    // CW TODO error: only one operator allowed
	    }
	}

	assert(idSet); // CW TODO error: missing _id specification

	return pGroup;
    }
}