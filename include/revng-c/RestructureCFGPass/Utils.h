#ifndef REVNGC_RESTRUCTURE_CFG_UTILS_H
#define REVNGC_RESTRUCTURE_CFG_UTILS_H

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

// Standard includes
#include <cstdlib>
#include <fstream>
#include <memory>
#include <set>
#include <sys/stat.h>

// Local libraries includes
#include "revng-c/RestructureCFGPass/ASTNode.h"
#include "revng-c/RestructureCFGPass/BasicBlockNode.h"

// TODO: move the definition of this object in an unique place, to avoid using
// an extern declaration
extern Logger<> CombLogger;

template<class NodeT>
using Edge = typename BasicBlockNode<NodeT>::EdgeDescriptor;

template<class NodeT>
inline void addEdge(std::pair<BasicBlockNode<NodeT> *,
                    BasicBlockNode<NodeT> *> NewEdge) {
  revng_assert(not NewEdge.first->isCheck());
  NewEdge.first->addSuccessor(NewEdge.second);
  NewEdge.second->addPredecessor(NewEdge.first);
}

template<class NodeT>
inline void removeEdge(std::pair<BasicBlockNode<NodeT> *,
                       BasicBlockNode<NodeT> *> Edge) {
  revng_assert(not Edge.first->isCheck());
  Edge.first->removeSuccessor(Edge.second);
  Edge.second->removePredecessor(Edge.first);
}

template<class NodeT>
inline void moveEdgeTarget(Edge<NodeT> Edge,
                           BasicBlockNode<NodeT> *NewTarget) {
  Edge.second->removePredecessor(Edge.first);

  // Special handle for dispatcher check nodes.
  if (Edge.first->isCheck()) {

    // Confirm that the old target of the edge was one of the two branches.
    revng_assert((Edge.first->getTrue() == Edge.second)
                 or (Edge.first->getFalse() == Edge.second));

    // Set the appropriate successor.
    if (Edge.first->getTrue() == Edge.second) {
      Edge.first->setTrue(NewTarget);
    } else if (Edge.first->getFalse() == Edge.second) {
      Edge.first->setFalse(NewTarget);
    } else {
      revng_abort("Wrong successor for check node");
    }
  } else {
    // General case when we are not handling a dispatcher check node.
    Edge.first->removeSuccessor(Edge.second);
    Edge.first->addSuccessor(NewTarget);
    NewTarget->addPredecessor(Edge.first);
  }
}

template<class NodeT>
// Helper function to find all nodes on paths between a source and a target
// node
inline std::set<BasicBlockNode<NodeT> *>
findReachableNodes(BasicBlockNode<NodeT> &Source,
                   BasicBlockNode<NodeT> &Target) {

  // Add to the Targets set the original target node.
  std::set<BasicBlockNode<NodeT> *> Targets;
  Targets.insert(&Target);

  // Exploration stack initialization.
  std::vector<std::pair<BasicBlockNode<NodeT> *, size_t>> Stack;
  Stack.push_back(std::make_pair(&Source, 0));

  // Visited nodes to avoid entering in a loop.
  std::set<Edge<NodeT>> VisitedEdges;

  // Exploration.
  while (!Stack.empty()) {
    auto StackElem = Stack.back();
    Stack.pop_back();
    BasicBlockNode<NodeT> *Vertex = StackElem.first;
    if (StackElem.second == 0) {
      if (Targets.count(Vertex) != 0) {
        for (auto StackElem : Stack) {
          Targets.insert(StackElem.first);
        }
        continue;
      }
    }
    size_t Index = StackElem.second;
    if (Index < StackElem.first->successor_size()) {
      BasicBlockNode<NodeT> *NextSuccessor = Vertex->getSuccessorI(Index);
      Index++;
      Stack.push_back(std::make_pair(Vertex, Index));
      if (VisitedEdges.count(std::make_pair(Vertex, NextSuccessor)) == 0
          and NextSuccessor != &Source) {
        Stack.push_back(std::make_pair(NextSuccessor, 0));
        VisitedEdges.insert(std::make_pair(Vertex, NextSuccessor));
      }
    }
  }

  return Targets;
}

inline void dumpASTOnFile(std::string FolderName,
                          std::string FunctionName,
                          std::string FileName,
                          ASTNode *RootNode) {

  std::ofstream ASTFile;
  std::string PathName = FolderName + "/" + FunctionName;
  mkdir(FolderName.c_str(), 0775);
  mkdir(PathName.c_str(), 0775);
  ASTFile.open(PathName + "/" + FileName + ".dot");
  ASTFile << "digraph CFGFunction {\n";
  RootNode->dump(ASTFile);
  ASTFile << "}\n";
  ASTFile.close();
}

#endif // REVNGC_RESTRUCTURE_CFG_UTILS_H
