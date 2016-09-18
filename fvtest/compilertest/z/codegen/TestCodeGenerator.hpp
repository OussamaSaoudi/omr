/*******************************************************************************
 *
 * (c) Copyright IBM Corp. 2000, 2016
 *
 *  This program and the accompanying materials are made available
 *  under the terms of the Eclipse Public License v1.0 and
 *  Apache License v2.0 which accompanies this distribution.
 *
 *      The Eclipse Public License is available at
 *      http://www.eclipse.org/legal/epl-v10.html
 *
 *      The Apache License v2.0 is available at
 *      http://www.opensource.org/licenses/apache2.0.php
 *
 * Contributors:
 *    Multiple authors (IBM Corp.) - initial implementation and documentation
 *******************************************************************************/

#ifndef TEST_Z_CODEGENERATORBASE_INCL
#define TEST_Z_CODEGENERATORBASE_INCL

/*
 * The following #define and typedef must appear before any #includes in this file
 */
#ifndef TEST_CODEGENERATORBASE_CONNECTOR
#define TEST_CODEGENERATORBASE_CONNECTOR

namespace Test { namespace Z { class CodeGenerator; } }
namespace Test { typedef Z::CodeGenerator CodeGeneratorConnector; }

#else
#error Test::Z::CodeGenerator expected to be a primary connector, but a Test connector is already defined
#endif


#include "compilertest/codegen/TestCodeGenerator.hpp"
#include "codegen/LinkageConventionsEnum.hpp"


namespace Test
{
namespace Z
{

class OMR_EXTENSIBLE CodeGenerator : public Test::CodeGenerator
   {
   public:

   CodeGenerator();

   TR::Linkage *createLinkage(TR_LinkageConventions lc);

   };

} // namespace Z
} // namespace Test

#endif // !defined(TEST_Z_CODEGENERATORBASE_INCL)


