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
#include "db/AccumulatorMinMax.h"

#include "db/Value.h"

namespace mongo
{
    shared_ptr<const Value> AccumulatorMinMax::evaluate(
	shared_ptr<Document> pDocument) const
    {
	assert(vpOperand.size() == 1);
	shared_ptr<const Value> prhs(vpOperand[0]->evaluate(pDocument));

	int cmp = Value::compare(pValue, prhs) * sense;
	if (cmp > 0)
	    pValue = prhs;

	return pValue;
    }

    AccumulatorMinMax::AccumulatorMinMax(int theSense):
	Accumulator(Value::getZero()),
	sense(int theSense)
    {
	assert((sense == 1) || (sense == -1)); // CW TODO error
    }

    shared_ptr<Accumulator> AccumulatorMinMax::createMin()
    {
	shared_ptr<AccumulatorMinMax> pAccumulator(new AccumulatorMinMax(1));
	return pAccumulator;
    }

    shared_ptr<Accumulator> AccumulatorMinMax::createMax()
    {
	shared_ptr<AccumulatorMinMax> pAccumulator(new AccumulatorMinMax(-1));
	return pAccumulator;
    }
}