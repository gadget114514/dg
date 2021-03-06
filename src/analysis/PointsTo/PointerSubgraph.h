#ifndef _DG_POINTER_SUBGRAPH_H_
#define _DG_POINTER_SUBGRAPH_H_

#include <cassert>
#include <vector>
#include <cstdarg>
#include <cstring> // for strdup

#include "Pointer.h"
#include "ADT/Queue.h"
#include "analysis/SubgraphNode.h"

namespace dg {
namespace analysis {
namespace pta {

void getNodes(std::set<PSNode *>& cont, PSNode *n, unsigned int dfsnum);
void getNodes(std::set<PSNode *>& cont, PSNode *n, PSNode *exit, unsigned int dfsnum);
    
enum class PSNodeType {
        // these are nodes that just represent memory allocation sites
        ALLOC = 1,
        DYN_ALLOC,
        LOAD,
        STORE,
        GEP,
        PHI,
        CAST,
        // support for calls via function pointers.
        // The FUNCTION node is the same as ALLOC
        // but having it as separate type has the nice
        // advantage of type checking
        FUNCTION,
        // support for interprocedural analysis,
        // operands are null terminated. It is a noop,
        // just for the user's convenience
        CALL,
        // call via function pointer
        CALL_FUNCPTR,
        // return from the subprocedure (in caller),
        // synonym to PHI
        CALL_RETURN,
        // this is the entry node of a subprocedure
        // and serves just as no op for our convenience,
        // can be optimized away later
        ENTRY,
        // this is the exit node of a subprocedure
        // that returns a value - works as phi node
        RETURN,
        // node that invalidates allocated memory
        // after returning from a function
        INVALIDATE_LOCALS,
        // node that invalidates memory after calling free
        // on a pointer
        FREE,
        // node that has only one points-to relation
        // that never changes
        CONSTANT,
        // no operation node - this nodes can be used as a branch or join
        // node for convenient PointerSubgraph generation. For example as an
        // unified entry to the function or unified return from the function.
        // These nodes can be optimized away later. No points-to computation
        // is performed on them
        NOOP,
        // copy whole block of memory
        MEMCPY,
        // special nodes
        NULL_ADDR,
        UNKNOWN_MEM,
        // tags memory as invalidated
        INVALIDATED
};

class PSNode : public SubgraphNode<PSNode>
{
    PSNodeType type;
    Offset offset; // for the case this node is GEP or MEMCPY
    Offset len; // for the case this node is MEMCPY

    // in some cases some nodes are kind of paired - like formal and actual
    // parameters or call and return node. Here the analasis can store
    // such a node - if it needs for generating the PointerSubgraph
    // - it is not used anyhow by the base analysis itself
    // XXX: maybe we cold store this somewhere in a map instead of in every
    // node (if the map is sparse, it would be much more memory efficient)
    PSNode *pairedNode;

    // in some cases we need to know from which function the node is
    // so we need to remember the entry node 
    PSNode *parent = nullptr;

