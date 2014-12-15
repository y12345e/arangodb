////////////////////////////////////////////////////////////////////////////////
/// @brief Aql, query AST
///
/// @file
///
/// DISCLAIMER
///
/// Copyright 2014 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Jan Steemann
/// @author Copyright 2014, ArangoDB GmbH, Cologne, Germany
/// @author Copyright 2012-2013, triAGENS GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGODB_AQL_AST_H
#define ARANGODB_AQL_AST_H 1

#include "Basics/Common.h"
#include "Aql/AstNode.h"
#include "Aql/BindParameters.h"
#include "Aql/Scopes.h"
#include "Aql/Variable.h"
#include "Aql/VariableGenerator.h"
#include "Basics/json.h"

#include <functional>

namespace triagens {
  namespace aql {

    class Query;

// -----------------------------------------------------------------------------
// --SECTION--                                                         class Ast
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief the AST
////////////////////////////////////////////////////////////////////////////////

    class Ast {

// -----------------------------------------------------------------------------
// --SECTION--                                        constructors / destructors
// -----------------------------------------------------------------------------

      public:

////////////////////////////////////////////////////////////////////////////////
/// @brief create the AST
////////////////////////////////////////////////////////////////////////////////

        Ast (Query*);

////////////////////////////////////////////////////////////////////////////////
/// @brief destroy the AST
////////////////////////////////////////////////////////////////////////////////

        ~Ast ();

// -----------------------------------------------------------------------------
// --SECTION--                                                    public methods
// -----------------------------------------------------------------------------

      public:

////////////////////////////////////////////////////////////////////////////////
/// @brief return the query
////////////////////////////////////////////////////////////////////////////////

