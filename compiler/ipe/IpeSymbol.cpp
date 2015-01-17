/*
 * Copyright 2004-2015 Cray Inc.
 * Other additional copyright holders may be indicated within.
 *
 * The entirety of this work is licensed under the Apache License,
 * Version 2.0 (the "License"); you may not use this file except
 * in compliance with the License.
 *
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "IpeSymbol.h"

#include "AstVisitor.h"

IpeSymbol::IpeSymbol(const char* name,
                     Type*       type,
                     int         depth,
                     int         offset) :
  Symbol(E_IpeSymbol, name, type)
{
  mDepth  = depth;
  mOffset = offset;

  gIpeSymbols.add(this);
}

IpeSymbol::~IpeSymbol()
{

}

void IpeSymbol::verify()
{
  Symbol::verify();

  if (astTag != E_IpeSymbol)
    INT_FATAL(this, "Bad IpeSymbol::astTag");

  if (type   == 0)
    INT_FATAL(this, "IpeSymbol::type is NULL");
}

IpeSymbol* IpeSymbol::copyInner(SymbolMap* map)
{
  IpeSymbol* retval = new IpeSymbol(name, type, mDepth, mOffset);

  retval->copyFlags(this);

  retval->cname = cname;

  return retval;
}

void IpeSymbol::replaceChild(BaseAST* oldAst, BaseAST* newAst)
{
  INT_FATAL(this, "Unexpected case in IpeSymbol::replaceChild");
}

bool IpeSymbol::isConstant() const
{
  return hasFlag(FLAG_CONST);
}

bool IpeSymbol::isConstValWillNotChange() const
{
  return hasFlag(FLAG_CONST);
}

bool IpeSymbol::isParameter() const
{
  return hasFlag(FLAG_PARAM);
}

void IpeSymbol::accept(AstVisitor* visitor)
{
  visitor->visitIpeSym(this);
}

int IpeSymbol::depth() const
{
  return mDepth;
}

int IpeSymbol::offset() const
{
  return mOffset;
}