    /// some additional information
    // was memory zeroed at initialization or right after allocating?
    bool zeroInitialized;
    // is memory allocated on heap?
    bool is_heap;
    // is it a global value?
    bool is_global;
    unsigned int dfsid;
public:
    ///
    // Construct a PSNode
    // \param t     type of the node
    // Different types take different arguments:
    //
    // ALLOC:        no argument
    // DYN_ALLOC:    no argument
    // FUNCTION:     no argument
    // NOOP:         no argument
    // ENTRY:        no argument
    // LOAD:         one argument representing pointer to location from where
    //               we're loading the value (another pointer in this case)
    // STORE:        first argument is the value (the pointer to be stored)
    //               in memory pointed by the second argument
    // GEP:          get pointer to memory on given offset (get element pointer)
    //               first argument is pointer to the memory, second is the offset
    //               (as Offset class instance, unknown offset is represented by
    //               UNKNOWN_OFFSET constant)
    // CAST:         cast pointer from one type to other type (like void * to
    //               int *). The pointers are just copied, so we can optimize
    //               away this node later. The argument is just the pointer
    //               (we don't care about types atm.)
    // MEMCPY:       Copy whole block of memory. <from> <to> <offset> <len>
    // FUNCTION:     Object representing the function in memory - so that it
    //               can be pointed to and used as an argument to the Pointer
    // CONSTANT:     node that keeps constant points-to information
    //               the argument is the pointer it points to
    // PHI:          phi node that gathers pointers from different paths in CFG
    //               arguments are null-terminated list of the relevant nodes
    //               from predecessors
    // CALL:         represents call of subprocedure,
    //               arguments are null-terminated list of nodes that can user
    //               use arbitrarily - they are not used by the analysis itself.
    //               The arguments can be used e. g. when mapping call arguments
    //               back to original CFG. Actually, the CALL node is not needed
    //               in most cases (just 'inline' the subprocedure into the PointerSubgraph
    //               when building it)
    // CALL_FUNCPTR: call via function pointer. The argument is the node that
    //               bears the pointers.
    //               FIXME: use more nodes (null-terminated list of pointer nodes)
    // CALL_RETURN:  site where given call returns. Bears the pointers
    //               returned from the subprocedure. Works like PHI
    // RETURN:       represents returning value from a subprocedure,
    //               works as a PHI node - it gathers pointers returned from
    //               the subprocedure
    // INVALIDATE_LOCALS:
    //               invalidates memory after returning from a function
    // FREE:         invalidates memory after calling free function on a pointer
    PSNode(PSNodeType t, ...)
    : SubgraphNode<PSNode>(), type(t), offset(0), pairedNode(nullptr),
      zeroInitialized(false), is_heap(false), is_global(false), dfsid(0)
    {
        // assing operands
        PSNode *op;
        va_list args;
        va_start(args, t);

        switch(type) {
            case PSNodeType::ALLOC:
            case PSNodeType::DYN_ALLOC:
            case PSNodeType::FUNCTION:
                // these always points-to itself
                // (they points to the node where the memory was allocated)
                addPointsTo(this, 0);
                break;
            case PSNodeType::NOOP:
            case PSNodeType::ENTRY:
            case PSNodeType::INVALIDATED:
                // no operands
                break;
            case PSNodeType::CAST:
            case PSNodeType::LOAD:
            case PSNodeType::CALL_FUNCPTR:
                operands.push_back(va_arg(args, PSNode *));
                break;
            case PSNodeType::STORE:
                operands.push_back(va_arg(args, PSNode *));
                operands.push_back(va_arg(args, PSNode *));
                break;
            case PSNodeType::MEMCPY:
                operands.push_back(va_arg(args, PSNode *));
                operands.push_back(va_arg(args, PSNode *));
                offset = va_arg(args, uint64_t);
                len = va_arg(args, uint64_t);
                break;
            case PSNodeType::GEP:
                operands.push_back(va_arg(args, PSNode *));
                offset = va_arg(args, uint64_t);
                break;
            case PSNodeType::CONSTANT:
                op = va_arg(args, PSNode *);
                offset = va_arg(args, uint64_t);
                pointsTo.insert(Pointer(op, offset));
                break;
            case PSNodeType::NULL_ADDR:
                pointsTo.insert(Pointer(this, 0));
                break;
            case PSNodeType::UNKNOWN_MEM:
                // UNKNOWN_MEMLOC points to itself
                pointsTo.insert(Pointer(this, UNKNOWN_OFFSET));
                break;
            case PSNodeType::INVALIDATE_LOCALS:
            case PSNodeType::FREE:
                operands.push_back(va_arg(args, PSNode *));
                break;
            case PSNodeType::CALL_RETURN:
            case PSNodeType::PHI:
            case PSNodeType::RETURN:
            case PSNodeType::CALL:
                op = va_arg(args, PSNode *);
                // the operands are null terminated
                while (op) {
                    operands.push_back(op);
                    op = va_arg(args, PSNode *);
                }
                break;
            default:
                assert(0 && "Unknown type");
        }

        va_end(args);
    }

    PSNodeType getType() const { return type; }

    void setOffset(uint64_t o) { offset = o; }
    
    void setParent(PSNode *p) { parent = p; }
    PSNode *getParent() { return parent; }

    PSNode *getPairedNode() const { return pairedNode; }
    void setPairedNode(PSNode *n) { pairedNode = n; }

    void setZeroInitialized() { zeroInitialized = true; }
    bool isZeroInitialized() const { return zeroInitialized; }

    void setIsHeap() { is_heap = true; }
    bool isHeap() const { return is_heap; }

    void setIsGlobal() { is_global = true; }
    bool isGlobal() { return is_global; }

    bool isNull() const { return type == PSNodeType::NULL_ADDR; }
    bool isUnknownMemory() const { return type == PSNodeType::UNKNOWN_MEM; }
    bool isInvalidated() const { return type == PSNodeType::INVALIDATED; }

    // make this public, that's basically the only
    // reason the PointerSubgraph node exists, so don't hide it
    PointsToSetT pointsTo;

    // convenient helper
    bool addPointsTo(PSNode *n, Offset o)
    {
        // do not add concrete offsets when we have the UNKNOWN_OFFSET
        // - unknown offset stands for any offset
        if (pointsTo.count(Pointer(n, UNKNOWN_OFFSET)))
            return false;

        if (o.isUnknown())
            return addPointsToUnknownOffset(n);
        else
            return pointsTo.insert(Pointer(n, o)).second;
    }

    bool addPointsTo(const Pointer& ptr)
    {
        return addPointsTo(ptr.target, ptr.offset);
    }

    bool addPointsTo(const std::set<Pointer>& ptrs)
    {
        bool changed = false;
        for (const Pointer& ptr: ptrs)
            changed |= addPointsTo(ptr);

        return changed;
    }

    bool doesPointsTo(const Pointer& p)
    {
        return pointsTo.count(p) == 1;
    }

    bool doesPointsTo(PSNode *n, Offset o = 0)
    {
        return doesPointsTo(Pointer(n, o));
    }

    bool addPointsToUnknownOffset(PSNode *target);

    // FIXME: maybe get rid of these friendships?
    friend class PointerAnalysis;
    friend class PointerSubgraph;

    friend void getNodes(std::set<PSNode *>& cont, PSNode *n, unsigned int dfsnum);
    friend void getNodes(std::set<PSNode *>& cont, PSNode *n, PSNode* exit, unsigned int dfsnum);
};

inline void getNodes(std::set<PSNode *>& cont, PSNode *n, unsigned int dfsnum)
{
    // default behaviour is to enqueue all pending nodes
    ++dfsnum;
    ADT::QueueFIFO<PSNode *> fifo;

    assert(n && "No starting node given."); 

    for (PSNode *succ : n->successors) {
        succ->dfsid = dfsnum;
        fifo.push(succ);
    }

    while (!fifo.empty()) {
        PSNode *cur = fifo.pop();
        bool ret = cont.insert(cur).second;
        assert(ret && "BUG: Tried to insert something twice");

        for (PSNode *succ : cur->successors) {
            if (succ->dfsid != dfsnum) {
                succ->dfsid = dfsnum;
                fifo.push(succ);
            }
        }
    }
}

inline void getNodes(std::set<PSNode *>& cont, PSNode *n, PSNode *exit, unsigned int dfsnum)
{
    // default behaviour is to enqueue all pending nodes
    ++dfsnum;
    ADT::QueueFIFO<PSNode *> fifo;

    assert(n && "No starting node given."); 

    for (PSNode *succ : n->successors) {
        succ->dfsid = dfsnum;
        fifo.push(succ);
    }

    while (!fifo.empty()) {
        PSNode *cur = fifo.pop();
        bool ret = cont.insert(cur).second;
        assert(ret && "BUG: Tried to insert something twice");

        for (PSNode *succ : cur->successors) {
            if (succ == exit) continue;
            if (succ->dfsid != dfsnum) {
                succ->dfsid = dfsnum;
                fifo.push(succ);
            }
        }
    }
}

class PointerSubgraph
{
    unsigned int dfsnum;

    // root of the pointer state subgraph
    PSNode *root;

public:
    PointerSubgraph() : dfsnum(0), root(nullptr) {}
    PointerSubgraph(PSNode *r) : dfsnum(0), root(r)
    {
        assert(root && "Cannot create PointerSubgraph with null root");
    }

    PSNode *getRoot() const { return root; }
    void setRoot(PSNode *r) { root = r; }

    // get nodes in BFS order and store them into
    // the container
    std::vector<PSNode *> getNodes(PSNode *start_node = nullptr,
                                   std::vector<PSNode *> *start_set = nullptr,
                                   unsigned expected_num = 0)
    {
        assert(root && "Do not have root");
        assert(!(start_set && start_node)
               && "Need either starting set or starting node, not both");

        ++dfsnum;
        ADT::QueueFIFO<PSNode *> fifo;

        if (start_set) {
            for (PSNode *s : *start_set) {
                fifo.push(s);
                s->dfsid = dfsnum;
            }
        } else {
            if (!start_node)
                start_node = root;

            fifo.push(start_node);
            start_node->dfsid = dfsnum;
        }

        std::vector<PSNode *> cont;
        if (expected_num != 0)
            cont.reserve(expected_num);

        while (!fifo.empty()) {
            PSNode *cur = fifo.pop();
            cont.push_back(cur);

            for (PSNode *succ : cur->successors) {
                if (succ->dfsid != dfsnum) {
                    succ->dfsid = dfsnum;
                    fifo.push(succ);
                }
            }
        }

        return cont;
    }

};

} // namespace pta
} // namespace analysis
} // namespace dg

#endif