        inline Query* query () const {
          return _query;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief return the variable generator
////////////////////////////////////////////////////////////////////////////////

        inline VariableGenerator* variables () {
          return &_variables;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief return the variable generator
////////////////////////////////////////////////////////////////////////////////

        inline VariableGenerator* variables () const {
          return const_cast<VariableGenerator*>(&_variables);
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief return the root of the AST
////////////////////////////////////////////////////////////////////////////////

        inline AstNode const* root () const {
          return _root;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief begin a subquery
////////////////////////////////////////////////////////////////////////////////

        void startSubQuery () {
          // insert a new root node
          AstNodeType type;

          if (_queries.empty()) {
            // root node of query
            type = NODE_TYPE_ROOT;
          }
          else {
            // sub query node
            type = NODE_TYPE_SUBQUERY;
          }

          auto root = createNode(type);

          // save the root node
          _queries.push_back(root);

          // set the current root node if everything went well
          _root = root;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief end a subquery
////////////////////////////////////////////////////////////////////////////////

        AstNode* endSubQuery () {
          // get the current root node
          AstNode* root = _queries.back();
          // remove it from the stack
          _queries.pop_back();

          // set root node to previous root node
          _root = _queries.back();

          // return the root node we just popped from the stack
          return root;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief whether or not we currently are in a subquery
////////////////////////////////////////////////////////////////////////////////

        bool isInSubQuery () const {
          return (_queries.size() > 1);
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief return a copy of our own bind parameters
////////////////////////////////////////////////////////////////////////////////

        std::unordered_set<std::string> bindParameters () const {
          return std::unordered_set<std::string>(_bindParameters);
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief get the query scopes
////////////////////////////////////////////////////////////////////////////////

        inline Scopes* scopes () {
          return &_scopes;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief track the write collection
////////////////////////////////////////////////////////////////////////////////

        inline void setWriteCollection (AstNode const* node) {
          TRI_ASSERT(node->type == NODE_TYPE_COLLECTION ||
                     node->type == NODE_TYPE_PARAMETER);

          _writeCollection = node;
        }

////////////////////////////////////////////////////////////////////////////////
/// @brief convert the AST into JSON
/// the caller is responsible for freeing the JSON later
////////////////////////////////////////////////////////////////////////////////

        TRI_json_t* toJson (TRI_memory_zone_t*,
                            bool) const;

////////////////////////////////////////////////////////////////////////////////
/// @brief add an operation to the root node
////////////////////////////////////////////////////////////////////////////////

        void addOperation (AstNode*);

////////////////////////////////////////////////////////////////////////////////
/// @brief create an AST for node
////////////////////////////////////////////////////////////////////////////////

        AstNode* createNodeFor (char const*,
                                AstNode const*);

////////////////////////////////////////////////////////////////////////////////
/// @brief create an AST let node
////////////////////////////////////////////////////////////////////////////////

        AstNode* createNodeLet (char const*,
                                AstNode const*,
                                bool);

////////////////////////////////////////////////////////////////////////////////
/// @brief create an AST filter node
////////////////////////////////////////////////////////////////////////////////

        AstNode* createNodeFilter (AstNode const*);

////////////////////////////////////////////////////////////////////////////////
/// @brief create an AST return node
////////////////////////////////////////////////////////////////////////////////

        AstNode* createNodeReturn (AstNode const*);

////////////////////////////////////////////////////////////////////////////////
/// @brief create an AST remove node
////////////////////////////////////////////////////////////////////////////////

        AstNode* createNodeRemove (AstNode const*,
                                   AstNode const*,
                                   AstNode const*);

////////////////////////////////////////////////////////////////////////////////
/// @brief create an AST insert node
////////////////////////////////////////////////////////////////////////////////

        AstNode* createNodeInsert (AstNode const*,
                                   AstNode const*,
                                   AstNode const*);

////////////////////////////////////////////////////////////////////////////////
/// @brief create an AST update node
////////////////////////////////////////////////////////////////////////////////

        AstNode* createNodeUpdate (AstNode const*,
                                   AstNode const*,
                                   AstNode const*,
                                   AstNode const*);

////////////////////////////////////////////////////////////////////////////////
/// @brief create an AST replace node
////////////////////////////////////////////////////////////////////////////////

        AstNode* createNodeReplace (AstNode const*,
                                    AstNode const*,
                                    AstNode const*,
                                    AstNode const*);

////////////////////////////////////////////////////////////////////////////////
/// @brief create an AST collect node
////////////////////////////////////////////////////////////////////////////////

        AstNode* createNodeCollect (AstNode const*,
                                    char const*,
                                    AstNode const*);

////////////////////////////////////////////////////////////////////////////////
/// @brief create an AST collect node, COUNT
////////////////////////////////////////////////////////////////////////////////

        AstNode* createNodeCollect (AstNode const*,
                                    char const*);

////////////////////////////////////////////////////////////////////////////////
/// @brief create an AST sort node
////////////////////////////////////////////////////////////////////////////////

        AstNode* createNodeSort (AstNode const*);

////////////////////////////////////////////////////////////////////////////////
/// @brief create an AST sort element node
////////////////////////////////////////////////////////////////////////////////

        AstNode* createNodeSortElement (AstNode const*,
                                        AstNode const*);

////////////////////////////////////////////////////////////////////////////////
/// @brief create an AST limit node
////////////////////////////////////////////////////////////////////////////////

        AstNode* createNodeLimit (AstNode const*,
                                  AstNode const*);

////////////////////////////////////////////////////////////////////////////////
/// @brief create an AST assign node
////////////////////////////////////////////////////////////////////////////////

        AstNode* createNodeAssign (char const*,
                                   AstNode const*);

////////////////////////////////////////////////////////////////////////////////
/// @brief create an AST variable node
////////////////////////////////////////////////////////////////////////////////

        AstNode* createNodeVariable (char const*,
                                     bool);

////////////////////////////////////////////////////////////////////////////////
/// @brief create an AST collection node
////////////////////////////////////////////////////////////////////////////////

        AstNode* createNodeCollection (char const*);

////////////////////////////////////////////////////////////////////////////////
/// @brief create an AST reference node
////////////////////////////////////////////////////////////////////////////////

        AstNode* createNodeReference (char const*);

////////////////////////////////////////////////////////////////////////////////
/// @brief create an AST parameter node
////////////////////////////////////////////////////////////////////////////////

        AstNode* createNodeParameter (char const*);

////////////////////////////////////////////////////////////////////////////////
/// @brief create an AST unary operator
////////////////////////////////////////////////////////////////////////////////

        AstNode* createNodeUnaryOperator (AstNodeType type,
                                          AstNode const*);

////////////////////////////////////////////////////////////////////////////////
/// @brief create an AST binary operator
////////////////////////////////////////////////////////////////////////////////

        AstNode* createNodeBinaryOperator (AstNodeType type,
                                           AstNode const*,
                                           AstNode const*);

////////////////////////////////////////////////////////////////////////////////
/// @brief create an AST ternary operator
////////////////////////////////////////////////////////////////////////////////

        AstNode* createNodeTernaryOperator (AstNode const*,
                                            AstNode const*,
                                            AstNode const*);

////////////////////////////////////////////////////////////////////////////////
/// @brief create an AST subquery node
////////////////////////////////////////////////////////////////////////////////

        AstNode* createNodeSubquery (char const*,
                                     AstNode const*);

////////////////////////////////////////////////////////////////////////////////
/// @brief create an AST attribute access node
////////////////////////////////////////////////////////////////////////////////

        AstNode* createNodeAttributeAccess (AstNode const*,
                                            char const*);

////////////////////////////////////////////////////////////////////////////////
/// @brief create an AST attribute access node w/ bind parameter
////////////////////////////////////////////////////////////////////////////////

        AstNode* createNodeBoundAttributeAccess (AstNode const*,
                                                 AstNode const*);

////////////////////////////////////////////////////////////////////////////////
/// @brief create an AST index access node
////////////////////////////////////////////////////////////////////////////////

        AstNode* createNodeIndexedAccess (AstNode const*,
                                          AstNode const*);

////////////////////////////////////////////////////////////////////////////////
/// @brief create an AST expand node
////////////////////////////////////////////////////////////////////////////////

        AstNode* createNodeExpand (AstNode const*,
                                   AstNode const*);

////////////////////////////////////////////////////////////////////////////////
/// @brief create an AST iterator node
////////////////////////////////////////////////////////////////////////////////

        AstNode* createNodeIterator (char const*,
                                     AstNode const*);

////////////////////////////////////////////////////////////////////////////////
/// @brief create an AST null value node
////////////////////////////////////////////////////////////////////////////////

        static AstNode* createNodeValueNull ();

////////////////////////////////////////////////////////////////////////////////
/// @brief create an AST bool value node
////////////////////////////////////////////////////////////////////////////////

        static AstNode* createNodeValueBool (bool);

////////////////////////////////////////////////////////////////////////////////
/// @brief create an AST int value node
////////////////////////////////////////////////////////////////////////////////

        AstNode* createNodeValueInt (int64_t);

////////////////////////////////////////////////////////////////////////////////
/// @brief create an AST double value node
////////////////////////////////////////////////////////////////////////////////

        AstNode* createNodeValueDouble (double);

////////////////////////////////////////////////////////////////////////////////
/// @brief create an AST string value node
////////////////////////////////////////////////////////////////////////////////

        AstNode* createNodeValueString (char const*);

////////////////////////////////////////////////////////////////////////////////
/// @brief create an AST list node
////////////////////////////////////////////////////////////////////////////////

        AstNode* createNodeList ();

////////////////////////////////////////////////////////////////////////////////
/// @brief create an AST array node
////////////////////////////////////////////////////////////////////////////////

        AstNode* createNodeArray ();

////////////////////////////////////////////////////////////////////////////////
/// @brief create an AST array element node
////////////////////////////////////////////////////////////////////////////////

        AstNode* createNodeArrayElement (char const*,
                                         AstNode const*);

////////////////////////////////////////////////////////////////////////////////
/// @brief create an AST function call node
////////////////////////////////////////////////////////////////////////////////

        AstNode* createNodeFunctionCall (char const*,
                                         AstNode const*);

////////////////////////////////////////////////////////////////////////////////
/// @brief create an AST range node
////////////////////////////////////////////////////////////////////////////////

        AstNode* createNodeRange (AstNode const*,
                                  AstNode const*); 

////////////////////////////////////////////////////////////////////////////////
/// @brief create an AST nop node
////////////////////////////////////////////////////////////////////////////////

        AstNode* createNodeNop ();

////////////////////////////////////////////////////////////////////////////////
/// @brief injects bind parameters into the AST
////////////////////////////////////////////////////////////////////////////////

        void injectBindParameters (BindParameters&);

////////////////////////////////////////////////////////////////////////////////
/// @brief replace variables
////////////////////////////////////////////////////////////////////////////////

        AstNode* replaceVariables (AstNode*,
                                   std::unordered_map<VariableId, Variable const*> const&);

////////////////////////////////////////////////////////////////////////////////
/// @brief optimizes the AST
////////////////////////////////////////////////////////////////////////////////

        void optimize ();

////////////////////////////////////////////////////////////////////////////////
/// @brief determines the variables referenced in an expression
////////////////////////////////////////////////////////////////////////////////

        static std::unordered_set<Variable*> getReferencedVariables (AstNode const*);


////////////////////////////////////////////////////////////////////////////////
/// @brief recursively clone a node
////////////////////////////////////////////////////////////////////////////////

        AstNode* clone (AstNode const*);

////////////////////////////////////////////////////////////////////////////////
/// @brief get the reversed operator for a comparison operator
////////////////////////////////////////////////////////////////////////////////

        static AstNodeType ReverseOperator (AstNodeType);

// -----------------------------------------------------------------------------
// --SECTION--                                                   private methods
// -----------------------------------------------------------------------------

      private:

////////////////////////////////////////////////////////////////////////////////
/// @brief create a number node for an arithmetic result, integer
////////////////////////////////////////////////////////////////////////////////

        AstNode* createArithmeticResultNode (int64_t);

////////////////////////////////////////////////////////////////////////////////
/// @brief create a number node for an arithmetic result, double
////////////////////////////////////////////////////////////////////////////////

        AstNode* createArithmeticResultNode (double);

////////////////////////////////////////////////////////////////////////////////
/// @brief executes an expression with constant parameters
////////////////////////////////////////////////////////////////////////////////

        AstNode* executeConstExpression (AstNode const*);

////////////////////////////////////////////////////////////////////////////////
/// @brief optimizes the unary operators + and -
/// the unary plus will be converted into a simple value node if the operand of
/// the operation is a constant number
////////////////////////////////////////////////////////////////////////////////

        AstNode* optimizeUnaryOperatorArithmetic (AstNode*);

////////////////////////////////////////////////////////////////////////////////
/// @brief optimizes the unary operator NOT with a non-constant expression
////////////////////////////////////////////////////////////////////////////////

        AstNode* optimizeNotExpression (AstNode*);

////////////////////////////////////////////////////////////////////////////////
/// @brief optimizes the unary operator NOT
////////////////////////////////////////////////////////////////////////////////

        AstNode* optimizeUnaryOperatorLogical (AstNode*);

////////////////////////////////////////////////////////////////////////////////
/// @brief optimizes the binary logical operators && and ||
////////////////////////////////////////////////////////////////////////////////

        AstNode* optimizeBinaryOperatorLogical (AstNode*, bool);

////////////////////////////////////////////////////////////////////////////////
/// @brief optimizes the binary relational operators <, <=, >, >=, ==, != and IN
////////////////////////////////////////////////////////////////////////////////

        AstNode* optimizeBinaryOperatorRelational (AstNode*);

////////////////////////////////////////////////////////////////////////////////
/// @brief optimizes the binary arithmetic operators +, -, *, / and %
////////////////////////////////////////////////////////////////////////////////

        AstNode* optimizeBinaryOperatorArithmetic (AstNode*);

////////////////////////////////////////////////////////////////////////////////
/// @brief optimizes the ternary operator
////////////////////////////////////////////////////////////////////////////////

        AstNode* optimizeTernaryOperator (AstNode*);

////////////////////////////////////////////////////////////////////////////////
/// @brief optimizes a call to a built-in function
////////////////////////////////////////////////////////////////////////////////

        AstNode* optimizeFunctionCall (AstNode*);

////////////////////////////////////////////////////////////////////////////////
/// @brief optimizes a reference to a variable
////////////////////////////////////////////////////////////////////////////////

        AstNode* optimizeReference (AstNode*);

////////////////////////////////////////////////////////////////////////////////
/// @brief optimizes the LET statement
////////////////////////////////////////////////////////////////////////////////

        AstNode* optimizeLet (AstNode*);

////////////////////////////////////////////////////////////////////////////////
/// @brief optimizes the FILTER statement
////////////////////////////////////////////////////////////////////////////////

        AstNode* optimizeFilter (AstNode*);

////////////////////////////////////////////////////////////////////////////////
/// @brief optimizes the FOR statement
/// no real optimizations are done here, but we do an early check if the
/// FOR loop operand is actually a list 
////////////////////////////////////////////////////////////////////////////////

        AstNode* optimizeFor (AstNode*);

////////////////////////////////////////////////////////////////////////////////
/// @brief create an AST node from JSON
////////////////////////////////////////////////////////////////////////////////

        AstNode* nodeFromJson (TRI_json_t const*);

////////////////////////////////////////////////////////////////////////////////
/// @brief traverse the AST, using pre- and post-order visitors
////////////////////////////////////////////////////////////////////////////////

        static AstNode* traverse (AstNode*,
                                  std::function<void(AstNode const*, void*)>,
                                  std::function<AstNode*(AstNode*, void*)>,
                                  std::function<void(AstNode const*, void*)>,
                                  void*);

////////////////////////////////////////////////////////////////////////////////
/// @brief traverse the AST using a visitor
////////////////////////////////////////////////////////////////////////////////

        static AstNode* traverse (AstNode*,
                                  std::function<AstNode*(AstNode*, void*)>,
                                  void*);

////////////////////////////////////////////////////////////////////////////////
/// @brief traverse the AST using a visitor, with const nodes
////////////////////////////////////////////////////////////////////////////////

        static void traverse (AstNode const*,
                              std::function<void(AstNode const*, void*)>,
                              void*);

////////////////////////////////////////////////////////////////////////////////
/// @brief normalize a function name
////////////////////////////////////////////////////////////////////////////////

        std::pair<std::string, bool> normalizeFunctionName (char const*);

////////////////////////////////////////////////////////////////////////////////
/// @brief create a node of the specified type
////////////////////////////////////////////////////////////////////////////////

        AstNode* createNode (AstNodeType);

// -----------------------------------------------------------------------------
// --SECTION--                                                  public variables
// -----------------------------------------------------------------------------

      public:

////////////////////////////////////////////////////////////////////////////////
/// @brief negated comparison operators
////////////////////////////////////////////////////////////////////////////////

        static std::unordered_map<int, AstNodeType> const NegatedOperators;

////////////////////////////////////////////////////////////////////////////////
/// @brief reverse comparison operators
////////////////////////////////////////////////////////////////////////////////

        static std::unordered_map<int, AstNodeType> const ReversedOperators;

// -----------------------------------------------------------------------------
// --SECTION--                                                 private variables
// -----------------------------------------------------------------------------

      private:

////////////////////////////////////////////////////////////////////////////////
/// @brief the query
////////////////////////////////////////////////////////////////////////////////

        Query*                             _query;

////////////////////////////////////////////////////////////////////////////////
/// @brief all scopes used in the query
////////////////////////////////////////////////////////////////////////////////
        
        Scopes                             _scopes;

////////////////////////////////////////////////////////////////////////////////
/// @brief generator for variables
////////////////////////////////////////////////////////////////////////////////

        VariableGenerator                  _variables;

////////////////////////////////////////////////////////////////////////////////
/// @brief the bind parameters we found in the query
////////////////////////////////////////////////////////////////////////////////

        std::unordered_set<std::string>    _bindParameters;

////////////////////////////////////////////////////////////////////////////////
/// @brief root node of the AST
////////////////////////////////////////////////////////////////////////////////

        AstNode*                           _root;

////////////////////////////////////////////////////////////////////////////////
/// @brief root nodes of queries and subqueries
////////////////////////////////////////////////////////////////////////////////

        std::vector<AstNode*>              _queries;

////////////////////////////////////////////////////////////////////////////////
/// @brief which collection is going to be modified in the query 
////////////////////////////////////////////////////////////////////////////////

        AstNode const*                     _writeCollection;

////////////////////////////////////////////////////////////////////////////////
/// @brief a singleton no-op node instance
////////////////////////////////////////////////////////////////////////////////

        static AstNode const NopNode;

////////////////////////////////////////////////////////////////////////////////
/// @brief a singleton null node instance
////////////////////////////////////////////////////////////////////////////////

        static AstNode const NullNode;

////////////////////////////////////////////////////////////////////////////////
/// @brief a singleton false node instance
////////////////////////////////////////////////////////////////////////////////

        static AstNode const FalseNode;

////////////////////////////////////////////////////////////////////////////////
/// @brief a singleton true node instance
////////////////////////////////////////////////////////////////////////////////

        static AstNode const TrueNode;

////////////////////////////////////////////////////////////////////////////////
/// @brief a singleton zero node instance
////////////////////////////////////////////////////////////////////////////////

        static AstNode const ZeroNode;

////////////////////////////////////////////////////////////////////////////////
/// @brief a singleton empty string node instance
////////////////////////////////////////////////////////////////////////////////

        static AstNode const EmptyStringNode;

    };

  }
}

#endif

// -----------------------------------------------------------------------------
// --SECTION--                                                       END-OF-FILE
// -----------------------------------------------------------------------------

// Local Variables:
// mode: outline-minor
// outline-regexp: "/// @brief\\|/// {@inheritDoc}\\|/// @page\\|// --SECTION--\\|/// @\\}"
// End:
